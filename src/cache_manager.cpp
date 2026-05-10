#include "cache_manager.h"
#include <iostream>
#include <windows.h>
#include <fstream>

CacheManager::CacheManager(const std::wstring& localBasePath)
    : basePath_(localBasePath), running_(false) {
}

CacheManager::~CacheManager() {
    Stop();
}

void CacheManager::Start() {
    if (running_) return;
    
    if (!driveClient_.Authenticate()) {
        std::wcout << L"[CacheManager] Failed to authenticate with Google Drive!" << std::endl;
        return;
    }

    running_ = true;
    syncThread_ = std::thread(&CacheManager::SyncThreadFunc, this);
}

void CacheManager::Stop() {
    if (!running_) return;
    running_ = false;
    queueCv_.notify_all();
    if (syncThread_.joinable()) {
        syncThread_.join();
    }
}

void CacheManager::QueueUpload(const std::wstring& localPath, const std::wstring& remoteName) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    taskQueue_.push({CacheAction::Upload, localPath, remoteName, L""});
    queueCv_.notify_one();
}

void CacheManager::QueueDelete(const std::wstring& remoteName) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    taskQueue_.push({CacheAction::Delete, L"", remoteName, L""});
    queueCv_.notify_one();
}

void CacheManager::QueueRename(const std::wstring& oldName, const std::wstring& newName) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    taskQueue_.push({CacheAction::Rename, L"", oldName, newName});
    queueCv_.notify_one();
}

void CacheManager::InitialSync() {
    std::wcout << L"[CacheManager] Performing initial sync..." << std::endl;
    auto remoteFiles = driveClient_.ListFiles();
    
    for (const auto& file : remoteFiles) {
        std::wstring fullPath = basePath_ + L"\\" + file.name;
        if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wcout << L"[CacheManager] Creating offline placeholder for " << file.name << L"..." << std::endl;
            
            HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_OFFLINE, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                // Make sparse and set size
                DWORD dwTemp;
                DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwTemp, NULL);
                
                LARGE_INTEGER liSize;
                liSize.QuadPart = file.size;
                SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN);
                SetEndOfFile(hFile);
                
                CloseHandle(hFile);
            }
        }
    }
    std::wcout << L"[CacheManager] Initial sync complete." << std::endl;
}

void CacheManager::DownloadFileSync(const std::wstring& remoteName, const std::wstring& localPath) {
    DWORD attrs = GetFileAttributesW(localPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_OFFLINE)) {
        std::wcout << L"[CacheManager] Downloading on-demand: " << remoteName << L"..." << std::endl;
        
        // Remove offline attribute and empty the file for download
        SetFileAttributesW(localPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        
        std::wstring fileId = driveClient_.GetFileIdByName(remoteName);
        if(!fileId.empty()) {
            driveClient_.DownloadFile(fileId, localPath);
        }
    }
}

void CacheManager::SyncThreadFunc() {
    InitialSync();

    while (running_) {
        CacheTask task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait_for(lock, std::chrono::seconds(5), [this] { 
                return !running_ || !taskQueue_.empty(); 
            });

            if (!running_) break;
            
            if (taskQueue_.empty()) {
                continue;
            }

            task = taskQueue_.front();
            taskQueue_.pop();
        }

        switch (task.action) {
            case CacheAction::Upload:
                driveClient_.UploadFile(task.localPath, task.remoteName);
                break;
            case CacheAction::Delete:
                driveClient_.RemoveFile(task.remoteName);
                break;
            case CacheAction::Rename:
                driveClient_.RenameFile(task.remoteName, task.newRemoteName);
                break;
        }
    }
}

