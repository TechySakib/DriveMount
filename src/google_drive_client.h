#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <windows.h>
#include <winhttp.h>

struct RemoteFile {
    std::wstring id;
    std::wstring name;
    bool isDirectory;
    uint64_t size;
    std::vector<std::wstring> parents;
};

class GoogleDriveClient {
public:
    GoogleDriveClient();
    ~GoogleDriveClient();

    bool Authenticate();
    
    std::vector<RemoteFile> ListFiles();
    bool UploadFile(const std::wstring& localPath, const std::wstring& remoteName);
    bool DownloadFile(const std::wstring& fileId, const std::wstring& localDestPath);
    bool RemoveFile(const std::wstring& remoteName);
    bool RenameFile(const std::wstring& oldName, const std::wstring& newName);
    std::wstring GetFileIdByName(const std::wstring& name);
    std::wstring GetFileIdByPath(const std::wstring& path);
    std::wstring CreateFolder(const std::wstring& folderName, const std::wstring& parentId);

private:
    std::mutex mutex_;
    std::wstring accessToken_;
    std::wstring refreshToken_;
    std::wstring clientId_;
    std::wstring clientSecret_;
    HINTERNET hSession_;
    HINTERNET hConnect_;

    bool RequestDeviceCode(std::wstring& deviceCode, std::wstring& userCode, std::wstring& verificationUrl, int& interval);
    bool PollForToken(const std::wstring& deviceCode, int interval);
    bool RefreshToken();

    std::string SendHttpRequest(const std::wstring& method, const std::wstring& path, const std::string& body = "", const std::wstring& contentType = L"");
};
