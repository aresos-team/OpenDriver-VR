# Developer Guide: Creating Plugins for OpenDriver 🔧

Plugins are the heart of OpenDriver. They allow you to add support for any hardware to SteamVR using a simple C++ API.

## 1. Plugin Lifecycle

Every plugin must implement the `IPlugin` interface. Here's how the system manages it:

1.  **Loading**: The `.so` library is loaded via `dlopen`. The system looks for the `CreatePlugin()` function.
2.  **Initialization (`OnInitialize`)**: Called once after loading. This is where you register your devices.
3.  **Main Loop (`OnTick`)**: Called approximately 90-250 times per second (depending on settings). This is where you update pose and inputs.
4.  **Events (`OnEvent`)**: Called when other parts of the system send events (e.g., haptics from SteamVR).
5.  **Shutdown (`OnShutdown`)**: Called before unloading the library. Clean up your resources here.

## 2. Device Registration (`Device`)

The `Device` structure defines how SteamVR sees your hardware.

### Device Types (`DeviceType`):
- `HMD`: VR Headset.
- `CONTROLLER`: Hand controllers.
- `GENERIC_TRACKER`: Leg, waist, etc., trackers.
- `HAND_TRACKER`: Hand tracking devices.
- `LIGHTHOUSE`: Base stations.

### Example HMD Configuration:
```cpp
opendriver::core::Device hmd;
hmd.id = "my_hmd";
hmd.type = opendriver::core::DeviceType::HMD;
hmd.display.width = 1920;
hmd.display.height = 1080;
hmd.display.refresh_rate = 90.0f;
hmd.display.fov_left = 110.0f;
hmd.display.fov_right = 110.0f;
context->RegisterDevice(hmd);
```

## 3. Sending Pose Data (`Pose`)

OpenDriver uses the OpenVR coordinate system:
- **X**: Right (+) / Left (-)
- **Y**: Up (+) / Down (-)
- **Z**: Back (+) / Forward (-) (Z-forward is negative)
- Units: **Meters**.

```cpp
context->UpdatePose(
    "device_id",
    x, y, z,             // Position (meters)
    qw, qx, qy, qz,       // Orientation (Quaternion)
    vx, vy, vz,          // Linear velocity (m/s) - optional
    ax, ay, az           // Angular velocity (rad/s) - optional
);
```

## 4. Handling Inputs (`Input`)

You can add buttons and axes to your devices.

### Defining inputs in `OnInitialize`:
```cpp
opendriver::core::Device d;
// ...
opendriver::core::InputComponent trigger;
trigger.name = "/input/trigger/value";
trigger.type = opendriver::core::InputType::SCALAR; // 0.0 - 1.0
d.inputs.push_back(trigger);

opendriver::core::InputComponent button;
button.name = "/input/grip/click";
button.type = opendriver::core::InputType::BOOLEAN; // true/false
d.inputs.push_back(button);
```

### Updating in `OnTick`:
```cpp
context->UpdateInput("device_id", "/input/trigger/value", 0.75f);
context->UpdateInput("device_id", "/input/grip/click", 1.0f);
```

## 5. Hot-Reload and Persistence

OpenDriver allows you to edit and compile your plugin's code **while SteamVR is running**. The system automatically detects changes to the `.so` file and reloads it.

To keep your application's state (e.g., calibration):
1. Implement `ExportState()` - return a pointer to your data.
2. Implement `ImportState(void* state)` - read data and delete the pointer.

```cpp
struct MyState { float calibration_offset; };

void* ExportState() override {
    return new MyState{ this->offset };
}

void ImportState(void* state) override {
    auto* s = (MyState*)state;
    this->offset = s->calibration_offset;
    delete s;
}
```

## 6. Tips and Best Practices

- **Performance**: `OnTick` is called frequently. Avoid memory allocations (`new`) and heavy I/O operations inside this method.
- **Logging**: Use `context->Log()`. Logs are sent to the Dashboard in real-time.
- **Threading**: If your plugin uses separate threads (e.g., for networking), sync data (mutex) before `OnTick` reads it.
