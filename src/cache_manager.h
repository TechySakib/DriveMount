#pragma once

#include "google_drive_client.h"
#include "metadata_db.h"
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

enum class CacheAction {
    Upload,
    Delete,
    Rename,
    CreateDir
};

struct CacheTask {
    CacheAction action;
    std::wstring localPath;
    std::wstring remoteName;
    std::wstring newRemoteName;
};

class CacheManager {
public:
    CacheManager(const std::wstring& localBasePath);
    ~CacheManager();

    void Start();
    void Stop();

    void QueueUpload(const std::wstring& localPath, const std::wstring& remoteName);
    void QueueDelete(const std::wstring& remoteName);
    void QueueRename(const std::wstring& oldName, const std::wstring& newName);
    void QueueCreateDir(const std::wstring& remoteName);
    void DownloadFileSync(const std::wstring& remoteName, const std::wstring& localPath);
    void DownloadFileChunk(const std::wstring& remoteName, const std::wstring& localPath, uint64_t offset, uint32_t length);

private:
    void SyncThreadFunc();
    void PollingThreadFunc();
    void InitialSync();
    void ProcessChange(const GoogleDriveClient::Change& change);
    std::wstring GetFileIdByPath(const std::wstring& path);

    std::wstring basePath_;
    GoogleDriveClient driveClient_;
    MetadataDb db_;
    
    std::thread syncThread_;
    std::thread pollingThread_;
    std::atomic<bool> running_;
    
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<CacheTask> taskQueue_;
};
