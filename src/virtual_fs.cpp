#include "virtual_fs.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>
namespace
{
    constexpr wchar_t kVolumeLabel[] = L"DriveMount";
    constexpr wchar_t kFileSystemName[] = L"DriveMountFS";

    UINT64 FileTimeToUInt64(const FILETIME& ft)
    {
        ULARGE_INTEGER li{};
        li.LowPart = ft.dwLowDateTime;
        li.HighPart = ft.dwHighDateTime;
        return li.QuadPart;
    }
}

VirtualFs::VirtualFs()
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    ULONG sdSize = 0;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)",
            SDDL_REVISION_1,
            &sd,
            &sdSize))
    {
        throw std::runtime_error("Failed to create security descriptor");
    }

    securityDescriptorStorage_ = std::make_unique<std::byte[]>(sdSize);
    std::memcpy(securityDescriptorStorage_.get(), sd, sdSize);
    securityDescriptorSize_ = sdSize;
    LocalFree(sd);
}

VirtualFs::~VirtualFs()
{
    Stop();
}

VirtualFs* VirtualFs::Self(FSP_FILE_SYSTEM* fs)
{
    return static_cast<VirtualFs*>(fs->UserContext);
}

std::wstring VirtualFs::VirtualToRealPath(const std::wstring& virtualPath) const
{
    if (virtualPath.empty() || virtualPath == L"\\")
        return basePath_;

    std::wstring path = virtualPath;
    std::replace(path.begin(), path.end(), L'/', L'\\');

    if (!path.empty() && path.front() == L'\\')
        path.erase(path.begin());

    return basePath_ + L"\\" + path;
}

bool VirtualFs::GetRealFileInfo(
    const std::wstring& realPath,
    FSP_FSCTL_FILE_INFO* fileInfo,
    bool* isDirectory)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};

    if (!GetFileAttributesExW(realPath.c_str(), GetFileExInfoStandard, &data))
        return false;

    const bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (isDirectory)
        *isDirectory = dir;

    if (fileInfo)
    {
        std::memset(fileInfo, 0, sizeof(*fileInfo));

        fileInfo->FileAttributes = data.dwFileAttributes;
        fileInfo->CreationTime = FileTimeToUInt64(data.ftCreationTime);
        fileInfo->LastAccessTime = FileTimeToUInt64(data.ftLastAccessTime);
        fileInfo->LastWriteTime = FileTimeToUInt64(data.ftLastWriteTime);
        fileInfo->ChangeTime = FileTimeToUInt64(data.ftLastWriteTime);

        ULARGE_INTEGER size{};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;

        if (dir)
        {
            fileInfo->FileSize = 0;
            fileInfo->AllocationSize = 0;
        }
        else
        {
            fileInfo->FileSize = size.QuadPart;
            fileInfo->AllocationSize = size.QuadPart;
        }

        fileInfo->IndexNumber = 0;
        fileInfo->HardLinks = 1;
    }

    return true;
}

NTSTATUS VirtualFs::Start(const std::wstring& mountPoint)
{
    if (fs_ != nullptr)
        return STATUS_DEVICE_BUSY;

    mountPoint_ = mountPoint;

    cacheManager_ = std::make_unique<CacheManager>(basePath_);
    cacheManager_->Start();

    if (!CreateDirectoryW(basePath_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        std::wcout << L"Failed to create base path: " << basePath_ << std::endl;
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    FSP_FSCTL_VOLUME_PARAMS volumeParams{};
    volumeParams.SectorSize = 4096;
    volumeParams.SectorsPerAllocationUnit = 1;
    volumeParams.MaxComponentLength = 255;
    volumeParams.VolumeSerialNumber = 0x19831116;
    volumeParams.FileInfoTimeout = 1000;
    volumeParams.CaseSensitiveSearch = 0;
    volumeParams.CasePreservedNames = 1;
    volumeParams.UnicodeOnDisk = 1;
    volumeParams.PersistentAcls = 1;
    volumeParams.PostCleanupWhenModifiedOnly = 1;
    wcscpy_s(volumeParams.FileSystemName, kFileSystemName);

    static FSP_FILE_SYSTEM_INTERFACE iface{};
    iface.GetVolumeInfo = &VirtualFs::GetVolumeInfo;
    iface.GetSecurityByName = &VirtualFs::GetSecurityByName;
    iface.Open = &VirtualFs::Open;
    iface.Create = &VirtualFs::Create;
    iface.Overwrite = &VirtualFs::Overwrite;
    iface.Cleanup = &VirtualFs::Cleanup;
    iface.Close = &VirtualFs::Close;
    iface.GetFileInfo = &VirtualFs::GetFileInfo;
    iface.Read = &VirtualFs::Read;
    iface.Write = &VirtualFs::Write;
    iface.Flush = &VirtualFs::Flush;
    iface.SetBasicInfo = &VirtualFs::SetBasicInfo;
    iface.SetFileSize = &VirtualFs::SetFileSize;
    iface.CanDelete = &VirtualFs::CanDelete;
    iface.Rename = &VirtualFs::Rename;
    iface.GetSecurity = &VirtualFs::GetSecurity;
    iface.SetSecurity = &VirtualFs::SetSecurity;
    iface.ReadDirectory = &VirtualFs::ReadDirectory;

    wchar_t deviceName[] = L"" FSP_FSCTL_DISK_DEVICE_NAME;

    NTSTATUS status = FspFileSystemCreate(
        deviceName,
        &volumeParams,
        &iface,
        &fs_);

    if (!NT_SUCCESS(status))
        return status;

    fs_->UserContext = this;

    FspFileSystemSetDebugLog(fs_, -1);

     status = FspFileSystemSetMountPoint(fs_, const_cast<PWSTR>(mountPoint_.c_str()));
    if (!NT_SUCCESS(status))    
    {
        std::wcout << L"Mount failed: 0x" << std::hex << status << std::endl;
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        return status;
    }

    status = FspFileSystemStartDispatcher(fs_, 0);
    if (!NT_SUCCESS(status))
    {
        std::wcout << L"Dispatcher failed: 0x" << std::hex << status << std::endl;
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        return status;
    }

    return STATUS_SUCCESS;
}

void VirtualFs::Stop()
{
    if (cacheManager_) cacheManager_->Stop();
    if (!fs_)
        return;

    FspFileSystemStopDispatcher(fs_);
    FspFileSystemDelete(fs_);
    fs_ = nullptr;
}

NTSTATUS VirtualFs::GetVolumeInfo(
    FSP_FILE_SYSTEM* FileSystem,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    (void)FileSystem;

    std::memset(VolumeInfo, 0, sizeof(*VolumeInfo));

    VolumeInfo->TotalSize = 1024ull * 1024ull * 1024ull;
    VolumeInfo->FreeSize = 512ull * 1024ull * 1024ull;

    const ULONG labelBytes = static_cast<ULONG>(wcslen(kVolumeLabel) * sizeof(wchar_t));
    VolumeInfo->VolumeLabelLength = labelBytes;
    std::memcpy(VolumeInfo->VolumeLabel, kVolumeLabel, labelBytes);

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::GetSecurityByName(
    FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName,
    PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T* PSecurityDescriptorSize)
{
    auto* self = Self(FileSystem);

    std::wstring virtualPath = FileName ? FileName : L"\\";
    std::wstring realPath = self->VirtualToRealPath(virtualPath);

    FSP_FSCTL_FILE_INFO info{};
    bool isDirectory = false;

    if (!GetRealFileInfo(realPath, &info, &isDirectory))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (PFileAttributes)
        *PFileAttributes = info.FileAttributes;

    if (!PSecurityDescriptorSize)
        return STATUS_SUCCESS;

    if (SecurityDescriptor == nullptr || *PSecurityDescriptorSize < self->securityDescriptorSize_)
    {
        *PSecurityDescriptorSize = self->securityDescriptorSize_;
        return STATUS_BUFFER_OVERFLOW;
    }

    std::memcpy(
        SecurityDescriptor,
        self->securityDescriptorStorage_.get(),
        self->securityDescriptorSize_);

    *PSecurityDescriptorSize = self->securityDescriptorSize_;

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::Open(
    FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    PVOID* PFileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    (void)GrantedAccess;

    auto* self = Self(FileSystem);
    std::wstring virtualPath = FileName ? FileName : L"\\";
    std::wcout << L"Open: " << virtualPath << L"" << std::endl;
    std::wstring realPath = self->VirtualToRealPath(virtualPath);

    bool isDirectory = false;

    if (!GetRealFileInfo(realPath, FileInfo, &isDirectory))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (!isDirectory && (FileInfo->FileAttributes & FILE_ATTRIBUTE_OFFLINE)) {
        std::wstring remoteName = virtualPath;
        if (!remoteName.empty() && remoteName.front() == L'\\') remoteName.erase(0, 1);
        self->cacheManager_->DownloadFileSync(remoteName, realPath);
        GetRealFileInfo(realPath, FileInfo, &isDirectory);
    }

    const bool wantsDirectory = (CreateOptions & FILE_DIRECTORY_FILE) != 0;

    if (wantsDirectory && !isDirectory)
        return STATUS_NOT_A_DIRECTORY;

    auto* ctx = new VirtualFs::FileContext{};
    ctx->realPath = realPath;
    ctx->virtualPath = virtualPath;
    ctx->isDirectory = isDirectory;

    *PFileContext = ctx;

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::Create(
    FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    UINT32 FileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    UINT64 AllocationSize,
    PVOID* PFileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    (void)GrantedAccess;
    (void)AllocationSize;
    (void)SecurityDescriptor;

    auto* self = Self(FileSystem);

    std::wstring virtualPath = FileName ? FileName : L"\\";
    std::wstring realPath = self->VirtualToRealPath(virtualPath);

    const bool createDirectory = (CreateOptions & FILE_DIRECTORY_FILE) != 0;

    if (createDirectory)
    {
        if (!CreateDirectoryW(realPath.c_str(), nullptr))
        {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_EXISTS) return STATUS_OBJECT_NAME_COLLISION;
            if (err == ERROR_PATH_NOT_FOUND) return STATUS_OBJECT_PATH_NOT_FOUND;
            return STATUS_ACCESS_DENIED;
        }
    }
    else
    {
        HANDLE hFile = CreateFileW(
            realPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FileAttributes == 0 ? FILE_ATTRIBUTE_NORMAL : FileAttributes,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_EXISTS) return STATUS_OBJECT_NAME_COLLISION;
            if (err == ERROR_PATH_NOT_FOUND) return STATUS_OBJECT_PATH_NOT_FOUND;
            return STATUS_ACCESS_DENIED;
        }
        CloseHandle(hFile);
    }

    bool isDirectory = false;
    if (!GetRealFileInfo(realPath, FileInfo, &isDirectory))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    auto* ctx = new VirtualFs::FileContext{};
    ctx->realPath = realPath;
    ctx->virtualPath = virtualPath;
    ctx->isDirectory = isDirectory;

    *PFileContext = ctx;

    return STATUS_SUCCESS;
}



VOID VirtualFs::Close(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext)
{
    (void)FileSystem;

    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    delete ctx;
}

NTSTATUS VirtualFs::GetFileInfo(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    (void)FileSystem;

    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);

    if (!ctx)
        return STATUS_INVALID_HANDLE;

    bool isDirectory = false;

    if (!GetRealFileInfo(ctx->realPath, FileInfo, &isDirectory))
        return STATUS_OBJECT_NAME_NOT_FOUND;

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::Read(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PVOID Buffer,
    UINT64 Offset,
    ULONG Length,
    PULONG PBytesTransferred)
{
    (void)FileSystem;

    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);

    if (!ctx || ctx->isDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;

    HANDLE file = CreateFileW(
        ctx->realPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(Offset);

    if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN))
    {
        CloseHandle(file);
        return STATUS_INVALID_PARAMETER;
    }

    DWORD bytesRead = 0;

    if (!ReadFile(file, Buffer, Length, &bytesRead, nullptr))
    {
        CloseHandle(file);
        return STATUS_UNSUCCESSFUL;
    }

    CloseHandle(file);

    *PBytesTransferred = bytesRead;
    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::ReadDirectory(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PWSTR Pattern,
    PWSTR Marker,
    PVOID Buffer,
    ULONG Length,
    PULONG PBytesTransferred)
{
    auto* self = Self(FileSystem);
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);

    if (!ctx)
        return STATUS_INVALID_HANDLE;

    *PBytesTransferred = 0;

    std::wstring dirPath = ctx->realPath.empty() ? self->basePath_ : ctx->realPath;
    std::wstring searchPath = dirPath + L"\\*";
    bool isRoot = (ctx->realPath.empty() || ctx->realPath == self->basePath_);

    WIN32_FIND_DATAW findData{};
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        FspFileSystemAddDirInfo(nullptr, Buffer, Length, PBytesTransferred);
        return STATUS_SUCCESS;
    }

    bool skipUntilMarker = (Marker != nullptr);

    do
    {
        const wchar_t* name = findData.cFileName;

        if (skipUntilMarker)
        {
            if (wcscmp(name, Marker) == 0)
                skipUntilMarker = false;
            continue;
        }

        if (isRoot && (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0))
            continue;

        std::wstring fullPath = dirPath + L"\\" + name;

        FSP_FSCTL_FILE_INFO fileInfo{};
        bool isDir = false;

        if (!GetRealFileInfo(fullPath, &fileInfo, &isDir))
        {
            // For . and .. GetRealFileInfo will still work, but let's be careful
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            {
                std::memset(&fileInfo, 0, sizeof(fileInfo));
                fileInfo.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            }
            else
            {
                continue;
            }
        }

        const ULONG nameLen = (ULONG)(wcslen(name) * sizeof(wchar_t));
        const ULONG entrySize = sizeof(FSP_FSCTL_DIR_INFO) + nameLen;

        std::vector<BYTE> temp(entrySize);
        auto* dirInfo = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(temp.data());

        memset(dirInfo, 0, entrySize);
        dirInfo->Size = entrySize;
        dirInfo->FileInfo = fileInfo;

        memcpy(dirInfo->FileNameBuf, name, nameLen);

        if (!FspFileSystemAddDirInfo(dirInfo, Buffer, Length, PBytesTransferred))
            break;

    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);

    FspFileSystemAddDirInfo(nullptr, Buffer, Length, PBytesTransferred);

    return STATUS_SUCCESS;
}
NTSTATUS VirtualFs::Overwrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx || ctx->isDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;

    HANDLE hFile = CreateFileW(
        ctx->realPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        TRUNCATE_EXISTING,
        FileAttributes == 0 ? FILE_ATTRIBUTE_NORMAL : FileAttributes,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_ACCESS_DENIED;
    }
    CloseHandle(hFile);

    bool isDir = false;
    GetRealFileInfo(ctx->realPath, FileInfo, &isDir);

    auto* self = Self(FileSystem);
    if (!ctx->virtualPath.empty() && ctx->virtualPath != L"\\") {
        std::wstring remoteName = ctx->virtualPath;
        if (!remoteName.empty() && remoteName.front() == L'\\') remoteName.erase(0, 1);
        if (self->cacheManager_) self->cacheManager_->QueueUpload(ctx->realPath, remoteName);
    }

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::Write(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx || ctx->isDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;

    HANDLE file = CreateFileW(
        ctx->realPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(Offset);

    if (WriteToEndOfFile)
    {
        SetFilePointerEx(file, {0}, nullptr, FILE_END);
    }
    else
    {
        SetFilePointerEx(file, li, nullptr, FILE_BEGIN);
    }

    DWORD bytesWritten = 0;
    if (!WriteFile(file, Buffer, Length, &bytesWritten, nullptr))
    {
        CloseHandle(file);
        return STATUS_UNSUCCESSFUL;
    }

    CloseHandle(file);

    *PBytesTransferred = bytesWritten;
    
    bool isDir = false;
    GetRealFileInfo(ctx->realPath, FileInfo, &isDir);

    auto* self = Self(FileSystem);
    if (!ctx->virtualPath.empty() && ctx->virtualPath != L"\\") {
        std::wstring remoteName = ctx->virtualPath;
        if (!remoteName.empty() && remoteName.front() == L'\\') remoteName.erase(0, 1);
        if (self->cacheManager_) self->cacheManager_->QueueUpload(ctx->realPath, remoteName);
    }

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::Flush(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx) return STATUS_SUCCESS;

    bool isDir = false;
    GetRealFileInfo(ctx->realPath, FileInfo, &isDir);

    auto* self = Self(FileSystem);
    if (!ctx->virtualPath.empty() && ctx->virtualPath != L"\\") {
        std::wstring remoteName = ctx->virtualPath;
        if (!remoteName.empty() && remoteName.front() == L'\\') remoteName.erase(0, 1);
        if (self->cacheManager_) self->cacheManager_->QueueUpload(ctx->realPath, remoteName);
    }

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::SetBasicInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes, UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime, FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;

    if (FileAttributes != INVALID_FILE_ATTRIBUTES && FileAttributes != 0)
    {
        SetFileAttributesW(ctx->realPath.c_str(), FileAttributes);
    }

    HANDLE file = CreateFileW(
        ctx->realPath.c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        ctx->isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file != INVALID_HANDLE_VALUE)
    {
        FILETIME ct{}, at{}, wt{};
        bool setCt = CreationTime != 0;
        bool setAt = LastAccessTime != 0;
        bool setWt = LastWriteTime != 0 || ChangeTime != 0;

        if (setCt) {
            ULARGE_INTEGER li; li.QuadPart = CreationTime;
            ct.dwLowDateTime = li.LowPart; ct.dwHighDateTime = li.HighPart;
        }
        if (setAt) {
            ULARGE_INTEGER li; li.QuadPart = LastAccessTime;
            at.dwLowDateTime = li.LowPart; at.dwHighDateTime = li.HighPart;
        }
        if (setWt) {
            ULARGE_INTEGER li; li.QuadPart = ChangeTime != 0 ? ChangeTime : LastWriteTime;
            wt.dwLowDateTime = li.LowPart; wt.dwHighDateTime = li.HighPart;
        }

        SetFileTime(file, setCt ? &ct : nullptr, setAt ? &at : nullptr, setWt ? &wt : nullptr);
        CloseHandle(file);
    }

    bool isDir = false;
    GetRealFileInfo(ctx->realPath, FileInfo, &isDir);
    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::SetFileSize(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx || ctx->isDirectory) return STATUS_INVALID_DEVICE_REQUEST;

    if (!SetAllocationSize)
    {
        HANDLE file = CreateFileW(
            ctx->realPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER li; li.QuadPart = NewSize;
            SetFilePointerEx(file, li, nullptr, FILE_BEGIN);
            SetEndOfFile(file);
            CloseHandle(file);
        }
    }

    bool isDir = false;
    GetRealFileInfo(ctx->realPath, FileInfo, &isDir);
    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::CanDelete(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;

    DWORD attr = GetFileAttributesW(ctx->realPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return STATUS_OBJECT_NAME_NOT_FOUND;

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::Rename(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
    auto* self = Self(FileSystem);
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);

    std::wstring oldPath = ctx->realPath;
    std::wstring newVirtPath = NewFileName;
    std::wstring newPath = self->VirtualToRealPath(newVirtPath);

    DWORD flags = ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0;
    if (!MoveFileExW(oldPath.c_str(), newPath.c_str(), flags))
    {
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS) return STATUS_OBJECT_NAME_COLLISION;
        return STATUS_ACCESS_DENIED;
    }

    ctx->realPath = newPath;
    std::wstring remoteOld = ctx->virtualPath;
    if (!remoteOld.empty() && remoteOld.front() == L'\\') remoteOld.erase(0, 1);
    ctx->virtualPath = newVirtPath;
    
    std::wstring remoteNew = newVirtPath;
    if (!remoteNew.empty() && remoteNew.front() == L'\\') remoteNew.erase(0, 1);
    if (self->cacheManager_) self->cacheManager_->QueueRename(remoteOld, remoteNew);

    return STATUS_SUCCESS;
}

NTSTATUS VirtualFs::GetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize) { return STATUS_ACCESS_DENIED; }
NTSTATUS VirtualFs::SetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor) { return STATUS_ACCESS_DENIED; }

VOID VirtualFs::Cleanup(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PWSTR FileName,
    ULONG Flags)
{
    auto* ctx = static_cast<VirtualFs::FileContext*>(FileContext);
    if (!ctx) return;

    if (Flags & FspCleanupDelete)
    {
        if (ctx->isDirectory)
        {
            RemoveDirectoryW(ctx->realPath.c_str());
        }
        else
        {
            DeleteFileW(ctx->realPath.c_str());
        }

        auto* self = Self(FileSystem);
        std::wstring remoteName = ctx->virtualPath;
        if (!remoteName.empty() && remoteName.front() == L'\\') remoteName.erase(0, 1);
        if (!remoteName.empty() && self->cacheManager_) self->cacheManager_->QueueDelete(remoteName);
    }
}












