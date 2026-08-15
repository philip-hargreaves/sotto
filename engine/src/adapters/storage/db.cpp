#include "adapters/storage/db.hpp"

#include <stdexcept>
#include <utility>

#include "sqlite3.h"

namespace sotto::store {

namespace {

[[noreturn]] void Throw(const char* what, sqlite3* db) {
    throw std::runtime_error(std::string(what) + ": " +
                             (db != nullptr ? sqlite3_errmsg(db) : "out of memory"));
}

}  // namespace

Db::Db(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    if (sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &db_) != SQLITE_OK) {
        std::string message = "open " + path.string() + ": " +
                              (db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory");
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(message);
    }
    // page_size only takes effect if it runs before the first table is created
    Exec("PRAGMA page_size=8192");
    Exec("PRAGMA journal_mode=WAL");
    Exec("PRAGMA synchronous=FULL");
    Exec("PRAGMA foreign_keys=ON");
}

Db::~Db() {
    sqlite3_close(db_);
}

void Db::Exec(const char* sql) {
    if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) Throw(sql, db_);
}

Db::Stmt Db::Prepare(const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) Throw(sql, db_);
    return Stmt(stmt, db_);
}

std::int64_t Db::QueryInt64(const char* sql) {
    Stmt stmt = Prepare(sql);
    if (!stmt.Step()) throw std::runtime_error(std::string(sql) + ": no row");
    return stmt.ColumnInt64(0);
}

std::int64_t Db::LastInsertRowId() const {
    return sqlite3_last_insert_rowid(db_);
}

Db::Stmt::Stmt(sqlite3_stmt* stmt, sqlite3* db) : stmt_(stmt), db_(db) {}

Db::Stmt::Stmt(Stmt&& other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)), db_(other.db_) {}

Db::Stmt& Db::Stmt::operator=(Stmt&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        stmt_ = std::exchange(other.stmt_, nullptr);
        db_ = other.db_;
    }
    return *this;
}

Db::Stmt::~Stmt() {
    sqlite3_finalize(stmt_);
}

void Db::Stmt::BindInt64(int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) Throw("bind int", db_);
}

void Db::Stmt::BindText(int index, std::string_view value) {
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        Throw("bind text", db_);
    }
}

void Db::Stmt::BindBlob(int index, std::span<const std::uint8_t> value) {
    if (sqlite3_bind_blob64(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        Throw("bind blob", db_);
    }
}

bool Db::Stmt::Step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    Throw("step", db_);
}

void Db::Stmt::Reset() {
    if (sqlite3_reset(stmt_) != SQLITE_OK) Throw("reset", db_);
    sqlite3_clear_bindings(stmt_);
}

std::int64_t Db::Stmt::ColumnInt64(int index) const {
    return sqlite3_column_int64(stmt_, index);
}

std::string Db::Stmt::ColumnText(int index) const {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    const int size = sqlite3_column_bytes(stmt_, index);
    return text != nullptr ? std::string(text, static_cast<std::size_t>(size)) : std::string();
}

std::vector<std::uint8_t> Db::Stmt::ColumnBlob(int index) const {
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt_, index));
    const int size = sqlite3_column_bytes(stmt_, index);
    return data != nullptr ? std::vector<std::uint8_t>(data, data + size)
                           : std::vector<std::uint8_t>();
}

Db::Transaction::Transaction(Db& db) : db_(db) {
    db_.Exec("BEGIN IMMEDIATE");
}

Db::Transaction::~Transaction() {
    if (!done_) {
        try {
            db_.Exec("ROLLBACK");
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
}

void Db::Transaction::Commit() {
    db_.Exec("COMMIT");
    done_ = true;
}

}  // namespace sotto::store
