#include "infrastructure/sqlite/sqlite_repository.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

std::int64_t processId()
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(::_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }
    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

  private:
    int failures_ = 0;
};

class TemporaryDatabase final
{
  public:
    TemporaryDatabase()
        : path_(std::filesystem::temp_directory_path() /
                ("ncs-sqlite-corruption-" + std::to_string(processId()) + ".db"))
    {
        cleanup();
    }

    ~TemporaryDatabase()
    {
        cleanup();
    }

    std::string path() const
    {
        return path_.string();
    }

  private:
    void cleanup() const
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::filesystem::path path_;
};

void writeGarbage(const std::string& path)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("test fixture write failed");
    for (int i = 0; i < 2048; ++i)
        file << "this is not a sqlite database ";
}

// Scrambles the btree region of page 1 (everything after the 100-byte
// header). The header stays intact so opening succeeds lazily, but the first
// statement that parses sqlite_master must hit the damage. A single flipped
// byte can land in page free space and stay unread, which is why the whole
// parsed region is corrupted.
void scramblePageOne(const std::string& path)
{
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
        throw std::runtime_error("test fixture open failed");
    char header[100]{};
    file.read(header, sizeof(header));
    const std::int64_t pageSize =
        (static_cast<unsigned char>(header[16]) << 8) | static_cast<unsigned char>(header[17]);
    std::vector<char> page(static_cast<std::size_t>(pageSize));
    file.read(page.data(), static_cast<std::streamsize>(page.size()));
    for (char& byte : page)
        byte = static_cast<char>(byte ^ 0xFF);
    file.seekp(100);
    file.write(page.data(), static_cast<std::streamsize>(page.size()));
}

void truncateFile(const std::string& path, const std::streamoff size)
{
    std::filesystem::resize_file(path, size);
}

void checkpoint(const std::string& path)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        throw std::runtime_error("test database open failed");
    char* error = nullptr;
    const int result =
        sqlite3_exec(database, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, &error);
    const std::string message = error ? error : "wal_checkpoint failed";
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error(message);
}

// NFR-R-02: a damaged database must surface an explicit, non-empty error
// instead of crashing or silently losing data.
std::string expectRepositoryFailure(const std::string& path)
{
    try
    {
        ncs::infrastructure::sqlite::SqliteRepository repository(path);
    }
    catch (const std::runtime_error& error)
    {
        return error.what();
    }
    return {};
}

} // namespace

int main()
{
    using ncs::infrastructure::sqlite::SqliteRepository;

    TestRunner tests;

    {
        // A file that is not a SQLite database at all: opening succeeds
        // lazily but the first statement must fail with an explicit error.
        TemporaryDatabase garbage;
        writeGarbage(garbage.path());
        const std::string message = expectRepositoryFailure(garbage.path());
        tests.check(!message.empty() && message.find("database") != std::string::npos,
                    "a non-database file reports an explicit open-time error");
    }

    {
        // A valid database whose first data page was damaged: initialization
        // reads sqlite_master, so the corruption must be reported instead of
        // being ignored.
        TemporaryDatabase damaged;
        {
            SqliteRepository repository(damaged.path());
            repository.ensureDevelopmentAdmin(true);
        }
        checkpoint(damaged.path());
        scramblePageOne(damaged.path());
        const std::string message = expectRepositoryFailure(damaged.path());
        tests.check(!message.empty() && message.find("database") != std::string::npos,
                    "a corrupted data page reports an explicit initialization error");
    }

    {
        // A valid database truncated below its first page: the header may
        // still parse but reading the truncated page must fail loudly.
        TemporaryDatabase truncated;
        {
            SqliteRepository repository(truncated.path());
            repository.ensureDevelopmentAdmin(true);
        }
        checkpoint(truncated.path());
        truncateFile(truncated.path(), 512);
        const std::string message = expectRepositoryFailure(truncated.path());
        tests.check(!message.empty() && message.find("database") != std::string::npos,
                    "a truncated database reports an explicit initialization error");
    }

    return tests.result();
}
