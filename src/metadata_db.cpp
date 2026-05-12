#include "metadata_db.h"
#include <windows.h>
#include <iostream>

MetadataDb::MetadataDb(const std::wstring& dbPath) : dbPath_(dbPath) {}

MetadataDb::~MetadataDb() {
    Close();
}

std::string MetadataDb::ws2s(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring MetadataDb::s2ws(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

bool MetadataDb::Open() {
    std::string path = ws2s(dbPath_);
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return InitSchema();
}

void MetadataDb::Close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MetadataDb::InitSchema() {
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS files ("
        "id TEXT PRIMARY KEY,"
        "parent_id TEXT,"
        "name TEXT,"
        "is_directory INTEGER,"
        "size INTEGER,"
        "local_path TEXT,"
        "status INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS settings ("
        "key TEXT PRIMARY KEY,"
        "value TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_path ON files(local_path);"
        "CREATE INDEX IF NOT EXISTS idx_parent ON files(parent_id);";

    char* errMsg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Schema error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool MetadataDb::UpsertFile(const DbFile& file) {
    const char* sql = "INSERT OR REPLACE INTO files (id, parent_id, name, is_directory, size, local_path, status) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, ws2s(file.id).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ws2s(file.parentId).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ws2s(file.name).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, file.isDirectory ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, file.size);
    sqlite3_bind_text(stmt, 6, ws2s(file.localPath).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, file.status);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

std::optional<DbFile> MetadataDb::GetFileById(const std::wstring& id) {
    const char* sql = "SELECT id, parent_id, name, is_directory, size, local_path, status FROM files WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, ws2s(id).c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        DbFile file;
        file.id = s2ws((const char*)sqlite3_column_text(stmt, 0));
        file.parentId = s2ws((const char*)sqlite3_column_text(stmt, 1));
        file.name = s2ws((const char*)sqlite3_column_text(stmt, 2));
        file.isDirectory = sqlite3_column_int(stmt, 3) != 0;
        file.size = sqlite3_column_int64(stmt, 4);
        file.localPath = s2ws((const char*)sqlite3_column_text(stmt, 5));
        file.status = sqlite3_column_int(stmt, 6);
        sqlite3_finalize(stmt);
        return file;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::optional<DbFile> MetadataDb::GetFileByPath(const std::wstring& path) {
    const char* sql = "SELECT id, parent_id, name, is_directory, size, local_path, status FROM files WHERE local_path = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, ws2s(path).c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        DbFile file;
        file.id = s2ws((const char*)sqlite3_column_text(stmt, 0));
        file.parentId = s2ws((const char*)sqlite3_column_text(stmt, 1));
        file.name = s2ws((const char*)sqlite3_column_text(stmt, 2));
        file.isDirectory = sqlite3_column_int(stmt, 3) != 0;
        file.size = sqlite3_column_int64(stmt, 4);
        file.localPath = s2ws((const char*)sqlite3_column_text(stmt, 5));
        file.status = sqlite3_column_int(stmt, 6);
        sqlite3_finalize(stmt);
        return file;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<DbFile> MetadataDb::GetChildren(const std::wstring& parentId) {
    std::vector<DbFile> children;
    const char* sql = "SELECT id, parent_id, name, is_directory, size, local_path, status FROM files WHERE parent_id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return children;

    sqlite3_bind_text(stmt, 1, ws2s(parentId).c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbFile file;
        file.id = s2ws((const char*)sqlite3_column_text(stmt, 0));
        file.parentId = s2ws((const char*)sqlite3_column_text(stmt, 1));
        file.name = s2ws((const char*)sqlite3_column_text(stmt, 2));
        file.isDirectory = sqlite3_column_int(stmt, 3) != 0;
        file.size = sqlite3_column_int64(stmt, 4);
        file.localPath = s2ws((const char*)sqlite3_column_text(stmt, 5));
        file.status = sqlite3_column_int(stmt, 6);
        children.push_back(file);
    }

    sqlite3_finalize(stmt);
    return children;
}

bool MetadataDb::DeleteFile(const std::wstring& id) {
    const char* sql = "DELETE FROM files WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, ws2s(id).c_str(), -1, SQLITE_TRANSIENT);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

bool MetadataDb::SetSetting(const std::string& key, const std::string& value) {
    const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

std::string MetadataDb::GetSetting(const std::string& key) {
    const char* sql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string value = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = (const char*)sqlite3_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

bool MetadataDb::RenameFile(const std::wstring& id, const std::wstring& newName, const std::wstring& newPath) {
    const char* sql = "UPDATE files SET name = ?, local_path = ? WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, ws2s(newName).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ws2s(newPath).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ws2s(id).c_str(), -1, SQLITE_TRANSIENT);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}
