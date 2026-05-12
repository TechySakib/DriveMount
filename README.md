# DriveMount ??

**DriveMount** is a high-performance Windows virtual filesystem (VFS) that mounts your **Google Drive** directly as a local disk drive (e.g., `M:`). Unlike the official client, DriveMount is designed for efficiency, using on-demand streaming, sparse file technology, and real-time polling to provide a seamless cloud-to-local experience.

---

## ?? Key Features

- **Native Windows Integration**: Fully compliant with Windows Explorer, CMD, and PowerShell. 
- **Subdirectory & Folder Support**: Navigate, create, delete, and rename nested folder hierarchies natively.
- **On-Demand File Streaming**: Access large files instantly. Data chunks are fetched from the cloud in real-time as you read them, using the HTTP `Range` header.
- **Local Metadata DB**: Uses **SQLite** to cache file structures and sync states, enabling near-instant directory browsing without hitting API rate limits.
- **Real-Time Polling**: Detects deletions, updates, and renames made via the Google Drive web interface and reflects them locally within seconds.
- **Automatic Cache Eviction**: Intelligent **LRU (Least Recently Used)** cache management ensures your local storage usage stays within a configurable limit (default 1GB).
- **Sparse File Technology**: Uses Windows Sparse Files to allocate disk space only for the data actually downloaded.

---

## ?? Technologies Used

- **C++20**: The core logic is built with modern C++ for high performance.
- **WinFsp**: The Windows File System Proxy used to create the virtual drive.
- **Google Drive REST API (v3)**: Direct integration with Google's cloud storage.
- **SQLite3**: Local persistent metadata storage.
- **WinHTTP**: Efficient, native Windows networking for API communication and streaming.
- **nlohmann/json**: Modern JSON parsing for API responses.

---

## ?? Project Structure

```text
DriveMount/
??? src/
?   ??? main.cpp                # Entry point & Mount logic
?   ??? virtual_fs.cpp/h        # WinFsp callback implementation
?   ??? cache_manager.cpp/h      # Background sync, polling & eviction engine
?   ??? google_drive_client.cpp/h # Google Drive REST API wrapper
?   ??? metadata_db.cpp/h       # SQLite metadata management
?   ??? sqlite3.c/h             # Embedded SQLite source
??? CMakeLists.txt              # Build configuration
??? README.md                   # You are here!
```

---

## ?? How to Run the Project

### 1. Prerequisites
- **WinFsp**: Download and install from [winfsp.dev](https://winfsp.dev/).
- **Visual Studio 2022**: With C++ development workloads.
- **CMake**: Version 3.21 or higher.

### 2. Google Drive API Setup
- Go to the [Google Cloud Console](https://console.cloud.google.com/).
- Create a new project and enable the **Google Drive API**.
- Create **OAuth 2.0 Credentials** (Desktop App).
- Set the following environment variables on your Windows machine:
  ```powershell
  $env:DRIVEMOUNT_CLIENT_ID = "your_client_id"
  $env:DRIVEMOUNT_CLIENT_SECRET = "your_client_secret"
  ```

### 3. Build & Run
```powershell
# Create build directory
mkdir build
cd build

# Configure and Build
cmake ..
cmake --build . --config Release

# Run (Mount to M: drive)
.\Release\DriveMount.exe M:
```

---

## ?? Roadmap & Future Plans

- [x] **Phase 1-6**: Core Filesystem & Basic API Sync.
- [x] **Phase 7**: SQLite Metadata Persistence.
- [x] **Phase 8**: Partial File Streaming (Range Requests).
- [x] **Phase 9**: Real-time Cloud Polling.
- [x] **Phase 10**: Automatic LRU Cache Eviction.
- [ ] **Phase 11**: Multi-Threaded Chunk Downloading.
- [ ] **Phase 12**: Windows Search Indexer Integration.
- [ ] **Phase 13**: Selective Sync (Mark folders for "Always Keep Offline").
- [ ] **Phase 14**: System Tray UI for status monitoring.

---

## ?? Contributing
Feel free to fork the repository and submit pull requests. For major changes, please open an issue first to discuss what you would like to change.

## ?? License
Distributed under the MIT License.
