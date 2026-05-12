#include "google_drive_client.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

// Helper to convert wstring to string
std::string ws2s(const std::wstring& wstr) {
    if(wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// Helper to convert string to wstring
std::wstring s2ws(const std::string& str) {
    if(str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

GoogleDriveClient::GoogleDriveClient() {
    hSession_ = WinHttpOpen(L"DriveMount/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    hConnect_ = WinHttpConnect(hSession_, L"www.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    
    // Read from env vars for now
    char* cid = nullptr;
    size_t len = 0;
    _dupenv_s(&cid, &len, "DRIVEMOUNT_CLIENT_ID");
    if(cid) { clientId_ = s2ws(cid); free(cid); }
    
    char* csec = nullptr;
    _dupenv_s(&csec, &len, "DRIVEMOUNT_CLIENT_SECRET");
    if(csec) { clientSecret_ = s2ws(csec); free(csec); }
}

GoogleDriveClient::~GoogleDriveClient() {
    if(hConnect_) WinHttpCloseHandle(hConnect_);
    if(hSession_) WinHttpCloseHandle(hSession_);
}

std::string GoogleDriveClient::SendHttpRequest(const std::wstring& method, const std::wstring& path, const std::string& body, const std::wstring& contentType) {
    HINTERNET hRequest = WinHttpOpenRequest(hConnect_, method.c_str(), path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if(!hRequest) return "";

    std::wstring headers = L"";
    if(!accessToken_.empty()) {
        headers += L"Authorization: Bearer " + accessToken_ + L"\r\n";
    }
    if(!contentType.empty()) {
        headers += L"Content-Type: " + contentType + L"\r\n";
    }

    BOOL bResults = WinHttpSendRequest(hRequest, 
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), 
        headers.empty() ? 0 : -1, 
        (LPVOID)(body.empty() ? WINHTTP_NO_REQUEST_DATA : body.c_str()), 
        body.size(), body.size(), 0);

    std::string responseStr = "";
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }
    if (bResults) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            dwSize = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if(dwSize > 0) {
                char* pszOutBuffer = new char[dwSize + 1];
                WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded);
                pszOutBuffer[dwDownloaded] = '\0';
                responseStr += pszOutBuffer;
                delete[] pszOutBuffer;
            }
        } while (dwSize > 0);
    }
    WinHttpCloseHandle(hRequest);
    return responseStr;
}

bool GoogleDriveClient::Authenticate() {
    // Basic mock authentication if no credentials
    if(clientId_.empty() || clientSecret_.empty()) {
        std::wcout << L"[WARNING] No Client ID/Secret set in environment variables! Skipping real OAuth.\n";
        return true;
    }
    // Device code flow
    std::wstring path = L"/oauth2/v4/device/code";
    std::string body = "client_id=" + ws2s(clientId_) + "&scope=https://www.googleapis.com/auth/drive";
    
    HINTERNET hAuthConnect = WinHttpConnect(hSession_, L"oauth2.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hAuthConnect, L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    
    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID)body.c_str(), body.size(), body.size(), 0);
    WinHttpReceiveResponse(hRequest, NULL);
    
    std::string responseStr;
    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    do {
        dwSize = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if(dwSize > 0) {
            char* pszOutBuffer = new char[dwSize + 1];
            WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded);
            pszOutBuffer[dwDownloaded] = '\0';
            responseStr += pszOutBuffer;
            delete[] pszOutBuffer;
        }
    } while (dwSize > 0);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hAuthConnect);

    try {
        auto j = json::parse(responseStr);
        std::string deviceCode = j["device_code"];
        std::string userCode = j["user_code"];
        std::string verificationUrl = j["verification_url"];
        int interval = j["interval"];

        std::wcout << L"=========================================\n";
        std::wcout << L"Please go to: " << s2ws(verificationUrl) << L"\n";
        std::wcout << L"And enter code: " << s2ws(userCode) << L"\n";
        std::wcout << L"Waiting for authorization...\n";
        std::wcout << L"=========================================\n";

        return PollForToken(s2ws(deviceCode), interval);
    } catch(...) {
        std::wcout << L"Failed to request device code.\n";
        return false;
    }
}

bool GoogleDriveClient::PollForToken(const std::wstring& deviceCode, int interval) {
    std::string body = "client_id=" + ws2s(clientId_) + 
                       "&client_secret=" + ws2s(clientSecret_) + 
                       "&device_code=" + ws2s(deviceCode) + 
                       "&grant_type=urn:ietf:params:oauth:grant-type:device_code";

    HINTERNET hAuthConnect = WinHttpConnect(hSession_, L"oauth2.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        
        HINTERNET hRequest = WinHttpOpenRequest(hAuthConnect, L"POST", L"/token", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
        WinHttpSendRequest(hRequest, headers.c_str(), -1, (LPVOID)body.c_str(), body.size(), body.size(), 0);
        WinHttpReceiveResponse(hRequest, NULL);
        
        std::string responseStr;
        DWORD dwSize = 0, dwDownloaded = 0;
        do {
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if(dwSize > 0) {
                char* pszOutBuffer = new char[dwSize + 1];
                WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded);
                pszOutBuffer[dwDownloaded] = '\0';
                responseStr += pszOutBuffer;
                delete[] pszOutBuffer;
            }
        } while (dwSize > 0);
        WinHttpCloseHandle(hRequest);

        try {
            auto j = json::parse(responseStr);
            if(j.contains("access_token")) {
                accessToken_ = s2ws(j["access_token"].get<std::string>());
                if(j.contains("refresh_token")) refreshToken_ = s2ws(j["refresh_token"].get<std::string>());
                std::wcout << L"Successfully authenticated!\n";
                WinHttpCloseHandle(hAuthConnect);
                return true;
            } else if(j.contains("error") && j["error"] == "authorization_pending") {
                continue;
            } else {
                std::wcout << L"Auth error: " << s2ws(responseStr) << L"\n";
                break;
            }
        } catch(...) { break; }
    }
    WinHttpCloseHandle(hAuthConnect);
    return false;
}

std::vector<RemoteFile> GoogleDriveClient::ListFiles() {
    std::vector<RemoteFile> files;
    if(accessToken_.empty()) {
        std::wcout << L"ListFiles Mock (No Token)\n";
        return files;
    }

    std::string resp = SendHttpRequest(L"GET", L"/drive/v3/files?fields=files(id,name,mimeType,size,parents)&q=trashed=false");
    try {
        auto j = json::parse(resp);
        if(j.contains("files")) {
            for(auto& f : j["files"]) {
                RemoteFile rf;
                rf.id = s2ws(f["id"].get<std::string>());
                rf.name = s2ws(f["name"].get<std::string>());
                rf.isDirectory = (f["mimeType"] == "application/vnd.google-apps.folder");
                rf.size = f.contains("size") ? std::stoull(f["size"].get<std::string>()) : 0;
                if(f.contains("parents")) {
                    for(auto& p : f["parents"]) {
                        rf.parents.push_back(s2ws(p.get<std::string>()));
                    }
                }
                files.push_back(rf);
            }
        }
    } catch(...) {}
    return files;
}

std::wstring GoogleDriveClient::GetFileIdByName(const std::wstring& name) {
    return GetFileIdByPath(name);
}

std::wstring GoogleDriveClient::GetFileIdByPath(const std::wstring& path) {
    if(accessToken_.empty()) return L"";
    if(path.empty() || path == L"\\" || path == L"/") return L"root";

    std::vector<std::wstring> parts;
    std::wstringstream wss(path);
    std::wstring part;
    while (std::getline(wss, part, L'\\')) {
        if (!part.empty()) parts.push_back(part);
    }

    std::wstring currentParentId = L"root";
    for (const auto& p : parts) {
        std::string q = "name='" + ws2s(p) + "' and '" + ws2s(currentParentId) + "' in parents and trashed=false";
        for(char& c : q) if(c == ' ') c = '+';

        std::string resp = SendHttpRequest(L"GET", L"/drive/v3/files?q=" + s2ws(q));
        std::wstring foundId = L"";
        try {
            auto j = json::parse(resp);
            if(j.contains("files") && j["files"].size() > 0) {
                foundId = s2ws(j["files"][0]["id"].get<std::string>());
            }
        } catch(...) {}

        if (foundId.empty()) return L"";
        currentParentId = foundId;
    }
    return currentParentId;
}

std::wstring GoogleDriveClient::CreateFolder(const std::wstring& folderName, const std::wstring& parentId) {
    if(accessToken_.empty()) return L"mock_folder_id";

    std::string metadata = "{\"name\": \"" + ws2s(folderName) + "\", \"mimeType\": \"application/vnd.google-apps.folder\"";
    if (!parentId.empty() && parentId != L"root") {
        metadata += ", \"parents\": [\"" + ws2s(parentId) + "\"]";
    }
    metadata += "}";

    std::string resp = SendHttpRequest(L"POST", L"/drive/v3/files", metadata, L"application/json");
    try {
        auto j = json::parse(resp);
        if(j.contains("id")) {
            return s2ws(j["id"].get<std::string>());
        }
    } catch(...) {}
    return L"";
}

bool GoogleDriveClient::UploadFile(const std::wstring& localPath, const std::wstring& remoteName) {
    if(accessToken_.empty()) return true; // mock

    // 1. Read local file
    std::ifstream file(localPath, std::ios::binary);
    if(!file) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::wstring fileId = GetFileIdByPath(remoteName);
    if(fileId.empty()) {
        std::wstring fileName = remoteName;
        std::wstring parentPath = L"";
        size_t lastSlash = remoteName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            fileName = remoteName.substr(lastSlash + 1);
            parentPath = remoteName.substr(0, lastSlash);
        }
        std::wstring parentId = GetFileIdByPath(parentPath);
        if (parentId.empty()) parentId = L"root";

        std::string metadata = "{\"name\": \"" + ws2s(fileName) + "\", \"parents\": [\"" + ws2s(parentId) + "\"]}";
        
        std::string boundary = "foo_bar_baz";
        std::string body = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" + metadata + "\r\n";
        body += "--" + boundary + "\r\nContent-Type: application/octet-stream\r\n\r\n" + content + "\r\n";
        body += "--" + boundary + "--";

        std::string resp = SendHttpRequest(L"POST", L"/upload/drive/v3/files?uploadType=multipart", body, L"multipart/related; boundary=" + s2ws(boundary));
        return resp.find("\"id\"") != std::string::npos;
    } else {
        // Update existing
        std::string resp = SendHttpRequest(L"PATCH", L"/upload/drive/v3/files/" + fileId + L"?uploadType=media", content, L"application/octet-stream");
        return resp.find("\"id\"") != std::string::npos;
    }
}

bool GoogleDriveClient::RemoveFile(const std::wstring& remoteName) {
    if(accessToken_.empty()) return true;
    std::wstring fileId = GetFileIdByPath(remoteName);
    if(fileId.empty()) return true;
    
    SendHttpRequest(L"DELETE", L"/drive/v3/files/" + fileId);
    return true;
}

bool GoogleDriveClient::RenameFile(const std::wstring& oldName, const std::wstring& newName) {
    if(accessToken_.empty()) return true;
    std::wstring fileId = GetFileIdByPath(oldName);
    if(fileId.empty()) return false;

    std::wstring newFileName = newName;
    size_t lastSlash = newName.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        newFileName = newName.substr(lastSlash + 1);
    }

    std::string body = "{\"name\": \"" + ws2s(newFileName) + "\"}";
    SendHttpRequest(L"PATCH", L"/drive/v3/files/" + fileId, body, L"application/json");
    return true;
}

bool GoogleDriveClient::DownloadFile(const std::wstring& fileId, const std::wstring& localDestPath) {
    if(accessToken_.empty()) return true;
    std::wstring path = L"/drive/v3/files/" + fileId + L"?alt=media";
    HINTERNET hRequest = WinHttpOpenRequest(hConnect_, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if(!hRequest) return false;

    std::wstring headers = L"Authorization: Bearer " + accessToken_ + L"\r\n";
    BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }
    if (bResults) {
        std::ofstream outFile(localDestPath, std::ios::binary);
        if(!outFile) {
            WinHttpCloseHandle(hRequest);
            return false;
        }
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            dwSize = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if(dwSize > 0) {
                char* pszOutBuffer = new char[dwSize];
                WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded);
                outFile.write(pszOutBuffer, dwDownloaded);
                delete[] pszOutBuffer;
            }
        } while (dwSize > 0);
        outFile.close();
    }
    WinHttpCloseHandle(hRequest);
    return bResults != 0;
}

bool GoogleDriveClient::DownloadFileRange(const std::wstring& fileId, const std::wstring& localDestPath, uint64_t offset, uint32_t length) {
    if(accessToken_.empty()) return true;
    std::wstring path = L"/drive/v3/files/" + fileId + L"?alt=media";
    HINTERNET hRequest = WinHttpOpenRequest(hConnect_, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if(!hRequest) return false;

    std::wstring rangeHeader = L"Range: bytes=" + std::to_wstring(offset) + L"-" + std::to_wstring(offset + length - 1) + L"\r\n";
    std::wstring authHeader = L"Authorization: Bearer " + accessToken_ + L"\r\n";
    std::wstring headers = authHeader + rangeHeader;

    BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }
    
    if (bResults) {
        DWORD dwStatusCode = 0;
        DWORD dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        
        if (dwStatusCode != 206 && dwStatusCode != 200) {
            std::wcerr << L"[GoogleDriveClient] Range request failed with status: " << dwStatusCode << std::endl;
            WinHttpCloseHandle(hRequest);
            return false;
        }

        HANDLE hFile = CreateFileW(localDestPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(hRequest);
            return false;
        }

        LARGE_INTEGER liOffset;
        liOffset.QuadPart = offset;
        SetFilePointerEx(hFile, liOffset, NULL, FILE_BEGIN);

        DWORD dwDataAvailable = 0;
        DWORD dwDownloaded = 0;
        do {
            dwDataAvailable = 0;
            WinHttpQueryDataAvailable(hRequest, &dwDataAvailable);
            if(dwDataAvailable > 0) {
                char* pszOutBuffer = new char[dwDataAvailable];
                if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwDataAvailable, &dwDownloaded)) {
                    DWORD dwWritten = 0;
                    WriteFile(hFile, pszOutBuffer, dwDownloaded, &dwWritten, NULL);
                }
                delete[] pszOutBuffer;
            }
        } while (dwDataAvailable > 0);
        CloseHandle(hFile);
    }
    WinHttpCloseHandle(hRequest);
    return bResults != 0;
}

std::wstring GoogleDriveClient::GetStartPageToken() {
    if(accessToken_.empty()) return L"";
    std::string resp = SendHttpRequest(L"GET", L"/drive/v3/changes/startPageToken");
    try {
        auto j = json::parse(resp);
        if(j.contains("startPageToken")) return s2ws(j["startPageToken"].get<std::string>());
    } catch(...) {}
    return L"";
}

std::vector<GoogleDriveClient::Change> GoogleDriveClient::GetChanges(const std::wstring& pageToken, std::wstring& nextPageToken) {
    std::vector<Change> changes;
    if(accessToken_.empty()) return changes;

    std::string resp = SendHttpRequest(L"GET", L"/drive/v3/changes?pageToken=" + pageToken + L"&fields=nextPageToken,newStartPageToken,changes(fileId,removed,file(id,name,mimeType,size,parents,trashed))");
    try {
        auto j = json::parse(resp);
        if(j.contains("nextPageToken")) nextPageToken = s2ws(j["nextPageToken"].get<std::string>());
        else if(j.contains("newStartPageToken")) nextPageToken = s2ws(j["newStartPageToken"].get<std::string>());

        if(j.contains("changes")) {
            for(auto& c : j["changes"]) {
                Change change;
                change.fileId = s2ws(c["fileId"].get<std::string>());
                change.removed = c["removed"].get<bool>();
                
                if(!change.removed && c.contains("file")) {
                    auto& f = c["file"];
                    if (f.contains("trashed") && f["trashed"].get<bool>()) {
                        change.removed = true;
                    } else {
                        change.file.id = s2ws(f["id"].get<std::string>());
                        change.file.name = s2ws(f["name"].get<std::string>());
                        change.file.isDirectory = (f["mimeType"] == "application/vnd.google-apps.folder");
                        change.file.size = f.contains("size") ? std::stoull(f["size"].get<std::string>()) : 0;
                        if(f.contains("parents")) {
                            for(auto& p : f["parents"]) change.file.parents.push_back(s2ws(p.get<std::string>()));
                        }
                    }
                }
                changes.push_back(change);
            }
        }
    } catch(...) {}
    return changes;
}

