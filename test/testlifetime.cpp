#include <stdexcept>
#include <source_location>
#include <string>
#include <string_view>
#include <iostream>
#include <utility>

#include <sqlite3pp.h>
#include <sqlite3ppext.h>

namespace
{
    void check(bool condition, const std::source_location location = std::source_location::current())
    {
        if (!condition) throw std::runtime_error("sqlite3pp lifetime check failed at line " + std::to_string(location.line()));
    }

    struct tracked_sum
    {
        static inline int constructed = 0;
        static inline int destroyed = 0;
        int value = 0;

        tracked_sum() { ++constructed; }
        ~tracked_sum() { ++destroyed; }
        void step(int next) { value += next; }
        int finish() { return value; }
    };

    struct throwing_sum
    {
        static inline int destroyed = 0;
        ~throwing_sum() { ++destroyed; }
        void step(int) { throw std::runtime_error("step failed"); }
        int finish() { return 0; }
    };

    void expect_error(sqlite3pp::query& query)
    {
        bool threw = false;
        try { (void)query.begin(); }
        catch (const sqlite3pp::database_error&) { threw = true; }
        check(threw);
    }
}

int main()
{
    using namespace sqlite3pp;
    try {

    database source(":memory:");
    int updates = 0;
    source.set_update_handler([&](int, char const*, char const*, long long) { ++updates; });
    database moved(std::move(source));
    check(moved.execute("CREATE TABLE items(value)") == SQLITE_OK);
    check(moved.execute("INSERT INTO items VALUES(1)") == SQLITE_OK);
    check(updates == 2);

    database replacement(":memory:");
    bool assignment_failed = false;
    {
        query active(replacement, "SELECT 1");
        database incoming(":memory:");
        try { replacement = std::move(incoming); }
        catch (const database_error&) { assignment_failed = true; }
        check(assignment_failed);
        check(active.begin() != active.end());
    }
    check(replacement.execute("SELECT 1") == SQLITE_OK);

    sqlite3* raw = nullptr;
    check(sqlite3_open(":memory:", &raw) == SQLITE_OK);
    {
        auto borrowed = ext::borrow(raw);
        borrowed.set_update_handler([](int, char const*, char const*, long long) {});
        check(borrowed.disconnect() == SQLITE_OK);
    }
    check(sqlite3_exec(raw, "SELECT 1", nullptr, nullptr, nullptr) == SQLITE_OK);
    check(sqlite3_close(raw) == SQLITE_OK);

    ext::function functions(moved);
    std::function<int(std::string_view)> byte_length = [](std::string_view value) {
        return static_cast<int>(value.size());
    };
    check(functions.create<int(std::string_view)>("byte_length", std::move(byte_length)) == SQLITE_OK);
    check(functions.create("throws", [](ext::context&) { throw std::runtime_error("callback failed"); }, 0) == SQLITE_OK);
    {
        ext::function temporary(moved);
        check(temporary.create("owned_until_connection_close", [](ext::context& context) { context.result(42); }, 0) == SQLITE_OK);
    }
    {
        query text(moved, "SELECT byte_length(CAST(X'410042' AS TEXT))");
        auto row = *text.begin();
        check(row.get<int>(0) == 3);
    }
    {
        query text(moved, "SELECT CAST(X'410042' AS TEXT)");
        auto row = *text.begin();
        check(row.get<std::string_view>(0).size() == 3);
        check(row.get<std::string>(0).size() == 3);
    }
    {
        query retained(moved, "SELECT owned_until_connection_close()");
        check((*retained.begin()).get<int>(0) == 42);
    }
    {
        query throwing(moved, "SELECT throws()");
        expect_error(throwing);
    }

    ext::aggregate aggregates(moved);
    check((aggregates.create<tracked_sum, int>("tracked_sum") == SQLITE_OK));
    check((aggregates.create<throwing_sum, int>("throwing_sum") == SQLITE_OK));
    {
        query sum(moved, "SELECT tracked_sum(value) FROM items");
        check((*sum.begin()).get<int>(0) == 1);
    }
    check(tracked_sum::constructed == 1 && tracked_sum::destroyed == 1);
    {
        query empty(moved, "SELECT tracked_sum(value) FROM items WHERE 0");
        check((*empty.begin()).column_type(0) == SQLITE_NULL);
    }
    check(tracked_sum::constructed == 1 && tracked_sum::destroyed == 1);
    {
        query throwing(moved, "SELECT throwing_sum(value) FROM items");
        expect_error(throwing);
    }
    check(throwing_sum::destroyed == 1);

    database backup_destination(":memory:");
    bool callback_threw = false;
    try {
        moved.backup(backup_destination, [](int, int, int) { throw std::runtime_error("backup callback failed"); });
    }
    catch (const std::runtime_error&) { callback_threw = true; }
    check(callback_threw);
    check(moved.backup(backup_destination) == SQLITE_DONE);
    }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}