#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace sotto::store {

// One SQLite connection, and the only place PRAGMAs are issued: every database
// this process opens gets the same page size, WAL journal and FULL sync
// (ADR-0017; FULL because NORMAL can roll back committed transactions on
// power loss). A connection serves one thread at a time.
class Db {
   public:
    // A prepared statement. Bind indices are 1-based, column indices 0-based,
    // following SQLite itself.
    class Stmt {
       public:
        Stmt(Stmt&& other) noexcept;
        Stmt& operator=(Stmt&& other) noexcept;
        ~Stmt();

        void BindInt64(int index, std::int64_t value);
        void BindText(int index, std::string_view value);
        void BindBlob(int index, std::span<const std::uint8_t> value);

        bool Step();  // true: a row is ready; false: done
        void Reset();

        std::int64_t ColumnInt64(int index) const;
        std::string ColumnText(int index) const;
        std::vector<std::uint8_t> ColumnBlob(int index) const;

       private:
        friend class Db;
        Stmt(sqlite3_stmt* stmt, sqlite3* db);

        sqlite3_stmt* stmt_;
        sqlite3* db_;
    };

    // Rolls back on destruction unless Commit() ran
    class Transaction {
       public:
        explicit Transaction(Db& db);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        void Commit();

       private:
        Db& db_;
        bool done_ = false;
    };

    explicit Db(const std::filesystem::path& path);
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;
    ~Db();

    void Exec(const char* sql);
    Stmt Prepare(const char* sql);
    std::int64_t QueryInt64(const char* sql);
    std::int64_t LastInsertRowId() const;

    // The schema version this file was created or last migrated at
    std::int64_t UserVersion();
    void SetUserVersion(std::int64_t version);

   private:
    sqlite3* db_ = nullptr;
};

}  // namespace sotto::store
