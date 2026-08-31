#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace ambient::store {

class Db {
   public:
    class Stmt {
       public:
        Stmt(Stmt&& other) noexcept;
        Stmt& operator=(Stmt&& other) noexcept;
        ~Stmt();

        void BindInt64(int index, std::int64_t value);
        void BindText(int index, std::string_view value);
        void BindBlob(int index, std::span<const std::uint8_t> value);
        void BindNull(int index);
        // Empty binds NULL: the store's optional texts are empty strings in C++
        void BindTextOrNull(int index, std::string_view value);

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

}  // namespace ambient::store
