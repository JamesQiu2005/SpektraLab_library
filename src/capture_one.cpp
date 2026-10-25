#include "capture_one.hpp"

#include <sys/stat.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include "sqlite.hpp"

namespace fs = std::filesystem;

namespace spklib::c1 {

// --- fingerprint and snapshot ------------------------------------------------------

Json Fingerprint::to_json() const {
    return {{"size", size}, {"mtime_ns", mtime_ns}, {"change_counter", change_counter}};
}

Fingerprint Fingerprint::from_json(const Json& j) {
    Fingerprint f;
    if (!j.is_object()) return f;
    f.size = j.value("size", int64_t{0});
    f.mtime_ns = j.value("mtime_ns", int64_t{0});
    f.change_counter = j.value("change_counter", uint32_t{0});
    return f;
}

Located locate(const std::string& path) {
    fs::path p(path);
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && (p.extension() == ".cocatalogdb" || p.extension() == ".cosessiondb")) {
        return {p.parent_path().string(), p.string()};
    }
    if (!fs::is_directory(p, ec)) throw std::runtime_error("not a Capture One catalog: " + path);
    // `<Name>.cocatalog/<Name>.cocatalogdb` is the layout Capture One writes;
    // a renamed package keeps the old inner name, so fall back to the one
    // database in the folder. `.backup` and the journals are not databases.
    fs::path expected = p / (p.stem().string() + ".cocatalogdb");
    if (fs::is_regular_file(expected, ec)) return {p.string(), expected.string()};
    std::vector<fs::path> found;
    // A session folder holds `<Name>.cosessiondb` the same way
    // (docs/SESSION-DESIGN.md §2): probe reads it; linking one waits until
    // the session schema is verified.
    for (auto& e : fs::directory_iterator(p, ec)) {
        auto x = e.path().extension();
        if (e.is_regular_file() && (x == ".cocatalogdb" || x == ".cosessiondb")) found.push_back(e.path());
    }
    if (found.size() == 1) return {p.string(), found[0].string()};
    throw std::runtime_error(found.empty() ? "no .cocatalogdb in " + path
                                           : "more than one .cocatalogdb in " + path);
}

Fingerprint fingerprint(const std::string& database) {
    struct stat st {};
    if (::stat(database.c_str(), &st) != 0) throw std::runtime_error("cannot stat " + database);
    Fingerprint f;
    f.size = st.st_size;
    f.mtime_ns = int64_t(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
    std::ifstream in(database, std::ios::binary);
    unsigned char h[100] = {};
    in.read(reinterpret_cast<char*>(h), sizeof h);
    if (in.gcount() == 100 && std::string(reinterpret_cast<char*>(h), 16) == std::string("SQLite format 3\0", 16)) {
        f.change_counter = (uint32_t(h[24]) << 24) | (uint32_t(h[25]) << 16) | (uint32_t(h[26]) << 8) | h[27];
    }
    return f;
}

namespace {

bool nonempty_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0;
}

/// A plain read-and-write copy. Not `fs::copy_file`, whose platform paths may
/// try to clone or carry metadata; this opens the source O_RDONLY and nothing
/// else.
void copy_bytes(const fs::path& from, const fs::path& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + from.string());
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + to.string());
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.write(buf.data(), in.gcount());
    }
    out.flush();
    if (!out) throw std::runtime_error("short write to " + to.string());
}

}  // namespace

Snapshot take_snapshot(const std::string& database, const std::string& dest_dir, int retries) {
    fs::create_directories(dest_dir);
    const fs::path db(database);
    const fs::path journal = database + "-journal";
    const fs::path wal = database + "-wal";
    const fs::path out = fs::path(dest_dir) / "snapshot.cocatalogdb";
    const fs::path out_wal = fs::path(dest_dir) / "snapshot.cocatalogdb-wal";

    std::string last_reason;
    for (int attempt = 1; attempt <= retries; ++attempt) {
        if (attempt > 1) std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
        // A non-empty rollback journal is a write in progress, or one that
        // crashed and will be rolled back when Capture One next opens the
        // file. Either way the database alone is not a committed state.
        if (nonempty_file(journal)) { last_reason = "Capture One is writing to the catalog (hot journal)"; continue; }

        Fingerprint before = fingerprint(database);
        copy_bytes(db, out);
        std::error_code ec;
        fs::remove(out_wal, ec);
        // WAL mode is not what Capture One uses, but if it ever does, the
        // committed state is the database *plus* the WAL. Copy both and fold
        // the WAL into the copy -- the copy is ours to write.
        bool had_wal = nonempty_file(wal);
        if (had_wal) copy_bytes(wal, out_wal);
        Fingerprint after = fingerprint(database);
        if (!(before == after) || nonempty_file(journal)) {
            last_reason = "the catalog changed while it was being copied";
            continue;
        }
        if (had_wal) {
            Db w(out.string(), Db::Mode::ReadWrite);
            w.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            w.exec("PRAGMA journal_mode=DELETE");
        }
        {
            Db check(out.string(), Db::Mode::ReadOnlyImmutable);
            auto s = check.prepare("PRAGMA quick_check");
            std::string verdict = s.step() ? s.text(0) : "no answer";
            if (verdict != "ok") { last_reason = "the copy failed quick_check: " + verdict; continue; }
        }
        return {out.string(), before, attempt};
    }
    throw SourceBusy(last_reason.empty() ? "could not snapshot the catalog" : last_reason);
}

// --- schema ----------------------------------------------------------------------------

const std::vector<std::pair<std::string, std::string>>& required_schema() {
    static const std::vector<std::pair<std::string, std::string>> r = {
        {"ZENTITIES", "Z_ENT"}, {"ZENTITIES", "ZNAME"},
        {"ZDOCUMENTCONTENT", "ZDOCUMENTUUID"},
        {"ZPATHLOCATION", "Z_PK"}, {"ZPATHLOCATION", "ZMACROOT"}, {"ZPATHLOCATION", "ZRELATIVEPATH"},
        {"ZPATHLOCATION", "ZISRELATIVE"},
        {"ZIMAGE", "Z_PK"}, {"ZIMAGE", "ZIMAGEUUID"}, {"ZIMAGE", "ZIMAGEFILENAME"}, {"ZIMAGE", "ZIMAGELOCATION"},
        {"ZVARIANT", "Z_PK"}, {"ZVARIANT", "ZVARIANTUUID"}, {"ZVARIANT", "ZIMAGE"}, {"ZVARIANT", "ZINDEX"},
        {"ZVARIANT", "ZCOMBINEDSETTINGS"},
        {"ZVARIANTLAYER", "Z_PK"},
        {"ZVARIANTMETADATA", "ZLAYER"}, {"ZVARIANTMETADATA", "ZBASIC_RATING"},
        {"ZCOLLECTION", "Z_PK"}, {"ZCOLLECTION", "Z_ENT"}, {"ZCOLLECTION", "ZNAME"}, {"ZCOLLECTION", "ZPARENT"},
        {"ZIMAGEINCOLLECTION", "ZCOLLECTION"}, {"ZIMAGEINCOLLECTION", "ZIMAGE"},
    };
    return r;
}

std::string collection_kind(const std::string& e) {
    static const std::map<std::string, std::string> m = {
        {"ProjectCollection", "project"},
        {"VirtualFolderCollection", "group"},
        {"AlbumCollection", "album"},
        {"SmartCollection", "smart_album"},
        {"CatalogFolderCollection", "folder"},
        {"SessionFolderCollection", "folder"},
        {"TempFolderCollection", "folder"},
        {"TrashCollection", "trash"},
        {"CatalogAllImagesCollection", "all_images"},
        {"CatalogInternalImagesCollection", "in_library"},
        {"VariantCollection", "variant_album"},
        {"SlideshowCollection", "slideshow"},
        {"CloudCollection", "cloud"},
        {"WCSCollection", "web"},
    };
    auto it = m.find(e);
    return it == m.end() ? "other" : it->second;
}

std::string resolve(const PathLocation& loc, const std::string& package) {
    std::string rel = loc.relative_path;
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    std::string base = loc.relative ? package : loc.mac_root;
    // Volume roots keep their trailing spaces ("/Volumes/NIKON Z 8   "): they
    // are part of the mount point's name. Only a trailing '/' is a separator.
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (rel.empty()) return base;
    return base == "/" ? "/" + rel : base + "/" + rel;
}

namespace {

/// `name` if the column exists, else NULL -- so one statement reads every
/// Capture One version that has the required columns.
struct Columns {
    Db& db;
    std::string table;
    std::string operator()(const std::string& name) const {
        return db.has_column(table, name) ? table + "." + name : "NULL";
    }
};

std::optional<std::string> nonempty(std::optional<std::string> s) {
    if (s && s->find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
    return s;
}

std::string image_kind(std::optional<int64_t> classification, const std::string& file) {
    if (classification) {
        switch (*classification) {
        case 17: return "raw";
        case 19: return "raster";
        case 6: return "movie";
        default: break;
        }
    }
    // Classification codes are only known for the three above; fall back to
    // the extension, which is what the app's decoder decides on anyway.
    auto dot = file.rfind('.');
    std::string ext = dot == std::string::npos ? "" : file.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::set<std::string> raw = {"nef", "arw", "cr2", "cr3", "raf", "dng", "orf", "rw2", "pef", "srw", "iiq", "3fr", "fff", "nrw", "rwl", "x3f", "erf", "mos", "dcr", "kdc", "mrw"};
    static const std::set<std::string> raster = {"jpg", "jpeg", "tif", "tiff", "png", "heic", "heif", "psd", "psb", "webp", "avif", "jxl"};
    static const std::set<std::string> movie = {"mov", "mp4", "m4v", "avi", "mts", "mxf"};
    if (raw.count(ext)) return "raw";
    if (raster.count(ext)) return "raster";
    if (movie.count(ext)) return "movie";
    return "other";
}

}  // namespace

Catalog read(const std::string& snapshot_path) {
    Db db(snapshot_path, Db::Mode::ReadOnlyImmutable);
    std::vector<std::string> missing;
    for (auto& [t, c] : required_schema()) {
        if (!db.has_column(t, c)) missing.push_back(t + "." + c);
    }
    if (!missing.empty()) {
        std::string list;
        for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
        throw std::runtime_error("unsupported Capture One schema, missing: " + list);
    }

    Catalog cat;
    std::map<int64_t, std::string> entity;
    {
        auto s = db.prepare("SELECT Z_ENT, ZNAME FROM ZENTITIES");
        while (s.step()) entity[s.i64(0)] = s.text(1);
    }
    {
        Columns c{db, "ZDOCUMENTCONTENT"};
        auto s = db.prepare("SELECT ZDOCUMENTUUID, " + c("ZDOCUMENTTYPE") + ", " + c("ZDATECREATED") + ", " +
                            c("ZDATEMODIFIED") + ", " + c("ZROOTCOLLECTION") + " FROM ZDOCUMENTCONTENT ORDER BY Z_PK LIMIT 1");
        if (s.step()) {
            cat.document_uuid = s.text(0);
            cat.document_type = s.opt_i64(1).value_or(0);
            cat.created_at = s.opt_f64(2);
            cat.modified_at = s.opt_f64(3);
            cat.root_collection = s.opt_i64(4).value_or(0);
        }
    }
    if (db.has_table("ZVERSIONINFO")) {
        auto s = db.prepare("SELECT ZFORMAT, ZCOMPATIBLEVERSION FROM ZVERSIONINFO ORDER BY ZVERSION DESC, Z_PK DESC LIMIT 1");
        if (s.step()) {
            cat.format = s.text(0);
            cat.compatible_version = s.opt_i64(1).value_or(0);
        }
    }
    {
        Columns c{db, "ZPATHLOCATION"};
        auto s = db.prepare("SELECT Z_PK, ZISRELATIVE, ZMACROOT, ZRELATIVEPATH, " + c("ZWINROOT") + ", " + c("ZVOLUME") +
                            " FROM ZPATHLOCATION ORDER BY Z_PK");
        while (s.step()) {
            PathLocation l;
            l.pk = s.i64(0);
            l.relative = s.i64(1) != 0;
            l.mac_root = s.text(2);
            l.relative_path = s.text(3);
            l.win_root = s.text(4);
            l.volume = s.text(5);
            cat.locations.push_back(std::move(l));
        }
    }
    {
        Columns c{db, "ZIMAGE"};
        std::string sql = "SELECT Z_PK, ZIMAGEUUID, ZIMAGEFILENAME, " + c("ZDISPLAYNAME") + ", ZIMAGELOCATION, " +
                          c("ZISINSIDECATALOG") + ", " + c("ZISTRASHED") + ", " + c("ZIMAGECLASSIFICATION") + ", " +
                          c("ZWIDTH") + ", " + c("ZHEIGHT") + ", " + c("ZFILE_SIZE") + ", " + c("ZISO") + ", " +
                          c("ZEXP_DATE") + ", " + c("ZIMPORTDATE") + ", " + c("ZEXP_APERTURE") + ", " +
                          c("ZEXP_SHUTTERSPEED") + ", " + c("ZEXP_FOCALLENGTH") + ", " + c("ZCAMERA_MAKE") + ", " +
                          c("ZCAMERA_MODEL") + ", " + c("ZCAMERA_LENS") + ", " + c("ZEXP_EXPOSURECOMPENSATION") + ", " +
                          c("ZGPSLATITUDE") + ", " + c("ZGPSLONGITUDE") + ", " + c("ZGPSALTITUDE") +
                          " FROM ZIMAGE ORDER BY Z_PK";
        auto s = db.prepare(sql);
        while (s.step()) {
            Image im;
            im.pk = s.i64(0);
            im.uuid = s.text(1);
            im.file_name = s.text(2);
            im.display_name = s.text(3);
            im.location = s.opt_i64(4).value_or(0);
            im.inside_catalog = s.opt_i64(5).value_or(0) != 0;
            im.trashed = s.opt_i64(6).value_or(0) != 0;
            im.kind = image_kind(s.opt_i64(7), im.file_name);
            im.width = s.opt_i64(8);
            im.height = s.opt_i64(9);
            im.file_size = s.opt_i64(10);
            im.iso = s.opt_i64(11);
            im.captured_local = s.opt_f64(12);
            im.imported_at = s.opt_f64(13);
            im.aperture = s.opt_f64(14);
            im.shutter = s.opt_f64(15);
            im.focal_length = s.opt_f64(16);
            im.camera_make = nonempty(s.opt_text(17));
            im.camera_model = nonempty(s.opt_text(18));
            im.lens = nonempty(s.opt_text(19));
            im.exposure_comp = nonempty(s.opt_text(20));
            im.gps_lat = nonempty(s.opt_text(21));
            im.gps_lon = nonempty(s.opt_text(22));
            im.gps_alt = nonempty(s.opt_text(23));
            if (im.uuid.empty()) { cat.warnings.push_back("image " + std::to_string(im.pk) + " has no UUID; skipped"); continue; }
            cat.images.push_back(std::move(im));
        }
    }
    {
        // The effective metadata is the combined layer's -- the merge of the
        // default and adjustment layers that Capture One shows. The
        // adjustment layer is the fallback for a variant without one.
        Columns m{db, "ZVARIANTMETADATA"};
        Columns l{db, "ZVARIANTLAYER"};
        Columns v{db, "ZVARIANT"};
        static const std::vector<std::pair<const char*, const char*>> text_fields = {
            {"ZSTATUS_TITLE", "title"}, {"ZCONTENT_HEADLINE", "headline"},
            {"ZCONTENT_DESCRIPTION", "description"}, {"ZCONTENT_KEYWORDS", "keywords"},
            {"ZCONTACT_CREATOR", "creator"}, {"ZSTATUS_COPYRIGHTNOTICE", "copyright"},
            {"ZBASIC_LABEL", "label"},
        };
        std::string sql = "SELECT ZVARIANT.Z_PK, ZVARIANTUUID, ZVARIANT.ZIMAGE, ZVARIANT.ZINDEX, " + v("ZISMODIFIED") +
                          ", ZVARIANTMETADATA.ZBASIC_RATING, " + m("ZCOLOR_TAG_INDEX") + ", " + l("ZCROP") + ", " +
                          l("ZROTATION") + ", " + l("ZFLIP") + ", " + l("ZCROPASPECTRATIONAME");
        for (auto& [col, _] : text_fields) sql += ", " + m(col);
        sql += " FROM ZVARIANT LEFT JOIN ZVARIANTLAYER ON ZVARIANTLAYER.Z_PK = COALESCE(ZVARIANT.ZCOMBINEDSETTINGS, " +
               v("ZADJUSTMENTLAYER") + ") LEFT JOIN ZVARIANTMETADATA ON ZVARIANTMETADATA.ZLAYER = ZVARIANTLAYER.Z_PK"
               " ORDER BY ZVARIANT.ZIMAGE, ZVARIANT.ZINDEX, ZVARIANT.Z_PK";
        auto s = db.prepare(sql);
        std::map<int64_t, int64_t> seen_per_image;
        std::set<int64_t> seen_variant;
        while (s.step()) {
            Variant va;
            va.pk = s.i64(0);
            // A layer can carry more than one metadata row in principle; the
            // first one wins and the rest are not separate variants.
            if (!seen_variant.insert(va.pk).second) continue;
            va.uuid = s.text(1);
            va.image = s.i64(2);
            va.position = ++seen_per_image[va.image];
            va.modified = s.opt_i64(4).value_or(0) != 0;
            va.rating = s.opt_i64(5);
            va.color_tag = s.opt_i64(6);
            if (va.color_tag && *va.color_tag == 0) va.color_tag.reset();
            if (auto crop = nonempty(s.opt_text(7))) va.geometry["crop"] = *crop;
            if (auto r = s.opt_f64(8)) va.geometry["rotation"] = *r;
            if (auto f = s.opt_i64(9)) va.geometry["flip"] = *f;
            if (auto a = nonempty(s.opt_text(10))) va.geometry["aspect_name"] = *a;
            for (size_t i = 0; i < text_fields.size(); ++i) {
                if (auto t = nonempty(s.opt_text(11 + static_cast<int>(i)))) va.meta[text_fields[i].second] = *t;
            }
            if (va.uuid.empty()) { cat.warnings.push_back("variant " + std::to_string(va.pk) + " has no UUID; skipped"); continue; }
            cat.variants.push_back(std::move(va));
        }
    }
    {
        Columns c{db, "ZCOLLECTION"};
        auto s = db.prepare("SELECT Z_PK, Z_ENT, " + c("ZUUID") + ", ZNAME, ZPARENT, " + c("ZCOLLECTIONINDEX") + ", " +
                            c("ZFOLDERLOCATION") + ", " + c("ZSMARTCRITERIA") + ", " + c("ZSORTORDER") + ", " +
                            c("ZDATECREATED") + " FROM ZCOLLECTION ORDER BY Z_PK");
        while (s.step()) {
            Collection co;
            co.pk = s.i64(0);
            co.entity = entity.count(s.i64(1)) ? entity[s.i64(1)] : "entity " + std::to_string(s.i64(1));
            co.kind = collection_kind(co.entity);
            co.uuid = s.text(2);
            if (co.uuid.empty()) co.uuid = "c1-collection-" + std::to_string(co.pk);
            co.name = s.text(3);
            co.parent = s.opt_i64(4);
            co.position = s.opt_i64(5);
            co.folder_location = s.opt_i64(6);
            co.smart_criteria = nonempty(s.opt_text(7));
            co.sort_order = s.text(8);
            co.created_at = s.opt_f64(9);
            cat.collections.push_back(std::move(co));
        }
        if (cat.root_collection) {
            auto r = db.prepare("SELECT " + c("ZLASTIMPORTSCOLLECTION") + ", " + c("ZLASTCAPTURESCOLLECTION") +
                                " FROM ZCOLLECTION WHERE Z_PK = ?");
            r.bind(1, cat.root_collection);
            if (r.step()) {
                cat.recent_imports_collection = r.opt_i64(0).value_or(0);
                cat.recent_captures_collection = r.opt_i64(1).value_or(0);
            }
        }
    }
    {
        // Membership is copied for albums only. Folders are the images whose
        // location is the folder's; the virtual collections (All Images,
        // Trash, In Catalog, Recent ...) are queries.
        std::set<int64_t> albums;
        for (auto& co : cat.collections) {
            if (co.kind == "album" || co.kind == "variant_album") albums.insert(co.pk);
        }
        auto s = db.prepare("SELECT ZCOLLECTION, ZIMAGE FROM ZIMAGEINCOLLECTION ORDER BY Z_PK");
        while (s.step()) {
            if (albums.count(s.i64(0))) cat.album_members.emplace_back(s.i64(0), s.i64(1));
        }
    }
    if (db.has_table("ZKEYWORD") && db.has_column("ZKEYWORD", "ZNAME")) {
        auto s = db.prepare("SELECT Z_PK, ZNAME, ZPARENT FROM ZKEYWORD ORDER BY Z_PK");
        while (s.step()) cat.keywords.push_back({s.i64(0), s.text(1), s.opt_i64(2)});
    }
    return cat;
}

Json probe(const std::string& path, const std::string& scratch_dir) {
    Json out;
    Located loc = locate(path);
    out["package"] = loc.package;
    out["document"] = fs::path(loc.database).extension() == ".cosessiondb" ? "session" : "catalog";
    out["database"] = loc.database;
    out["fingerprint"] = fingerprint(loc.database).to_json();
    std::error_code ec;
    out["journal"] = nonempty_file(loc.database + "-journal") ? "hot" : "none";
    out["wal"] = nonempty_file(loc.database + "-wal");
    Snapshot snap = take_snapshot(loc.database, scratch_dir);
    try {
        Catalog cat = read(snap.path);
        {
            Db db(snap.path, Db::Mode::ReadOnlyImmutable);
            auto s = db.prepare("PRAGMA journal_mode");
            out["journal_mode"] = s.step() ? s.text(0) : "";
        }
        out["document_uuid"] = cat.document_uuid;
        out["document_type"] = cat.document_type;
        out["format"] = cat.format;
        out["compatible_version"] = cat.compatible_version;
        Json counts;
        counts["locations"] = cat.locations.size();
        counts["images"] = cat.images.size();
        counts["variants"] = cat.variants.size();
        counts["collections"] = cat.collections.size();
        counts["album_members"] = cat.album_members.size();
        counts["keywords"] = cat.keywords.size();
        size_t inside = 0, trashed = 0, rated = 0, tagged = 0;
        for (auto& i : cat.images) { inside += i.inside_catalog; trashed += i.trashed; }
        for (auto& v : cat.variants) { rated += v.rating.value_or(0) > 0; tagged += v.color_tag.has_value(); }
        counts["inside_catalog"] = inside;
        counts["trashed"] = trashed;
        counts["rated"] = rated;
        counts["color_tagged"] = tagged;
        out["counts"] = counts;
        out["warnings"] = cat.warnings;
        out["supported"] = true;
    } catch (const std::exception& e) {
        out["supported"] = false;
        out["reason"] = e.what();
    }
    fs::remove_all(scratch_dir, ec);
    return out;
}

}  // namespace spklib::c1
