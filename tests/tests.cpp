// tests.cpp -- the SDK through its C ABI, the way the app reaches it.
//
// Part 1 builds Capture One catalogs from the real 16.8 schema
// (fixtures/capture_one_16_8_schema.sql: tables and entity names, no user
// data) and drives every method against them. Part 2 runs only when
// SPKLIB_C1_CATALOG names a real catalog: it links it, checks every image
// against an independent reading of the same database, and proves the
// catalog package is byte-for-byte and stat-for-stat what it was.

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "spklib/spklib.h"
#include "sqlite.hpp"

namespace fs = std::filesystem;
using spklib::Db;
using spklib::Json;

// --- harness ---------------------------------------------------------------------------------

static int g_failed = 0, g_passed = 0;
static std::string g_test;

#define CHECK(cond)                                                                                    \
    do {                                                                                               \
        if (cond) ++g_passed;                                                                          \
        else { ++g_failed; std::fprintf(stderr, "  FAIL %s: %s (line %d)\n", g_test.c_str(), #cond, __LINE__); } \
    } while (0)

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        auto _a = (a); auto _b = (b);                                                                  \
        if (_a == _b) ++g_passed;                                                                      \
        else {                                                                                         \
            ++g_failed;                                                                                \
            std::ostringstream _s; _s << _a << " != " << _b;                                           \
            std::fprintf(stderr, "  FAIL %s: %s == %s: %s (line %d)\n", g_test.c_str(), #a, #b, _s.str().c_str(), __LINE__); \
        }                                                                                              \
    } while (0)

static void run(const std::string& name, const std::function<void()>& f) {
    g_test = name;
    int before = g_failed;
    try {
        f();
    } catch (const std::exception& e) {
        ++g_failed;
        std::fprintf(stderr, "  FAIL %s: threw %s\n", name.c_str(), e.what());
    }
    std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name.c_str());
}

struct Ctx {
    spklib_context* c;
    explicit Ctx(const Json& opts = Json::object()) { c = spklib_create(opts.dump().c_str(), nullptr); }
    ~Ctx() { spklib_destroy(c); }
    /// The result, or throws "code: message".
    Json operator()(const std::string& m, const Json& p = Json::object()) {
        char* out = nullptr;
        int rc = spklib_call(c, m.c_str(), p.dump().c_str(), &out);
        Json j = Json::parse(out);
        spklib_free(out);
        if (rc != SPKLIB_OK) throw std::runtime_error(j["code"].get<std::string>() + ": " + j["message"].get<std::string>());
        return j;
    }
    /// The error code of a call expected to fail ("" if it succeeded).
    std::string err(const std::string& m, const Json& p = Json::object()) {
        char* out = nullptr;
        int rc = spklib_call(c, m.c_str(), p.dump().c_str(), &out);
        Json j = Json::parse(out);
        spklib_free(out);
        return rc == SPKLIB_OK ? "" : j["code"].get<std::string>();
    }
};

static fs::path g_tmp;

static fs::path tmpdir(const std::string& name) {
    fs::path p = g_tmp / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

static void touch(const fs::path& p, const std::string& bytes = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Every file under `root`: path -> size, mtime and (for files under 256 MB)
/// a 64-bit FNV-1a of the bytes. What "the package did not change" means.
static std::map<std::string, std::string> census(const fs::path& root, bool hash = true) {
    std::map<std::string, std::string> out;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        struct stat st {};
        if (::lstat(it->path().c_str(), &st) != 0) continue;
        std::string v = std::to_string(st.st_size) + "/" + std::to_string(st.st_mtimespec.tv_sec) + "." +
                        std::to_string(st.st_mtimespec.tv_nsec) + "/" + std::to_string(st.st_mode);
        if (hash && S_ISREG(st.st_mode) && st.st_size < (256 << 20) && it->path().extension() != ".cot" &&
            it->path().extension() != ".cop" && it->path().extension() != ".cof") {
            uint64_t h = 1469598103934665603ull;
            std::ifstream in(it->path(), std::ios::binary);
            std::vector<char> buf(1 << 20);
            while (in) {
                in.read(buf.data(), std::streamsize(buf.size()));
                for (std::streamsize k = 0; k < in.gcount(); ++k) { h ^= uint8_t(buf[size_t(k)]); h *= 1099511628211ull; }
            }
            v += "/" + std::to_string(h);
        }
        out[it->path().string()] = v;
    }
    return out;
}

// --- a Capture One catalog, built ----------------------------------------------------------------

/// A catalog with the real schema and a small, known content:
///  - locations: an absolute folder with three images, a sub-folder with one,
///    a relative in-catalog folder with one
///  - variants: one each, image 1 has two; ratings 0/3/5, a colour tag
///  - collections: root, All Images, Recent Imports with one import album,
///    Recent Captures, Trash, Catalog; a user album; folders, one of them
///    with a parent row that is gone (as in the owner's real catalog)
struct FakeCatalog {
    fs::path package, db, photos;
    std::string uuid = "11111111-2222-3333-4444-555555555555";

    explicit FakeCatalog(const fs::path& root) {
        package = root / "Fake.cocatalog";
        photos = root / "Photos";
        fs::create_directories(package / "Originals/2026/01/02/1");
        db = package / "Fake.cocatalogdb";
        touch(photos / "A.NEF", "nef-a");
        touch(photos / "B.NEF", "nef-b");
        touch(photos / "C.JPG", "jpg-c");
        touch(photos / "Sub/D.ARW", "arw-d");
        touch(package / "Originals/2026/01/02/1/E.tif", "tif-e");
        Db d(db.string(), Db::Mode::Create);
        d.exec(read_file(fs::path(SPKLIB_FIXTURES) / "capture_one_16_8_schema.sql"));
        d.exec("INSERT INTO ZVERSIONINFO VALUES (32, 1, '16.8.6.25 Pro Mac', '9.0.0 Mac', 160800, '16.8.6.25 Pro Mac', 160800)");
        d.exec("INSERT INTO ZDOCUMENTCONTENT (Z_ENT, Z_PK, ZDATECREATED, ZDOCUMENTTYPE, ZDOCUMENTUUID, ZROOTCOLLECTION) VALUES (34, 1, 1729735821, 1, '" + uuid + "', 1)");
        std::string ph = photos.string();
        d.exec("INSERT INTO ZPATHLOCATION (Z_ENT, Z_PK, ZMACROOT, ZRELATIVEPATH, ZISRELATIVE, ZVOLUME) VALUES"
               " (38, 3, '/', '" + ph.substr(1) + "', 0, '/'),"
               " (38, 4, '/', '" + ph.substr(1) + "/Sub', 0, '/'),"
               " (38, 5, NULL, 'Originals/2026/01/02/1', 1, NULL),"
               " (38, 6, '/', '" + fs::path(ph).parent_path().string().substr(1) + "', 0, '/')");
        // ZEXP_DATE is the camera's wall clock as if UTC: 2024-01-08 23:39:04.
        d.exec("INSERT INTO ZIMAGE (Z_ENT, Z_PK, ZIMAGEUUID, ZIMAGEFILENAME, ZDISPLAYNAME, ZIMAGELOCATION, ZISINSIDECATALOG,"
               " ZISTRASHED, ZIMAGECLASSIFICATION, ZWIDTH, ZHEIGHT, ZEXP_DATE, ZIMPORTDATE, ZCAMERA_MAKE, ZCAMERA_MODEL, ZISO) VALUES"
               " (12, 1, 'IMG-A', 'A.NEF', 'A', 3, 0, 0, 17, 6048, 4032, 1704757144.25, 1729841315.9, 'NIKON CORPORATION', 'NIKON Z6_3', 250),"
               " (12, 2, 'IMG-B', 'B.NEF', 'B', 3, 0, 0, 17, 6048, 4032, 1704757148, 1729841315.9, 'NIKON CORPORATION', 'NIKON Z6_3', 1000),"
               " (12, 3, 'IMG-C', 'C.JPG', 'C', 3, 0, 1, 19, 4000, 3000, NULL, 1729841316, NULL, NULL, NULL),"
               " (12, 4, 'IMG-D', 'D.ARW', 'D', 4, 0, 0, 17, 9504, 6336, 1704757200, 1729841316, 'SONY', 'ILCE-7RM5', 100),"
               " (12, 5, 'IMG-E', 'E.tif', 'E', 5, 1, 0, 19, 100, 100, NULL, 1729841317, NULL, NULL, NULL)");
        // Variants and their combined layers (Z_ENT 20) carrying metadata.
        struct V { int pk, image, index, layer; const char* uuid; int rating; int color; };
        std::vector<V> vs = {{1, 1, 127, 101, "VAR-A1", 0, 0}, {2, 1, 255, 102, "VAR-A2", 4, 0}, {3, 2, 127, 103, "VAR-B1", 3, 2},
                             {4, 3, 127, 104, "VAR-C1", 5, 0}, {5, 4, 127, 105, "VAR-D1", 1, 0}, {6, 5, 127, 106, "VAR-E1", 0, 0}};
        for (auto& v : vs) {
            d.exec("INSERT INTO ZVARIANT (Z_ENT, Z_PK, ZINDEX, ZVARIANTUUID, ZIMAGE, ZCOMBINEDSETTINGS, ZISMODIFIED) VALUES (15, " +
                   std::to_string(v.pk) + ", " + std::to_string(v.index) + ", '" + v.uuid + "', " + std::to_string(v.image) + ", " +
                   std::to_string(v.layer) + ", 0)");
            d.exec("INSERT INTO ZVARIANTLAYER (Z_ENT, Z_PK, ZVARIANT, ZROTATION, ZFLIP) VALUES (20, " + std::to_string(v.layer) + ", " +
                   std::to_string(v.pk) + ", -90.0, 0)");
            d.exec("INSERT INTO ZVARIANTMETADATA (Z_ENT, Z_PK, ZLAYER, ZBASIC_RATING, ZCOLOR_TAG_INDEX, ZCONTACT_CREATOR) VALUES (13, " +
                   std::to_string(v.layer) + ", " + std::to_string(v.layer) + ", " + std::to_string(v.rating) + ", " +
                   std::to_string(v.color) + ", 'A Photographer')");
        }
        d.exec("INSERT INTO ZCOLLECTION (Z_ENT, Z_PK, ZUUID, ZNAME, ZPARENT, ZCOLLECTIONINDEX, ZFOLDERLOCATION, ZALLIMAGESCOLLECTION,"
               " ZLASTIMPORTSCOLLECTION, ZLASTCAPTURESCOLLECTION, ZTRASHCOLLECTION) VALUES"
               " (7, 1, 'COL-ROOT', 'root', NULL, 0, NULL, 2, 3, 4, 5),"
               " (40, 2, 'COL-ALL', '全部图像', 1, 1, NULL, NULL, NULL, NULL, NULL),"
               " (8, 3, 'COL-RI', '最近导入', 1, 2, NULL, NULL, NULL, NULL, NULL),"
               " (8, 4, 'COL-RC', '最近拍摄', 1, 3, NULL, NULL, NULL, NULL, NULL),"
               " (11, 5, 'COL-TRASH', '回收箱', 1, 4, NULL, NULL, NULL, NULL, NULL),"
               " (39, 6, 'COL-INCAT', 'Catalog', 1, 5, NULL, NULL, NULL, NULL, NULL),"
               " (2, 7, 'COL-IMPORT', '2026年10月4日 21:31', 3, 1, NULL, NULL, NULL, NULL, NULL),"
               " (2, 8, 'COL-PICKS', 'Picks', 1, 6, NULL, NULL, NULL, NULL, NULL),"
               " (5, 9, 'COL-SMART', 'Five stars', 1, 7, NULL, NULL, NULL, NULL, NULL),"
               " (36, 20, 'COL-F-ROOT', NULL, NULL, NULL, 6, NULL, NULL, NULL, NULL),"
               " (36, 21, 'COL-F-PHOTOS', NULL, 20, NULL, 3, NULL, NULL, NULL, NULL),"
               " (36, 22, 'COL-F-SUB', NULL, 999, NULL, 4, NULL, NULL, NULL, NULL)");
        d.exec("UPDATE ZCOLLECTION SET ZSMARTCRITERIA = '<rating>5</rating>' WHERE Z_PK = 9");
        // The import album's time, a UTC instant: 2026-10-04 13:31:37Z.
        d.exec("UPDATE ZCOLLECTION SET ZDATECREATED = 1791120697 WHERE Z_PK = 7");
        d.exec("UPDATE ZVARIANTMETADATA SET ZCONTENT_KEYWORDS = 'street, night;Shanghai' WHERE Z_PK = 101");
        d.exec("UPDATE ZVARIANTMETADATA SET ZCONTENT_KEYWORDS = 'night' WHERE Z_PK = 103");
        d.exec("INSERT INTO ZIMAGEINCOLLECTION (Z_ENT, Z_PK, ZCOLLECTION, ZIMAGE) VALUES"
               " (47, 1, 7, 1), (47, 2, 7, 2), (47, 3, 7, 3), (47, 4, 8, 2), (47, 5, 8, 4),"
               " (47, 6, 21, 1), (47, 7, 21, 2), (47, 8, 21, 3), (47, 9, 22, 4)");
    }

    /// Change the catalog the way Capture One would, between syncs.
    void edit(const std::string& sql) {
        Db d(db.string(), Db::Mode::ReadWrite);
        d.exec(sql);
    }
};

static Json find_by(const Json& arr, const std::string& key, const std::string& value) {
    for (auto& j : arr) if (j.contains(key) && j[key] == value) return j;
    return nullptr;
}

// --- part 1 ------------------------------------------------------------------------------------

static void synthetic() {
    run("probe reads version, counts and a verdict", [] {
        FakeCatalog fc(tmpdir("probe"));
        Ctx c;
        Json p = c("capture_one.probe", {{"path", fc.package.string()}});
        CHECK(p["supported"].get<bool>());
        CHECK_EQ(p["document_uuid"].get<std::string>(), fc.uuid);
        CHECK_EQ(p["format"].get<std::string>(), std::string("16.8.6.25 Pro Mac"));
        CHECK_EQ(p["journal_mode"].get<std::string>(), std::string("delete"));
        CHECK_EQ(p["counts"]["images"].get<int>(), 5);
        CHECK_EQ(p["counts"]["variants"].get<int>(), 6);
        CHECK_EQ(p["counts"]["trashed"].get<int>(), 1);
        CHECK_EQ(p["counts"]["inside_catalog"].get<int>(), 1);
        // The database file inside the package is accepted too.
        CHECK(c("capture_one.probe", {{"path", fc.db.string()}})["supported"].get<bool>());
    });

    run("link mirrors the catalog and never writes to it", [] {
        fs::path root = tmpdir("link");
        FakeCatalog fc(root);
        auto before = census(fc.package);
        Ctx c;
        Json r = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "Mirror").string()}});
        std::string L = r["library"];
        CHECK(fs::exists(root / "Mirror.spkcatalog/Library.spkdb"));
        CHECK_EQ(r["sync"]["images"]["added"].get<int>(), 5);
        CHECK_EQ(r["sync"]["variants"]["added"].get<int>(), 6);
        CHECK_EQ(r["sync"]["collections"]["parents_repaired"].get<int>(), 1);
        CHECK_EQ(r["info"]["origin"]["document_uuid"].get<std::string>(), fc.uuid);
        CHECK_EQ(r["info"]["origin"]["write_back"].get<bool>(), false);

        Json all = c("images.list", {{"library", L}, {"check_files", true}})["images"];
        CHECK_EQ(all.size(), size_t(4));  // C is trashed
        for (auto& im : all) CHECK(im["exists"].get<bool>());
        Json a = find_by(all, "uuid", "IMG-A");
        CHECK_EQ(a["path"].get<std::string>(), (fc.photos / "A.NEF").string());
        CHECK_EQ(a["captured_local"].get<std::string>(), std::string("2024-01-08T23:39:04"));
        CHECK_EQ(a["variants"].size(), size_t(2));
        CHECK_EQ(a["variants"][1]["rating"].get<int>(), 4);
        Json e = find_by(all, "uuid", "IMG-E");
        CHECK_EQ(e["path"].get<std::string>(), (fc.package / "Originals/2026/01/02/1/E.tif").string());
        Json b = find_by(all, "uuid", "IMG-B");
        CHECK_EQ(b["rating"].get<int>(), 3);
        CHECK_EQ(b["color_tag"].get<int>(), 2);

        Json cols = c("collections.list", {{"library", L}})["collections"];
        CHECK_EQ(find_by(cols, "uuid", "COL-ROOT")["kind"].get<std::string>(), std::string("root"));
        CHECK_EQ(find_by(cols, "uuid", "COL-RI")["kind"].get<std::string>(), std::string("recent_imports"));
        CHECK_EQ(find_by(cols, "uuid", "COL-RC")["kind"].get<std::string>(), std::string("recent_captures"));
        CHECK_EQ(find_by(cols, "uuid", "COL-ALL")["image_count"].get<int>(), 4);
        CHECK_EQ(find_by(cols, "uuid", "COL-TRASH")["image_count"].get<int>(), 1);
        CHECK_EQ(find_by(cols, "uuid", "COL-INCAT")["image_count"].get<int>(), 1);
        CHECK_EQ(find_by(cols, "uuid", "COL-IMPORT")["image_count"].get<int>(), 2);  // C is trashed
        CHECK_EQ(find_by(cols, "uuid", "COL-RI")["image_count"].get<int>(), 2);
        CHECK_EQ(find_by(cols, "uuid", "COL-PICKS")["image_count"].get<int>(), 2);
        CHECK_EQ(find_by(cols, "uuid", "COL-SMART")["supported"].get<bool>(), false);
        CHECK_EQ(find_by(cols, "uuid", "COL-F-PHOTOS")["image_count"].get<int>(), 2);
        // The folder whose parent row is gone hangs from its nearest ancestor.
        CHECK_EQ(find_by(cols, "uuid", "COL-F-SUB")["parent"].get<std::string>(), std::string("COL-F-PHOTOS"));
        CHECK_EQ(find_by(cols, "uuid", "COL-PICKS")["editable"].get<bool>(), false);

        Json picks = c("images.list", {{"library", L}, {"collection", "COL-PICKS"}})["images"];
        CHECK_EQ(picks.size(), size_t(2));
        Json rec = c("images.list", {{"library", L}, {"collection", "COL-F-PHOTOS"}, {"recursive", true}})["images"];
        CHECK_EQ(rec.size(), size_t(3));
        Json trash = c("images.list", {{"library", L}, {"collection", "COL-TRASH"}})["images"];
        CHECK_EQ(trash.size(), size_t(1));

        c("library.sync", {{"library", L}});
        c("library.sync", {{"library", L}, {"force", true}});
        CHECK(before == census(fc.package));
        CHECK(!fs::exists(fc.db.string() + "-journal"));
        CHECK(!fs::exists(fc.db.string() + "-wal"));
        CHECK(!fs::exists(fc.db.string() + "-shm"));
    });

    run("resync: skipped when unchanged, exact when forced", [] {
        fs::path root = tmpdir("resync");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        Json s1 = c("library.sync", {{"library", L}});
        CHECK(s1["skipped"].get<bool>());
        Json s2 = c("library.sync", {{"library", L}, {"force", true}});
        CHECK(!s2["skipped"].get<bool>());
        CHECK_EQ(s2["images"]["unchanged"].get<int>(), 5);
        CHECK_EQ(s2["images"]["updated"].get<int>(), 0);
        CHECK_EQ(s2["variants"]["unchanged"].get<int>(), 6);
    });

    run("SpektraLab's data survives what Capture One does", [] {
        fs::path root = tmpdir("survive");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        Json e1 = c("edit.put", {{"library", L}, {"variant", "VAR-B1"}, {"base_revision", 0},
                                 {"edit", {{"schemaVersion", 3}, {"params", {{"film_stock", "kodak_portra_400"}}}}}});
        CHECK_EQ(e1["revision"].get<int>(), 1);
        c("edit.put", {{"library", L}, {"variant", "VAR-D1"}, {"base_revision", 0}, {"edit", {{"schemaVersion", 3}}}});
        c("meta.set", {{"library", L}, {"variants", {"VAR-A1"}}, {"rating", 2}});

        // In Capture One: B re-rated, A re-rated (overridden here), D deleted,
        // a new image F, the Picks album renamed and D's membership gone.
        touch(fc.photos / "F.NEF", "nef-f");
        fc.edit("UPDATE ZVARIANTMETADATA SET ZBASIC_RATING = 1 WHERE ZLAYER = 103;"
                "UPDATE ZVARIANTMETADATA SET ZBASIC_RATING = 5 WHERE ZLAYER = 101;"
                "DELETE FROM ZIMAGEINCOLLECTION WHERE ZIMAGE = 4; DELETE FROM ZVARIANTMETADATA WHERE ZLAYER = 105;"
                "DELETE FROM ZVARIANTLAYER WHERE Z_PK = 105; DELETE FROM ZVARIANT WHERE Z_PK = 5; DELETE FROM ZIMAGE WHERE Z_PK = 4;"
                "INSERT INTO ZIMAGE (Z_ENT, Z_PK, ZIMAGEUUID, ZIMAGEFILENAME, ZIMAGELOCATION, ZIMAGECLASSIFICATION) VALUES (12, 6, 'IMG-F', 'F.NEF', 3, 17);"
                "INSERT INTO ZVARIANT (Z_ENT, Z_PK, ZINDEX, ZVARIANTUUID, ZIMAGE, ZCOMBINEDSETTINGS) VALUES (15, 7, 127, 'VAR-F1', 6, 107);"
                "INSERT INTO ZVARIANTLAYER (Z_ENT, Z_PK, ZVARIANT) VALUES (20, 107, 7);"
                "INSERT INTO ZVARIANTMETADATA (Z_ENT, Z_PK, ZLAYER, ZBASIC_RATING) VALUES (13, 107, 107, 4);"
                "UPDATE ZCOLLECTION SET ZNAME = 'Best' WHERE Z_PK = 8;");
        Json s = c("library.sync", {{"library", L}});
        CHECK(!s["skipped"].get<bool>());
        CHECK_EQ(s["images"]["added"].get<int>(), 1);
        CHECK_EQ(s["images"]["removed"].get<int>(), 1);
        CHECK_EQ(s["variants"]["updated"].get<int>(), 2);
        CHECK_EQ(s["variants"]["removed"].get<int>(), 1);

        Json b = c("image.get", {{"library", L}, {"image", "IMG-B"}});
        CHECK_EQ(b["variants"][0]["rating"].get<int>(), 1);       // Capture One's new value
        CHECK_EQ(c("edit.get", {{"library", L}, {"variant", "VAR-B1"}})["edit"]["params"]["film_stock"].get<std::string>(),
                 std::string("kodak_portra_400"));
        Json a = c("image.get", {{"library", L}, {"image", "IMG-A"}});
        CHECK_EQ(a["variants"][0]["rating"].get<int>(), 2);       // SpektraLab's override wins
        CHECK_EQ(a["variants"][0]["capture_one"]["rating"].get<int>(), 5);
        // D is gone from Capture One: hidden, not deleted, edit kept.
        Json all = c("images.list", {{"library", L}})["images"];
        CHECK(find_by(all, "uuid", "IMG-D").is_null());
        CHECK(!find_by(all, "uuid", "IMG-F").is_null());
        Json d = c("image.get", {{"library", L}, {"image", "IMG-D"}});
        CHECK_EQ(d["origin_state"].get<std::string>(), std::string("removed"));
        CHECK_EQ(c("edit.get", {{"library", L}, {"variant", "VAR-D1"}})["revision"].get<int>(), 1);
        Json cols = c("collections.list", {{"library", L}})["collections"];
        CHECK_EQ(find_by(cols, "uuid", "COL-PICKS")["name"].get<std::string>(), std::string("Best"));
        CHECK_EQ(find_by(cols, "uuid", "COL-PICKS")["image_count"].get<int>(), 1);
        // Clearing the override hands the rating back to Capture One.
        c("meta.set", {{"library", L}, {"variants", {"VAR-A1"}}, {"rating", nullptr}});
        CHECK_EQ(c("image.get", {{"library", L}, {"image", "IMG-A"}})["variants"][0]["rating"].get<int>(), 5);
    });

    run("Capture One's rows are read-only through the API", [] {
        fs::path root = tmpdir("readonly");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        CHECK_EQ(c.err("collection.add", {{"library", L}, {"collection", "COL-PICKS"}, {"images", {"IMG-A"}}}), std::string("read_only"));
        CHECK_EQ(c.err("collection.update", {{"library", L}, {"collection", "COL-PICKS"}, {"name", "x"}}), std::string("read_only"));
        CHECK_EQ(c.err("collection.delete", {{"library", L}, {"collection", "COL-PICKS"}}), std::string("read_only"));
        CHECK_EQ(c.err("collection.create", {{"library", L}, {"kind", "album"}, {"name", "x"}, {"parent", "COL-RI"}}), std::string("read_only"));
        CHECK_EQ(c.err("images.add", {{"library", L}, {"collection", "COL-PICKS"}, {"items", Json::array()}}), std::string("read_only"));
        // A SpektraLab album beside them is fine, and may hold their images.
        std::string mine = c("collection.create", {{"library", L}, {"kind", "album"}, {"name", "SpektraLab picks"}})["collection"];
        CHECK_EQ(c("collection.add", {{"library", L}, {"collection", mine}, {"images", {"IMG-A", "IMG-B"}}})["added"].get<int>(), 2);
        c("library.sync", {{"library", L}, {"force", true}});
        Json cols = c("collections.list", {{"library", L}})["collections"];
        CHECK_EQ(find_by(cols, "uuid", mine)["image_count"].get<int>(), 2);
        CHECK_EQ(c.err("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}}), std::string(""));
    });

    run("edits: revision guards a lost update", [] {
        fs::path root = tmpdir("edits");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        CHECK(c("edit.get", {{"library", L}, {"variant", "VAR-A1"}})["edit"].is_null());
        c("edit.put", {{"library", L}, {"variant", "VAR-A1"}, {"base_revision", 0}, {"edit", {{"k", 1}}}});
        CHECK_EQ(c.err("edit.put", {{"library", L}, {"variant", "VAR-A1"}, {"base_revision", 0}, {"edit", {{"k", 2}}}}), std::string("conflict"));
        CHECK_EQ(c.err("edit.put", {{"library", L}, {"variant", "VAR-A1"}, {"edit", {{"k", 2}}}}), std::string("invalid_params"));
        CHECK_EQ(c.err("edit.put", {{"library", L}, {"variant", "VAR-A1"}, {"base_revision", 1}, {"edit", Json::array()}}), std::string("invalid_params"));
        CHECK_EQ(c("edit.put", {{"library", L}, {"variant", "VAR-A1"}, {"base_revision", 1}, {"edit", {{"k", 2}}}})["revision"].get<int>(), 2);
        CHECK_EQ(c("edit.get", {{"library", L}, {"variant", "VAR-A1"}})["edit"]["k"].get<int>(), 2);
        Json v = c("variant.create", {{"library", L}, {"image", "IMG-A"}, {"from_variant", "VAR-A1"}});
        CHECK(v["edit_copied"].get<bool>());
        CHECK_EQ(v["position"].get<int>(), 3);
        CHECK_EQ(c("edit.get", {{"library", L}, {"variant", v["variant"]}})["edit"]["k"].get<int>(), 2);
        CHECK(c("edit.delete", {{"library", L}, {"variant", "VAR-A1"}})["deleted"].get<bool>());
        CHECK_EQ(c.err("edit.get", {{"library", L}, {"variant", "NOPE"}}), std::string("not_found"));
    });

    run("a busy, foreign or unreadable catalog is refused cleanly", [] {
        fs::path root = tmpdir("refuse");
        FakeCatalog fc(root);
        Ctx c;
        touch(fc.db.string() + "-journal", "hot");
        CHECK_EQ(c.err("capture_one.probe", {{"path", fc.package.string()}}), std::string("source_busy"));
        fs::remove(fc.db.string() + "-journal");
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        // Another catalog put where this one was.
        fs::path other_root = tmpdir("refuse-other");
        FakeCatalog other(other_root);
        other.edit("UPDATE ZDOCUMENTCONTENT SET ZDOCUMENTUUID = 'ANOTHER'");
        CHECK_EQ(c.err("library.sync", {{"library", L}, {"source", other.package.string()}}), std::string("conflict"));
        // A schema without what the reader needs.
        fs::path bad = tmpdir("refuse-bad") / "Bad.cocatalog";
        fs::create_directories(bad);
        { Db d((bad / "Bad.cocatalogdb").string(), Db::Mode::Create); d.exec("CREATE TABLE ZIMAGE (Z_PK INTEGER)"); }
        Json p = c("capture_one.probe", {{"path", bad.string()}});
        CHECK(!p["supported"].get<bool>());
        CHECK(p["reason"].get<std::string>().find("ZPATHLOCATION") != std::string::npos);
        CHECK_EQ(c.err("capture_one.probe", {{"path", (root / "nothing").string()}}), std::string("not_found"));
        CHECK_EQ(c.err("unknown.method"), std::string("unknown_method"));
        // A Capture One session is probed, not linked (yet).
        fs::path sess = tmpdir("refuse-session") / "Shoot";
        fs::create_directories(sess);
        fs::copy_file(fc.db, sess / "Shoot.cosessiondb");
        Json sp = c("capture_one.probe", {{"path", sess.string()}});
        CHECK_EQ(sp["document"].get<std::string>(), std::string("session"));
        CHECK_EQ(c.err("capture_one.link", {{"source", sess.string()}, {"path", (root / "S").string()}}), std::string("unsupported"));
        CHECK(!fs::exists(root / "S.spkcatalog"));
    });

    run("store keying: a moved catalog finds its library again", [] {
        fs::path root = tmpdir("store");
        FakeCatalog fc(root);
        Ctx c({{"store", (root / "Store").string()}});
        Json r1 = c("capture_one.link", {{"source", fc.package.string()}});
        CHECK(fs::exists(root / "Store/Linked" / (fc.uuid + ".spkcatalog")));
        c("edit.put", {{"library", r1["library"]}, {"variant", "VAR-A1"}, {"base_revision", 0}, {"edit", {{"k", 1}}}});
        c("library.close", {{"library", r1["library"]}});
        fs::rename(fc.package, root / "Moved.cocatalog");
        Json r2 = c("capture_one.link", {{"source", (root / "Moved.cocatalog").string()}});
        CHECK_EQ(r2["info"]["origin"]["path"].get<std::string>(), (root / "Moved.cocatalog").string());
        CHECK_EQ(c("edit.get", {{"library", r2["library"]}, {"variant", "VAR-A1"}})["revision"].get<int>(), 1);
    });

    run("a native catalog: add, folders, albums, smart albums, trash, copy", [] {
        fs::path root = tmpdir("native");
        touch(root / "Shoot/Day1/a.NEF", "1");
        touch(root / "Shoot/Day1/b.JPG", "2");
        touch(root / "Shoot/c.tif", "3");
        Ctx c;
        Json cr = c("library.create", {{"path", (root / "Mine").string()}, {"kind", "catalog"}});
        std::string L = cr["library"];
        CHECK_EQ(cr["info"]["origin"]["kind"].get<std::string>(), std::string("none"));
        std::string album = c("collection.create", {{"library", L}, {"kind", "album"}, {"name", "Roll 1"}})["collection"];
        Json add = c("images.add", {{"library", L}, {"collection", album},
                                    {"items", {(root / "Shoot/Day1/a.NEF").string(),
                                               {{"path", (root / "Shoot/Day1/b.JPG").string()}, {"metadata", {{"captured_local", "2026-10-01T09:30:00"}, {"width", 4000}}}},
                                               (root / "Shoot/c.tif").string(), (root / "Shoot/missing.NEF").string()}}});
        CHECK_EQ(add["added"].get<int>(), 3);
        CHECK(add["results"][3].contains("error"));
        Json again = c("images.add", {{"library", L}, {"items", {(root / "Shoot/c.tif").string()}}});
        CHECK_EQ(again["added"].get<int>(), 0);
        CHECK(again["results"][0]["existing"].get<bool>());
        Json cols = c("collections.list", {{"library", L}})["collections"];
        int folders = 0;
        std::string day1_parent;
        for (auto& col : cols) if (col["kind"] == "folder") {
            ++folders;
            if (col["name"] == "Day1") day1_parent = col["parent"].is_null() ? "" : col["parent"].get<std::string>();
        }
        CHECK_EQ(folders, 2);
        CHECK(!day1_parent.empty());  // Day1 hangs from Shoot, created after it
        CHECK_EQ(find_by(cols, "uuid", album)["image_count"].get<int>(), 3);

        std::string b = add["results"][1]["image"];
        Json bi = c("image.get", {{"library", L}, {"image", b}});
        CHECK_EQ(bi["captured_local"].get<std::string>(), std::string("2026-10-01T09:30:00"));
        CHECK_EQ(bi["kind"].get<std::string>(), std::string("raster"));
        c("meta.set", {{"library", L}, {"images", {b}}, {"rating", 5}, {"color_tag", 3}});
        std::string smart = c("collection.create", {{"library", L}, {"kind", "smart_album"}, {"name", "5 stars"},
                                                    {"criteria", {{"min_rating", 5}}}})["collection"];
        Json sm = c("images.list", {{"library", L}, {"collection", smart}})["images"];
        CHECK_EQ(sm.size(), size_t(1));
        CHECK_EQ(sm[0]["color_tag"].get<int>(), 3);
        CHECK_EQ(c("images.list", {{"library", L}, {"filter", {{"kinds", {"raw"}}}}})["total"].get<int>(), 1);
        CHECK_EQ(c("images.list", {{"library", L}, {"filter", {{"text", "b"}}}})["total"].get<int>(), 1);
        CHECK_EQ(c("images.list", {{"library", L}, {"filter", {{"captured_from", "2026-10-01"}, {"captured_to", "2026-10-02"}}}})["total"].get<int>(), 1);
        CHECK_EQ(c("images.list", {{"library", L}, {"sort", "name"}, {"descending", true}, {"limit", 1}})["images"][0]["file_name"].get<std::string>(), std::string("c.tif"));

        c("images.trash", {{"library", L}, {"images", {b}}, {"trashed", true}});
        CHECK(fs::exists(root / "Shoot/Day1/b.JPG"));  // the flag moves no file
        CHECK_EQ(c("images.list", {{"library", L}})["total"].get<int>(), 2);
        CHECK_EQ(c("images.list", {{"library", L}, {"include_trashed", true}})["total"].get<int>(), 3);

        Json lk = c("images.lookup", {{"library", L}, {"paths", {(root / "Shoot/Day1/a.NEF").string(), (root / "Shoot/zzz").string()}}})["results"];
        CHECK(lk[0]["found"].get<bool>());
        CHECK(!lk[1]["found"].get<bool>());

        Json cp = c("images.add", {{"library", L}, {"mode", "copy"}, {"items", {(root / "Shoot/c.tif").string()}}});
        std::string stored = cp["results"][0]["stored_at"];
        CHECK(stored.find("Mine.spkcatalog/Originals/") != std::string::npos);
        CHECK(fs::exists(stored));
        CHECK(fs::exists(root / "Shoot/c.tif"));
        CHECK_EQ(c("image.get", {{"library", L}, {"image", cp["results"][0]["image"]}})["inside_library"].get<bool>(), true);

        CHECK_EQ(c.err("collection.delete", {{"library", L}, {"collection", album}}), std::string(""));
        CHECK_EQ(c("images.list", {{"library", L}, {"include_trashed", true}})["total"].get<int>(), 4);
        c("library.close", {{"library", L}});
        CHECK_EQ(c.err("library.create", {{"path", (root / "Mine").string()}}), std::string("exists"));
        std::string L2 = c("library.open", {{"path", (root / "Mine.spkcatalog").string()}})["library"];
        CHECK_EQ(c("library.info", {{"library", L2}})["counts"]["images"].get<int>(), 4);
    });

    run("relink: a renamed volume is found in one step", [] {
        fs::path root = tmpdir("relink");
        touch(root / "VolA/2026/x.NEF", "1");
        touch(root / "VolA/2025/y.NEF", "2");
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "R").string()}})["library"];
        c("images.add", {{"library", L}, {"items", {(root / "VolA/2026/x.NEF").string(), (root / "VolA/2025/y.NEF").string()}}});
        fs::rename(root / "VolA", root / "VolA 1");
        Json locs = c("locations.list", {{"library", L}})["locations"];
        CHECK_EQ(locs.size(), size_t(2));
        for (auto& l : locs) CHECK(!l["online"].get<bool>());
        std::string one = locs[0]["path"].get<std::string>() == (root / "VolA/2026").string() ? locs[0]["uuid"] : locs[1]["uuid"];
        Json rl = c("location.relink", {{"library", L}, {"location", one}, {"path", (root / "VolA 1/2026").string()}});
        CHECK_EQ(rl["relinked"].size(), size_t(2));
        Json im = c("images.list", {{"library", L}, {"check_files", true}})["images"];
        for (auto& j : im) CHECK(j["exists"].get<bool>());
        CHECK_EQ(c.err("location.relink", {{"library", L}, {"location", one}, {"path", (root / "nowhere").string()}}), std::string("not_found"));
        c("location.relink", {{"library", L}, {"location", one}, {"path", nullptr}});
        CHECK(!c("image.get", {{"library", L}, {"image", im[0]["uuid"]}})["exists"].get<bool>() ||
              !c("image.get", {{"library", L}, {"image", im[1]["uuid"]}})["exists"].get<bool>());
    });

    run("a SpektraLab session is one portable folder", [] {
        fs::path root = tmpdir("session");
        touch(root / "card/DSC_1.NEF", "1");
        Ctx c;
        Json cr = c("library.create", {{"path", (root / "Shoot 2026-10-18").string()}, {"kind", "session"}});
        std::string L = cr["library"];
        fs::path folder = root / "Shoot 2026-10-18";
        CHECK(fs::exists(folder / "Shoot 2026-10-18.spksession"));
        for (auto d : {"Capture", "Selects", "Output", "Trash"}) CHECK(fs::is_directory(folder / d));
        Json add = c("images.add", {{"library", L}, {"mode", "copy"}, {"items", {(root / "card/DSC_1.NEF").string()}}});
        CHECK_EQ(add["results"][0]["stored_at"].get<std::string>(), (folder / "Capture/DSC_1.NEF").string());
        c("edit.put", {{"library", L}, {"variant", add["results"][0]["variant"]}, {"base_revision", 0}, {"edit", {{"k", 1}}}});
        Json locs = c("locations.list", {{"library", L}})["locations"];
        CHECK_EQ(locs[0]["base"].get<std::string>(), std::string("library"));
        c("library.close", {{"library", L}});
        // One file, no -wal/-shm left beside it.
        CHECK(!fs::exists(folder / "Shoot 2026-10-18.spksession-wal"));
        // Move the folder: everything still resolves.
        fs::rename(folder, root / "Moved");
        fs::rename(root / "Moved/Shoot 2026-10-18.spksession", root / "Moved/Moved.spksession");
        std::string L2 = c("library.open", {{"path", (root / "Moved").string()}})["library"];
        Json im = c("images.list", {{"library", L2}, {"check_files", true}})["images"];
        CHECK_EQ(im[0]["path"].get<std::string>(), (root / "Moved/Capture/DSC_1.NEF").string());
        CHECK(im[0]["exists"].get<bool>());
        CHECK_EQ(c("edit.get", {{"library", L2}, {"variant", im[0]["variants"][0]["uuid"]}})["revision"].get<int>(), 1);
        CHECK_EQ(c.err("capture_one.link", {{"source", "/nowhere"}, {"path", (root / "Moved").string()}}), std::string("not_found"));
    });

    run("a session's folders: lock, move, restore, scan, trash hand-off", [] {
        fs::path root = tmpdir("session-fs");
        touch(root / "card/A.NEF", "a");
        touch(root / "card/B.NEF", "b");
        fs::path folder = root / "S";
        Ctx c, other;
        std::string L = c("library.create", {{"path", folder.string()}, {"kind", "session"}})["library"];
        CHECK_EQ(other.err("library.open", {{"path", folder.string()}}), std::string("in_use"));
        CHECK_EQ(c("library.open", {{"path", folder.string()}})["library"].get<std::string>(), L);  // same process: same handle
        Json add = c("images.add", {{"library", L}, {"mode", "copy"}, {"items", {(root / "card/A.NEF").string(), (root / "card/B.NEF").string()}}});
        std::string a = add["results"][0]["image"], b = add["results"][1]["image"];
        Json roles = c("session.folders", {{"library", L}})["folders"];
        int with_role = 0;
        for (auto& f : roles) with_role += !f["role"].is_null();
        CHECK_EQ(with_role, 4);

        Json mv = c("session.move", {{"library", L}, {"images", {a}}, {"to", "selects"}});
        CHECK_EQ(mv["moved"].size(), size_t(1));
        CHECK(fs::exists(folder / "Selects/A.NEF"));
        CHECK(!fs::exists(folder / "Capture/A.NEF"));
        CHECK_EQ(c("image.get", {{"library", L}, {"image", a}})["path"].get<std::string>(), (folder / "Selects/A.NEF").string());
        // Never overwrites: a same-named file already in Trash is a conflict.
        touch(folder / "Trash/B.NEF", "someone else's");
        Json cf = c("session.move", {{"library", L}, {"images", {b}}, {"to", "trash"}});
        CHECK_EQ(cf["conflicts"].size(), size_t(1));
        CHECK_EQ(read_file(folder / "Trash/B.NEF"), std::string("someone else's"));
        fs::remove(folder / "Trash/B.NEF");
        c("session.move", {{"library", L}, {"images", {b}}, {"to", "trash"}});
        c("session.restore", {{"library", L}, {"images", {b}}});
        CHECK(fs::exists(folder / "Capture/B.NEF"));
        c("session.move", {{"library", L}, {"images", {b}}, {"to", "trash"}});

        // A tethered camera drops a file into Capture.
        touch(folder / "Capture/C.NEF", "c");
        touch(folder / "Capture/.hidden.NEF", "h");
        Json sc = c("session.scan", {{"library", L}});
        CHECK_EQ(sc["added"].get<int>(), 1);
        CHECK_EQ(sc["missing"].get<int>(), 0);

        // The app empties the Trash: spklib names the files, the app moves
        // them, spklib forgets only what really left.
        Json plan = c("session.trash.plan", {{"library", L}})["files"];
        CHECK_EQ(plan.size(), size_t(1));
        std::string bpath = plan[0]["path"];
        CHECK_EQ(c("session.trash.commit", {{"library", L}, {"removed_paths", {bpath}}})["refused"].size(), size_t(1));  // still there
        fs::remove(bpath);
        CHECK_EQ(c("session.trash.commit", {{"library", L}, {"removed_paths", {bpath}}})["dropped"].get<int>(), 1);
        CHECK_EQ(c("images.list", {{"library", L}})["total"].get<int>(), 2);

        // A crash between the rename and the row: the next open finishes it.
        c("library.close", {{"library", L}});
        CHECK(!fs::exists(folder / "S.spksession-journal"));
        {
            Db d((folder / "S.spksession").string(), Db::Mode::ReadWrite);
            auto s = d.prepare("SELECT i.id, l.id FROM image i, path_location l JOIN session_folder f ON f.location_id = l.id"
                               " WHERE f.role = 'selects' AND i.file_name = 'C.NEF'");
            CHECK(s.step());
            d.exec("INSERT INTO fs_intent(op, image_id, from_path, to_path, to_location_id, at) VALUES ('move', " +
                   std::to_string(s.i64(0)) + ", '" + (folder / "Capture/C.NEF").string() + "', '" +
                   (folder / "Selects/C.NEF").string() + "', " + std::to_string(s.i64(1)) + ", 0)");
        }
        fs::rename(folder / "Capture/C.NEF", folder / "Selects/C.NEF");
        std::string L2 = c("library.open", {{"path", folder.string()}})["library"];
        Json im = c("images.list", {{"library", L2}, {"check_files", true}})["images"];
        for (auto& j : im) CHECK(j["exists"].get<bool>());
        CHECK(!find_by(im, "path", (folder / "Selects/C.NEF").string()).is_null());
        // A catalog has no session folders.
        std::string cat = c("library.create", {{"path", (root / "Cat").string()}})["library"];
        CHECK_EQ(c.err("session.move", {{"library", cat}, {"images", Json::array()}, {"to", "trash"}}), std::string("invalid_params"));
    });

    run("lookup matches a path in either Unicode normalisation", [] {
        fs::path root = tmpdir("nfc");
        std::string nfd = "Cafe\xCC\x81";   // e + combining acute
        std::string nfc = "Caf\xC3\xA9";    // é
        touch(root / nfd / "x.NEF", "1");
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "U").string()}})["library"];
        c("images.add", {{"library", L}, {"items", {(root / nfd / "x.NEF").string()}}});
        CHECK(c("images.lookup", {{"library", L}, {"paths", {(root / nfc / "x.NEF").string()}}})["results"][0]["found"].get<bool>());
        CHECK(c("images.lookup", {{"library", L}, {"paths", {(root / nfd / "x.NEF").string()}}})["results"][0]["found"].get<bool>());
    });

    run("a watched folder keeps in step: new, moved, gone, back, offline", [] {
        fs::path root = tmpdir("watch");
        touch(root / "Card/DCIM/100/A.NEF", "aaaa");
        touch(root / "Card/DCIM/100/B.NEF", "bbbbbb");
        touch(root / "Card/DCIM/101/C.jpg", "cc");
        touch(root / "Card/DCIM/101/notes.txt", "not a picture");
        touch(root / "Card/.Trashes/D.NEF", "hidden");
        touch(root / "Card/CaptureOne/Cache/E.jpg", "a cache");
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "W").string()}})["library"];
        Json fa = c("folders.add", {{"library", L}, {"path", (root / "Card").string()}, {"skip", {"CaptureOne"}}});
        CHECK_EQ(fa["sync"]["added"].get<int>(), 3);
        CHECK_EQ(fa["sync"]["offline"].get<bool>(), false);
        CHECK(fa["sync"].contains("recent_import"));  // the first look is an import
        std::string a = find_by(c("images.list", {{"library", L}})["images"], "file_name", "A.NEF")["uuid"];
        c("meta.set", {{"library", L}, {"images", {a}}, {"rating", 4}});

        // Nothing changed: nothing reported.
        Json s0 = c("folders.sync", {{"library", L}});
        CHECK_EQ(s0["folders"][0]["unchanged"].get<int>(), 3);
        CHECK_EQ(s0["added"].get<int>() + s0["moved"].get<int>() + s0["removed"].get<int>(), 0);

        // Finder: A moves to another folder, B is deleted, F arrives.
        fs::create_directories(root / "Card/DCIM/102");
        fs::rename(root / "Card/DCIM/100/A.NEF", root / "Card/DCIM/102/A.NEF");
        fs::remove(root / "Card/DCIM/100/B.NEF");
        touch(root / "Card/DCIM/101/F.NEF", "ffff");
        Json s1 = c("folders.sync", {{"library", L}});
        CHECK_EQ(s1["moved"].get<int>(), 1);
        CHECK_EQ(s1["removed"].get<int>(), 1);
        CHECK_EQ(s1["added"].get<int>(), 1);
        CHECK(!s1["folders"][0].contains("recent_import"));  // a later sync is not an import
        // The row followed the file: same image, same rating.
        Json ai = c("image.get", {{"library", L}, {"image", a}});
        CHECK_EQ(ai["path"].get<std::string>(), (root / "Card/DCIM/102/A.NEF").string());
        CHECK_EQ(ai["variants"][0]["rating"].get<int>(), 4);
        CHECK_EQ(c("images.list", {{"library", L}})["total"].get<int>(), 3);
        Json removed = c("images.list", {{"library", L}, {"include_removed", true}})["images"];
        CHECK_EQ(find_by(removed, "file_name", "B.NEF")["origin_state"].get<std::string>(), std::string("removed"));

        // B comes back: the same row, present again.
        touch(root / "Card/DCIM/100/B.NEF", "bbbbbb");
        Json s2 = c("folders.sync", {{"library", L}});
        CHECK_EQ(s2["restored"].get<int>(), 1);
        CHECK_EQ(s2["added"].get<int>(), 0);
        CHECK_EQ(c("images.list", {{"library", L}})["total"].get<int>(), 4);

        // The card is ejected: offline, nothing marked.
        fs::rename(root / "Card", root / "Card-away");
        Json s3 = c("folders.sync", {{"library", L}});
        CHECK(s3["folders"][0]["offline"].get<bool>());
        CHECK_EQ(c("images.list", {{"library", L}})["total"].get<int>(), 4);
        Json fl = c("folders.list", {{"library", L}})["folders"];
        CHECK_EQ(fl.size(), size_t(1));
        CHECK(!fl[0]["online"].get<bool>());
        CHECK_EQ(fl[0]["image_count"].get<int>(), 4);
        fs::rename(root / "Card-away", root / "Card");

        // Two files of one name and size went and one came: not a move.
        touch(root / "Card/X/G.NEF", "gg");
        touch(root / "Card/Y/G.NEF", "gg");
        c("folders.sync", {{"library", L}});
        fs::create_directories(root / "Card/Z");
        fs::rename(root / "Card/X/G.NEF", root / "Card/Z/G.NEF");
        fs::remove(root / "Card/Y/G.NEF");
        Json s4 = c("folders.sync", {{"library", L}});
        CHECK_EQ(s4["moved"].get<int>(), 0);
        CHECK_EQ(s4["added"].get<int>(), 1);
        CHECK_EQ(s4["removed"].get<int>(), 2);

        // The folder's tree is collections, by kind.
        Json cols = c("collections.list", {{"library", L}})["collections"];
        CHECK(!find_by(cols, "title", "Card").is_null());
        CHECK(!find_by(cols, "title", "102").is_null());
        CHECK(find_by(cols, "title", "CaptureOne").is_null());
        CHECK_EQ(c.err("folders.add", {{"library", L}, {"path", (root / "W.spkcatalog/Originals").string()}}), std::string("not_found"));
        CHECK_EQ(c.err("folders.add", {{"library", L}, {"path", (root / "W.spkcatalog").string()}}), std::string("invalid_params"));
        c("folders.remove", {{"library", L}, {"folder", fl[0]["folder"]}});
        CHECK_EQ(c("folders.list", {{"library", L}})["folders"].size(), size_t(0));
        CHECK_EQ(c("images.list", {{"library", L}})["total"].get<int>(), 5);  // the images stay
    });

    run("a session keeps its own folder in step", [] {
        fs::path root = tmpdir("watch-session");
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "S").string()}, {"kind", "session"}})["library"];
        touch(root / "S/Capture/A.NEF", "a");
        touch(root / "S/Selects/B.NEF", "b");
        Json s = c("folders.sync", {{"library", L}});
        CHECK_EQ(s["session"]["added"].get<int>(), 2);
        fs::rename(root / "S/Capture/A.NEF", root / "S/Selects/A.NEF");
        CHECK_EQ(c("folders.sync", {{"library", L}})["session"]["moved"].get<int>(), 1);
        // A session's library has the fixed rows but no "In Catalog".
        Json cols = c("collections.list", {{"library", L}})["collections"];
        CHECK(!find_by(cols, "kind", "all_images").is_null());
        CHECK(find_by(cols, "kind", "in_library").is_null());
    });

    run("a Capture One mirror's folders are Capture One's", [] {
        fs::path root = tmpdir("watch-c1");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        CHECK_EQ(c.err("folders.add", {{"library", L}, {"path", fc.photos.string()}}), std::string("read_only"));
        CHECK_EQ(c.err("folders.sync", {{"library", L}}), std::string("read_only"));
        // Its tree is Capture One's only: one root, no SpektraLab rows beside it.
        Json cols = c("collections.list", {{"library", L}})["collections"];
        int roots = 0;
        for (auto& col : cols) roots += col["kind"] == "root";
        CHECK_EQ(roots, 1);
        // A Recent Imports album carries Capture One's own time.
        CHECK_EQ(find_by(cols, "uuid", "COL-IMPORT")["created_at"].get<std::string>(), std::string("2026-10-04T13:31:37Z"));
        // A folder without a name is called by its own.
        CHECK_EQ(find_by(cols, "uuid", "COL-F-SUB")["title"].get<std::string>(), std::string("Sub"));
        CHECK(!find_by(cols, "uuid", "COL-PICKS")["editable"].get<bool>());
    });

    run("facets count each facet without its own filter", [] {
        fs::path root = tmpdir("facets");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        Json f = c("images.facets", {{"library", L}});
        // Four visible images (C is in Capture One's trash).
        CHECK_EQ(f["total"].get<int>(), 4);
        std::map<int64_t, int64_t> rating;
        for (auto& r : f["rating"]) rating[r["value"].get<int64_t>()] = r["count"].get<int64_t>();
        // Primary variants: A 0, B 3, D 1, E 0.
        CHECK_EQ(rating[0], int64_t(2));
        CHECK_EQ(rating[3], int64_t(1));
        CHECK_EQ(rating[1], int64_t(1));
        CHECK_EQ(find_by(f["camera"], "value", "NIKON Z6_3")["count"].get<int>(), 2);
        CHECK_EQ(f["captured"]["undated"].get<int>(), 1);
        CHECK_EQ(find_by(f["captured"]["days"], "value", "2024-01-08")["count"].get<int>(), 3);
        CHECK_EQ(find_by(f["keyword"], "value", "night")["count"].get<int>(), 2);
        CHECK_EQ(find_by(f["keyword"], "value", "Shanghai")["count"].get<int>(), 1);
        // Ticking 3 stars: the total narrows, the rating facet does not, the camera facet does.
        Json g = c("images.facets", {{"library", L}, {"filter", {{"ratings", {3}}}}});
        CHECK_EQ(g["total"].get<int>(), 1);
        CHECK_EQ(g["rating"].size(), f["rating"].size());
        CHECK_EQ(g["camera"].size(), size_t(1));
        // The new filter keys list the same images the facets counted.
        CHECK_EQ(c("images.list", {{"library", L}, {"filter", {{"keywords", {"night"}}}}})["total"].get<int>(), 2);
        CHECK_EQ(c("images.list", {{"library", L}, {"filter", {{"camera_models", {"ILCE-7RM5", nullptr}}}}})["total"].get<int>(), 2);
        CHECK_EQ(c("images.list", {{"library", L}, {"filter", {{"ratings", {0, 1}}}}})["total"].get<int>(), 3);
        // Scoped to a collection, as the browser asks.
        CHECK_EQ(c("images.facets", {{"library", L}, {"collection", "COL-PICKS"}})["total"].get<int>(), 2);
    });

    run("describe: the app's header reading fills SpektraLab's rows only", [] {
        fs::path root = tmpdir("describe");
        touch(root / "P/A.NEF", "a");
        FakeCatalog fc(root);
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "D").string()}})["library"];
        Json fa = c("folders.add", {{"library", L}, {"path", (root / "P").string()}});
        Json f0 = c("images.facets", {{"library", L}});
        CHECK_EQ(f0["undescribed"].get<int>(), 1);
        std::string a = c("images.list", {{"library", L}, {"filter", {{"described", false}}}})["images"][0]["uuid"];
        Json d = c("images.describe", {{"library", L}, {"items", {{{"image", a}, {"metadata", {{"captured_local", "2026-10-03T16:53:00"},
                                                                                            {"camera_model", "NIKON Z 8"}, {"lens", "NIKKOR Z 24-120mm f/4 S"}}}}}}});
        CHECK_EQ(d["updated"].get<int>(), 1);
        Json f1 = c("images.facets", {{"library", L}});
        CHECK_EQ(f1["undescribed"].get<int>(), 0);
        CHECK_EQ(find_by(f1["lens"], "value", "NIKKOR Z 24-120mm f/4 S")["count"].get<int>(), 1);
        CHECK_EQ(c("images.list", {{"library", L}, {"full", true}})["images"][0]["captured_local"].get<std::string>(), std::string("2026-10-03T16:53:00"));
        std::string M = c("capture_one.link", {{"source", fc.package.string()}, {"path", (root / "M").string()}})["library"];
        Json r = c("images.describe", {{"library", M}, {"items", {{{"image", "IMG-A"}, {"metadata", {{"lens", "x"}}}}}}});
        CHECK_EQ(r["updated"].get<int>(), 0);
        CHECK_EQ(r["results"][0]["error"]["code"].get<std::string>(), std::string("read_only"));
    });

    run("each import is a Recent Imports album", [] {
        fs::path root = tmpdir("recent");
        touch(root / "a.NEF", "a");
        touch(root / "b.NEF", "b");
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "R").string()}})["library"];
        Json cols = c("collections.list", {{"library", L}})["collections"];
        Json ri = find_by(cols, "kind", "recent_imports");
        CHECK(!ri.is_null());
        CHECK(!ri["editable"].get<bool>());
        Json add = c("images.add", {{"library", L}, {"items", {(root / "a.NEF").string(), (root / "b.NEF").string()}}});
        std::string album = add["recent_import"];
        cols = c("collections.list", {{"library", L}})["collections"];
        Json al = find_by(cols, "uuid", album);
        CHECK_EQ(al["parent"].get<std::string>(), ri["uuid"].get<std::string>());
        CHECK_EQ(al["image_count"].get<int>(), 2);
        CHECK_EQ(find_by(cols, "kind", "recent_imports")["image_count"].get<int>(), 2);
        CHECK_EQ(c("images.list", {{"library", L}, {"collection", album}})["total"].get<int>(), 2);
        // Adding the same files again is no import.
        CHECK(!c("images.add", {{"library", L}, {"items", {(root / "a.NEF").string()}}}).contains("recent_import"));
    });

    run("a schema-1 library is migrated forward on open", [] {
        fs::path root = tmpdir("migrate");
        touch(root / "P/a.NEF", "a");
        Ctx c;
        std::string L = c("library.create", {{"path", (root / "Old").string()}})["library"];
        c("images.add", {{"library", L}, {"items", {(root / "P/a.NEF").string()}}, {"recent_import", false}});
        c("library.close", {{"library", L}});
        {
            // Back to what spklib 0.1.0 wrote: schema 1, no fixed rows.
            Db d((root / "Old.spkcatalog/Library.spkdb").string(), Db::Mode::ReadWrite);
            d.exec("DROP INDEX image_captured; DROP INDEX variant_keyword_keyword; DROP TABLE variant_keyword; DROP TABLE watched_folder;"
                   " ALTER TABLE collection DROP COLUMN origin_created_at; ALTER TABLE image DROP COLUMN described_at;"
                   " DELETE FROM collection WHERE kind IN ('root', 'all_images', 'recent_imports', 'trash', 'in_library');"
                   " UPDATE meta SET value = '1' WHERE key = 'schema_version'");
        }
        std::string L2 = c("library.open", {{"path", (root / "Old.spkcatalog").string()}})["library"];
        CHECK_EQ(c("library.info", {{"library", L2}})["schema_version"].get<int>(), 2);
        Json cols = c("collections.list", {{"library", L2}})["collections"];
        CHECK(!find_by(cols, "kind", "all_images").is_null());
        CHECK_EQ(find_by(cols, "kind", "all_images")["image_count"].get<int>(), 1);
        CHECK_EQ(c("images.facets", {{"library", L2}})["total"].get<int>(), 1);
        c("folders.add", {{"library", L2}, {"path", (root / "P").string()}});
        CHECK_EQ(c("images.list", {{"library", L2}})["total"].get<int>(), 1);  // the same file, not twice
    });

    run("the method table and the version", [] {
        Ctx c;
        Json v = c("sdk.version");
        CHECK_EQ(v["abi"].get<int>(), SPKLIB_ABI_VERSION);
        CHECK_EQ(v["capture_one"]["write_back"].get<bool>(), false);
        CHECK(c("sdk.methods")["methods"].size() > 20);
        CHECK_EQ(c.err("images.list", {{"library", "L999"}}), std::string("not_found"));
        CHECK_EQ(c.err("images.list"), std::string("invalid_params"));
        char* out = nullptr;
        CHECK_EQ(spklib_call(c.c, "sdk.version", "{not json", &out), int(SPKLIB_ERROR));
        spklib_free(out);
    });
}

// --- part 2: the real catalog ----------------------------------------------------------------------

static std::string oracle_resolve(bool relative, const std::string& root, const std::string& rel, const std::string& package) {
    // Written again here, independently of the SDK, from the dump in
    // docs/CAPTURE-ONE-CATALOG.md: root + "/" + relative path, relative to the
    // package when ZISRELATIVE.
    std::string r = rel;
    while (!r.empty() && r[0] == '/') r.erase(0, 1);
    std::string b = relative ? package : root;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    if (r.empty()) return b;
    return b == "/" ? "/" + r : b + "/" + r;
}

static void real_catalog(const std::string& package) {
    std::printf("-- real catalog: %s\n", package.c_str());
    auto t0 = std::chrono::steady_clock::now();
    auto before = census(package);
    auto t1 = std::chrono::steady_clock::now();
    std::printf("   census: %zu entries in %.1f s\n", before.size(), std::chrono::duration<double>(t1 - t0).count());

    // The oracle reads its own copy, made with cp, not with the SDK.
    fs::path oracle_dir = tmpdir("oracle");
    fs::path dbfile;
    for (auto& e : fs::directory_iterator(package))
        if (e.path().extension() == ".cocatalogdb") dbfile = e.path();
    fs::copy_file(dbfile, oracle_dir / "copy.db");
    Db o((oracle_dir / "copy.db").string(), Db::Mode::ReadOnlyImmutable);

    fs::path root = tmpdir("real");
    Ctx c;
    Json r;
    run("link the real catalog", [&] {
        auto a = std::chrono::steady_clock::now();
        r = c("capture_one.link", {{"source", package}, {"path", (root / "Real").string()}});
        std::printf("   link: %.2f s %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count(),
                    r["sync"]["ms"].dump().c_str());
        auto n = [&](const char* sql) { auto s = o.prepare(sql); s.step(); return s.i64(0); };
        CHECK_EQ(r["sync"]["images"]["added"].get<int64_t>(), n("SELECT count(*) FROM ZIMAGE"));
        CHECK_EQ(r["sync"]["variants"]["added"].get<int64_t>(), n("SELECT count(*) FROM ZVARIANT"));
        CHECK_EQ(r["sync"]["collections"]["added"].get<int64_t>(), n("SELECT count(*) FROM ZCOLLECTION"));
        CHECK_EQ(r["sync"]["locations"]["added"].get<int64_t>(), n("SELECT count(*) FROM ZPATHLOCATION"));
        std::printf("   warnings: %s\n", r["sync"]["warnings"].dump().c_str());
    });
    std::string L = r["library"];

    run("every image: path, existence, capture time, rating, colour tag", [&] {
        Json all = c("images.list", {{"library", L}, {"include_trashed", true}, {"check_files", true}, {"full", true}})["images"];
        std::map<std::string, Json> by_uuid;
        for (auto& j : all) by_uuid[j["uuid"]] = j;
        auto s = o.prepare(
            "SELECT i.ZIMAGEUUID, p.ZISRELATIVE, p.ZMACROOT, p.ZRELATIVEPATH, i.ZIMAGEFILENAME, i.ZISTRASHED,"
            " strftime('%Y-%m-%dT%H:%M:%S', i.ZEXP_DATE, 'unixepoch'), i.ZWIDTH, i.ZCAMERA_MODEL"
            " FROM ZIMAGE i JOIN ZPATHLOCATION p ON p.Z_PK = i.ZIMAGELOCATION");
        int64_t n = 0, exist_sdk = 0, exist_oracle = 0, path_mismatch = 0, time_mismatch = 0, other = 0;
        while (s.step()) {
            ++n;
            auto it = by_uuid.find(s.text(0));
            if (it == by_uuid.end()) { ++other; continue; }
            const Json& j = it->second;
            std::string want = oracle_resolve(s.i64(1) != 0, s.text(2), s.text(3), package) + "/" + s.text(4);
            if (j["path"].get<std::string>() != want) {
                if (++path_mismatch <= 3) std::fprintf(stderr, "   path %s vs %s\n", j["path"].get<std::string>().c_str(), want.c_str());
            }
            struct stat st {};
            exist_oracle += ::stat(want.c_str(), &st) == 0;
            exist_sdk += j["exists"].get<bool>();
            std::string t = s.is_null(6) ? "" : s.text(6);
            std::string got = j["captured_local"].is_null() ? "" : j["captured_local"].get<std::string>();
            if (t != got) ++time_mismatch;
            if (j["trashed"].get<bool>() != (s.i64(5) != 0)) ++other;
            if (j["width"].is_null() != s.is_null(7)) ++other;
        }
        std::printf("   images %lld, found on disk %lld (oracle %lld)\n", (long long)n, (long long)exist_sdk, (long long)exist_oracle);
        CHECK_EQ(int64_t(all.size()), n);
        CHECK_EQ(path_mismatch, int64_t(0));
        CHECK_EQ(exist_sdk, exist_oracle);
        CHECK_EQ(time_mismatch, int64_t(0));
        CHECK_EQ(other, int64_t(0));

        // Ratings and colour tags, per variant, from the combined layer.
        std::map<std::string, std::pair<int64_t, int64_t>> sdk;
        for (auto& j : all)
            for (auto& v : j["variants"])
                sdk[v["uuid"]] = {v["rating"].get<int64_t>(), v["color_tag"].is_null() ? 0 : v["color_tag"].get<int64_t>()};
        auto vs = o.prepare("SELECT v.ZVARIANTUUID, COALESCE(m.ZBASIC_RATING, 0), COALESCE(m.ZCOLOR_TAG_INDEX, 0) FROM ZVARIANT v"
                            " JOIN ZVARIANTLAYER l ON l.Z_PK = v.ZCOMBINEDSETTINGS JOIN ZVARIANTMETADATA m ON m.ZLAYER = l.Z_PK");
        int64_t vn = 0, vbad = 0;
        while (vs.step()) {
            ++vn;
            auto it = sdk.find(vs.text(0));
            if (it == sdk.end() || it->second != std::pair(vs.i64(1), vs.i64(2))) ++vbad;
        }
        std::printf("   variants checked %lld\n", (long long)vn);
        CHECK_EQ(vbad, int64_t(0));
        CHECK_EQ(int64_t(sdk.size()), vn);
    });

    run("every collection: kind, parent, count", [&] {
        Json cols = c("collections.list", {{"library", L}})["collections"];
        auto s = o.prepare("SELECT c.ZUUID, count(ic.Z_PK) FROM ZCOLLECTION c JOIN ZENTITIES e ON e.Z_ENT = c.Z_ENT"
                           " LEFT JOIN ZIMAGEINCOLLECTION ic ON ic.ZCOLLECTION = c.Z_PK"
                           " LEFT JOIN ZIMAGE i ON i.Z_PK = ic.ZIMAGE AND i.ZISTRASHED = 0"
                           " WHERE e.ZNAME IN ('AlbumCollection', 'CatalogFolderCollection') AND (ic.Z_PK IS NULL OR i.Z_PK IS NOT NULL)"
                           " GROUP BY c.Z_PK");
        int64_t checked = 0, bad = 0;
        while (s.step()) {
            Json col = find_by(cols, "uuid", s.text(0));
            ++checked;
            if (col.is_null() || col["image_count"].is_null() || col["image_count"].get<int64_t>() != s.i64(1)) {
                ++bad;
                std::fprintf(stderr, "   collection %s: sdk %s, oracle %lld\n", s.text(0).c_str(),
                             col.is_null() ? "missing" : col["image_count"].dump().c_str(), (long long)s.i64(1));
            }
        }
        std::printf("   albums and folders checked %lld, parents repaired %lld\n", (long long)checked,
                    (long long)r["sync"]["collections"]["parents_repaired"].get<int64_t>());
        CHECK_EQ(bad, int64_t(0));
        int roots = 0, folders_without_parent = 0;
        std::set<std::string> uuids;
        for (auto& col : cols) uuids.insert(col["uuid"]);
        for (auto& col : cols) {
            roots += col["kind"] == "root";
            if (!col["parent"].is_null() && !uuids.count(col["parent"])) ++folders_without_parent;
        }
        CHECK_EQ(roots, 1);
        CHECK_EQ(folders_without_parent, 0);
    });

    run("Recent Imports: each album's time and count are Capture One's", [&] {
        Json cols = c("collections.list", {{"library", L}})["collections"];
        auto s = o.prepare("SELECT c.ZUUID, strftime('%Y-%m-%dT%H:%M:%SZ', c.ZDATECREATED, 'unixepoch'), c.ZNAME,"
                           " (SELECT count(*) FROM ZIMAGEINCOLLECTION ic JOIN ZIMAGE i ON i.Z_PK = ic.ZIMAGE"
                           "  WHERE ic.ZCOLLECTION = c.Z_PK AND i.ZISTRASHED = 0)"
                           " FROM ZCOLLECTION c WHERE c.ZPARENT = (SELECT ZLASTIMPORTSCOLLECTION FROM ZCOLLECTION"
                           "  WHERE Z_PK = (SELECT ZROOTCOLLECTION FROM ZDOCUMENTCONTENT))");
        int64_t n = 0, bad = 0;
        while (s.step()) {
            ++n;
            Json col = find_by(cols, "uuid", s.text(0));
            if (col.is_null() || col["created_at"] != s.text(1) || col["image_count"] != s.i64(3) || col["title"] != s.text(2)) {
                ++bad;
                std::fprintf(stderr, "   recent import %s: %s\n", s.text(2).c_str(), col.dump().c_str());
            }
            if (n <= 3) std::printf("   %s · %lld (%s)\n", s.text(2).c_str(), (long long)s.i64(3), s.text(1).c_str());
        }
        std::printf("   recent imports checked %lld\n", (long long)n);
        CHECK(n > 0);
        CHECK_EQ(bad, int64_t(0));
    });

    run("facets agree with an independent count", [&] {
        auto a = std::chrono::steady_clock::now();
        Json f = c("images.facets", {{"library", L}});
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        auto n = [&](const std::string& sql) { auto s = o.prepare(sql); s.step(); return s.i64(0); };
        int64_t visible = n("SELECT count(*) FROM ZIMAGE WHERE ZISTRASHED = 0");
        CHECK_EQ(f["total"].get<int64_t>(), visible);
        // The first variant's rating, as images.list shows it.
        auto rs = o.prepare("SELECT COALESCE(m.ZBASIC_RATING, 0), count(*) FROM ZIMAGE i"
                            " JOIN ZVARIANT v ON v.Z_PK = (SELECT Z_PK FROM ZVARIANT WHERE ZIMAGE = i.Z_PK ORDER BY ZINDEX, Z_PK LIMIT 1)"
                            " LEFT JOIN ZVARIANTLAYER l ON l.Z_PK = v.ZCOMBINEDSETTINGS LEFT JOIN ZVARIANTMETADATA m ON m.ZLAYER = l.Z_PK"
                            " WHERE i.ZISTRASHED = 0 GROUP BY 1");
        int64_t bad = 0;
        while (rs.step()) {
            int64_t got = 0;
            for (auto& r : f["rating"]) if (r["value"].get<int64_t>() == rs.i64(0)) got = r["count"].get<int64_t>();
            if (got != rs.i64(1)) { ++bad; std::fprintf(stderr, "   rating %lld: sdk %lld, oracle %lld\n", (long long)rs.i64(0), (long long)got, (long long)rs.i64(1)); }
        }
        CHECK_EQ(bad, int64_t(0));
        int64_t cams = 0, days = 0;
        for (auto& x : f["camera"]) cams += x["count"].get<int64_t>();
        for (auto& x : f["captured"]["days"]) days += x["count"].get<int64_t>();
        CHECK_EQ(cams, visible);
        CHECK_EQ(days + f["captured"]["undated"].get<int64_t>(), visible);
        CHECK_EQ(find_by(f["camera"], "value", "NIKON Z 8").is_null() ? int64_t(-1) : find_by(f["camera"], "value", "NIKON Z 8")["count"].get<int64_t>(),
                 n("SELECT count(*) FROM ZIMAGE WHERE ZISTRASHED = 0 AND ZCAMERA_MODEL = 'NIKON Z 8'"));
        std::printf("   facets in %.0f ms: %zu ratings, %zu cameras, %zu lenses, %zu days, %zu keywords\n", ms, f["rating"].size(),
                    f["camera"].size(), f["lens"].size(), f["captured"]["days"].size(), f["keyword"].size());
    });

    run("timings: every image, and one collection", [&] {
        auto t = [&](const char* what, const Json& p) {
            auto a = std::chrono::steady_clock::now();
            Json r = c("images.list", p);
            std::printf("   %s: %zu of %lld in %.0f ms\n", what, r["images"].size(), (long long)r["total"].get<int64_t>(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
            return r;
        };
        Json all = t("all images, sorted by capture", {{"library", L}});
        t("all images, full rows", {{"library", L}, {"full", true}});
        t("all images, full rows, files checked", {{"library", L}, {"full", true}, {"check_files", true}});
        t("first page of 200, by name", {{"library", L}, {"sort", "name"}, {"limit", 200}, {"full", true}});
        auto s = o.prepare("SELECT c.ZUUID FROM ZCOLLECTION c ORDER BY (SELECT count(*) FROM ZIMAGEINCOLLECTION WHERE ZCOLLECTION = c.Z_PK) DESC LIMIT 1");
        s.step();
        std::string big = s.text(0);
        t("the largest collection, full rows", {{"library", L}, {"collection", big}, {"full", true}});
        auto a = std::chrono::steady_clock::now();
        Json cols = c("collections.list", {{"library", L}})["collections"];
        std::printf("   collections.list: %zu in %.0f ms\n", cols.size(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
        CHECK(all["total"].get<int64_t>() > 0);
    });

    run("every path in the catalog is found by lookup", [&] {
        Json page = c("images.list", {{"library", L}, {"include_trashed", true}})["images"];
        Json paths = Json::array();
        for (auto& j : page) paths.push_back(j["path"]);
        Json res = c("images.lookup", {{"library", L}, {"paths", paths}})["results"];
        int64_t found = 0;
        for (auto& x : res) found += x["found"].get<bool>();
        CHECK_EQ(found, int64_t(paths.size()));
    });

    run("a forced resync of the real catalog changes nothing", [&] {
        Json s = c("library.sync", {{"library", L}, {"force", true}});
        CHECK_EQ(s["images"]["updated"].get<int64_t>(), int64_t(0));
        CHECK_EQ(s["variants"]["updated"].get<int64_t>(), int64_t(0));
        CHECK_EQ(s["collections"]["updated"].get<int64_t>(), int64_t(0));
        CHECK(c("library.sync", {{"library", L}})["skipped"].get<bool>());
    });

    run("the Capture One package is exactly as it was", [&] {
        auto after = census(package);
        int64_t changed = 0;
        for (auto& [k, v] : before) {
            auto it = after.find(k);
            if (it == after.end() || it->second != v) {
                if (++changed <= 5) std::fprintf(stderr, "   changed: %s\n", k.c_str());
            }
        }
        for (auto& [k, v] : after)
            if (!before.count(k)) { ++changed; std::fprintf(stderr, "   new: %s\n", k.c_str()); }
        CHECK_EQ(changed, int64_t(0));
    });
}

int main() {
    g_tmp = fs::temp_directory_path() / ("spklib-tests-" + std::to_string(::getpid()));
    fs::create_directories(g_tmp);
    synthetic();
    if (const char* real = std::getenv("SPKLIB_C1_CATALOG")) real_catalog(real);
    else std::printf("-- SPKLIB_C1_CATALOG not set: the real-catalog pass is skipped\n");
    fs::remove_all(g_tmp);
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
