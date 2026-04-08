# OpenDriver - Documentation

This documentation covers how to build the OpenDriver system and provides a quick-start guide for creating custom plugins.

---

## 1. System Compilation

Thanks to the updated `CMakeLists.txt`, all compiled binaries (the driver and the runner) automatically output into the `driver/bin/<platform>/` folder in the root directory. 
This structure allows you to **immediately register the `driver` folder with SteamVR**, without manually moving files around.

### Build Instructions

#### 🪟 Windows (Native - Recommended)
Ensure you have **Visual Studio 2022** installed with the "Desktop development with C++" workload and CMake support.
If you want to build the Qt6 GUI, you must install Qt6 (e.g., via the Qt Online Installer, selecting the MSVC 2019/2022 64-bit component).

Open the `Developer Command Prompt for VS 2022` and run:
```cmd
cd C:\path\to\opendriver
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release -DQt6_DIR="C:\Qt\6.x\msvc2019_64\lib\cmake\Qt6"
cmake --build build --config Release --parallel
```
The resulting `driver_opendriver.dll` and `opendriver_runner.exe` will be located in `driver/bin/win64/`.

#### 🐧 Linux (Native)
On Linux, a standard GCC/Clang toolchain with CMake is sufficient.
```bash
cd opendriver
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```
The resulting `.so` and binaries will be located in `driver/bin/linux64/`.

#### 🌐 Cross-Compiling on Linux (For Windows target)
You can compile Windows `.dll` and `.exe` binaries from a Linux machine using MinGW-w64.
*Note: This requires a cross-compiled Qt6 version for Windows. If you don't have one, append `-DBUILD_GUI=OFF` to disable the Qt GUI build.*

Create a `mingw-toolchain.cmake` file with MinGW compiler targets, then run:
```bash
cmake -B build_win -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build_win
```

---

## 2. Writing Basic Plugins

Plugins for OpenDriver are standard shared libraries (Dynamic Link Libraries - `.dll` on Windows, `.so` on Linux). 
Every plugin must inherit from the `IPlugin` class. Through the `IPluginContext` parameter, the plugin gains complete access to core features including the device registry, logging, and events.

### Basic C++ Plugin Skeleton

Include `#include <opendriver/core/plugin_interface.h>` and override the virtual methods:

```cpp
#include <opendriver/core/plugin_interface.h>

using namespace opendriver::core;

class MyTestPlugin : public IPlugin {
public:
    // M E T A D A T A
    const char* GetName() const override { return "my_test_plugin"; }
    const char* GetVersion() const override { return "1.0.0"; }
    const char* GetDescription() const override { return "My test motion plugin."; }
    const char* GetAuthor() const override { return "John Doe"; }

    // I N I T I A L I Z A T I O N
    bool OnInitialize(IPluginContext* context) override {
        m_context = context;
        m_context->LogInfo("MyTestPlugin successfully initialized!");

        // Example: Registering a virtual device
        // Device controller;
        // controller.id = "my_tracker_1"; ...
        // context->RegisterDevice(controller);

        return true; 
    }

    void OnShutdown() override {
        m_context->LogInfo("MyTestPlugin successfully shut down.");
    }

    // M A I N   L O O P   (Tick)
    void OnTick(float delta_time) override {
        // Read sockets, update physics, compute positions here.
        // Example pose update:
        /* 
        m_context->UpdatePose("my_tracker_1", 
                    0.0, 1.5, 0.0, // x, y, z in meters
                    1.0, 0.0, 0.0, 0.0 // w, x, y, z unit quaternion
        ); 
        */
    }

    // E V E N T S
    void OnEvent(const Event& event) override {
        // Handle subscribed events (e.g., SteamVR haptic feedback)
    }

    bool IsActive() const override { return true; }
    std::string GetStatus() const override { return "Online and steady"; }

private:
    IPluginContext* m_context = nullptr;
};

// ============================================================================
// C-EXPORTS REQUIRED BY THE LOADER
// ============================================================================
extern "C" {
#if defined(_WIN32)
    __declspec(dllexport) IPlugin* CreatePlugin() {
#else
    __attribute__((visibility("default"))) IPlugin* CreatePlugin() {
#endif
        return new MyTestPlugin();
    }

#if defined(_WIN32)
    __declspec(dllexport) void DestroyPlugin(IPlugin* plugin) {
#else
    __attribute__((visibility("default"))) void DestroyPlugin(IPlugin* plugin) {
#endif
        delete plugin;
    }
}
```

### Standalone Compilation (CMake Settings for Plugins)
One of the best design features is that you do **not** need to compile your plugin within the OpenDriver environment or link against its core libraries! You can place your plugin in a completely separate and isolated GitHub repository.

**How to do it setup a standalone plugin:**
1. Copy the `include/opendriver/core/` headers from this repository into your new plugin's standalone source folder (e.g., placing them inside an `include/` directory).
2. Set up your standalone `CMakeLists.txt` to compile as a **SHARED** library.

Here's an exact minimal `CMakeLists.txt` for your standalone project:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_test_plugin)

# 1. We create a Dynamic Library (.dll / .so)
add_library(my_test_plugin SHARED my_test_plugin.cpp)

# 2. We point the compiler to the copied OpenDriver headers
target_include_directories(my_test_plugin PRIVATE "include/")

# 3. NOTICE: We leave out target_link_libraries()!
# The core system loads the DLL dynamically and links the context pointer at runtime via C-ABI!
```
