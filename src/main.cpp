#include "virtual_fs.h"

#include <windows.h>
#include <winfsp/winfsp.h>

#include <iostream>
#include <memory>
#include <string>

namespace
{
    VirtualFs* g_fs = nullptr;

    BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
    {
        switch (ctrlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_fs)
                g_fs->Stop();
            return TRUE;
        default:
            return FALSE;
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (!NT_SUCCESS(FspLoad(0)))
    {
        std::wcerr << L"Failed to load WinFsp runtime.\n";
        return ERROR_DELAY_LOAD_FAILED;
    }

    std::wstring mountPoint = L"G:";
    if (argc >= 2)
        mountPoint = argv[1];

    auto fs = std::make_unique<VirtualFs>();
    g_fs = fs.get();

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
    {
        std::wcerr << L"Failed to install console control handler.\n";
        return 1;
    }

    NTSTATUS status = fs->Start(mountPoint);
    if (!NT_SUCCESS(status))
    {
        std::wcerr << L"Failed to mount filesystem. NTSTATUS=0x"
                   << std::hex << static_cast<unsigned long>(status) << L"\n";
        return 1;
    }

    std::wcout << L"DriveMount mounted at " << mountPoint << L"\n";
    std::wcout << L"Open Explorer and browse the drive.\n";
    std::wcout << L"Press Enter to unmount.\n";
    std::wstring line;
    std::getline(std::wcin, line);

    fs->Stop();
    g_fs = nullptr;
    return 0;
}