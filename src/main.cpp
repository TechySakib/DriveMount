#include "virtual_fs.h"

#include <windows.h>
#include <winfsp/winfsp.h>

#include <iostream>
#include <memory>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    std::wcout << L"DriveMount starting...\n";

    NTSTATUS loadStatus = FspLoad(0);
    if (!NT_SUCCESS(loadStatus))
    {
        std::wcerr << L"FspLoad failed: 0x" << std::hex << loadStatus << L"\n";
        return 1;
    }

    std::wstring mountPoint = L"X:";
    if (argc >= 2)
        mountPoint = argv[1];

    std::wcout << L"Requested mount point: [" << mountPoint << L"]\n";

    auto fs = std::make_unique<VirtualFs>();

    NTSTATUS status = fs->Start(mountPoint);
    if (!NT_SUCCESS(status))
    {
        std::wcerr << L"Start failed: 0x" << std::hex << status << L"\n";
        std::wcout << L"Press Enter to exit...\n";
        std::wstring dummy;
        std::getline(std::wcin, dummy);
        return 1;
    }

    std::wcout << L"DriveMount mounted at " << mountPoint << L"\n";
    std::wcout << L"Keep this window open.\n";
    std::wcout << L"Press Enter to unmount.\n";

    std::wstring dummy;
    std::getline(std::wcin, dummy);

    fs->Stop();

    std::wcout << L"Unmounted.\n";
    return 0;
}