# OpenDriver-VR

OpenDriver-VR is a native SteamVR/OpenVR driver project built in modern C++. The repository contains the full SteamVR driver payload in `driver/`, the runtime process launched by the driver, and an optional plugin system for adding devices such as HMDs, controllers, trackers, and custom integrations.

The current Windows layout is designed so the repo itself can be registered directly as a SteamVR driver after build. You do not need to manually assemble a separate driver folder.

## Current Focus

- Native SteamVR driver in `driver/`
- Windows runtime in `driver/bin/win64/`
- Optional plugin loading from `%APPDATA%\opendriver\plugins`
- Qt-based runner/dashboard bundled with `opendriver_runner.exe`
- Media Foundation based Windows video path in the driver

## Repository Layout

```text
OpenDriver-VR/
  driver/
    driver.vrdrivermanifest
    bin/win64/
      driver_opendriver.dll
      opendriver_runner.exe
  include/
  src/
  plugins/
  scripts/
    install_driver.ps1
    install_driver_only.ps1
  docs/
```

## Windows Build

Requirements:

- Visual Studio 2022 with `Desktop development with C++`
- CMake 3.16+
- Qt6 for MSVC x64

Example configure/build:

```powershell
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release -DQt6_DIR="C:\Qt\6.x\msvc2022_64\lib\cmake\Qt6"
cmake --build build --config Release --parallel
```

Build output is copied into `driver/bin/win64/`.

More Windows notes: [docs/BUILDING_WINDOWS.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/BUILDING_WINDOWS.md)

## SteamVR Installation

### Driver only

Registers only the native SteamVR driver and does not deploy any plugins:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_driver_only.ps1
```

Build first, then install in one step:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_driver_only.ps1 -BuildRelease
```

### Driver with plugins

If you want the older full install flow that also deploys plugin payloads:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install_driver.ps1
```

Both scripts register the `driver` folder with SteamVR through `vrpathreg.exe`.

## Runtime and Plugins

The SteamVR-loaded DLL stays lightweight and launches `opendriver_runner.exe` from the same `bin/win64` directory. The runtime initializes config/logging and scans:

```text
%APPDATA%\opendriver\plugins
```

If the folder is empty, the runtime still starts normally. Plugins are optional.

## Creating Plugins

Plugins are standalone shared libraries that export `CreatePlugin` and `DestroyPlugin`. They are discovered through `plugin.json` files inside subfolders of `%APPDATA%\opendriver\plugins`.

Minimal plugin shape:

```cpp
#include <opendriver/core/plugin_interface.h>

using namespace opendriver::core;

class MyPlugin : public IPlugin {
public:
    const char* GetName() const override { return "my_plugin"; }
    const char* GetVersion() const override { return "1.0.0"; }
    const char* GetDescription() const override { return "Example plugin"; }
    const char* GetAuthor() const override { return "You"; }

    bool OnInitialize(IPluginContext* context) override { return true; }
    void OnShutdown() override {}
    void OnTick(float delta_time) override {}
    void OnEvent(const Event& event) override {}
    bool IsActive() const override { return true; }
};
```

More details:

- [docs/DEVELOPER_GUIDE.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/DEVELOPER_GUIDE.md)
- [docs/PLUGINS_API.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/PLUGINS_API.md)

## Troubleshooting

- SteamVR logs: `%LOCALAPPDATA%\Steam\logs`
- OpenDriver log: `%APPDATA%\opendriver\opendriver.log`
- If SteamVR does not load the driver, verify `driver/driver.vrdrivermanifest` exists and the `driver` folder was registered successfully.
- If the runner fails to start, verify `driver/bin/win64/` still contains `opendriver_runner.exe`, Qt DLLs, and `platforms/qwindows.dll`.

## Documentation

- [docs/DOCS.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/DOCS.md)
- [docs/BUILDING_WINDOWS.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/BUILDING_WINDOWS.md)
- [docs/DEVELOPER_GUIDE.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/DEVELOPER_GUIDE.md)
- [docs/CHANGELOG.md](/c:/Users/Tomasz/Desktop/OpenDriver-VR/docs/CHANGELOG.md)
