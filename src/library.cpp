#include "library.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <variant>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "capture_one.hpp"

namespace fs = std::filesystem;

namespace spklib {

// --- small things ------------------------------------------------------------------------

std::string new_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // RFC 4122 variant
    char buf[37];
    std::snprintf(buf, sizeof buf, "%08X-%04X-%04X-%04X-%012llX", unsigned(a >> 32), unsigned((a >> 16) & 0xFFFF),
                  unsigned(a & 0xFFFF), unsigned(b >> 48), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

double now_unix() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

namespace {

constexpr const char* kOriginC1 = "capture_one";
constexpr const char* kCatalogDb = "Library.spkdb";

/// Unicode NFC, so a path typed by the app and a path stored by Capture One
/// compare equal however each spelled "未命名文件夹" or "é". Identity off
/// Apple platforms for now (docs/DESIGN.md, open items).
std::string nfc(const std::string& s) {
#ifdef __APPLE__
    bool ascii = std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii) return s;
    CFStringRef in = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()),
                                             kCFStringEncodingUTF8, false);
    if (!in) return s;
    CFMutableStringRef m = CFStringCreateMutableCopy(nullptr, 0, in);
    CFStringNormalize(m, kCFStringNormalizationFormC);
    CFIndex len = CFStringGetLength(m), used = 0;
    CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false, nullptr, 0, &used);
    std::string out(size_t(used), '\0');
    CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false,
                     reinterpret_cast<UInt8*>(out.data()), used, &used);
    CFRelease(m);
    CFRelease(in);
    return out;
#else
    return s;
#endif
}

std::string iso(double t, bool utc) {
    time_t secs = static_cast<time_t>(std::floor(t));
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, utc ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

/// "2024-01-08T23:39:04" (no zone) -> the floating value Capture One stores.
std::optional<double> parse_local(const std::string& s) {
    std::tm tm{};
    int y, mo, d, h = 0, mi = 0;
    double sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 3) return std::nullopt;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = 0;
    return double(timegm(&tm)) + sec;
}

std::string lower_ext(const std::string& name) {
    auto dot = name.rfind('.');
    std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string kind_for_extension(const std::string& name) {
    static const std::set<std::string> raw = {"nef", "arw", "cr2", "cr3", "raf", "dng", "orf", "rw2", "pef", "srw", "iiq", "3fr", "fff", "nrw", "rwl", "x3f", "erf", "mos", "dcr", "kdc", "mrw"};
    static const std::set<std::string> raster = {"jpg", "jpeg", "tif", "tiff", "png", "heic", "heif", "psd", "psb", "webp", "avif", "jxl"};
    static const std::set<std::string> movie = {"mov", "mp4", "m4v", "avi", "mts", "mxf"};
    std::string e = lower_ext(name);
    if (raw.count(e)) return "raw";
    if (raster.count(e)) return "raster";
    if (movie.count(e)) return "movie";
    return "other";
}

std::string join_path(const std::string& base, const std::string& rel_in) {
    std::string rel = rel_in;
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    std::string b = base;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    if (rel.empty()) return b;
    return b == "/" ? "/" + rel : b + "/" + rel;
}

bool is_prefix_dir(const std::string& ancestor, const std::string& child) {
    if (ancestor == child) return false;
    if (ancestor == "/") return child.size() > 1 && child[0] == '/';
    return child.size() > ancestor.size() && child.compare(0, ancestor.size(), ancestor) == 0 &&
           child[ancestor.size()] == '/';
}

/// Strings that name things in params. Throws invalid_params with the key.
std::string need_string(const Json& p, const char* key) {
    if (!p.is_object() || !p.contains(key) || !p[key].is_string() || p[key].get<std::string>().empty())
        throw ApiError("invalid_params", std::string("'") + key + "' must be a non-empty string");
    return p[key].get<std::string>();
}

std::vector<std::string> string_list(const Json& p, const char* key) {
    std::vector<std::string> out;
    if (!p.is_object() || !p.contains(key)) return out;
    const Json& v = p[key];
    if (v.is_string()) { out.push_back(v.get<std::string>()); return out; }
    if (!v.is_array()) throw ApiError("invalid_params", std::string("'") + key + "' must be a string or an array of strings");
    for (auto& e : v) {
        if (!e.is_string()) throw ApiError("invalid_params", std::string("'") + key + "' must hold strings");
        out.push_back(e.get<std::string>());
    }
    return out;
}

Json opt(const std::optional<int64_t>& v) { return v ? Json(*v) : Json(nullptr); }
Json opt(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }
Json opt(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

// --- the schema --------------------------------------------------------------------------

const char* kSchema = R"SQL(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);

CREATE TABLE path_location(
  id INTEGER PRIMARY KEY,
  uuid TEXT NOT NULL UNIQUE,
  base TEXT NOT NULL CHECK (base IN ('absolute', 'library', 'origin')),
  root TEXT NOT NULL DEFAULT '',
  path TEXT NOT NULL DEFAULT '',
  volume TEXT,
  relinked_dir TEXT,
  origin TEXT NOT NULL DEFAULT 'local',
  origin_key TEXT,
  origin_state TEXT NOT NULL DEFAULT 'present',
  UNIQUE (origin, origin_key));

CREATE TABLE image(
  id INTEGER PRIMARY KEY,
  uuid TEXT NOT NULL UNIQUE,
  location_id INTEGER NOT NULL REFERENCES path_location(id),
  file_name TEXT NOT NULL,
  display_name TEXT,
  kind TEXT NOT NULL DEFAULT 'other',
  inside_library INTEGER NOT NULL DEFAULT 0,
  width INTEGER, height INTEGER, file_size INTEGER,
  captured_local REAL, imported_at REAL,
  camera_make TEXT, camera_model TEXT, lens TEXT, iso INTEGER,
  aperture REAL, shutter REAL, focal_length REAL, exposure_comp TEXT,
  gps_lat TEXT, gps_lon TEXT, gps_alt TEXT,
  origin_trashed INTEGER NOT NULL DEFAULT 0,
  trashed INTEGER,
  previous_location_id INTEGER REFERENCES path_location(id),
  origin TEXT NOT NULL DEFAULT 'local',
  origin_key TEXT,
  origin_state TEXT NOT NULL DEFAULT 'present',
  added_at REAL NOT NULL,
  UNIQUE (origin, origin_key));
CREATE INDEX image_location ON image(location_id, file_name);

CREATE TABLE variant(
  id INTEGER PRIMARY KEY,
  uuid TEXT NOT NULL UNIQUE,
  image_id INTEGER NOT NULL REFERENCES image(id),
  position INTEGER NOT NULL,
  origin_rating INTEGER, origin_color_tag INTEGER,
  origin_meta TEXT, origin_geometry TEXT, origin_modified INTEGER,
  rating INTEGER, color_tag INTEGER,
  origin TEXT NOT NULL DEFAULT 'local',
  origin_key TEXT,
  origin_state TEXT NOT NULL DEFAULT 'present',
  created_at REAL NOT NULL,
  UNIQUE (origin, origin_key));
CREATE INDEX variant_image ON variant(image_id, position);

CREATE TABLE edit(
  variant_id INTEGER PRIMARY KEY REFERENCES variant(id),
  json TEXT NOT NULL,
  revision INTEGER NOT NULL,
  modified_at REAL NOT NULL);

CREATE TABLE collection(
  id INTEGER PRIMARY KEY,
  uuid TEXT NOT NULL UNIQUE,
  parent_id INTEGER REFERENCES collection(id),
  kind TEXT NOT NULL,
  name TEXT NOT NULL DEFAULT '',
  position INTEGER,
  folder_location_id INTEGER REFERENCES path_location(id),
  criteria TEXT,
  criteria_format TEXT,
  origin TEXT NOT NULL DEFAULT 'local',
  origin_key TEXT,
  origin_state TEXT NOT NULL DEFAULT 'present',
  origin_entity TEXT,
  created_at REAL NOT NULL,
  UNIQUE (origin, origin_key));
CREATE INDEX collection_parent ON collection(parent_id);

CREATE TABLE collection_image(
  collection_id INTEGER NOT NULL REFERENCES collection(id) ON DELETE CASCADE,
  image_id INTEGER NOT NULL REFERENCES image(id),
  position INTEGER,
  origin TEXT NOT NULL DEFAULT 'local',
  PRIMARY KEY (collection_id, image_id));
CREATE INDEX collection_image_image ON collection_image(image_id);

CREATE TABLE keyword(
  id INTEGER PRIMARY KEY,
  uuid TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL,
  parent_id INTEGER REFERENCES keyword(id),
  origin TEXT NOT NULL DEFAULT 'local',
  origin_key TEXT,
  origin_state TEXT NOT NULL DEFAULT 'present',
  UNIQUE (origin, origin_key));

CREATE TABLE sync_log(id INTEGER PRIMARY KEY, at REAL NOT NULL, report TEXT NOT NULL);

CREATE TABLE session_folder(
  role TEXT PRIMARY KEY CHECK (role IN ('capture', 'selects', 'output', 'trash')),
  location_id INTEGER NOT NULL REFERENCES path_location(id));

CREATE TABLE fs_intent(
  id INTEGER PRIMARY KEY,
  op TEXT NOT NULL,
  image_id INTEGER NOT NULL REFERENCES image(id),
  from_path TEXT NOT NULL,
  to_path TEXT NOT NULL,
  to_location_id INTEGER NOT NULL REFERENCES path_location(id),
  at REAL NOT NULL);
)SQL";

// --- upserting origin rows ---------------------------------------------------------------

using Val = std::variant<std::monostate, int64_t, double, std::string>;

Val v(const std::optional<int64_t>& x) { return x ? Val(*x) : Val(); }
Val v(const std::optional<double>& x) { return x ? Val(*x) : Val(); }
Val v(const std::optional<std::string>& x) { return x ? Val(*x) : Val(); }
Val v(const std::string& x) { return Val(x); }
Val v(int64_t x) { return Val(x); }
Val v(bool x) { return Val(int64_t(x)); }

void bind_val(Stmt& s, int i, const Val& x) {
    std::visit([&](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, std::monostate>) s.bind(i, nullptr);
        else s.bind(i, a);
    }, x);
}

/// One origin table's sync: update a row only when a value differs, insert
/// when it is new, and say which of the three happened.
class Upserter {
public:
    enum Status { Added, Updated, Unchanged };

    Upserter(Db& db, const std::string& table, std::vector<std::string> cols, std::vector<std::string> insert_only)
        : cols_(std::move(cols)), insert_only_(std::move(insert_only)),
          upd_(db.prepare(update_sql(table))), sel_(db.prepare("SELECT id FROM " + table +
                                                              " WHERE origin='capture_one' AND origin_key=?1")),
          ins_(db.prepare(insert_sql(table))), db_(db) {}

    std::pair<int64_t, Status> put(const std::string& key, const std::string& uuid, const std::vector<Val>& vals,
                                   const std::vector<Val>& insert_vals = {}) {
        const int n = static_cast<int>(cols_.size());
        for (int i = 0; i < n; ++i) bind_val(upd_, i + 1, vals[size_t(i)]);
        upd_.bind(n + 1, key);
        upd_.run();
        Status st = db_.changes() > 0 ? Updated : Unchanged;
        sel_.bind(1, key);
        if (sel_.step()) {
            int64_t id = sel_.i64(0);
            sel_.reset();
            return {id, st};
        }
        sel_.reset();
        ins_.bind(1, uuid).bind(2, key);
        for (int i = 0; i < n; ++i) bind_val(ins_, 3 + i, vals[size_t(i)]);
        for (size_t i = 0; i < insert_only_.size(); ++i) bind_val(ins_, 3 + n + int(i), insert_vals[i]);
        ins_.run();
        return {db_.last_insert_id(), Added};
    }

private:
    std::string update_sql(const std::string& t) const {
        std::string set, same;
        for (size_t i = 0; i < cols_.size(); ++i) {
            std::string p = "?" + std::to_string(i + 1);
            set += (i ? ", " : "") + cols_[i] + " = " + p;
            same += cols_[i] + " IS " + p + " AND ";
        }
        std::string k = "?" + std::to_string(cols_.size() + 1);
        return "UPDATE " + t + " SET " + set + ", origin_state = 'present' WHERE origin = 'capture_one' AND origin_key = " +
               k + " AND NOT (" + same + "origin_state = 'present')";
    }
    std::string insert_sql(const std::string& t) const {
        std::string c = "uuid, origin, origin_key", q = "?1, 'capture_one', ?2";
        int i = 3;
        for (auto& col : cols_) { c += ", " + col; q += ", ?" + std::to_string(i++); }
        for (auto& col : insert_only_) { c += ", " + col; q += ", ?" + std::to_string(i++); }
        return "INSERT INTO " + t + " (" + c + ") VALUES (" + q + ")";
    }

    std::vector<std::string> cols_, insert_only_;
    Stmt upd_, sel_, ins_;
    Db& db_;
};

struct Tally {
    int64_t added = 0, updated = 0, unchanged = 0, removed = 0;
    void note(Upserter::Status s) { (s == Upserter::Added ? added : s == Upserter::Updated ? updated : unchanged)++; }
    Json to_json() const { return {{"added", added}, {"updated", updated}, {"unchanged", unchanged}, {"removed", removed}}; }
};

/// Mark origin rows the source no longer has. `seen` holds row ids.
int64_t mark_removed(Db& db, const std::string& table, const std::set<int64_t>& seen) {
    db.exec("CREATE TEMP TABLE IF NOT EXISTS seen_ids(id INTEGER PRIMARY KEY)");
    db.exec("DELETE FROM seen_ids");
    {
        auto ins = db.prepare("INSERT INTO seen_ids(id) VALUES (?)");
        for (int64_t id : seen) { ins.bind(1, id); ins.run(); }
    }
    db.exec("UPDATE " + table + " SET origin_state = 'removed' WHERE origin = 'capture_one' AND origin_state = 'present'"
            " AND id NOT IN (SELECT id FROM seen_ids)");
    return db.changes();
}

}  // namespace

// --- location cache ------------------------------------------------------------------------

struct Library::LocationCache {
    std::unordered_map<int64_t, std::string> dir;            // id -> resolved absolute folder
    std::unordered_map<std::string, std::vector<int64_t>> by_dir;  // nfc(folder) -> ids
};

void Library::build_location_cache() {
    auto cache = std::make_shared<LocationCache>();
    std::string lib = path_;
    std::string pkg = origin_package();
    auto s = db_.prepare("SELECT id, base, root, path, relinked_dir FROM path_location");
    while (s.step()) {
        std::string base = s.text(1), root = s.text(2);
        std::string dir = base == "library" ? join_path(lib, s.text(3))
                        : base == "origin"  ? join_path(pkg, s.text(3))
                        : !s.is_null(4)     ? s.text(4)
                                            : join_path(root.empty() ? "/" : root, s.text(3));
        cache->dir[s.i64(0)] = dir;
        cache->by_dir[nfc(dir)].push_back(s.i64(0));
    }
    locations_ = cache;
}

std::string Library::resolve_location(int64_t id) {
    if (!locations_) build_location_cache();
    auto it = locations_->dir.find(id);
    return it == locations_->dir.end() ? std::string() : it->second;
}

// --- create / open -------------------------------------------------------------------------

Library::~Library() = default;

void Library::lock_session() {
    // One SpektraLab at a time per session: the folder may be on a shared or
    // removable disk, and two writers moving files is the one way a session
    // can lose a photograph. SQLite's exclusive locking mode holds the
    // database's lock from the first write until the connection closes, so
    // the guard lives in the one file the session already has. (An flock on
    // the same file collides with Apple's SQLite build: measured.)
    db_.exec("PRAGMA locking_mode = EXCLUSIVE");
    sqlite3_busy_timeout(db_.raw(), 250);
    try {
        db_.exec("BEGIN EXCLUSIVE");
        db_.exec("COMMIT");
    } catch (const DbError& e) {
        sqlite3_busy_timeout(db_.raw(), 5000);
        if (e.code == SQLITE_BUSY || e.code == SQLITE_LOCKED) throw ApiError("in_use", "the session is open in another SpektraLab");
        throw;
    }
    sqlite3_busy_timeout(db_.raw(), 5000);
}

Library::Library(std::string path, std::string kind, Db db)
    : path_(std::move(path)), kind_(std::move(kind)), db_(std::move(db)) {
    db_.exec("PRAGMA foreign_keys = ON");
    // A catalog lives in Application Support: WAL. A session is a folder the
    // user carries between disks and machines, so it keeps everything in its
    // one database file -- no -wal/-shm to leave behind (SESSION-DESIGN.md).
    db_.exec(kind_ == "session" ? "PRAGMA journal_mode = DELETE" : "PRAGMA journal_mode = WAL");
    db_.exec(kind_ == "session" ? "PRAGMA synchronous = FULL" : "PRAGMA synchronous = NORMAL");
}

std::string Library::database_path(const std::string& path, const std::string& kind) {
    if (kind == "session") return (fs::path(path) / (fs::path(path).filename().string() + ".spksession")).string();
    return (fs::path(path) / kCatalogDb).string();
}

std::unique_ptr<Library> Library::create(const std::string& path_in, const std::string& kind, const Json& options) {
    if (kind != "catalog" && kind != "session") throw ApiError("invalid_params", "kind must be 'catalog' or 'session'");
    fs::path p = fs::absolute(fs::path(path_in)).lexically_normal();
    if (p.filename().empty()) p = p.parent_path();
    if (kind == "catalog" && p.extension() != ".spkcatalog") p += ".spkcatalog";
    std::error_code ec;
    if (fs::exists(p, ec)) {
        if (!fs::is_directory(p, ec)) throw ApiError("exists", "a file is in the way: " + p.string());
        if (fs::exists(database_path(p.string(), kind), ec)) throw ApiError("exists", "a library already exists at " + p.string());
    }
    fs::create_directories(p);
    std::string dbp = database_path(p.string(), kind);
    Db db(dbp, Db::Mode::Create);
    std::unique_ptr<Library> lib(new Library(p.string(), kind, std::move(db)));
    {
        Transaction t(lib->db_);
        lib->db_.exec(kSchema);
        std::string name = options.is_object() && options.contains("name") && options["name"].is_string()
                               ? options["name"].get<std::string>()
                               : p.stem().string();
        lib->set_meta_value("format", "spklib.library");
        lib->set_meta_value("schema_version", std::to_string(kSchemaVersion));
        lib->set_meta_value("uuid", new_uuid());
        lib->set_meta_value("kind", kind);
        lib->set_meta_value("name", name);
        lib->set_meta_value("created_at", std::to_string(now_unix()));
        lib->set_meta_value("origin_kind", "none");
        t.commit();
    }
    if (kind == "session") {
        // The session's working folders, as Capture One lays them out
        // (docs/SESSION-DESIGN.md §3.1). Their roles live in the database, so
        // a renamed folder keeps its role.
        Json names = options.is_object() && options.contains("folders") ? options["folders"] : Json::object();
        Transaction t(lib->db_);
        for (auto [role, def] : std::vector<std::pair<const char*, const char*>>{
                 {"capture", "Capture"}, {"selects", "Selects"}, {"output", "Output"}, {"trash", "Trash"}}) {
            std::string name = names.is_object() && names.contains(role) && names[role].is_string() ? names[role].get<std::string>() : def;
            fs::create_directories(p / name);
            int64_t loc = lib->ensure_location((p / name).string());
            auto s = lib->db_.prepare("INSERT INTO session_folder(role, location_id) VALUES (?, ?)");
            s.bind(1, role).bind(2, loc);
            s.run();
        }
        t.commit();
        lib->lock_session();
    }
    return lib;
}

std::pair<std::string, std::string> Library::locate(const std::string& path_in) {
    fs::path p = fs::absolute(fs::path(path_in)).lexically_normal();
    if (p.filename().empty()) p = p.parent_path();
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) p = p.parent_path();
    if (fs::is_regular_file(database_path(p.string(), "catalog"), ec)) return {p.string(), "catalog"};
    if (fs::is_regular_file(database_path(p.string(), "session"), ec)) return {p.string(), "session"};
    // A session folder that was renamed keeps its old database name: the one
    // .spksession inside is the session (SESSION-DESIGN.md §3.1).
    std::vector<fs::path> found;
    for (auto& e : fs::directory_iterator(p, ec))
        if (e.path().extension() == ".spksession") found.push_back(e.path());
    if (found.size() == 1) {
        fs::rename(found[0], database_path(p.string(), "session"), ec);
        if (!ec) return {p.string(), "session"};
    }
    if (found.size() > 1) throw ApiError("ambiguous", "more than one .spksession in " + p.string());
    throw ApiError("not_found", "no SpektraLab library at " + p.string());
}

std::unique_ptr<Library> Library::open(const std::string& path_in) {
    auto [dir, kind] = locate(path_in);
    fs::path p(dir);
    Db db(database_path(p.string(), kind), Db::Mode::ReadWrite);
    // A session held by another SpektraLab answers BUSY to the very first
    // statement; ask briefly and call it what it is.
    if (kind == "session") sqlite3_busy_timeout(db.raw(), 250);
    try {
        std::unique_ptr<Library> lib(new Library(p.string(), kind, std::move(db)));
        if (!lib->db_.has_table("meta") || lib->meta("format") != "spklib.library")
            throw ApiError("unsupported", "not a SpektraLab library: " + p.string());
        lib->migrate();
        if (kind == "session") {
            lib->lock_session();
            lib->reconcile_intents();
        }
        return lib;
    } catch (const DbError& e) {
        if (kind == "session" && (e.code == SQLITE_BUSY || e.code == SQLITE_LOCKED))
            throw ApiError("in_use", "the session is open in another SpektraLab");
        throw;
    }
}

void Library::migrate() {
    int v = std::stoi(meta("schema_version", "0"));
    if (v > kSchemaVersion)
        throw ApiError("unsupported", "library schema " + std::to_string(v) + " is newer than this SDK (" +
                                          std::to_string(kSchemaVersion) + ")");
    // Schema 1 is the first; later versions add their steps here.
}

std::string Library::meta(const std::string& key, const std::string& fallback) {
    auto s = db_.prepare("SELECT value FROM meta WHERE key=?");
    s.bind(1, key);
    return s.step() ? s.text(0) : fallback;
}

void Library::set_meta_value(const std::string& key, const std::string& value) {
    auto s = db_.prepare("INSERT INTO meta(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, key).bind(2, value);
    s.run();
}

std::string Library::uuid() { return meta("uuid"); }
std::string Library::origin_package() { return meta("origin_path"); }

Json Library::info() {
    Json j;
    j["uuid"] = uuid();
    j["kind"] = kind_;
    j["name"] = meta("name");
    j["path"] = path_;
    j["database"] = database_path(path_, kind_);
    j["schema_version"] = std::stoi(meta("schema_version", "0"));
    j["created_at"] = iso(std::stod(meta("created_at", "0")), true);
    Json origin;
    origin["kind"] = meta("origin_kind", "none");
    if (origin["kind"] != "none") {
        origin["path"] = meta("origin_path");
        origin["database"] = meta("origin_database");
        origin["document_uuid"] = meta("origin_document_uuid");
        origin["format"] = meta("origin_format");
        std::string synced = meta("origin_synced_at");
        origin["synced_at"] = synced.empty() ? Json(nullptr) : Json(iso(std::stod(synced), true));
        std::string fp = meta("origin_fingerprint");
        origin["fingerprint"] = fp.empty() ? Json(nullptr) : Json::parse(fp);
        origin["write_back"] = false;
    }
    j["origin"] = origin;
    Json counts;
    for (auto [name, sql] : std::vector<std::pair<const char*, const char*>>{
             {"images", "SELECT count(*) FROM image WHERE origin_state='present'"},
             {"variants", "SELECT count(*) FROM variant WHERE origin_state='present'"},
             {"collections", "SELECT count(*) FROM collection WHERE origin_state='present'"},
             {"edits", "SELECT count(*) FROM edit"},
             {"removed_images", "SELECT count(*) FROM image WHERE origin_state='removed'"}}) {
        auto s = db_.prepare(sql);
        s.step();
        counts[name] = s.i64(0);
    }
    j["counts"] = counts;
    return j;
}

// --- Capture One -----------------------------------------------------------------------------

Json Library::link_capture_one(const std::string& source, const Json& options) {
    if (kind_ != "catalog") throw ApiError("invalid_params", "a Capture One catalog links to a SpektraLab catalog, not a session");
    if (meta("origin_kind", "none") != "none") throw ApiError("exists", "this library already links " + meta("origin_path"));
    c1::Located loc = c1::locate(source);
    if (fs::path(loc.database).extension() == ".cosessiondb")
        throw ApiError("unsupported", "linking a Capture One session waits for its schema to be verified (SESSION-DESIGN.md §6); "
                                      "capture_one.probe reads it");
    {
        Transaction t(db_);
        set_meta_value("origin_kind", "capture_one.catalog");
        set_meta_value("origin_path", loc.package);
        set_meta_value("origin_database", loc.database);
        t.commit();
    }
    Json o = options.is_object() ? options : Json::object();
    o["force"] = true;
    return sync(o);
}

Json Library::sync(const Json& options) {
    if (meta("origin_kind", "none") != "capture_one.catalog") throw ApiError("invalid_params", "this library links no Capture One catalog");
    bool force = options.is_object() && options.value("force", false);
    // The package may have moved since the link: an explicit `source` points
    // at its new place, and the document UUID check below proves it is the
    // same catalog.
    std::string package = meta("origin_path");
    if (options.is_object() && options.contains("source") && options["source"].is_string()) package = options["source"].get<std::string>();
    c1::Located loc;
    try {
        loc = c1::locate(package);
    } catch (const std::exception& e) {
        throw ApiError("not_found", std::string("the Capture One catalog is not reachable: ") + e.what());
    }
    c1::Fingerprint fp = c1::fingerprint(loc.database);
    std::string last = meta("origin_fingerprint");
    if (!force && !last.empty() && c1::Fingerprint::from_json(Json::parse(last)) == fp && package == meta("origin_path")) {
        return {{"skipped", true}, {"reason", "unchanged since the last sync"}, {"fingerprint", fp.to_json()}};
    }

    auto t0 = std::chrono::steady_clock::now();
    std::string scratch = options.is_object() && options.contains("scratch") && options["scratch"].is_string()
                              ? options["scratch"].get<std::string>()
                              : (fs::temp_directory_path() / ("spklib-snapshot-" + new_uuid())).string();
    c1::Snapshot snap;
    try {
        snap = c1::take_snapshot(loc.database, scratch);
    } catch (const c1::SourceBusy& e) {
        std::error_code ec;
        fs::remove_all(scratch, ec);
        throw ApiError("source_busy", e.what());
    }
    c1::Catalog cat;
    try {
        cat = c1::read(snap.path);
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove_all(scratch, ec);
        throw ApiError("unsupported", e.what());
    }
    std::error_code ec;
    fs::remove_all(scratch, ec);
    auto t_read = std::chrono::steady_clock::now();

    std::string known = meta("origin_document_uuid");
    if (!known.empty() && known != cat.document_uuid)
        throw ApiError("conflict", "a different Capture One catalog is at " + loc.package + " (document " + cat.document_uuid +
                                       ", this library links " + known + ")");

    Json report;
    std::vector<std::string> warnings = cat.warnings;
    const double now = now_unix();
    {
        Transaction t(db_);

        // Locations. Capture One's own primary key is the only identity a
        // location has; it is stable for the life of the catalog file.
        Tally tl;
        std::map<int64_t, int64_t> loc_id;
        std::set<int64_t> seen;
        {
            Upserter up(db_, "path_location", {"base", "root", "path", "volume"}, {});
            for (auto& l : cat.locations) {
                auto [id, st] = up.put(std::to_string(l.pk), new_uuid(),
                                       {v(std::string(l.relative ? "origin" : "absolute")), v(l.mac_root),
                                        v(l.relative_path), v(l.volume)});
                tl.note(st);
                loc_id[l.pk] = id;
                seen.insert(id);
            }
        }
        tl.removed = mark_removed(db_, "path_location", seen);
        report["locations"] = tl.to_json();

        Tally ti;
        std::map<int64_t, int64_t> img_id;
        seen.clear();
        {
            Upserter up(db_, "image",
                        {"location_id", "file_name", "display_name", "kind", "inside_library", "width", "height",
                         "file_size", "captured_local", "imported_at", "camera_make", "camera_model", "lens", "iso",
                         "aperture", "shutter", "focal_length", "exposure_comp", "gps_lat", "gps_lon", "gps_alt",
                         "origin_trashed"},
                        {"added_at"});
            for (auto& im : cat.images) {
                auto l = loc_id.find(im.location);
                if (l == loc_id.end()) {
                    warnings.push_back("image " + im.uuid + " (" + im.file_name + ") has no location; skipped");
                    continue;
                }
                auto [id, st] = up.put(im.uuid, im.uuid,
                                       {v(l->second), v(im.file_name), v(im.display_name), v(im.kind),
                                        v(im.inside_catalog), v(im.width), v(im.height), v(im.file_size),
                                        v(im.captured_local), v(im.imported_at), v(im.camera_make),
                                        v(im.camera_model), v(im.lens), v(im.iso), v(im.aperture), v(im.shutter),
                                        v(im.focal_length), v(im.exposure_comp), v(im.gps_lat), v(im.gps_lon),
                                        v(im.gps_alt), v(im.trashed)},
                                       {Val(now)});
                ti.note(st);
                img_id[im.pk] = id;
                seen.insert(id);
            }
        }
        ti.removed = mark_removed(db_, "image", seen);
        report["images"] = ti.to_json();

        Tally tv;
        seen.clear();
        {
            Upserter up(db_, "variant",
                        {"image_id", "position", "origin_rating", "origin_color_tag", "origin_meta", "origin_geometry",
                         "origin_modified"},
                        {"created_at"});
            for (auto& va : cat.variants) {
                auto im = img_id.find(va.image);
                if (im == img_id.end()) continue;  // its image was skipped above
                auto [id, st] = up.put(va.uuid, va.uuid,
                                       {v(im->second), v(va.position), v(va.rating), v(va.color_tag),
                                        va.meta.empty() ? Val() : Val(va.meta.dump()),
                                        va.geometry.empty() ? Val() : Val(va.geometry.dump()), v(va.modified)},
                                       {Val(now)});
                tv.note(st);
                seen.insert(id);
            }
        }
        tv.removed = mark_removed(db_, "variant", seen);
        report["variants"] = tv.to_json();

        // Collections, then their parents once every row has an id.
        Tally tc;
        std::map<int64_t, int64_t> col_id;
        std::map<int64_t, const c1::Collection*> by_pk;
        seen.clear();
        // The root and the virtual collections are named in the UI's
        // language ("全部图像", "最近导入"); their role comes from the root
        // row's pointers and the entity, never from the name.
        {
            Upserter up(db_, "collection",
                        {"kind", "name", "position", "folder_location_id", "criteria", "criteria_format", "origin_entity"},
                        {"created_at"});
            for (auto& co : cat.collections) {
                by_pk[co.pk] = &co;
                std::string kind = co.kind;
                if (co.pk == cat.root_collection) kind = "root";
                else if (co.pk == cat.recent_imports_collection) kind = "recent_imports";
                else if (co.pk == cat.recent_captures_collection) kind = "recent_captures";
                std::optional<int64_t> folder;
                if (co.folder_location) {
                    auto l = loc_id.find(*co.folder_location);
                    if (l != loc_id.end()) folder = l->second;
                }
                auto [id, st] = up.put(co.uuid, co.uuid,
                                       {v(kind), v(co.name), v(co.position), v(folder), v(co.smart_criteria),
                                        co.smart_criteria ? Val(std::string("capture_one")) : Val(), v(co.entity)},
                                       {Val(now)});
                tc.note(st);
                col_id[co.pk] = id;
                seen.insert(id);
            }
        }
        tc.removed = mark_removed(db_, "collection", seen);

        // Parents. A folder whose parent row is gone -- Capture One leaves
        // these behind when an intermediate folder is removed -- hangs from
        // its nearest ancestor folder by path, which is where Capture One
        // itself shows it.
        int64_t repaired = 0;
        build_location_cache();
        std::vector<std::pair<std::string, int64_t>> folder_dirs;  // resolved dir, collection pk
        for (auto& co : cat.collections) {
            if (co.kind == "folder" && co.folder_location && loc_id.count(*co.folder_location))
                folder_dirs.emplace_back(resolve_location(loc_id[*co.folder_location]), co.pk);
        }
        {
            auto setp = db_.prepare("UPDATE collection SET parent_id = ?1 WHERE id = ?2 AND parent_id IS NOT ?1");
            for (auto& co : cat.collections) {
                std::optional<int64_t> parent;
                if (co.parent && col_id.count(*co.parent)) {
                    parent = col_id[*co.parent];
                } else if (co.parent) {
                    if (co.kind == "folder" && co.folder_location && loc_id.count(*co.folder_location)) {
                        std::string mine = resolve_location(loc_id[*co.folder_location]);
                        size_t best = 0;
                        for (auto& [dir, pk] : folder_dirs) {
                            if (pk != co.pk && is_prefix_dir(dir, mine) && dir.size() >= best) {
                                best = dir.size();
                                parent = col_id[pk];
                            }
                        }
                    } else if (cat.root_collection && col_id.count(cat.root_collection)) {
                        parent = col_id[cat.root_collection];
                    }
                    ++repaired;
                }
                setp.bind(1, parent).bind(2, col_id[co.pk]);
                setp.run();
            }
        }
        Json cj = tc.to_json();
        cj["parents_repaired"] = repaired;
        report["collections"] = cj;

        // Album membership is Capture One's, so it is replaced whole.
        db_.exec("DELETE FROM collection_image WHERE origin = 'capture_one'");
        {
            auto ins = db_.prepare("INSERT OR IGNORE INTO collection_image(collection_id, image_id, position, origin)"
                                   " VALUES (?, ?, ?, 'capture_one')");
            int64_t n = 0;
            for (auto& [c, i] : cat.album_members) {
                auto ci = col_id.find(c);
                auto ii = img_id.find(i);
                if (ci == col_id.end() || ii == img_id.end()) continue;
                ins.bind(1, ci->second).bind(2, ii->second).bind(3, ++n);
                ins.run();
            }
            report["album_members"] = n;
        }

        Tally tk;
        seen.clear();
        {
            Upserter up(db_, "keyword", {"name"}, {});
            std::map<int64_t, int64_t> kw_id;
            for (auto& k : cat.keywords) {
                auto [id, st] = up.put(std::to_string(k.pk), new_uuid(), {v(k.name)});
                tk.note(st);
                kw_id[k.pk] = id;
                seen.insert(id);
            }
            auto setp = db_.prepare("UPDATE keyword SET parent_id = ? WHERE id = ?");
            for (auto& k : cat.keywords) {
                std::optional<int64_t> parent;
                if (k.parent && kw_id.count(*k.parent)) parent = kw_id[*k.parent];
                setp.bind(1, parent).bind(2, kw_id[k.pk]);
                setp.run();
            }
        }
        tk.removed = mark_removed(db_, "keyword", seen);
        report["keywords"] = tk.to_json();

        set_meta_value("origin_document_uuid", cat.document_uuid);
        set_meta_value("origin_format", cat.format);
        set_meta_value("origin_path", loc.package);
        set_meta_value("origin_database", loc.database);
        set_meta_value("origin_fingerprint", snap.source.to_json().dump());
        set_meta_value("origin_synced_at", std::to_string(now));
        if (meta("name").empty() || meta("name") == fs::path(path_).stem().string()) {
            // A linked library is named after the catalog it mirrors.
            set_meta_value("name", fs::path(loc.package).stem().string());
        }
        report["warnings"] = warnings;
        report["document_uuid"] = cat.document_uuid;
        report["format"] = cat.format;
        report["snapshot_attempts"] = snap.attempts;
        auto t_end = std::chrono::steady_clock::now();
        report["ms"] = {{"snapshot_and_read", std::chrono::duration<double, std::milli>(t_read - t0).count()},
                        {"write", std::chrono::duration<double, std::milli>(t_end - t_read).count()}};
        auto log = db_.prepare("INSERT INTO sync_log(at, report) VALUES (?, ?)");
        log.bind(1, now).bind(2, report.dump());
        log.run();
        t.commit();
    }
    locations_.reset();
    report["skipped"] = false;
    return report;
}

// --- reading ---------------------------------------------------------------------------------

namespace {

/// The parts of an images query that become SQL. Shared by `images` and by
/// a SpektraLab smart album, which is a saved filter.
struct Where {
    std::string sql = "1";
    std::vector<Val> binds;
    void add(const std::string& clause) { sql += " AND (" + clause + ")"; }
    void add(const std::string& clause, Val b) { add(clause); binds.push_back(std::move(b)); }
};

const char* kPrimary =
    "LEFT JOIN variant pv ON pv.id = (SELECT id FROM variant WHERE image_id = i.id AND origin_state = 'present'"
    " ORDER BY position LIMIT 1)";
const char* kRating = "COALESCE(pv.rating, pv.origin_rating, 0)";
const char* kColor = "NULLIF(COALESCE(pv.color_tag, pv.origin_color_tag, 0), 0)";
const char* kTrashed = "COALESCE(i.trashed, i.origin_trashed)";

void apply_filter(Where& w, const Json& f) {
    if (!f.is_object()) return;
    if (f.contains("min_rating")) w.add(std::string(kRating) + " >= ?", Val(f["min_rating"].get<int64_t>()));
    if (f.contains("max_rating")) w.add(std::string(kRating) + " <= ?", Val(f["max_rating"].get<int64_t>()));
    if (f.contains("color_tags") && f["color_tags"].is_array() && !f["color_tags"].empty()) {
        std::string in;
        for (auto& c : f["color_tags"]) in += (in.empty() ? "" : ",") + std::to_string(c.get<int64_t>());
        w.add(std::string("COALESCE(") + kColor + ", 0) IN (" + in + ")");
    }
    if (f.contains("kinds") && f["kinds"].is_array() && !f["kinds"].empty()) {
        std::string in;
        for (auto& k : f["kinds"]) {
            std::string s = k.get<std::string>();
            if (s.find('\'') != std::string::npos) throw ApiError("invalid_params", "bad kind");
            in += (in.empty() ? "'" : ",'") + s + "'";
        }
        w.add("i.kind IN (" + in + ")");
    }
    if (f.contains("text") && f["text"].is_string() && !f["text"].get<std::string>().empty()) {
        std::string like = "%" + f["text"].get<std::string>() + "%";
        w.add("i.file_name LIKE ? OR i.display_name LIKE ?");
        w.binds.push_back(Val(like));
        w.binds.push_back(Val(like));
    }
    if (f.contains("captured_from")) {
        auto t = parse_local(f["captured_from"].get<std::string>());
        if (!t) throw ApiError("invalid_params", "captured_from must be YYYY-MM-DD[THH:MM:SS]");
        w.add("i.captured_local >= ?", Val(*t));
    }
    if (f.contains("captured_to")) {
        auto t = parse_local(f["captured_to"].get<std::string>());
        if (!t) throw ApiError("invalid_params", "captured_to must be YYYY-MM-DD[THH:MM:SS]");
        w.add("i.captured_local < ?", Val(*t));
    }
    if (f.contains("camera_model") && f["camera_model"].is_string()) w.add("i.camera_model = ?", Val(f["camera_model"].get<std::string>()));
    if (f.contains("has_edit") && f["has_edit"].is_boolean())
        w.add(std::string(f["has_edit"].get<bool>() ? "" : "NOT ") +
              "EXISTS (SELECT 1 FROM edit e JOIN variant ev ON ev.id = e.variant_id WHERE ev.image_id = i.id)");
}

}  // namespace

int64_t Library::image_id(const std::string& uuid) {
    auto s = db_.prepare("SELECT id FROM image WHERE uuid = ?");
    s.bind(1, uuid);
    if (!s.step()) throw ApiError("not_found", "no image " + uuid);
    return s.i64(0);
}

int64_t Library::variant_id(const std::string& uuid) {
    auto s = db_.prepare("SELECT id FROM variant WHERE uuid = ?");
    s.bind(1, uuid);
    if (!s.step()) throw ApiError("not_found", "no variant " + uuid);
    return s.i64(0);
}

int64_t Library::collection_id(const std::string& uuid, std::string* kind, std::string* origin) {
    auto s = db_.prepare("SELECT id, kind, origin FROM collection WHERE uuid = ?");
    s.bind(1, uuid);
    if (!s.step()) throw ApiError("not_found", "no collection " + uuid);
    if (kind) *kind = s.text(1);
    if (origin) *origin = s.text(2);
    return s.i64(0);
}

Json Library::collections(const Json& params) {
    bool include_removed = params.is_object() && params.value("include_removed", false);
    struct Row {
        int64_t id;
        std::optional<int64_t> parent;
        std::string uuid, kind, name, origin, state;
        std::optional<int64_t> position, folder;
        std::optional<std::string> criteria, criteria_format, entity;
    };
    std::vector<Row> rows;
    {
        auto s = db_.prepare("SELECT id, parent_id, uuid, kind, name, origin, origin_state, position, folder_location_id,"
                             " criteria, criteria_format, origin_entity FROM collection ORDER BY parent_id, position, name");
        while (s.step()) {
            Row r{s.i64(0), s.opt_i64(1), s.text(2), s.text(3), s.text(4), s.text(5), s.text(6), s.opt_i64(7),
                  s.opt_i64(8), s.opt_text(9), s.opt_text(10), s.opt_text(11)};
            if (!include_removed && r.state != "present") continue;
            rows.push_back(std::move(r));
        }
    }
    std::map<int64_t, std::vector<int64_t>> children;
    std::map<int64_t, const Row*> by_id;
    for (auto& r : rows) {
        by_id[r.id] = &r;
        if (r.parent) children[*r.parent].push_back(r.id);
    }
    std::map<int64_t, std::string> uuid_of;
    for (auto& r : rows) uuid_of[r.id] = r.uuid;

    auto count = [&](const std::string& where, const std::vector<Val>& binds) -> int64_t {
        auto s = db_.prepare(std::string("SELECT count(*) FROM image i ") + kPrimary + " WHERE i.origin_state = 'present' AND " + where);
        for (size_t k = 0; k < binds.size(); ++k) bind_val(s, int(k) + 1, binds[k]);
        s.step();
        return s.i64(0);
    };
    std::string visible = std::string(kTrashed) + " = 0";

    Json out = Json::array();
    for (auto& r : rows) {
        Json j;
        j["uuid"] = r.uuid;
        j["parent"] = r.parent && uuid_of.count(*r.parent) ? Json(uuid_of[*r.parent]) : Json(nullptr);
        j["kind"] = r.kind;
        j["name"] = r.name;
        j["position"] = opt(r.position);
        j["origin"] = r.origin;
        if (r.state != "present") j["origin_state"] = r.state;
        j["editable"] = r.origin == "local";
        Json count_j = nullptr;
        if (r.kind == "album" || r.kind == "variant_album") {
            count_j = count(visible + " AND i.id IN (SELECT image_id FROM collection_image WHERE collection_id = ?)", {Val(r.id)});
        } else if (r.kind == "folder" && r.folder) {
            j["folder"] = resolve_location(*r.folder);
            count_j = count(visible + " AND i.location_id = ?", {Val(*r.folder)});
        } else if (r.kind == "all_images") {
            count_j = count(visible, {});
        } else if (r.kind == "trash") {
            count_j = count(std::string(kTrashed) + " = 1", {});
        } else if (r.kind == "in_library") {
            count_j = count(visible + " AND i.inside_library = 1", {});
        } else if (r.kind == "smart_album") {
            if (r.criteria_format.value_or("") == "spklib" && r.criteria) {
                Where w;
                apply_filter(w, Json::parse(*r.criteria));
                count_j = count(visible + " AND " + w.sql, w.binds);
                j["criteria"] = Json::parse(*r.criteria);
            } else {
                j["supported"] = false;  // Capture One's criteria are stored, not evaluated (docs/DESIGN.md)
            }
        } else if (r.kind == "project" || r.kind == "group" || r.kind == "root" || r.kind == "recent_imports" ||
                   r.kind == "recent_captures") {
            // A container shows what its albums hold.
            std::vector<int64_t> stack{r.id}, albums;
            while (!stack.empty()) {
                int64_t c = stack.back();
                stack.pop_back();
                for (int64_t ch : children[c]) {
                    auto& k = by_id[ch]->kind;
                    if (k == "album" || k == "variant_album") albums.push_back(ch);
                    else if (k == "project" || k == "group" || k == "recent_imports" || k == "recent_captures") stack.push_back(ch);
                }
            }
            if (r.kind != "root") {
                std::string in;
                for (int64_t a : albums) in += (in.empty() ? "" : ",") + std::to_string(a);
                count_j = albums.empty() ? 0 : count(visible + " AND i.id IN (SELECT image_id FROM collection_image WHERE collection_id IN (" + in + "))", {});
            }
        }
        j["image_count"] = count_j;
        if (r.entity) j["capture_one_entity"] = *r.entity;
        out.push_back(std::move(j));
    }
    return {{"collections", out}};
}

Json Library::images(const Json& params) {
    const Json p = params.is_object() ? params : Json::object();
    Where w;
    bool include_removed = p.value("include_removed", false);
    bool include_trashed = p.value("include_trashed", false);
    if (!include_removed) w.add("i.origin_state = 'present'");
    bool trashed_view = false;

    if (p.contains("collection") && !p["collection"].is_null()) {
        std::string kind;
        int64_t cid = collection_id(need_string(p, "collection"), &kind);
        auto s = db_.prepare("SELECT folder_location_id, criteria, criteria_format FROM collection WHERE id = ?");
        s.bind(1, cid);
        s.step();
        if (kind == "album" || kind == "variant_album") {
            w.add("i.id IN (SELECT image_id FROM collection_image WHERE collection_id = ?)", Val(cid));
        } else if (kind == "folder") {
            if (s.is_null(0)) throw ApiError("unsupported", "folder without a location");
            if (p.value("recursive", false)) {
                if (!locations_) build_location_cache();
                std::string mine = resolve_location(s.i64(0));
                std::string in = std::to_string(s.i64(0));
                for (auto& [id, dir] : locations_->dir)
                    if (is_prefix_dir(mine, dir)) in += "," + std::to_string(id);
                w.add("i.location_id IN (" + in + ")");
            } else {
                w.add("i.location_id = ?", Val(s.i64(0)));
            }
        } else if (kind == "trash") {
            trashed_view = true;
        } else if (kind == "in_library") {
            w.add("i.inside_library = 1");
        } else if (kind == "all_images" || kind == "root") {
        } else if (kind == "smart_album") {
            if (s.opt_text(2).value_or("") != "spklib") throw ApiError("unsupported", "Capture One smart album criteria are not evaluated");
            apply_filter(w, Json::parse(s.text(1)));
        } else if (kind == "project" || kind == "group" || kind == "recent_imports" || kind == "recent_captures") {
            w.add("i.id IN (WITH RECURSIVE sub(id) AS (SELECT ? UNION SELECT c.id FROM collection c JOIN sub ON c.parent_id = sub.id)"
                  " SELECT image_id FROM collection_image WHERE collection_id IN (SELECT id FROM sub))",
                  Val(cid));
        } else {
            throw ApiError("unsupported", "cannot list a collection of kind " + kind);
        }
    }
    if (p.contains("images")) {
        auto uuids = string_list(p, "images");
        std::string in;
        for (size_t k = 0; k < uuids.size(); ++k) in += k ? ",?" : "?";
        w.add(uuids.empty() ? "0" : "i.uuid IN (" + in + ")");
        for (auto& u : uuids) w.binds.push_back(Val(u));
    }
    if (trashed_view) w.add(std::string(kTrashed) + " = 1");
    else if (!include_trashed) w.add(std::string(kTrashed) + " = 0");
    if (p.contains("filter")) apply_filter(w, p["filter"]);

    std::string sort = p.value("sort", std::string("captured"));
    std::string order = sort == "captured" ? "i.captured_local IS NULL, i.captured_local, i.file_name"
                      : sort == "imported" ? "i.imported_at, i.file_name"
                      : sort == "name"     ? "i.file_name COLLATE NOCASE, i.id"
                      : sort == "rating"   ? std::string(kRating) + " DESC, i.captured_local, i.file_name"
                      : sort == "added"    ? "i.added_at, i.id"
                                           : "";
    if (order.empty()) throw ApiError("invalid_params", "sort must be captured, imported, name, rating or added");
    if (p.value("descending", false)) {
        // Reverse every key; NULL-last for capture time stays NULL-last.
        if (sort == "captured") order = "i.captured_local IS NULL, i.captured_local DESC, i.file_name DESC";
        else if (sort == "imported") order = "i.imported_at DESC, i.file_name DESC";
        else if (sort == "name") order = "i.file_name COLLATE NOCASE DESC, i.id DESC";
        else if (sort == "rating") order = std::string(kRating) + " ASC, i.captured_local DESC";
        else order = "i.added_at DESC, i.id DESC";
    }
    int64_t offset = p.value("offset", int64_t{0});
    int64_t limit = p.value("limit", int64_t{-1});

    int64_t total;
    {
        auto s = db_.prepare(std::string("SELECT count(*) FROM image i ") + kPrimary + " WHERE " + w.sql);
        for (size_t k = 0; k < w.binds.size(); ++k) bind_val(s, int(k) + 1, w.binds[k]);
        s.step();
        total = s.i64(0);
    }

    std::string sql = std::string(
        "SELECT i.id, i.uuid, i.location_id, i.file_name, i.display_name, i.kind, i.inside_library, i.width, i.height,"
        " i.file_size, i.captured_local, i.imported_at, i.camera_make, i.camera_model, i.lens, i.iso, i.aperture,"
        " i.shutter, i.focal_length, i.exposure_comp, ") + kTrashed + ", i.origin, i.origin_state, " + kRating + ", " +
        kColor + ", i.gps_lat, i.gps_lon, i.gps_alt FROM image i " + kPrimary + " WHERE " + w.sql + " ORDER BY " + order +
        " LIMIT ? OFFSET ?";
    auto s = db_.prepare(sql);
    size_t k = 0;
    for (; k < w.binds.size(); ++k) bind_val(s, int(k) + 1, w.binds[k]);
    s.bind(int(k) + 1, limit).bind(int(k) + 2, offset);

    bool check = p.value("check_files", false);
    bool full = p.value("full", false);
    if (!locations_) build_location_cache();
    Json list = Json::array();
    std::vector<int64_t> ids;
    std::map<int64_t, size_t> index_of;
    while (s.step()) {
        Json j;
        j["uuid"] = s.text(1);
        std::string dir = resolve_location(s.i64(2));
        j["path"] = join_path(dir, s.text(3));
        j["file_name"] = s.text(3);
        j["kind"] = s.text(5);
        j["rating"] = s.i64(23);
        j["color_tag"] = opt(s.opt_i64(24));
        j["trashed"] = s.i64(20) != 0;
        j["captured_local"] = s.is_null(10) ? Json(nullptr) : Json(iso(s.f64(10), false));
        if (full) {
            j["display_name"] = s.text(4);
            j["inside_library"] = s.i64(6) != 0;
            j["width"] = opt(s.opt_i64(7));
            j["height"] = opt(s.opt_i64(8));
            j["file_size"] = opt(s.opt_i64(9));
            j["imported_at"] = s.is_null(11) ? Json(nullptr) : Json(iso(s.f64(11), true));
            j["camera"] = {{"make", opt(s.opt_text(12))}, {"model", opt(s.opt_text(13))}, {"lens", opt(s.opt_text(14))}};
            j["exposure"] = {{"iso", opt(s.opt_i64(15))}, {"aperture", opt(s.opt_f64(16))}, {"shutter", opt(s.opt_f64(17))},
                             {"focal_length", opt(s.opt_f64(18))}, {"compensation", opt(s.opt_text(19))}};
            j["gps"] = {{"lat", opt(s.opt_text(25))}, {"lon", opt(s.opt_text(26))}, {"alt", opt(s.opt_text(27))}};
        }
        j["origin"] = s.text(21);
        if (s.text(22) != "present") j["origin_state"] = s.text(22);
        if (check) {
            struct stat st {};
            j["exists"] = ::stat(j["path"].get<std::string>().c_str(), &st) == 0;
        }
        ids.push_back(s.i64(0));
        index_of[s.i64(0)] = list.size();
        list.push_back(std::move(j));
    }
    // Variants for the page, in one statement.
    if (!ids.empty()) {
        std::string in;
        for (auto id : ids) in += (in.empty() ? "" : ",") + std::to_string(id);
        auto vs = db_.prepare("SELECT v.image_id, v.uuid, v.position, COALESCE(v.rating, v.origin_rating, 0),"
                              " NULLIF(COALESCE(v.color_tag, v.origin_color_tag, 0), 0), e.revision, v.origin,"
                              " v.origin_state FROM variant v LEFT JOIN edit e ON e.variant_id = v.id WHERE v.image_id IN (" +
                              in + ") ORDER BY v.image_id, v.position");
        while (vs.step()) {
            if (!include_removed && vs.text(7) != "present") continue;
            Json vj{{"uuid", vs.text(1)}, {"position", vs.i64(2)}, {"rating", vs.i64(3)}, {"color_tag", opt(vs.opt_i64(4))},
                    {"edit_revision", opt(vs.opt_i64(5))}, {"origin", vs.text(6)}};
            list[index_of[vs.i64(0)]]["variants"].push_back(std::move(vj));
        }
    }
    return {{"total", total}, {"offset", offset}, {"images", list}};
}

Json Library::image(const Json& params) {
    int64_t id = image_id(need_string(params, "image"));
    const std::string uuid = params["image"].get<std::string>();
    // One image through the list path, so the two can never disagree.
    Json listed = images({{"include_removed", true}, {"include_trashed", true}, {"full", true}, {"check_files", true},
                          {"images", Json::array({uuid})}});
    Json one = listed["images"].at(0);
    // Per-variant detail: what Capture One says, what SpektraLab overrides.
    auto vs = db_.prepare("SELECT v.uuid, v.position, v.origin_rating, v.origin_color_tag, v.rating, v.color_tag,"
                          " v.origin_meta, v.origin_geometry, v.origin_modified, e.revision, e.modified_at, v.origin,"
                          " v.origin_state FROM variant v LEFT JOIN edit e ON e.variant_id = v.id WHERE v.image_id = ?"
                          " ORDER BY v.position");
    vs.bind(1, id);
    Json variants = Json::array();
    while (vs.step()) {
        Json vj;
        vj["uuid"] = vs.text(0);
        vj["position"] = vs.i64(1);
        vj["rating"] = vs.opt_i64(4) ? vs.i64(4) : vs.opt_i64(2).value_or(0);
        auto ct = vs.opt_i64(5) ? vs.opt_i64(5) : vs.opt_i64(3);
        vj["color_tag"] = ct && *ct != 0 ? Json(*ct) : Json(nullptr);
        vj["local"] = {{"rating", opt(vs.opt_i64(4))}, {"color_tag", opt(vs.opt_i64(5))}};
        vj["origin"] = vs.text(11);
        if (vs.text(11) == kOriginC1) {
            vj["capture_one"] = {{"rating", opt(vs.opt_i64(2))}, {"color_tag", opt(vs.opt_i64(3))},
                                 {"metadata", vs.is_null(6) ? Json::object() : Json::parse(vs.text(6))},
                                 {"geometry", vs.is_null(7) ? Json::object() : Json::parse(vs.text(7))},
                                 {"adjusted", vs.opt_i64(8).value_or(0) != 0}};
        }
        vj["edit_revision"] = opt(vs.opt_i64(9));
        if (!vs.is_null(10)) vj["edit_modified_at"] = iso(vs.f64(10), true);
        if (vs.text(12) != "present") vj["origin_state"] = vs.text(12);
        variants.push_back(std::move(vj));
    }
    one["variants"] = variants;
    Json albums = Json::array();
    auto as = db_.prepare("SELECT c.uuid, c.name, c.origin FROM collection_image ci JOIN collection c ON c.id = ci.collection_id"
                          " WHERE ci.image_id = ? AND c.origin_state = 'present' ORDER BY c.name");
    as.bind(1, id);
    while (as.step()) albums.push_back({{"uuid", as.text(0)}, {"name", as.text(1)}, {"origin", as.text(2)}});
    one["albums"] = albums;
    return one;
}

Json Library::lookup(const Json& params) {
    if (!locations_) build_location_cache();
    auto s = db_.prepare("SELECT i.uuid, i.origin_state, (SELECT uuid FROM variant WHERE image_id = i.id AND origin_state = 'present'"
                         " ORDER BY position LIMIT 1) FROM image i WHERE i.location_id = ? AND i.file_name = ?");
    auto by_name = db_.prepare("SELECT i.uuid, i.origin_state, (SELECT uuid FROM variant WHERE image_id = i.id AND origin_state = 'present'"
                               " ORDER BY position LIMIT 1), i.file_name FROM image i WHERE i.location_id = ?");
    Json out = Json::array();
    for (auto& path : string_list(params, "paths")) {
        fs::path p = fs::path(path).lexically_normal();
        std::string dir = nfc(p.parent_path().string()), name = p.filename().string();
        Json hit = nullptr;
        auto it = locations_->by_dir.find(dir);
        if (it != locations_->by_dir.end()) {
            for (int64_t loc : it->second) {
                s.bind(1, loc).bind(2, name);
                if (s.step()) {
                    hit = {{"image", s.text(0)}, {"variant", opt(s.opt_text(2))}, {"origin_state", s.text(1)}};
                }
                s.reset();
                if (!hit.is_null()) break;
                // The same name in another normalisation form.
                std::string want = nfc(name);
                by_name.bind(1, loc);
                while (by_name.step()) {
                    if (nfc(by_name.text(3)) == want) {
                        hit = {{"image", by_name.text(0)}, {"variant", opt(by_name.opt_text(2))}, {"origin_state", by_name.text(1)}};
                        break;
                    }
                }
                by_name.reset();
                if (!hit.is_null()) break;
            }
        }
        out.push_back({{"path", path}, {"found", !hit.is_null()}, {"match", hit}});
    }
    return {{"results", out}};
}

Json Library::locations(const Json&) {
    if (!locations_) build_location_cache();
    Json out = Json::array();
    auto s = db_.prepare("SELECT l.id, l.uuid, l.base, l.root, l.path, l.volume, l.relinked_dir, l.origin, l.origin_state,"
                         " (SELECT count(*) FROM image i WHERE i.location_id = l.id AND i.origin_state = 'present')"
                         " FROM path_location l ORDER BY l.id");
    while (s.step()) {
        std::string dir = resolve_location(s.i64(0));
        std::error_code ec;
        Json j{{"uuid", s.text(1)}, {"base", s.text(2)}, {"path", dir}, {"volume", opt(s.opt_text(5))},
               {"relinked", !s.is_null(6)}, {"origin", s.text(7)}, {"image_count", s.i64(9)},
               {"online", fs::is_directory(dir, ec)}};
        if (s.text(8) != "present") j["origin_state"] = s.text(8);
        out.push_back(std::move(j));
    }
    return {{"locations", out}};
}

// --- writes SpektraLab owns ------------------------------------------------------------------

Json Library::edit_get(const Json& params) {
    int64_t vid = variant_id(need_string(params, "variant"));
    auto s = db_.prepare("SELECT json, revision, modified_at FROM edit WHERE variant_id = ?");
    s.bind(1, vid);
    if (!s.step()) return {{"variant", params["variant"]}, {"revision", 0}, {"edit", nullptr}};
    return {{"variant", params["variant"]}, {"revision", s.i64(1)}, {"modified_at", iso(s.f64(2), true)},
            {"edit", Json::parse(s.text(0))}};
}

Json Library::edit_put(const Json& params) {
    int64_t vid = variant_id(need_string(params, "variant"));
    if (!params.contains("edit") || !params["edit"].is_object()) throw ApiError("invalid_params", "'edit' must be a JSON object");
    if (!params.contains("base_revision") || !params["base_revision"].is_number_integer())
        throw ApiError("invalid_params", "'base_revision' is required: the revision this edit was made on (0 for none)");
    int64_t base = params["base_revision"].get<int64_t>();
    Transaction t(db_);
    int64_t current = 0;
    {
        auto s = db_.prepare("SELECT revision FROM edit WHERE variant_id = ?");
        s.bind(1, vid);
        if (s.step()) current = s.i64(0);
    }
    if (base != current)
        throw ApiError("conflict", "the edit is at revision " + std::to_string(current) + ", not " + std::to_string(base),
                       {{"revision", current}});
    // nlohmann's dump of the parsed object -- the text the app sent, minus
    // whitespace. Key order is the app's.
    std::string text = params["edit"].dump();
    auto s = db_.prepare("INSERT INTO edit(variant_id, json, revision, modified_at) VALUES (?1, ?2, ?3, ?4)"
                         " ON CONFLICT(variant_id) DO UPDATE SET json = ?2, revision = ?3, modified_at = ?4");
    double now = now_unix();
    s.bind(1, vid).bind(2, text).bind(3, current + 1).bind(4, now);
    s.run();
    t.commit();
    return {{"variant", params["variant"]}, {"revision", current + 1}, {"modified_at", iso(now, true)}};
}

Json Library::edit_delete(const Json& params) {
    int64_t vid = variant_id(need_string(params, "variant"));
    auto s = db_.prepare("DELETE FROM edit WHERE variant_id = ?");
    s.bind(1, vid);
    s.run();
    return {{"variant", params["variant"]}, {"deleted", db_.changes() > 0}};
}

Json Library::set_meta(const Json& params) {
    std::vector<int64_t> targets;
    for (auto& u : string_list(params, "variants")) targets.push_back(variant_id(u));
    for (auto& u : string_list(params, "images")) {
        int64_t iid = image_id(u);
        auto s = db_.prepare("SELECT id FROM variant WHERE image_id = ? AND origin_state = 'present' ORDER BY position LIMIT 1");
        s.bind(1, iid);
        if (!s.step()) throw ApiError("not_found", "image " + u + " has no variant");
        targets.push_back(s.i64(0));
    }
    if (targets.empty()) throw ApiError("invalid_params", "give 'variants' or 'images'");
    bool has_rating = params.contains("rating"), has_color = params.contains("color_tag");
    if (!has_rating && !has_color) throw ApiError("invalid_params", "give 'rating' and/or 'color_tag' (null clears SpektraLab's value)");
    auto check = [&](const char* key, int lo, int hi) -> std::optional<int64_t> {
        const Json& x = params[key];
        if (x.is_null()) return std::nullopt;
        if (!x.is_number_integer() || x.get<int64_t>() < lo || x.get<int64_t>() > hi)
            throw ApiError("invalid_params", std::string(key) + " must be " + std::to_string(lo) + "..." + std::to_string(hi) + " or null");
        return x.get<int64_t>();
    };
    std::optional<int64_t> rating, color;
    if (has_rating) rating = check("rating", 0, 5);
    if (has_color) color = check("color_tag", 0, 7);
    Transaction t(db_);
    auto up_r = db_.prepare("UPDATE variant SET rating = ? WHERE id = ?");
    auto up_c = db_.prepare("UPDATE variant SET color_tag = ? WHERE id = ?");
    for (int64_t id : targets) {
        if (has_rating) { up_r.bind(1, rating).bind(2, id); up_r.run(); }
        if (has_color) { up_c.bind(1, color).bind(2, id); up_c.run(); }
    }
    t.commit();
    return {{"updated", targets.size()}};
}

Json Library::set_trashed(const Json& params) {
    auto uuids = string_list(params, "images");
    if (uuids.empty()) throw ApiError("invalid_params", "give 'images'");
    if (!params.contains("trashed") || !(params["trashed"].is_boolean() || params["trashed"].is_null()))
        throw ApiError("invalid_params", "'trashed' must be true, false or null (null follows Capture One)");
    Transaction t(db_);
    auto s = db_.prepare("UPDATE image SET trashed = ? WHERE id = ?");
    for (auto& u : uuids) {
        int64_t id = image_id(u);
        if (params["trashed"].is_null()) s.bind(1, nullptr);
        else s.bind(1, int64_t(params["trashed"].get<bool>()));
        s.bind(2, id);
        s.run();
    }
    t.commit();
    return {{"updated", uuids.size()}, {"files_moved", false}};
}

Json Library::variant_create(const Json& params) {
    int64_t iid = image_id(need_string(params, "image"));
    Transaction t(db_);
    int64_t pos;
    {
        auto s = db_.prepare("SELECT COALESCE(MAX(position), 0) + 1 FROM variant WHERE image_id = ?");
        s.bind(1, iid);
        s.step();
        pos = s.i64(0);
    }
    std::string uuid = new_uuid();
    auto ins = db_.prepare("INSERT INTO variant(uuid, image_id, position, created_at) VALUES (?, ?, ?, ?)");
    ins.bind(1, uuid).bind(2, iid).bind(3, pos).bind(4, now_unix());
    ins.run();
    int64_t vid = db_.last_insert_id();
    bool copied = false;
    if (params.contains("from_variant") && params["from_variant"].is_string()) {
        int64_t from = variant_id(params["from_variant"].get<std::string>());
        auto cp = db_.prepare("INSERT INTO edit(variant_id, json, revision, modified_at) SELECT ?, json, 1, ? FROM edit WHERE variant_id = ?");
        cp.bind(1, vid).bind(2, now_unix()).bind(3, from);
        cp.run();
        copied = db_.changes() > 0;
    }
    t.commit();
    return {{"variant", uuid}, {"position", pos}, {"edit_copied", copied}};
}

int64_t Library::ensure_location(const std::string& dir_in) {
    std::string dir = fs::path(dir_in).lexically_normal().string();
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (!locations_) build_location_cache();
    auto it = locations_->by_dir.find(nfc(dir));
    if (it != locations_->by_dir.end()) {
        // Prefer SpektraLab's own row: a local add must not hang off a
        // location a sync can mark removed.
        for (int64_t id : it->second) {
            auto s = db_.prepare("SELECT origin FROM path_location WHERE id = ?");
            s.bind(1, id);
            if (s.step() && s.text(0) == "local") return id;
        }
    }
    // Inside the library (a session's folders, a catalog's Originals): stored
    // relative, so the library can move as one folder.
    std::string base = "absolute", root = "/", rel = dir;
    if (dir == path_ || is_prefix_dir(path_, dir)) {
        base = "library";
        root = "";
        rel = dir == path_ ? "" : dir.substr(path_.size() + 1);
    } else {
        rel = dir.size() > 1 ? dir.substr(1) : "";
    }
    auto ins = db_.prepare("INSERT INTO path_location(uuid, base, root, path) VALUES (?, ?, ?, ?)");
    ins.bind(1, new_uuid()).bind(2, base).bind(3, root).bind(4, rel);
    ins.run();
    int64_t id = db_.last_insert_id();
    locations_->dir[id] = dir;
    locations_->by_dir[nfc(dir)].push_back(id);
    return id;
}

int64_t Library::ensure_folder(int64_t location_id) {
    {
        auto s = db_.prepare("SELECT id FROM collection WHERE kind = 'folder' AND folder_location_id = ? AND origin = 'local'");
        s.bind(1, location_id);
        if (s.step()) return s.i64(0);
    }
    std::string mine = resolve_location(location_id);
    std::optional<int64_t> parent;
    size_t best = 0;
    auto s = db_.prepare("SELECT id, folder_location_id FROM collection WHERE kind = 'folder' AND origin = 'local'");
    while (s.step()) {
        std::string d = resolve_location(s.i64(1));
        if (is_prefix_dir(d, mine) && d.size() >= best) { best = d.size(); parent = s.i64(0); }
    }
    auto ins = db_.prepare("INSERT INTO collection(uuid, parent_id, kind, name, folder_location_id, created_at) VALUES (?, ?, 'folder', ?, ?, ?)");
    ins.bind(1, new_uuid()).bind(2, parent).bind(3, fs::path(mine).filename().string()).bind(4, location_id).bind(5, now_unix());
    ins.run();
    int64_t id = db_.last_insert_id();
    // Re-hang existing folders that live below the new one.
    auto kids = db_.prepare("SELECT id, folder_location_id FROM collection WHERE kind = 'folder' AND origin = 'local' AND id != ?");
    kids.bind(1, id);
    std::vector<int64_t> move;
    while (kids.step()) {
        std::string d = resolve_location(kids.i64(1));
        if (is_prefix_dir(mine, d)) {
            auto pp = db_.prepare("SELECT parent_id FROM collection WHERE id = ?");
            pp.bind(1, kids.i64(0));
            pp.step();
            std::optional<int64_t> cur = pp.opt_i64(0);
            std::string cur_dir;
            if (cur) {
                auto cd = db_.prepare("SELECT folder_location_id FROM collection WHERE id = ?");
                cd.bind(1, *cur);
                if (cd.step()) cur_dir = resolve_location(cd.i64(0));
            }
            if (!cur || cur_dir.size() < mine.size()) move.push_back(kids.i64(0));
        }
    }
    auto mv = db_.prepare("UPDATE collection SET parent_id = ? WHERE id = ?");
    for (int64_t k : move) { mv.bind(1, id).bind(2, k); mv.run(); }
    return id;
}

Json Library::add_images(const Json& params) {
    if (!params.contains("items") || !params["items"].is_array()) throw ApiError("invalid_params", "'items' must be an array");
    std::string mode = params.value("mode", std::string("reference"));
    if (mode != "reference" && mode != "copy") throw ApiError("invalid_params", "mode must be 'reference' or 'copy'");
    std::optional<int64_t> album;
    if (params.contains("collection") && params["collection"].is_string()) {
        std::string kind, origin;
        album = collection_id(params["collection"].get<std::string>(), &kind, &origin);
        if (origin != "local") throw ApiError("read_only", "Capture One's albums are read-only");
        if (kind != "album") throw ApiError("invalid_params", "images are added to an album");
    }
    std::string dest_rel = params.value("destination", std::string(kind_ == "session" ? "Capture" : ""));
    Json results = Json::array();
    int64_t added = 0;
    Transaction t(db_);
    const double now = now_unix();
    for (auto& item : params["items"]) {
        std::string path = item.is_string() ? item.get<std::string>() : item.value("path", std::string());
        Json r{{"path", path}};
        std::error_code ec;
        fs::path src = fs::absolute(fs::path(path)).lexically_normal();
        if (path.empty() || !fs::is_regular_file(src, ec)) {
            r["error"] = {{"code", "not_found"}, {"message", "not a file"}};
            results.push_back(r);
            continue;
        }
        fs::path target = src;
        if (mode == "copy") {
            // Into the library: a catalog's Originals/<yyyy>/<mm>/<dd>, a
            // session's Capture (or the given destination). Never overwrites.
            fs::path dir;
            if (!dest_rel.empty()) {
                dir = fs::path(path_) / dest_rel;
            } else {
                time_t secs = time_t(now);
                std::tm tm{};
                localtime_r(&secs, &tm);
                char buf[16];
                std::strftime(buf, sizeof buf, "%Y/%m/%d", &tm);
                dir = fs::path(path_) / "Originals" / buf;
            }
            fs::create_directories(dir);
            target = dir / src.filename();
            for (int n = 1; fs::exists(target, ec); ++n)
                target = dir / (src.stem().string() + "_" + std::to_string(n) + src.extension().string());
            fs::copy_file(src, target, ec);
            if (ec) {
                r["error"] = {{"code", "io"}, {"message", ec.message()}};
                results.push_back(r);
                continue;
            }
        }
        Json found = lookup({{"paths", Json::array({target.string()})}})["results"][0];
        if (found["found"].get<bool>() && found["match"]["origin_state"] == "present") {
            r["image"] = found["match"]["image"];
            r["variant"] = found["match"]["variant"];
            r["existing"] = true;
            results.push_back(r);
            continue;
        }
        int64_t loc = ensure_location(target.parent_path().string());
        ensure_folder(loc);
        const Json meta_in = item.is_object() && item.contains("metadata") ? item["metadata"] : Json::object();
        std::string uuid = new_uuid(), vuuid = new_uuid();
        auto ins = db_.prepare(
            "INSERT INTO image(uuid, location_id, file_name, display_name, kind, inside_library, width, height, file_size,"
            " captured_local, imported_at, camera_make, camera_model, lens, iso, aperture, shutter, focal_length,"
            " exposure_comp, added_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        auto s_or_null = [&](const char* k) -> std::optional<std::string> {
            return meta_in.contains(k) && meta_in[k].is_string() ? std::optional(meta_in[k].get<std::string>()) : std::nullopt;
        };
        auto i_or_null = [&](const char* k) -> std::optional<int64_t> {
            return meta_in.contains(k) && meta_in[k].is_number() ? std::optional(meta_in[k].get<int64_t>()) : std::nullopt;
        };
        auto f_or_null = [&](const char* k) -> std::optional<double> {
            return meta_in.contains(k) && meta_in[k].is_number() ? std::optional(meta_in[k].get<double>()) : std::nullopt;
        };
        std::optional<double> captured;
        if (auto c = s_or_null("captured_local")) captured = parse_local(*c);
        ins.bind(1, uuid).bind(2, loc).bind(3, target.filename().string()).bind(4, target.stem().string())
            .bind(5, kind_for_extension(target.filename().string()))
            .bind(6, int64_t(mode == "copy" || is_prefix_dir(path_, target.string())))
            .bind(7, i_or_null("width")).bind(8, i_or_null("height"))
            .bind(9, int64_t(fs::file_size(target, ec))).bind(10, captured).bind(11, now)
            .bind(12, s_or_null("camera_make")).bind(13, s_or_null("camera_model")).bind(14, s_or_null("lens"))
            .bind(15, i_or_null("iso")).bind(16, f_or_null("aperture")).bind(17, f_or_null("shutter"))
            .bind(18, f_or_null("focal_length")).bind(19, s_or_null("exposure_comp")).bind(20, now);
        ins.run();
        int64_t iid = db_.last_insert_id();
        auto vi = db_.prepare("INSERT INTO variant(uuid, image_id, position, created_at) VALUES (?, ?, 1, ?)");
        vi.bind(1, vuuid).bind(2, iid).bind(3, now);
        vi.run();
        if (album) {
            auto ai = db_.prepare("INSERT OR IGNORE INTO collection_image(collection_id, image_id, position)"
                                  " VALUES (?, ?, (SELECT COALESCE(MAX(position), 0) + 1 FROM collection_image WHERE collection_id = ?))");
            ai.bind(1, *album).bind(2, iid).bind(3, *album);
            ai.run();
        }
        r["image"] = uuid;
        r["variant"] = vuuid;
        r["existing"] = false;
        if (mode == "copy") r["stored_at"] = target.string();
        ++added;
        results.push_back(r);
    }
    t.commit();
    return {{"added", added}, {"results", results}};
}

Json Library::collection_create(const Json& params) {
    std::string kind = need_string(params, "kind");
    if (kind != "album" && kind != "group" && kind != "project" && kind != "smart_album")
        throw ApiError("invalid_params", "kind must be album, group, project or smart_album");
    std::string name = need_string(params, "name");
    std::optional<int64_t> parent;
    if (params.contains("parent") && params["parent"].is_string()) {
        std::string pk, po;
        parent = collection_id(params["parent"].get<std::string>(), &pk, &po);
        if (po != "local") throw ApiError("read_only", "Capture One's collections are read-only; put SpektraLab's beside them");
        if (pk != "project" && pk != "group") throw ApiError("invalid_params", "a collection's parent is a project or a group");
    }
    std::optional<std::string> criteria;
    if (kind == "smart_album") {
        if (!params.contains("criteria") || !params["criteria"].is_object())
            throw ApiError("invalid_params", "a smart album needs 'criteria' (the images filter)");
        Where w;
        apply_filter(w, params["criteria"]);  // validates
        criteria = params["criteria"].dump();
    }
    std::string uuid = new_uuid();
    auto s = db_.prepare("INSERT INTO collection(uuid, parent_id, kind, name, position, criteria, criteria_format, created_at)"
                         " VALUES (?, ?, ?, ?, (SELECT COALESCE(MAX(position), 0) + 1 FROM collection WHERE parent_id IS ?), ?, ?, ?)");
    s.bind(1, uuid).bind(2, parent).bind(3, kind).bind(4, name).bind(5, parent).bind(6, criteria)
        .bind(7, criteria ? std::optional<std::string>("spklib") : std::nullopt).bind(8, now_unix());
    s.run();
    return {{"collection", uuid}};
}

Json Library::collection_update(const Json& params) {
    std::string kind, origin;
    int64_t id = collection_id(need_string(params, "collection"), &kind, &origin);
    if (origin != "local") throw ApiError("read_only", "Capture One's collections are read-only");
    Transaction t(db_);
    if (params.contains("name")) {
        auto s = db_.prepare("UPDATE collection SET name = ? WHERE id = ?");
        s.bind(1, need_string(params, "name")).bind(2, id);
        s.run();
    }
    if (params.contains("criteria")) {
        if (kind != "smart_album") throw ApiError("invalid_params", "only a smart album has criteria");
        Where w;
        apply_filter(w, params["criteria"]);
        auto s = db_.prepare("UPDATE collection SET criteria = ? WHERE id = ?");
        s.bind(1, params["criteria"].dump()).bind(2, id);
        s.run();
    }
    if (params.contains("parent")) {
        std::optional<int64_t> parent;
        if (params["parent"].is_string()) {
            std::string pk, po;
            parent = collection_id(params["parent"].get<std::string>(), &pk, &po);
            if (po != "local") throw ApiError("read_only", "Capture One's collections are read-only");
            if (pk != "project" && pk != "group") throw ApiError("invalid_params", "a collection's parent is a project or a group");
            for (std::optional<int64_t> a = parent; a;) {
                if (*a == id) throw ApiError("invalid_params", "a collection cannot be inside itself");
                auto s = db_.prepare("SELECT parent_id FROM collection WHERE id = ?");
                s.bind(1, *a);
                a = s.step() ? s.opt_i64(0) : std::nullopt;
            }
        }
        auto s = db_.prepare("UPDATE collection SET parent_id = ? WHERE id = ?");
        s.bind(1, parent).bind(2, id);
        s.run();
    }
    if (params.contains("position")) {
        auto s = db_.prepare("UPDATE collection SET position = ? WHERE id = ?");
        s.bind(1, params["position"].get<int64_t>()).bind(2, id);
        s.run();
    }
    t.commit();
    return {{"collection", params["collection"]}};
}

Json Library::collection_delete(const Json& params) {
    std::string kind, origin;
    int64_t id = collection_id(need_string(params, "collection"), &kind, &origin);
    if (origin != "local") throw ApiError("read_only", "Capture One's collections are read-only");
    if (kind == "folder") throw ApiError("invalid_params", "a folder is where images are; it goes when they do");
    Transaction t(db_);
    std::vector<int64_t> all{id};
    for (size_t k = 0; k < all.size(); ++k) {
        auto s = db_.prepare("SELECT id FROM collection WHERE parent_id = ?");
        s.bind(1, all[k]);
        while (s.step()) all.push_back(s.i64(0));
    }
    if (all.size() > 1 && !params.value("recursive", false))
        throw ApiError("invalid_params", "the collection holds others; pass recursive: true");
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        auto d = db_.prepare("DELETE FROM collection WHERE id = ?");
        d.bind(1, *it);
        d.run();
    }
    t.commit();
    return {{"deleted", all.size()}, {"images_deleted", 0}};
}

Json Library::collection_add(const Json& params) {
    std::string kind, origin;
    int64_t id = collection_id(need_string(params, "collection"), &kind, &origin);
    if (origin != "local") throw ApiError("read_only", "Capture One's albums are read-only");
    if (kind != "album") throw ApiError("invalid_params", "images are added to an album");
    Transaction t(db_);
    auto s = db_.prepare("INSERT OR IGNORE INTO collection_image(collection_id, image_id, position)"
                         " VALUES (?1, ?2, (SELECT COALESCE(MAX(position), 0) + 1 FROM collection_image WHERE collection_id = ?1))");
    int64_t n = 0;
    for (auto& u : string_list(params, "images")) {
        s.bind(1, id).bind(2, image_id(u));
        s.run();
        n += db_.changes();
    }
    t.commit();
    return {{"added", n}};
}

Json Library::collection_remove(const Json& params) {
    std::string kind, origin;
    int64_t id = collection_id(need_string(params, "collection"), &kind, &origin);
    if (origin != "local") throw ApiError("read_only", "Capture One's albums are read-only");
    Transaction t(db_);
    auto s = db_.prepare("DELETE FROM collection_image WHERE collection_id = ? AND image_id = ?");
    int64_t n = 0;
    for (auto& u : string_list(params, "images")) {
        s.bind(1, id).bind(2, image_id(u));
        s.run();
        n += db_.changes();
    }
    t.commit();
    return {{"removed", n}};
}

Json Library::relink_location(const Json& params) {
    std::string uuid = need_string(params, "location");
    auto s = db_.prepare("SELECT id, base FROM path_location WHERE uuid = ?");
    s.bind(1, uuid);
    if (!s.step()) throw ApiError("not_found", "no location " + uuid);
    if (s.text(1) != "absolute") throw ApiError("invalid_params", "only an absolute location can be relinked; a relative one moves with its library");
    const int64_t id = s.i64(0);
    if (!locations_) build_location_cache();
    Transaction t(db_);
    auto set = db_.prepare("UPDATE path_location SET relinked_dir = ? WHERE id = ?");
    Json changed = Json::array();
    if (params.contains("path") && params["path"].is_null()) {
        set.bind(1, nullptr).bind(2, id);
        set.run();
        changed.push_back(uuid);
    } else {
        // "Locate this folder": the user shows where one folder went, and the
        // part of its path that changed is changed for every location under
        // the same old prefix -- a renamed volume is found in one step. Only
        // SpektraLab's own column moves; Capture One's root and path stay.
        std::string old_dir = resolve_location(id);
        std::string new_dir = fs::path(need_string(params, "path")).lexically_normal().string();
        while (new_dir.size() > 1 && new_dir.back() == '/') new_dir.pop_back();
        std::error_code ec;
        if (!fs::is_directory(new_dir, ec)) throw ApiError("not_found", "no folder at " + new_dir);
        std::string old_prefix = old_dir, new_prefix = new_dir;
        while (true) {
            fs::path a(old_prefix), b(new_prefix);
            if (a.filename() != b.filename() || a.filename().empty() || !a.has_parent_path() || !b.has_parent_path() ||
                a.parent_path() == a || b.parent_path() == b)
                break;
            old_prefix = a.parent_path().string();
            new_prefix = b.parent_path().string();
        }
        bool all = params.value("all_matching", true);
        auto q = db_.prepare("SELECT id, uuid FROM path_location WHERE base = 'absolute'");
        while (q.step()) {
            std::string d = resolve_location(q.i64(0));
            if (q.i64(0) != id && !(all && (d == old_prefix || is_prefix_dir(old_prefix, d)))) continue;
            std::string moved = new_prefix + d.substr(old_prefix.size());
            set.bind(1, moved).bind(2, q.i64(0));
            set.run();
            changed.push_back(q.text(1));
        }
    }
    t.commit();
    locations_.reset();
    std::error_code ec;
    std::string dir = resolve_location(id);
    return {{"location", uuid}, {"path", dir}, {"online", fs::is_directory(dir, ec)}, {"relinked", changed}};
}

// --- a SpektraLab session's folders ------------------------------------------------------------

void Library::require_session() {
    if (kind_ != "session") throw ApiError("invalid_params", "this is a catalog, not a session");
    // A session mirrored from Capture One would be Capture One's folder:
    // moving its files is write-back, and write-back is not in v2.0.0.
    if (meta("origin_kind", "none") != "none") throw ApiError("read_only", "a Capture One session's files are Capture One's");
}

std::optional<int64_t> Library::role_location(const std::string& role) {
    auto s = db_.prepare("SELECT location_id FROM session_folder WHERE role = ?");
    s.bind(1, role);
    if (s.step()) return s.i64(0);
    return std::nullopt;
}

void Library::reconcile_intents() {
    // A move is written as an intent, then the rename, then the row. A crash
    // between the rename and the row leaves an intent whose file is already
    // at `to`: finish it. One whose file is still at `from` never happened.
    std::vector<std::tuple<int64_t, int64_t, std::string, std::string, int64_t>> pending;
    {
        auto s = db_.prepare("SELECT id, image_id, from_path, to_path, to_location_id FROM fs_intent");
        while (s.step()) pending.emplace_back(s.i64(0), s.i64(1), s.text(2), s.text(3), s.i64(4));
    }
    if (pending.empty()) return;
    Transaction t(db_);
    for (auto& [id, image, from, to, loc] : pending) {
        std::error_code ec;
        if (fs::exists(to, ec) && !fs::exists(from, ec)) {
            auto u = db_.prepare("UPDATE image SET previous_location_id = location_id, location_id = ?, file_name = ? WHERE id = ?");
            u.bind(1, loc).bind(2, fs::path(to).filename().string()).bind(3, image);
            u.run();
        }
        auto d = db_.prepare("DELETE FROM fs_intent WHERE id = ?");
        d.bind(1, id);
        d.run();
    }
    t.commit();
}

Json Library::session_folders(const Json&) {
    if (kind_ != "session") throw ApiError("invalid_params", "this is a catalog, not a session");
    if (!locations_) build_location_cache();
    std::map<int64_t, std::string> roles;
    {
        auto s = db_.prepare("SELECT role, location_id FROM session_folder");
        while (s.step()) roles[s.i64(1)] = s.text(0);
    }
    Json out = Json::array();
    auto s = db_.prepare("SELECT l.id, l.uuid, l.base, (SELECT count(*) FROM image i WHERE i.location_id = l.id AND i.origin_state = 'present')"
                         " FROM path_location l ORDER BY l.id");
    while (s.step()) {
        std::string dir = resolve_location(s.i64(0));
        std::error_code ec;
        Json j{{"location", s.text(1)}, {"path", dir},
               {"relative_path", dir == path_ ? std::string() : is_prefix_dir(path_, dir) ? dir.substr(path_.size() + 1) : std::string()},
               {"role", roles.count(s.i64(0)) ? Json(roles[s.i64(0)]) : Json(nullptr)},
               {"outside_session", s.text(2) != "library"}, {"exists", fs::is_directory(dir, ec)}, {"image_count", s.i64(3)}};
        out.push_back(std::move(j));
    }
    return {{"root", path_}, {"folders", out}};
}

Json Library::session_scan(const Json& params) {
    // The files pass: what Finder, a tethered camera or a card copy put into
    // the session's folders since the last look. Adds by reference -- the
    // files are already in the session -- and reports what has gone missing
    // without marking anything (the app decides; an unplugged disk is not a
    // deletion).
    require_session();
    static const std::set<std::string> skip_ext = {"spksession", "json", "xmp", "cos", "txt"};
    std::vector<std::string> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(path_, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& p = it->path();
        std::string name = p.filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (name.find(".spksession") != std::string::npos) continue;
        std::string kind = kind_for_extension(name);
        if (kind == "other" || skip_ext.count(lower_ext(name))) continue;
        found.push_back(p.string());
    }
    std::sort(found.begin(), found.end());
    Json items = Json::array();
    for (auto& f : found) items.push_back(f);
    Json added = add_images({{"items", items}, {"mode", "reference"}});
    int64_t missing = 0;
    {
        if (!locations_) build_location_cache();
        auto s = db_.prepare("SELECT location_id, file_name FROM image WHERE origin_state = 'present'");
        while (s.step()) {
            struct stat st {};
            if (::stat(join_path(resolve_location(s.i64(0)), s.text(1)).c_str(), &st) != 0) ++missing;
        }
    }
    (void)params;
    return {{"seen", found.size()}, {"added", added["added"]}, {"missing", missing}};
}

Json Library::move_image(int64_t image_id, int64_t to_location, const std::string& op, bool suffix) {
    std::string from, name;
    int64_t from_loc;
    {
        auto s = db_.prepare("SELECT location_id, file_name, uuid FROM image WHERE id = ?");
        s.bind(1, image_id);
        if (!s.step()) throw ApiError("not_found", "no image");
        from_loc = s.i64(0);
        name = s.text(1);
        from = join_path(resolve_location(from_loc), name);
    }
    if (from_loc == to_location) return {{"from", from}, {"to", from}, {"moved", false}};
    std::string dir = resolve_location(to_location);
    if (!is_prefix_dir(path_, from) || !is_prefix_dir(path_, dir))
        throw ApiError("invalid_params", "only files inside the session folder are moved");
    fs::path target = fs::path(dir) / name;
    std::error_code ec;
    for (int n = 1; suffix && fs::exists(target, ec); ++n)
        target = fs::path(dir) / (fs::path(name).stem().string() + " (" + std::to_string(n) + ")" + fs::path(name).extension().string());
    if (fs::exists(target, ec)) return {{"from", from}, {"to", target.string()}, {"moved", false}, {"conflict", true}};
    fs::create_directories(dir, ec);
    int64_t intent;
    {
        auto s = db_.prepare("INSERT INTO fs_intent(op, image_id, from_path, to_path, to_location_id, at) VALUES (?, ?, ?, ?, ?, ?)");
        s.bind(1, op).bind(2, image_id).bind(3, from).bind(4, target.string()).bind(5, to_location).bind(6, now_unix());
        s.run();
        intent = db_.last_insert_id();
    }
#ifdef __APPLE__
    // RENAME_EXCL: never replace a file that appeared since the check above.
    int rc = ::renamex_np(from.c_str(), target.c_str(), RENAME_EXCL);
#else
    int rc = ::rename(from.c_str(), target.c_str());
#endif
    if (rc != 0) {
        int e = errno;
        auto d = db_.prepare("DELETE FROM fs_intent WHERE id = ?");
        d.bind(1, intent);
        d.run();
        if (e == EXDEV) throw ApiError("cross_volume", "the destination is on another disk");
        if (e == EEXIST) return {{"from", from}, {"to", target.string()}, {"moved", false}, {"conflict", true}};
        if (e == ENOENT) throw ApiError("not_found", "the file is not where the library says: " + from);
        throw ApiError("io", std::string("rename failed: ") + std::strerror(e));
    }
    auto u = db_.prepare("UPDATE image SET previous_location_id = location_id, location_id = ?, file_name = ? WHERE id = ?");
    u.bind(1, to_location).bind(2, target.filename().string()).bind(3, image_id);
    u.run();
    auto d = db_.prepare("DELETE FROM fs_intent WHERE id = ?");
    d.bind(1, intent);
    d.run();
    return {{"from", from}, {"to", target.string()}, {"moved", true}};
}

Json Library::session_move(const Json& params) {
    require_session();
    std::string to = need_string(params, "to");
    std::optional<int64_t> loc = role_location(to);
    if (!loc) {
        // Not a role: a folder inside the session, relative to its root.
        fs::path dir = fs::path(path_) / to;
        if (to.find("..") != std::string::npos) throw ApiError("invalid_params", "'to' must stay inside the session");
        loc = ensure_location(dir.string());
    }
    bool suffix = params.value("on_conflict", std::string("fail")) == "suffix";
    Json moved = Json::array(), conflicts = Json::array();
    for (auto& u : string_list(params, "images")) {
        int64_t id = image_id(u);
        Transaction t(db_);
        Json r = move_image(id, *loc, "move", suffix);
        t.commit();
        r["image"] = u;
        (r.value("conflict", false) ? conflicts : moved).push_back(r);
    }
    return {{"moved", moved}, {"conflicts", conflicts}};
}

Json Library::session_restore(const Json& params) {
    require_session();
    bool suffix = params.value("on_conflict", std::string("fail")) == "suffix";
    Json moved = Json::array(), conflicts = Json::array();
    for (auto& u : string_list(params, "images")) {
        int64_t id = image_id(u);
        auto s = db_.prepare("SELECT previous_location_id FROM image WHERE id = ?");
        s.bind(1, id);
        s.step();
        std::optional<int64_t> back = s.opt_i64(0);
        if (!back) back = role_location("capture");
        Transaction t(db_);
        Json r = move_image(id, *back, "restore", suffix);
        t.commit();
        r["image"] = u;
        (r.value("conflict", false) ? conflicts : moved).push_back(r);
    }
    return {{"moved", moved}, {"conflicts", conflicts}};
}

Json Library::session_trash_plan(const Json&) {
    // spklib never deletes a photograph. It names what is in the session's
    // Trash; the app moves those files to the macOS Trash
    // (FileManager.trashItem) and then commits the paths it actually moved.
    require_session();
    auto trash = role_location("trash");
    if (!trash) throw ApiError("not_found", "the session has no Trash folder");
    Json out = Json::array();
    auto s = db_.prepare("SELECT uuid, file_name FROM image WHERE location_id = ? AND origin_state = 'present' ORDER BY file_name");
    s.bind(1, *trash);
    std::string dir = resolve_location(*trash);
    while (s.step()) out.push_back({{"image", s.text(0)}, {"path", join_path(dir, s.text(1))}});
    return {{"files", out}};
}

Json Library::session_trash_commit(const Json& params) {
    require_session();
    auto trash = role_location("trash");
    if (!trash) throw ApiError("not_found", "the session has no Trash folder");
    std::string dir = resolve_location(*trash);
    int64_t dropped = 0;
    Json refused = Json::array();
    Transaction t(db_);
    for (auto& p : string_list(params, "removed_paths")) {
        fs::path fp(p);
        std::error_code ec;
        // Only a file that really left the session's Trash folder loses its row.
        if (nfc(fp.parent_path().string()) != nfc(dir) || fs::exists(fp, ec)) { refused.push_back(p); continue; }
        auto s = db_.prepare("SELECT id FROM image WHERE location_id = ? AND file_name = ?");
        s.bind(1, *trash).bind(2, fp.filename().string());
        if (!s.step()) { refused.push_back(p); continue; }
        int64_t id = s.i64(0);
        for (const char* sql : {"DELETE FROM edit WHERE variant_id IN (SELECT id FROM variant WHERE image_id = ?)",
                                "DELETE FROM variant WHERE image_id = ?", "DELETE FROM collection_image WHERE image_id = ?",
                                "DELETE FROM fs_intent WHERE image_id = ?", "DELETE FROM image WHERE id = ?"}) {
            auto d = db_.prepare(sql);
            d.bind(1, id);
            d.run();
        }
        ++dropped;
    }
    t.commit();
    return {{"dropped", dropped}, {"refused", refused}};
}

}  // namespace spklib
