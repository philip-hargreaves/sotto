#include "adapters/storage/db.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace sotto::store {
namespace {

struct TempDb {
    std::filesystem::path path;

    TempDb() {
        path = std::filesystem::temp_directory_path() /
               ("sotto-db-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db");
    }

    ~TempDb() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
};

TEST(Db, AppliesThePragmaPolicyOnOpen) {
    TempDb temp;
    Db db(temp.path);
    EXPECT_EQ(db.QueryInt64("PRAGMA page_size"), 8192);
    EXPECT_EQ(db.QueryInt64("PRAGMA synchronous"), 2);  // 2 is FULL
    EXPECT_EQ(db.QueryInt64("PRAGMA foreign_keys"), 1);
    Db::Stmt journal = db.Prepare("PRAGMA journal_mode");
    ASSERT_TRUE(journal.Step());
    EXPECT_EQ(journal.ColumnText(0), "wal");
}

TEST(Db, RoundTripsEveryColumnType) {
    TempDb temp;
    Db db(temp.path);
    db.Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL, data BLOB NOT NULL)");

    const std::vector<std::uint8_t> blob = {0x00, 0xFF, 0x7F, 0x80, 0x01};
    Db::Stmt insert = db.Prepare("INSERT INTO t(id, name, data) VALUES(?, ?, ?)");
    insert.BindInt64(1, 42);
    insert.BindText(2, "consultation");
    insert.BindBlob(3, blob);
    EXPECT_FALSE(insert.Step());
    EXPECT_EQ(db.LastInsertRowId(), 42);

    Db::Stmt select = db.Prepare("SELECT id, name, data FROM t");
    ASSERT_TRUE(select.Step());
    EXPECT_EQ(select.ColumnInt64(0), 42);
    EXPECT_EQ(select.ColumnText(1), "consultation");
    EXPECT_EQ(select.ColumnBlob(2), blob);
    EXPECT_FALSE(select.Step());
}

TEST(Db, ResetAllowsAStatementToRunAgain) {
    TempDb temp;
    Db db(temp.path);
    db.Exec("CREATE TABLE t(seq INTEGER PRIMARY KEY)");

    Db::Stmt insert = db.Prepare("INSERT INTO t(seq) VALUES(?)");
    for (std::int64_t seq = 0; seq < 3; ++seq) {
        insert.BindInt64(1, seq);
        EXPECT_FALSE(insert.Step());
        insert.Reset();
    }
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM t"), 3);
}

TEST(Db, CommittedTransactionSurvivesReopen) {
    TempDb temp;
    {
        Db db(temp.path);
        db.Exec("CREATE TABLE t(seq INTEGER PRIMARY KEY)");
        Db::Transaction txn(db);
        db.Exec("INSERT INTO t(seq) VALUES(1)");
        txn.Commit();
    }
    Db reopened(temp.path);
    EXPECT_EQ(reopened.QueryInt64("SELECT COUNT(*) FROM t"), 1);
}

TEST(Db, UncommittedTransactionRollsBack) {
    TempDb temp;
    Db db(temp.path);
    db.Exec("CREATE TABLE t(seq INTEGER PRIMARY KEY)");
    {
        Db::Transaction txn(db);
        db.Exec("INSERT INTO t(seq) VALUES(1)");
    }
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM t"), 0);
}

TEST(Db, BadSqlThrows) {
    TempDb temp;
    Db db(temp.path);
    EXPECT_THROW(db.Exec("NOT ACTUAL SQL"), std::runtime_error);
    EXPECT_THROW(db.Prepare("SELECT * FROM missing"), std::runtime_error);
}

TEST(Db, OpenInMissingDirectoryThrows) {
    const auto path = std::filesystem::temp_directory_path() / "sotto-db-no-such-dir" / "x.db";
    EXPECT_THROW(Db{path}, std::runtime_error);
}

}  // namespace
}  // namespace sotto::store
