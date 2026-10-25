// sqlite.hpp -- a thin RAII layer over the SQLite C API. Nothing here knows
// about Capture One or SpektraLab; it exists so every statement is finalised
// and every error carries the SQL that raised it.

#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace spklib {

struct DbError : std::runtime_error {
    int code;
    DbError(int c, const std::string& what) : std::runtime_error(what), code(c) {}
};

class Stmt;

class Db {
public:
    enum class Mode {
        /// The user's file, never written: SQLITE_OPEN_READONLY plus
        /// `PRAGMA query_only`. Only ever pointed at a snapshot.
        ReadOnlyImmutable,
        ReadWrite,
        Create,
    };

    Db() = default;
    Db(const std::string& path, Mode mode);
    ~Db();
    Db(Db&& o) noexcept : db_(o.db_) { o.db_ = nullptr; }
    Db& operator=(Db&& o) noexcept;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void exec(const std::string& sql);
    Stmt prepare(const std::string& sql);
    int64_t last_insert_id() const { return sqlite3_last_insert_rowid(db_); }
    int changes() const { return sqlite3_changes(db_); }
    sqlite3* raw() const { return db_; }
    bool is_open() const { return db_ != nullptr; }

    bool has_table(const std::string& table);
    bool has_column(const std::string& table, const std::string& column);

private:
    sqlite3* db_ = nullptr;
};

/// BEGIN IMMEDIATE ... COMMIT, rolled back unless `commit()` is reached.
class Transaction {
public:
    explicit Transaction(Db& db);
    ~Transaction();
    void commit();

private:
    Db& db_;
    bool done_ = false;
};

class Stmt {
public:
    Stmt(sqlite3* db, sqlite3_stmt* s, std::string sql) : db_(db), s_(s), sql_(std::move(sql)) {}
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    Stmt(Stmt&& o) noexcept : db_(o.db_), s_(o.s_), sql_(std::move(o.sql_)) { o.s_ = nullptr; }
    Stmt(const Stmt&) = delete;

    Stmt& bind(int i, std::nullptr_t);
    Stmt& bind(int i, int64_t v);
    Stmt& bind(int i, int v) { return bind(i, static_cast<int64_t>(v)); }
    Stmt& bind(int i, double v);
    Stmt& bind(int i, std::string_view v);
    Stmt& bind(int i, const char* v) { return v ? bind(i, std::string_view(v)) : bind(i, nullptr); }
    Stmt& bind(int i, const std::string& v) { return bind(i, std::string_view(v)); }
    template <class T> Stmt& bind(int i, const std::optional<T>& v) {
        return v ? bind(i, *v) : bind(i, nullptr);
    }

    /// True while a row is available.
    bool step();
    /// Step a statement that returns no rows.
    void run();
    void reset();

    bool is_null(int col) const { return sqlite3_column_type(s_, col) == SQLITE_NULL; }
    /// SQLITE_INTEGER, SQLITE_FLOAT, SQLITE_TEXT, SQLITE_BLOB or SQLITE_NULL.
    int type(int col) const { return sqlite3_column_type(s_, col); }
    int64_t i64(int col) const { return sqlite3_column_int64(s_, col); }
    double f64(int col) const { return sqlite3_column_double(s_, col); }
    std::string text(int col) const;
    std::optional<int64_t> opt_i64(int col) const { return is_null(col) ? std::nullopt : std::optional(i64(col)); }
    std::optional<double> opt_f64(int col) const { return is_null(col) ? std::nullopt : std::optional(f64(col)); }
    std::optional<std::string> opt_text(int col) const { return is_null(col) ? std::nullopt : std::optional(text(col)); }

private:
    void check(int rc, const char* what);
    sqlite3* db_;
    sqlite3_stmt* s_;
    std::string sql_;
};

}  // namespace spklib
