# OpenDriver Plugin Architecture Guide

The plugin architecture in OpenDriver relies on a simple C++ Dependency Injection model. The core library (`opendriver_core`) abstracts all communication with SteamVR, the GUI, IPC processes, and configuration setups. As a plugin developer, your primary task is responding to system events or feeding tracking data from peripheral devices in a standardized format.

---

## 1. Plugin Lifecycle

The system utilizes dynamic loading for shared libraries (`.dll` on Windows, `.so` on Linux). When `opendriver_runner` starts, it scans for shared libraries in the `plugins/` directory and executes their exported `CreatePlugin()` function.

The lifecycle closely mirrors what you'd find in Game Engines (like Unity or Unreal):
1. **`CreatePlugin()` (C-API)**: Constructs the plugin instance.
2. **`GetName()`, `GetVersion()`**: The core queries metadata to populate the GUI and logs.
3. **`OnInitialize(IPluginContext* context)`**: Fired immediately upon loading. You preserve the `context` pointer, as it's the gateway for system interaction. This is where you register devices.
4. **`OnTick(float delta_time)`** (In-loop): Called every reference frame (roughly 90 times per second for VR). Excellent for reading sockets, computing physics, tracking algorithms, or hardware polling.
5. **`OnEvent(const Event& event)`** (Async): Delivers events derived from the Event Bus that your plugin subscribed to during initialization (e.g., "settings menu closed", "HMD powered on").
6. **`OnShutdown()`**: The core sends this notification before the system terminates. Gracefully close your custom threads and memory here!
7. **`DestroyPlugin()` (C-API)**: Complete deallocation of the plugin instance.

---

## 2. The `IPluginContext` API

The `IPluginContext` object acts as the absolute control panel for OpenDriver. What tools does it provide, and **why do we use them**?

### A. Device Registry
Whenever you want to inform SteamVR that a custom virtual controller, tracker, or headset just connected:
```cpp
// 1. Define the Hardware
Device tracker;
tracker.id = "my_tracker_01";
tracker.name = "Ultimate Tracking Puck";
tracker.type = DeviceType::GENERIC_TRACKER; // Tells SteamVR what this translates to
tracker.manufacturer = "My Custom Hardware Co.";

// 2. Register it with the Core!
context->RegisterDevice(tracker);
```
Once registered, `opendriver_core` negotiates via internal IPC with `vrserver`, automatically injecting your new device icon into the SteamVR dashboard.

### B. Tracking and Inputs
Assuming your plugin reads from Bluetooth, OpenCV webcams, or phone sensors, you update real-time positions using the `context`:

```cpp
// 3D Positional Update (Offset + Quaternion)
context->UpdatePose(
    "my_tracker_01",
    1.0, 1.5, -0.5,    // Position X, Y, Z in meters inside the VR play space
    1.0, 0.0, 0.0, 0.0 // Rotation as a Quaternion (W, X, Y, Z)
);
```
This function is intentionally thread-safe. Usually, you invoke it routinely within your `OnTick(...)` override.

You can also simulate button presses (e.g., faking Valve Index triggers):
```cpp
context->UpdateInput("my_gamepad_01", "/input/trigger/value", 0.75f); // 75% trigger squeeze
context->UpdateInput("my_gamepad_01", "/input/a/click", 1.0f);        // "A" Button Clicked (0=off, 1=on)
```

### C. Unified Logging
Do not use `std::cout` - it will fall into a pipeline void and will never reach the GUI. Plugins should dump traces directly into the unified debug log to ensure tracking reliability.

Format:
```cpp
context->LogInfo("Attempting to read from virtual USB port...");
context->LogError("Bluetooth handshake failed.");
context->LogDebug("Tick latency: 2ms");
```

### D. Settings Integration (JSON Config)
A major perk of OpenDriver is that every plugin gets an isolated namespace automatically stored in the central `config.json`. The engine runs on the reliable `nlohmann::json` spec.

```json
{
   "plugins": {
       "my_tracker": {
           "offset_x": 1.5,
           "enable_smoothing": true
       }
   }
}
```
You can dynamically fetch user choices at runtime without writing your own file parser:
```cpp
auto& cfg = context->GetConfig();
float offset = cfg.GetValue("plugins/my_tracker/offset_x", 0.0f); // defaults back to 0.0f
bool smoothing = cfg.GetValue("plugins/my_tracker/enable_smoothing", false);
```

### E. Thread Safety (`PostToMainThread`)
If your plugin spawns private asynchronous threads dealing with UDP/TCP/Bluetooth polling, you MUST synchronize state shifts if they modify other plugins or trigger drastic context changes. Avoiding C++ Memory Segfaults (like race conditions or thread collision) is solved by queuing tasks back onto the UI engine loop.

```cpp
void MyPrivateUDPThread() {
    auto message = BlockAndReceiveSignal();
    
    // DANGEROUS FROM SIDE-THREAD: context->CrashOtherPlugin(message) !!!
    
    // SAFE: Queue it tightly to the Main Render Environment
    m_context->PostToMainThread([this, message]() {
        // This lambda triggers safely alongside the Main GUI Loop
        ApplyHardwareError(message);
    });
}
```

---

## 3. Advanced Modules & Hot-Reloading Support

OpenDriver packs a highly experimental developer feature: After explicitly recompiling your custom `.dll` or `.so`, the runner instantly swaps out the library during SteamVR use **without having to restart SteamVR!**

To ensure your variables (sockets, buffers, active offsets) aren't cleared sequentially upon reload, `IPlugin` includes two voluntary state methods:
* **`ExportState()`**: Returns a `void*` pointer. If the plugin's memory frame is about to be terminated for an update, pack your internal values into a heap struct and eject its pointer.
* **`ImportState(void* state)`**: When the newly compiled plugin boots up in the brief swap interval, the core feeds the pointer back to you. Re-cast it to restore your exact context variables from right before the `.so` was disconnected.

*If you are only doing simple tasks or starting off, feel free to ignore these overrides entirely.*

## 4. Standalone Compilation (No Core Repo Required!)

One of the most powerful aspects of OpenDriver's dependency injection model is that **you do not need to link your plugin against the OpenDriver core**, nor do you need to compile it inside the same repository.

Here is how you set up a completely isolated, standalone plugin repository:

1. **Copy the Headers:** Grab the `include/opendriver/core/` folder from the main OpenDriver repository and paste it directly into your own project (e.g., inside an `include/` directory).
2. **Setup CMake:** You only need to build a standard `SHARED` library and include those headers. 

**Example `CMakeLists.txt` for your plugin repository:**
```cmake
cmake_minimum_required(VERSION 3.16)
project(my_tracking_hardware)

# 1. Build a dynamic library (.dll or .so)
add_library(my_tracker SHARED src/my_main.cpp)

# 2. Provide the copied headers to the compiler
target_include_directories(my_tracker PRIVATE "include/")

# 3. NOTHING TO LINK - DO NOT USE target_link_libraries()!
# The C++ Virtual Interfaces (IPluginContext) are resolved at runtime by the loader.
```
Once your `.dll` or `.so` is built, just drop it into the user's `plugins/` directory and it will work immediately.

---

## Conclusion
