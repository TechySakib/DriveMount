#pragma once

#include "google_drive_client.h"
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

enum class CacheAction {
    Upload,
    Delete,
    Rename
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
    void DownloadFileSync(const std::wstring& remoteName, const std::wstring& localPath);

private:
    void SyncThreadFunc();
    void InitialSync();

    std::wstring basePath_;
    GoogleDriveClient driveClient_;
    
    std::thread syncThread_;
    std::atomic<bool> running_;
    
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<CacheTask> taskQueue_;
};
