#pragma once

#include <string>
#include <vector>
#include <optional>
#include "sqlite3.h"

struct DbFile {
    std::wstring id;
    std::wstring parentId;
    std::wstring name;
    bool isDirectory;
    uint64_t size;
    std::wstring localPath;
    int status; // 0: Offline, 1: Synced, 2: Modified
    uint64_t lastAccess;
};

class MetadataDb {
public:
    MetadataDb(const std::wstring& dbPath);
    ~MetadataDb();

    bool Open();
    void Close();

    bool UpsertFile(const DbFile& file);
    std::optional<DbFile> GetFileById(const std::wstring& id);
    std::optional<DbFile> GetFileByPath(const std::wstring& path);
    std::vector<DbFile> GetChildren(const std::wstring& parentId);
    bool DeleteFile(const std::wstring& id);
    bool RenameFile(const std::wstring& id, const std::wstring& newName, const std::wstring& newPath);
    bool UpdateLastAccess(const std::wstring& id);
    std::vector<DbFile> GetFilesForEviction();

    bool SetSetting(const std::string& key, const std::string& value);
    std::string GetSetting(const std::string& key);

private:
    std::wstring dbPath_;
    sqlite3* db_ = nullptr;

    bool InitSchema();
    std::string ws2s(const std::wstring& wstr);
    std::wstring s2ws(const std::string& str);
};
