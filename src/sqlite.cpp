#include "sqlite.hpp"

#include <cctype>

namespace spklib {

namespace {
std::string uri_escape(const std::string& path) {
    // RFC 3986 percent-encoding for a file: URI's path. SQLite needs '?', '#'
    // and '%' escaped; spaces and non-ASCII are escaped too so the URI is
    // unambiguous ("Capture One Catalog.cocatalogdb" has spaces).
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
}  // namespace

Db::Db(const std::string& path, Mode mode) {
    int flags = SQLITE_OPEN_FULLMUTEX;
    std::string target = path;
    switch (mode) {
    case Mode::ReadOnlyImmutable:
        // immutable=1: SQLite takes no locks and never looks for a journal,
        // so nothing -- not even a -shm or a lock byte -- touches the disk.
        // Only correct because the file is a snapshot nobody else writes.
        flags |= SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
        target = "file:" + uri_escape(path) + "?mode=ro&immutable=1";
        break;
    case Mode::ReadWrite: flags |= SQLITE_OPEN_READWRITE; break;
    case Mode::Create: flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE; break;
    }
    int rc = sqlite3_open_v2(target.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw DbError(rc, "cannot open " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    if (mode == Mode::ReadOnlyImmutable) exec("PRAGMA query_only = 1");
}

Db::~Db() {
    if (db_) sqlite3_close_v2(db_);
}

Db& Db::operator=(Db&& o) noexcept {
    if (this != &o) {
        if (db_) sqlite3_close_v2(db_);
        db_ = o.db_;
        o.db_ = nullptr;
    }
    return *this;
}

void Db::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        throw DbError(rc, msg + " in: " + sql.substr(0, 200));
    }
}

Stmt Db::prepare(const std::string& sql) {
    sqlite3_stmt* s = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &s, nullptr);
    if (rc != SQLITE_OK) throw DbError(rc, std::string(sqlite3_errmsg(db_)) + " in: " + sql.substr(0, 200));
    return Stmt(db_, s, sql);
}

bool Db::has_table(const std::string& table) {
    auto s = prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    s.bind(1, table);
    return s.step();
}

bool Db::has_column(const std::string& table, const std::string& column) {
    auto s = prepare("SELECT 1 FROM pragma_table_info(?) WHERE name=?");
    s.bind(1, table).bind(2, column);
    return s.step();
}

Transaction::Transaction(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
Transaction::~Transaction() {
    if (!done_) {
        try { db_.exec("ROLLBACK"); } catch (...) {}
    }
}
void Transaction::commit() {
    db_.exec("COMMIT");
    done_ = true;
}

void Stmt::check(int rc, const char* what) {
    if (rc != SQLITE_OK) throw DbError(rc, std::string(what) + ": " + sqlite3_errmsg(db_) + " in: " + sql_.substr(0, 200));
}

Stmt& Stmt::bind(int i, std::nullptr_t) { check(sqlite3_bind_null(s_, i), "bind"); return *this; }
Stmt& Stmt::bind(int i, int64_t v) { check(sqlite3_bind_int64(s_, i, v), "bind"); return *this; }
Stmt& Stmt::bind(int i, double v) { check(sqlite3_bind_double(s_, i, v), "bind"); return *this; }
Stmt& Stmt::bind(int i, std::string_view v) {
    check(sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT), "bind");
    return *this;
}

bool Stmt::step() {
    int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(rc, std::string("step: ") + sqlite3_errmsg(db_) + " in: " + sql_.substr(0, 200));
}

void Stmt::run() {
    while (step()) {}
    reset();
}

void Stmt::reset() {
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
}

std::string Stmt::text(int col) const {
    const unsigned char* p = sqlite3_column_text(s_, col);
    return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s_, col)) : std::string();
}

}  // namespace spklib
