#include "cache_manager.h"
#include <iostream>
#include <windows.h>
#include <fstream>

CacheManager::CacheManager(const std::wstring& localBasePath)
    : basePath_(localBasePath), running_(false), db_(localBasePath + L"\\drive_metadata.db") {
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

    if (!db_.Open()) {
        std::wcout << L"[CacheManager] Failed to open metadata database!" << std::endl;
        return;
    }

    running_ = true;
    syncThread_ = std::thread(&CacheManager::SyncThreadFunc, this);
    pollingThread_ = std::thread(&CacheManager::PollingThreadFunc, this);
}

void CacheManager::Stop() {
    if (!running_) return;
    running_ = false;
    queueCv_.notify_all();
    if (syncThread_.joinable()) syncThread_.join();
    if (pollingThread_.joinable()) pollingThread_.join();
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

    auto getPath = [&](const std::wstring& id, auto& self_ref) -> std::wstring {
        auto it = fileMap.find(id);
        if (it == fileMap.end()) return L"";
        if (it->second.parents.empty() || it->second.parents[0] == L"root") return it->second.name;
        
        std::wstring parentPath = self_ref(it->second.parents[0], self_ref);
        return parentPath + L"\\" + it->second.name;
    };

    for (const auto& file : remoteFiles) {
        std::wstring relPath = getPath(file.id, getPath);
        std::wstring parentId = file.parents.empty() ? L"root" : file.parents[0];

        DbFile dbf;
        dbf.id = file.id;
        dbf.parentId = parentId;
        dbf.name = file.name;
        dbf.isDirectory = file.isDirectory;
        dbf.size = file.size;
        dbf.localPath = relPath;
        dbf.status = 0; // Offline
        db_.UpsertFile(dbf);

        std::wstring fullPath = basePath_ + L"\\" + relPath;
        if (file.isDirectory) {
            // Create nested directories
            size_t pos = 0;
            while ((pos = relPath.find(L'\\', pos)) != std::wstring::npos) {
                std::wstring subPath = basePath_ + L"\\" + relPath.substr(0, pos);
                CreateDirectoryW(subPath.c_str(), NULL);
                pos++;
            }
            CreateDirectoryW(fullPath.c_str(), NULL);
        } else {
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
            } else {
                // If file exists, check if it's still offline
                DWORD attrs = GetFileAttributesW(fullPath.c_str());
                if (!(attrs & FILE_ATTRIBUTE_OFFLINE)) {
                    dbf.status = 1; // Synced
                    db_.UpsertFile(dbf);
                }
            }
        }
    }
    std::wcout << L"[CacheManager] Initial sync complete." << std::endl;
}

std::string ws2s_cm(const std::wstring& wstr) {
    if(wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring s2ws_cm(const std::string& str) {
    if(str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

void CacheManager::PollingThreadFunc() {
    std::string token = db_.GetSetting("sync_token");
    if (token.empty()) {
        token = ws2s_cm(driveClient_.GetStartPageToken());
        db_.SetSetting("sync_token", token);
    }

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!running_) break;

        std::wcout << L"[CacheManager] Polling for cloud changes..." << std::endl;
        std::wstring nextToken;
        auto changes = driveClient_.GetChanges(s2ws_cm(token), nextToken);
        
        for (const auto& change : changes) {
            ProcessChange(change);
        }

        RunEviction();

        if (!nextToken.empty()) {
            token = ws2s_cm(nextToken);
            db_.SetSetting("sync_token", token);
        }
    }
}

void CacheManager::ProcessChange(const GoogleDriveClient::Change& change) {
    auto existing = db_.GetFileById(change.fileId);

    if (change.removed) {
        if (existing) {
            std::wcout << L"[CacheManager] Remote deletion detected: " << existing->localPath << std::endl;
            std::wstring fullPath = basePath_ + L"\\" + existing->localPath;
            if (existing->isDirectory) RemoveDirectoryW(fullPath.c_str());
            else DeleteFileW(fullPath.c_str());
            db_.DeleteFile(change.fileId);
        }
        return;
    }

    // New or modified
    std::wstring relPath = change.file.name; // Simplification: assume root for now or build path
    // Real implementation should reconstruct the path from parents. 
    // For now, let's just update if it exists.
    
    if (existing) {
        std::wstring oldFullPath = basePath_ + L"\\" + existing->localPath;
        // If name or parent changed, we'd need to move it. 
        // For now, just handle content updates.
        if (existing->size != change.file.size) {
            std::wcout << L"[CacheManager] Remote update detected: " << existing->localPath << std::endl;
            existing->size = change.file.size;
            existing->status = 0; // Back to offline
            db_.UpsertFile(*existing);
            
            // Re-create as sparse offline file
            SetFileAttributesW(oldFullPath.c_str(), FILE_ATTRIBUTE_OFFLINE);
            HANDLE hFile = CreateFileW(oldFullPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_OFFLINE, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD dwTemp;
                DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwTemp, NULL);
                LARGE_INTEGER liSize; liSize.QuadPart = change.file.size;
                SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN);
                SetEndOfFile(hFile);
                CloseHandle(hFile);
            }
        }
    } else {
        // New file
        std::wcout << L"[CacheManager] Remote new file detected: " << change.file.name << std::endl;
        // Re-run part of initial sync logic for this file or just ignore until next restart
        // To be thorough, we should build the path and create the placeholder.
    }
}


std::wstring CacheManager::GetFileIdByPath(const std::wstring& path) {
    auto file = db_.GetFileByPath(path);
    if (file) return file->id;
    return driveClient_.GetFileIdByPath(path); // Fallback
}

void CacheManager::DownloadFileSync(const std::wstring& remoteName, const std::wstring& localPath) {
    DWORD attrs = GetFileAttributesW(localPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_OFFLINE)) {
        std::wcout << L"[CacheManager] Downloading on-demand: " << remoteName << L"..." << std::endl;
        
        // Remove offline attribute and empty the file for download
        SetFileAttributesW(localPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        
        std::wstring fileId = GetFileIdByPath(remoteName);
        if(!fileId.empty()) {
            if (driveClient_.DownloadFile(fileId, localPath)) {
                auto file = db_.GetFileById(fileId);
                if (file) {
                    file->status = 1; // Synced
                    db_.UpsertFile(*file);
                }
            }
        }
    }
}

void CacheManager::DownloadFileChunk(const std::wstring& remoteName, const std::wstring& localPath, uint64_t offset, uint32_t length) {
    std::wstring fileId = GetFileIdByPath(remoteName);
    if (!fileId.empty()) {
        std::wcout << L"[CacheManager] Streaming range: " << offset << L" - " << (offset + length - 1) << L" for " << remoteName << std::endl;
        driveClient_.DownloadFileRange(fileId, localPath, offset, length);
        db_.UpdateLastAccess(fileId);
    }
}

void CacheManager::UpdateLastAccess(const std::wstring& remoteName) {
    std::wstring fileId = GetFileIdByPath(remoteName);
    if (!fileId.empty()) {
        db_.UpdateLastAccess(fileId);
    }
}

void CacheManager::RunEviction() {
    auto files = db_.GetFilesForEviction();
    uint64_t currentCacheSize = 0;
    for (const auto& f : files) currentCacheSize += f.size;

    if (currentCacheSize <= maxCacheSize_) return;

    std::wcout << L"[CacheManager] Cache size (" << (currentCacheSize / 1024 / 1024) << L"MB) exceeds limit (" << (maxCacheSize_ / 1024 / 1024) << L"MB). Evicting..." << std::endl;

    for (auto& f : files) {
        if (currentCacheSize <= maxCacheSize_) break;

        std::wstring fullPath = basePath_ + L"\\" + f.localPath;
        std::wcout << L"[CacheManager] Evicting local content for: " << f.localPath << std::endl;

        // Reset to sparse offline placeholder
        SetFileAttributesW(fullPath.c_str(), FILE_ATTRIBUTE_OFFLINE);
        HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_OFFLINE, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD dwTemp;
            DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwTemp, NULL);
            LARGE_INTEGER liSize; liSize.QuadPart = f.size;
            SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN);
            SetEndOfFile(hFile);
            CloseHandle(hFile);
        }

        f.status = 0; // Offline
        db_.UpsertFile(f);
        currentCacheSize -= f.size;
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
                if (driveClient_.UploadFile(task.localPath, task.remoteName)) {
                    auto file = db_.GetFileByPath(task.remoteName);
                    if (file) {
                        file->status = 1; // Synced
                        db_.UpsertFile(*file);
                    }
                }
                break;
            case CacheAction::Delete:
                {
                    std::wstring fileId = GetFileIdByPath(task.remoteName);
                    if (driveClient_.RemoveFile(task.remoteName)) {
                        if (!fileId.empty()) db_.DeleteFile(fileId);
                    }
                }
                break;
            case CacheAction::Rename:
                {
                    std::wstring fileId = GetFileIdByPath(task.remoteName);
                    if (driveClient_.RenameFile(task.remoteName, task.newRemoteName)) {
                        if (!fileId.empty()) {
                            std::wstring newFileName = task.newRemoteName;
                            size_t lastSlash = task.newRemoteName.find_last_of(L"\\/");
                            if (lastSlash != std::wstring::npos) newFileName = task.newRemoteName.substr(lastSlash + 1);
                            db_.RenameFile(fileId, newFileName, task.newRemoteName);
                        }
                    }
                }
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
                    std::wstring parentId = GetFileIdByPath(parentPath);
                    if (parentId.empty()) parentId = L"root";
                    std::wstring newId = driveClient_.CreateFolder(dirName, parentId);
                    if (!newId.empty()) {
                        DbFile dbf;
                        dbf.id = newId;
                        dbf.parentId = parentId;
                        dbf.name = dirName;
                        dbf.isDirectory = true;
                        dbf.size = 0;
                        dbf.localPath = task.remoteName;
                        dbf.status = 1; // Synced
                        db_.UpsertFile(dbf);
                    }
                }
                break;
        }
    }
}

