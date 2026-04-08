# OpenDriver 👓

**OpenDriver** is a modular, high-performance, and cross-platform virtual reality driver wrapper for **SteamVR / OpenVR**. Completely rewritten in modern C++ (from its earlier Rust implementation), it bypasses common SteamVR errors (such as Linux Compositor Error 303 / DRM leasing issues) and serves as a powerful foundation for developers to inject custom trackers, hands, or virtual HMDs into the SteamVR environment using a headless dependency-injected plugin system.

---

## ⚡ Features

- **Cross-Platform**: Fully supports both Windows and Linux out of the box.
- **Advanced Video Pipelines**:
  - **Windows**: Zero-copy hardware encoding using DirectX 11 / DXGI shared handles coupled with Media Foundation API.
  - **Linux**: CPU-based highly-optimized `x264` encoding pipeline bypassing standard constraints using low-level `dma-buf` memory mapping. *(Note: HMD emulation is currently disabled on Linux, support may be added in the future!)*
  - **DRM Bypass (Linux)**: Ships with `opendriver_shim` (`LD_PRELOAD`) to spoof DRM Leasing, forcing SteamVR into working on Linux virtual displays.
- **Robust IPC & GUI Architecture**: The actual OpenVR `.so`/`.dll` injected into `vrserver` is kept extremely lightweight. The heavy lifting (and the Qt6 debugging interface) operates securely in `opendriver_runner` using Named Pipes / Unix Sockets.
- **Infinite Modularity**: Instantly add arbitrary hardware (smartphones, custom gloves, OpenCV camera tracking) using isolated `SHARED_LIBRARY` plugins that never compel you to mess with SteamVR API structs.

---

## 🛠️ Building the Project

The CMake build automatically routes the resulting compiled binaries into the `driver/bin/` folder. This means **the repository itself acts as the SteamVR driver folder**! You can register it immediately without copying build files.

### Windows (Native — Recommended)
Ensure you have **Visual Studio 2022** installed with "Desktop development with C++" and CMake enabled. If you want the GUI dashboard, install Qt6 for MSVC 2019/2022 64-bit.
```cmd
cd C:\path\to\opendriver
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release -DQt6_DIR="C:\Qt\6.x\msvc2019_64\lib\cmake\Qt6"
cmake --build build --config Release --parallel
```
*(For detailed Windows quirks, check out `BUILDING_WINDOWS.md`).*

### Linux 
To build natively on Linux, ensure you have a standard GCC/Clang toolkit installed.
```bash
git clone --recursive <opendriver-url>
cd opendriver
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```
OpenDriver includes its own static fetch configurations for `FFmpeg` and `x264` under Linux to prevent any dependency hell.

---

## 📦 Registering with SteamVR

Once compiled, simply inform SteamVR where your driver resides.

**Via Command Line (Recommended):**
```bash
# Windows
"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" adddriver "C:\path\to\opendriver\driver"

# Linux
~/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/vrpathreg.sh adddriver "/path/to/opendriver/driver"
```
Or do it manually through the SteamVR GUI settings (*Settings -> OpenVR -> Manage Add-Ons*).

---

## 🔌 Developing Custom Plugins (API)

Tired of learning the monolithic OpenVR API? OpenDriver provides a vastly simplified dependency-injected `IPluginContext` model. 

### Why Use OpenDriver Plugins?
1. **No Core Linking Required**: Your plugin is compiled in a **100% standalone** repository. You do not link to the OpenDriver libraries. The context pointer is assigned dynamically at runtime via C-ABI (`dlopen`/`LoadLibrary`).
2. **Hot-Reloading**: Altered a file and recompiled your `.dll`? OpenDriver runner will snap the new plugin into SteamVR while it's still running, saving variables via `ExportState()`/`ImportState()`.
3. **Thread Safety**: Dealing with async Bluetooth sockets? Use `m_context->PostToMainThread()` to seamlessly merge it back into the frame-accurate engine loop.

### Quick Start Standalone Plugin
1. Copy the `include/opendriver/core/` folder from this repo to your standalone plugin project's `include/` folder.
2. Build a standard shared library exposing `CreatePlugin` and `DestroyPlugin`.

**my_plugin.cpp**
```cpp
#include <opendriver/core/plugin_interface.h>

using namespace opendriver::core;

class MyPlugin : public IPlugin {
    IPluginContext* m_context = nullptr;

public:
    const char* GetName() const override { return "my_cool_tracker"; }
    const char* GetVersion() const override { return "1.0.0"; }
    const char* GetDescription() const override { return "Reads IMU data"; }
    const char* GetAuthor() const override { return "You"; }

    bool OnInitialize(IPluginContext* context) override {
        m_context = context;
        
        // Let's register a floating wand!
        Device tracker;
        tracker.id = "my_wand_01";
        tracker.type = DeviceType::GENERIC_TRACKER;
        m_context->RegisterDevice(tracker);
        
        return true; 
    }

    void OnTick(float delta_time) override {
        // Automatically handled ~90 times a second
        // Set the wand to hover 1.5 meters up!
        m_context->UpdatePose("my_wand_01", 0.0, 1.5, 0.0, 1.0, 0.0, 0.0, 0.0);
    }
    
    void OnShutdown() override {}
    void OnEvent(const Event& event) override {}
    bool IsActive() const override { return true; }
};

// C-API Export
extern "C" {
#if defined(_WIN32)
    __declspec(dllexport) IPlugin* CreatePlugin() { return new MyPlugin(); }
    __declspec(dllexport) void DestroyPlugin(IPlugin* ptr) { delete ptr; }
#else
    __attribute__((visibility("default"))) IPlugin* CreatePlugin() { return new MyPlugin(); }
    __attribute__((visibility("default"))) void DestroyPlugin(IPlugin* ptr) { delete ptr; }
#endif
}
```

**CMakeLists.txt**
```cmake
cmake_minimum_required(VERSION 3.16)
project(my_tracking_hardware)

add_library(my_tracker SHARED src/my_plugin.cpp)
target_include_directories(my_tracker PRIVATE "include/")

# DO NOT LINK ANYTHING! NO target_link_libraries()
```
Drop `my_tracker.dll` (or `.so`) into OpenDriver's `plugins/` directory and spin up SteamVR. That's it!

---

*OpenDriver replaces a previously heavy Rust FFI system with native C++ stability. It heavily leverages `nlohmann::json` and `spdlog` for maximum configuration efficiency and monitoring.*
