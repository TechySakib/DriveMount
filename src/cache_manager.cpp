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

void CacheManager::QueueCreateDir(const std::wstring& remoteName) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    taskQueue_.push({CacheAction::CreateDir, L"", remoteName, L""});
    queueCv_.notify_one();
}

void CacheManager::InitialSync() {
    std::wcout << L"[CacheManager] Performing initial sync..." << std::endl;
    auto remoteFiles = driveClient_.ListFiles();
    
    std::map<std::wstring, RemoteFile> fileMap;
    for (const auto& f : remoteFiles) fileMap[f.id] = f;

    // Helper to build path from parents
    auto getPath = [&](const std::wstring& id, auto& self_ref) -> std::wstring {
        auto it = fileMap.find(id);
        if (it == fileMap.end()) return L"";
        if (it->second.parents.empty() || it->second.parents[0] == L"root") return it->second.name;
        
        std::wstring parentPath = self_ref(it->second.parents[0], self_ref);
        return parentPath + L"\\" + it->second.name;
    };

    // Sort files so directories are created first (optional but safer with simple CreateDirectoryW)
    // Actually, we can just iterate twice.
    
    // Phase 1: Directories
    for (const auto& file : remoteFiles) {
        if (file.isDirectory) {
            std::wstring relPath = getPath(file.id, getPath);
            std::wstring fullPath = basePath_ + L"\\" + relPath;
            // Create nested directories
            size_t pos = 0;
            while ((pos = relPath.find(L'\\', pos)) != std::wstring::npos) {
                std::wstring subPath = basePath_ + L"\\" + relPath.substr(0, pos);
                CreateDirectoryW(subPath.c_str(), NULL);
                pos++;
            }
            CreateDirectoryW(fullPath.c_str(), NULL);
        }
    }

    // Phase 2: Files
    for (const auto& file : remoteFiles) {
        if (!file.isDirectory) {
            std::wstring relPath = getPath(file.id, getPath);
            std::wstring fullPath = basePath_ + L"\\" + relPath;
            
            if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wcout << L"[CacheManager] Creating offline placeholder for " << relPath << L"..." << std::endl;
                
                HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_OFFLINE, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
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
            case CacheAction::CreateDir:
                {
                    std::wstring dirName = task.remoteName;
                    std::wstring parentPath = L"";
                    size_t lastSlash = task.remoteName.find_last_of(L"\\/");
                    if (lastSlash != std::wstring::npos) {
                        dirName = task.remoteName.substr(lastSlash + 1);
                        parentPath = task.remoteName.substr(0, lastSlash);
                    }
                    std::wstring parentId = driveClient_.GetFileIdByPath(parentPath);
                    if (parentId.empty()) parentId = L"root";
                    driveClient_.CreateFolder(dirName, parentId);
                }
                break;
        }
    }
}

