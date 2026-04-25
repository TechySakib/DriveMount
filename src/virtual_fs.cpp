#include "virtual_fs.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kVolumeLabel[] = L"DriveMount";
    constexpr wchar_t kFileSystemName[] = L"DriveMountFS";

    UINT64 ToUInt64(const FILETIME& ft)
    {
        ULARGE_INTEGER li{};
        li.LowPart = ft.dwLowDateTime;
        li.HighPart = ft.dwHighDateTime;
        return li.QuadPart;
    }
}

VirtualFs::VirtualFs()
{
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);

    root_.path = L"\\";
    root_.name = L"";
    root_.isDirectory = true;
    root_.attributes = FILE_ATTRIBUTE_DIRECTORY;
    root_.creationTime = now;
    root_.lastAccessTime = now;
    root_.lastWriteTime = now;
    root_.changeTime = now;

    hello_.path = L"\\hello.txt";
    hello_.name = L"hello.txt";
    hello_.isDirectory = false;
    hello_.attributes = FILE_ATTRIBUTE_ARCHIVE;
    hello_.creationTime = now;
    hello_.lastAccessTime = now;
    hello_.lastWriteTime = now;
    hello_.changeTime = now;

    const char* text =
        "DriveMount is mounted.\r\n"
        "\r\n"
        "Next step: replace this in-memory file with your Google Drive cache layer.\r\n";
    hello_.data.assign(text, text + std::strlen(text));

    PSECURITY_DESCRIPTOR sd = nullptr;
    ULONG sdSize = 0;

    // Read access for everyone, full control for admins/system.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)",
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

std::wstring VirtualFs::NormalizePath(const std::wstring& path)
{
    if (path.empty() || path == L"\\")
        return L"\\";

    std::wstring out = path;
    std::replace(out.begin(), out.end(), L'/', L'\\');

    if (out.front() != L'\\')
        out.insert(out.begin(), L'\\');

    while (out.size() > 1 && out.back() == L'\\')
        out.pop_back();

    return out;
}

VirtualFs* VirtualFs::Self(FSP_FILE_SYSTEM* fs)
{
    return static_cast<VirtualFs*>(fs->UserContext);
}

VirtualFs::Node* VirtualFs::FindNode(const std::wstring& path)
{
    const auto normalized = NormalizePath(path);
    if (normalized == root_.path)
        return &root_;
    if (_wcsicmp(normalized.c_str(), hello_.path.c_str()) == 0)
        return &hello_;
    return nullptr;
}

const VirtualFs::Node* VirtualFs::FindNode(const std::wstring& path) const
{
    const auto normalized = NormalizePath(path);
    if (normalized == root_.path)
        return &root_;
    if (_wcsicmp(normalized.c_str(), hello_.path.c_str()) == 0)
        return &hello_;
    return nullptr;
}

void VirtualFs::FillFileInfo(const Node& node, FSP_FSCTL_FILE_INFO* fileInfo)
{
    std::memset(fileInfo, 0, sizeof(*fileInfo));

    fileInfo->FileAttributes = node.attributes;
    fileInfo->CreationTime = ToUInt64(node.creationTime);
    fileInfo->LastAccessTime = ToUInt64(node.lastAccessTime);
    fileInfo->LastWriteTime = ToUInt64(node.lastWriteTime);
    fileInfo->ChangeTime = ToUInt64(node.changeTime);

    if (node.isDirectory)
    {
        fileInfo->AllocationSize = 0;
        fileInfo->FileSize = 0;
    }
    else
    {
        fileInfo->AllocationSize = static_cast<UINT64>(node.data.size());
        fileInfo->FileSize = static_cast<UINT64>(node.data.size());
    }

    fileInfo->IndexNumber = node.isDirectory ? 1 : 2;
    fileInfo->HardLinks = 1;
}

NTSTATUS VirtualFs::Start(const std::wstring& mountPoint)
{
    if (fs_ != nullptr)
        return STATUS_DEVICE_BUSY;

    mountPoint_ = mountPoint;

    FSP_FSCTL_VOLUME_PARAMS volumeParams{};
    volumeParams.SectorSize = 4096;
    volumeParams.SectorsPerAllocationUnit = 1;
    volumeParams.VolumeCreationTime = ToUInt64(root_.creationTime);
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
    iface.Close = &VirtualFs::Close;
    iface.GetFileInfo = &VirtualFs::GetFileInfo;
    iface.Read = &VirtualFs::Read;
    iface.ReadDirectory = &VirtualFs::ReadDirectory;

    NTSTATUS status = FspFileSystemCreate(
        L"" FSP_FSCTL_DISK_DEVICE_NAME,
        &volumeParams,
        &iface,
        &fs_);

    if (!NT_SUCCESS(status))
        return status;

    fs_->UserContext = this;

    status = FspFileSystemSetMountPoint(fs_, const_cast<PWSTR>(mountPoint_.c_str()));
    if (!NT_SUCCESS(status))
    {
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        return status;
    }

    status = FspFileSystemStartDispatcher(fs_, 0);
    if (!NT_SUCCESS(status))
    {
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        return status;
    }

    return STATUS_SUCCESS;
}

void VirtualFs::Stop()
{
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
    auto* self = Self(FileSystem);
    (void)self;

    std::memset(VolumeInfo, 0, sizeof(*VolumeInfo));

    VolumeInfo->TotalSize = 1024ull * 1024ull * 1024ull;
    VolumeInfo->FreeSize = 1024ull * 1024ull * 1024ull - 4096ull;

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
    auto* node = self->FindNode(FileName ? FileName : L"\\");
    if (!node)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (PFileAttributes)
        *PFileAttributes = node->attributes;

    if (!PSecurityDescriptorSize)
        return STATUS_SUCCESS;

    if (SecurityDescriptor == nullptr || *PSecurityDescriptorSize < self->securityDescriptorSize_)
    {
        *PSecurityDescriptorSize = self->securityDescriptorSize_;
        return STATUS_BUFFER_OVERFLOW;
    }

    std::memcpy(SecurityDescriptor,
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
    auto* node = self->FindNode(FileName ? FileName : L"\\");

    if (!node)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    const bool wantsDirectory = (CreateOptions & FILE_DIRECTORY_FILE) != 0;
    if (wantsDirectory && !node->isDirectory)
        return STATUS_NOT_A_DIRECTORY;
    if (!wantsDirectory && node->isDirectory && NormalizePath(FileName ? FileName : L"\\") != L"\\")
        return STATUS_FILE_IS_A_DIRECTORY;

    auto* ctx = new FileContext{};
    ctx->node = node;
    *PFileContext = ctx;

    FillFileInfo(*node, FileInfo);
    return STATUS_SUCCESS;
}

VOID VirtualFs::Close(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext)
{
    (void)FileSystem;
    auto* ctx = static_cast<FileContext*>(FileContext);
    delete ctx;
}

NTSTATUS VirtualFs::GetFileInfo(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    (void)FileSystem;
    auto* ctx = static_cast<FileContext*>(FileContext);
    if (!ctx || !ctx->node)
        return STATUS_INVALID_HANDLE;

    FillFileInfo(*ctx->node, FileInfo);
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
    auto* ctx = static_cast<FileContext*>(FileContext);
    if (!ctx || !ctx->node || ctx->node->isDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;

    const auto& data = ctx->node->data;
    if (Offset >= data.size())
    {
        *PBytesTransferred = 0;
        return STATUS_SUCCESS;
    }

    const size_t remaining = data.size() - static_cast<size_t>(Offset);
    const size_t toCopy = std::min<size_t>(remaining, Length);

    std::memcpy(Buffer, data.data() + static_cast<size_t>(Offset), toCopy);
    *PBytesTransferred = static_cast<ULONG>(toCopy);
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
    (void)Pattern;

    auto* self = Self(FileSystem);
    auto* ctx = static_cast<FileContext*>(FileContext);
    if (!ctx || !ctx->node || !ctx->node->isDirectory)
        return STATUS_NOT_A_DIRECTORY;

    *PBytesTransferred = 0;

    struct EntryView
    {
        const wchar_t* name;
        const Node* node;
    };

    std::vector<EntryView> entries = {
        {L".", &self->root_},
        {L"..", &self->root_},
        {self->hello_.name.c_str(), &self->hello_},
    };

    bool start = (Marker == nullptr || Marker[0] == L'\0');

    for (const auto& entry : entries)
    {
        if (!start)
        {
            if (_wcsicmp(entry.name, Marker) > 0)
                start = true;
            else
                continue;
        }

        const ULONG nameBytes = static_cast<ULONG>(wcslen(entry.name) * sizeof(wchar_t));
        const ULONG dirInfoSize = sizeof(FSP_FSCTL_DIR_INFO) + nameBytes;

        std::vector<std::byte> temp(dirInfoSize);
        auto* dirInfo = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(temp.data());
        std::memset(dirInfo, 0, dirInfoSize);

        dirInfo->Size = dirInfoSize;
        FillFileInfo(*entry.node, &dirInfo->FileInfo);
        dirInfo->FileNameBuf.Size = nameBytes;
        std::memcpy(dirInfo->FileNameBuf.Buffer, entry.name, nameBytes);

        if (!FspFileSystemAddDirInfo(dirInfo, Buffer, Length, PBytesTransferred))
            return STATUS_SUCCESS;
    }

    FspFileSystemAddDirInfo(nullptr, Buffer, Length, PBytesTransferred);
    return STATUS_SUCCESS;
}