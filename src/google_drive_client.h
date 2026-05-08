#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>

struct RemoteFile {
    std::wstring id;
    std::wstring name;
    bool isDirectory;
    uint64_t size;
};

class GoogleDriveClient {
public:
    GoogleDriveClient();
    ~GoogleDriveClient();

    bool Authenticate();
    
    std::vector<RemoteFile> ListFiles();
    bool UploadFile(const std::wstring& localPath, const std::wstring& remoteName);
    bool RemoveFile(const std::wstring& remoteName);
    bool RenameFile(const std::wstring& oldName, const std::wstring& newName);

private:
    std::mutex mutex_;
    std::map<std::wstring, RemoteFile> mockCloud_;
};

