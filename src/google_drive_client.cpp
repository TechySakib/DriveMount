#include "google_drive_client.h"
#include <iostream>
#include <thread>
#include <chrono>

GoogleDriveClient::GoogleDriveClient() {
    mockCloud_[L"welcome_to_drive.txt"] = { L"id_1", L"welcome_to_drive.txt", false, 42 };
}

GoogleDriveClient::~GoogleDriveClient() {}

bool GoogleDriveClient::Authenticate() {
    std::wcout << L"[MockDrive] Authenticating..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::wcout << L"[MockDrive] Authentication successful!" << std::endl;
    return true;
}

std::vector<RemoteFile> GoogleDriveClient::ListFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RemoteFile> files;
    for (const auto& pair : mockCloud_) {
        files.push_back(pair.second);
    }
    return files;
}

bool GoogleDriveClient::UploadFile(const std::wstring& localPath, const std::wstring& remoteName) {
    std::wcout << L"[MockDrive] Uploading " << remoteName << L"..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    std::lock_guard<std::mutex> lock(mutex_);
    RemoteFile file;
    file.id = L"id_" + remoteName;
    file.name = remoteName;
    file.isDirectory = false;
    file.size = 1024;
    mockCloud_[remoteName] = file;
    
    std::wcout << L"[MockDrive] Uploaded " << remoteName << L" successfully." << std::endl;
    return true;
}

bool GoogleDriveClient::RemoveFile(const std::wstring& remoteName) {
    std::wcout << L"[MockDrive] Deleting " << remoteName << L"..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::lock_guard<std::mutex> lock(mutex_);
    mockCloud_.erase(remoteName);
    std::wcout << L"[MockDrive] Deleted " << remoteName << L" successfully." << std::endl;
    return true;
}

bool GoogleDriveClient::RenameFile(const std::wstring& oldName, const std::wstring& newName) {
    std::wcout << L"[MockDrive] Renaming " << oldName << L" to " << newName << L"..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::lock_guard<std::mutex> lock(mutex_);
    if (mockCloud_.find(oldName) != mockCloud_.end()) {
        RemoteFile file = mockCloud_[oldName];
        file.name = newName;
        file.id = L"id_" + newName;
        mockCloud_.erase(oldName);
        mockCloud_[newName] = file;
    }
    return true;
}

