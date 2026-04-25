#pragma once

#include <windows.h>
#include <winfsp/winfsp.h>

#include <memory>
#include <string>
#include <vector>

class VirtualFs
{
public:
    VirtualFs();
    ~VirtualFs();

    NTSTATUS Start(const std::wstring& mountPoint);
    void Stop();

private:
    struct Node
    {
        std::wstring path;      // full path: "\" or "\hello.txt"
        std::wstring name;      // display name
        bool isDirectory = false;
        std::vector<std::uint8_t> data;
        FILETIME creationTime{};
        FILETIME lastAccessTime{};
        FILETIME lastWriteTime{};
        FILETIME changeTime{};
        UINT32 attributes = 0;
    };

    struct FileContext
    {
        Node* node = nullptr;
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

    static VirtualFs* Self(FSP_FILE_SYSTEM* fs);
    Node* FindNode(const std::wstring& path);
    const Node* FindNode(const std::wstring& path) const;
    static void FillFileInfo(const Node& node, FSP_FSCTL_FILE_INFO* fileInfo);

    static std::wstring NormalizePath(const std::wstring& path);

private:
    FSP_FILE_SYSTEM* fs_ = nullptr;
    std::wstring mountPoint_;

    std::unique_ptr<std::byte[]> securityDescriptorStorage_;
    SIZE_T securityDescriptorSize_ = 0;

    Node root_;
    Node hello_;
};