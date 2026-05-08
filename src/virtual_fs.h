#pragma once

#include <windows.h>
#include <winfsp/winfsp.h>

#include <memory>
#include <string>

class VirtualFs
{
public:
    VirtualFs();
    ~VirtualFs();

    NTSTATUS Start(const std::wstring& mountPoint);
    void Stop();

private:
    struct FileContext
    {
        std::wstring realPath;
        bool isDirectory = false;
    };

    static NTSTATUS GetVolumeInfo(
        FSP_FILE_SYSTEM* FileSystem,
        FSP_FSCTL_VOLUME_INFO* VolumeInfo);

    static NTSTATUS GetSecurityByName(
        FSP_FILE_SYSTEM* FileSystem,
        PWSTR FileName,
        PUINT32 PFileAttributes,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        SIZE_T* PSecurityDescriptorSize);

    static NTSTATUS Open(
        FSP_FILE_SYSTEM* FileSystem,
        PWSTR FileName,
        UINT32 CreateOptions,
        UINT32 GrantedAccess,
        PVOID* PFileContext,
        FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS Create(
        FSP_FILE_SYSTEM* FileSystem,
        PWSTR FileName,
        UINT32 CreateOptions,
        UINT32 GrantedAccess,
        UINT32 FileAttributes,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        UINT64 AllocationSize,
        PVOID* PFileContext,
        FSP_FSCTL_FILE_INFO* FileInfo);

    static VOID Cleanup(
        FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext,
        PWSTR FileName,
        ULONG Flags);

    static VOID Close(
        FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext);

    static NTSTATUS GetFileInfo(
        FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext,
        FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS Read(
        FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext,
        PVOID Buffer,
        UINT64 Offset,
        ULONG Length,
        PULONG PBytesTransferred);

    static NTSTATUS ReadDirectory(
        FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext,
        PWSTR Pattern,
        PWSTR Marker,
        PVOID Buffer,
        ULONG Length,
        PULONG PBytesTransferred);

    static NTSTATUS Overwrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO *FileInfo);
    static NTSTATUS Write(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo, PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO *FileInfo);
    static NTSTATUS Flush(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo);
    static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes, UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime, FSP_FSCTL_FILE_INFO *FileInfo);
    static NTSTATUS SetFileSize(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo);
    static NTSTATUS CanDelete(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName);
    static NTSTATUS Rename(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists);
    static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize);
    static NTSTATUS SetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor);

    static VirtualFs* Self(FSP_FILE_SYSTEM* fs);

    std::wstring VirtualToRealPath(const std::wstring& virtualPath) const;

    static bool GetRealFileInfo(
        const std::wstring& realPath,
        FSP_FSCTL_FILE_INFO* fileInfo,
        bool* isDirectory);

private:
    FSP_FILE_SYSTEM* fs_ = nullptr;
    std::wstring mountPoint_;

    std::wstring basePath_ = L"F:\\DriveMountData";

    std::unique_ptr<std::byte[]> securityDescriptorStorage_;
    SIZE_T securityDescriptorSize_ = 0;
};
