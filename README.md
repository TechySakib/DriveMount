# DriveMount

**DriveMount** is a C++ Windows Virtual Filesystem application built on top of [WinFsp](https://winfsp.dev/). It allows you to mount a remote cloud drive (like Google Drive) as a native local disk volume (e.g., M:\) in Windows Explorer. 

Users can seamlessly browse, open, edit, create, and delete files inside this virtual drive just as they would with a physical USB stick or hard drive. 

## ??? Architecture

The system operates using a three-tier architecture:

1. **Virtual Filesystem Layer (VirtualFs)**: A high-performance WinFsp driver wrapper that translates native Windows I/O operations (like CreateFile, ReadFile, WriteFile, MoveFile) into backend cache operations.
2. **Cache Manager (CacheManager)**: An asynchronous, multi-threaded background service. It utilizes a concurrent task queue to intercept local filesystem events and asynchronously propagate them to the cloud without blocking Windows Explorer.
3. **Cloud Client (GoogleDriveClient)**: The networking layer responsible for OAuth authentication and REST API communication with Google Drive. *(Implemented using WinHTTP and nlohmann/json for direct communication with the Google Drive v3 REST API).*

## ? Features

- **Native Windows Integration**: Fully compliant with Windows Explorer, CMD, and PowerShell. 
- **Read & Write Support**: Modify text files, save images, and seamlessly interact with cloud files locally.
- **Asynchronous Sync**: File uploads, deletions, and renames are processed in the background by the CacheManager to ensure the OS UI never freezes.
- **Root Directory Security**: Configured with a rigid SDDL (Security Descriptor) granting Built-in Administrators full access to ensure smooth native I/O.

## ??? Prerequisites

To build and run this project, you need:

- **Windows 10 / 11**
- [**WinFsp**](https://winfsp.dev/) (Windows File System Proxy) installed on your machine.
- **CMake** (v3.21 or higher)
- **Visual Studio Build Tools 2026** (C++20 support required)

## ?? Build Instructions

1. Clone the repository:
   `cmd
   git clone https://github.com/TechySakib/DriveMount.git
   cd DriveMount
   `

2. Build the project using CMake:
   `cmd
   cmake --build build --config Release
   `

## ?? Usage

Once built, you can start the virtual drive by specifying a mount point letter:

`cmd
.\build\Release\DriveMount.exe M:
`

- A console window will remain open detailing background synchronization logs.
- Open **Windows Explorer** and navigate to M:\.
- You will see the mock file welcome_to_drive.txt. You can edit this file, create new files, or delete them.
- Look at the console window to observe the asynchronous sync events in real-time!
- To unmount the drive, simply focus the console window and press Enter.

## ??? Roadmap

- [x] **Phase 1**: WinFsp Virtual Disk integration and directory enumeration.
- [x] **Phase 2**: Full local Read/Write/Rename/Delete file support.
- [x] **Phase 3**: Background Cache Manager thread and Mock Cloud sync.
- [x] **Phase 4**: Replaced Mock GoogleDriveClient with real WinHTTP Google Drive REST API integration.
- [x] **Phase 5**: Real On-Demand File Fetching (creates sparse offline files and downloads content upon file open).
- [ ] **Phase 6**: Subdirectory & Folder Support (navigate, create, and delete nested folders).
- [ ] **Phase 7**: File Streaming (partial file reads/writes instead of full-file caching).
- [ ] **Phase 8**: Cloud-to-Local Polling (sync changes made from Google Drive website).
- [ ] **Phase 9**: Cache Eviction (automatically delete old cached files to free up disk space).

