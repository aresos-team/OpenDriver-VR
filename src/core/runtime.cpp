#include <opendriver/core/runtime.h>
#include <filesystem>
#include <iostream>
#include <fstream>

namespace fs = std::filesystem;

namespace opendriver::core {

Runtime::Runtime() : plugin_loader(this) {}

Runtime::~Runtime() {
    if (is_running) {
        Shutdown();
    }
}

Runtime& Runtime::GetInstance() {
    static Runtime instance;
    return instance;
}

bool Runtime::Initialize(const std::string& config_dir) {
    if (is_running) return true;
    
    // 1. Create config dir if not exists
    m_configDir = config_dir;
    if (!fs::exists(m_configDir)) {
        fs::create_directories(m_configDir);
    }
    
    // 2. Load config
    std::string config_path = (fs::path(config_dir) / "config.json").string();
    config_manager.Load(config_path);
    
    // 3. Initialize logging
    std::string log_path = (fs::path(config_dir) / "opendriver.log").string();
    Logger::GetInstance().Initialize(log_path, LogLevel::INFO, true);
    
    Logger::GetInstance().Info("Core", "Runtime starting...");
    
    // 4. Initialize event bus (already done in constructor)
    // 5. Initialize device registry (already done in constructor)
    
    // 6. Load plugins
    std::string plugins_dir = (fs::path(config_dir) / "plugins").string();
    if (!fs::exists(plugins_dir)) {
        fs::create_directories(plugins_dir);
    }
    
    int loaded = plugin_loader.LoadDirectory(plugins_dir);
    Logger::GetInstance().Info("Core", "Loaded " + std::to_string(loaded) + " plugins");

    // 7. Start Bridge (IPC for SteamVR)
    bridge = std::make_unique<Bridge>(event_bus, device_registry);
    if (bridge->Start("/tmp/opendriver.sock")) {
        event_bus.Subscribe(EventType::DEVICE_CONNECTED, bridge.get());
        event_bus.Subscribe(EventType::POSE_UPDATE, bridge.get());
        event_bus.Subscribe(EventType::INPUT_UPDATE, bridge.get());
        Logger::GetInstance().Info("Core", "Bridge started on /tmp/opendriver.sock");
    }
    
    is_running = true;
    
    // Publish STARTUP event
    Event evt(EventType::CORE_STARTUP, "core");
    event_bus.Publish(evt);
    
    return true;
}

void Runtime::UpdateInput(const char* device_id, const char* component_name, float value) {
    IPCInputUpdate payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.device_id, device_id, sizeof(payload.device_id) - 1);
    strncpy(payload.component_name, component_name, sizeof(payload.component_name) - 1);
    payload.value = value;

    Event evt(EventType::INPUT_UPDATE, "core");
    evt.data = payload;
    event_bus.Publish(evt);
}

void Runtime::UpdatePose(const char* device_id, 
                         double x, double y, double z, 
                         double qw, double qx, double qy, double qz,
                         double vx, double vy, double vz,
                         double avx, double avy, double avz) {
    IPCPoseData payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.device_id, device_id, sizeof(payload.device_id) - 1);
    
    payload.posX = x; payload.posY = y; payload.posZ = z;
    payload.rotW = qw; payload.rotX = qx; payload.rotY = qy; payload.rotZ = qz;
    payload.velX = vx; payload.velY = vy; payload.velZ = vz;
    payload.angVelX = avx; payload.angVelY = avy; payload.angVelZ = avz;

    Event evt(EventType::POSE_UPDATE, "core");
    evt.data = payload;
    event_bus.Publish(evt);
}

void Runtime::Shutdown() {
    if (!is_running) return;
    
    Logger::GetInstance().Info("Core", "Runtime shutting down...");

    if (bridge) {
        event_bus.Unsubscribe(EventType::DEVICE_CONNECTED, bridge.get());
        event_bus.Unsubscribe(EventType::POSE_UPDATE, bridge.get());
        bridge->Stop();
    }
    
    // Publish SHUTDOWN event
    Event evt(EventType::CORE_SHUTDOWN, "core");
    event_bus.Publish(evt);
    
    // Unload all plugins
    plugin_loader.UnloadAll();
    
    // Clear device registry
    // (In our implementation, they are in the map, and we can just let it clear on destruction or manually)
    
    is_running = false;
    
    Logger::GetInstance().Shutdown();
}

void Runtime::Tick(float delta_time) {
    if (!is_running) return;
    
    // Process callbacks
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        while (!main_thread_callbacks.empty()) {
            callbacks.push_back(std::move(main_thread_callbacks.front()));
            main_thread_callbacks.pop();
        }
    }
    
    for (auto& cb : callbacks) {
        cb();
    }
    
    // Tick plugins
    plugin_loader.TickAll(delta_time);

    // Auto-discover new plugins check (every ~1s)
    static float auto_discover_timer = 0.0f;
    auto_discover_timer += delta_time;
    if (auto_discover_timer >= 1.0f) {
        auto_discover_timer = 0.0f;
        ScanPlugins();
        auto available = GetAvailablePlugins();
        for (const auto& ap : available) {
            if (ap.is_enabled && !ap.is_loaded) {
                Logger::GetInstance().Info("Core", "Auto-discovered new plugin: " + ap.name);
                EnablePlugin(ap.name);
            }
        }
    }
}

bool Runtime::LoadPluginRuntime(const std::string& plugin_path) {
    return plugin_loader.Load(plugin_path);
}

bool Runtime::UnloadPluginRuntime(const std::string& plugin_name) {
    return plugin_loader.Unload(plugin_name);
}

void Runtime::Log(int level, const char* message) {
    Logger::GetInstance().Log(static_cast<LogLevel>(level), "plugin", message);
}

void Runtime::RegisterDevice(const Device& device) {
    if (device_registry.Register(device)) {
        Event evt(EventType::DEVICE_CONNECTED, "core");
        evt.data = device.id;
        event_bus.Publish(evt);
    }
}

const Device* Runtime::GetDevice(const char* device_id) const {
    return device_registry.Get(device_id);
}

void Runtime::UnregisterDevice(const char* device_id) {
    if (device_registry.Unregister(device_id)) {
        Event evt(EventType::DEVICE_DISCONNECTED, "core");
        evt.data = std::string(device_id);
        event_bus.Publish(evt);
    }
}

void Runtime::UnregisterDevicesByPlugin(const char* plugin_name) {
    auto removed_ids = device_registry.UnregisterByPlugin(plugin_name);
    for (const auto& id : removed_ids) {
        Event evt(EventType::DEVICE_DISCONNECTED, "core");
        evt.data = id;
        event_bus.Publish(evt);
        Logger::GetInstance().Info("Core", "Auto-unregistered orphaned device: " + id + " (owned by " + std::string(plugin_name) + ")");
    }
}

IPlugin* Runtime::GetPlugin(const char* name) {
    return plugin_loader.Get(name);
}

void Runtime::PostToMainThread(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    main_thread_callbacks.push(std::move(callback));
}

void Runtime::ScanPlugins() {
    if (m_configDir.empty()) return;
    std::string plugins_dir = (fs::path(m_configDir) / "plugins").string();
    plugin_loader.ScanDirectory(plugins_dir);
}

void Runtime::ReloadPlugins() {
    Logger::GetInstance().Info("Core", "Reloading plugins...");
    
    // 1. Unload all
    plugin_loader.UnloadAll();
    device_registry.Clear();
    
    // 2. Rescan and load
    ScanPlugins();
    auto available = plugin_loader.GetAvailablePlugins();
    for (auto& ap : available) {
        if (ap.is_enabled) {
            EnablePlugin(ap.name);
        }
    }
    
    Logger::GetInstance().Info("Core", "Plugins reloaded.");
}

std::vector<PluginLoader::AvailablePlugin> Runtime::GetAvailablePlugins() {
    return plugin_loader.GetAvailablePlugins();
}

bool Runtime::EnablePlugin(const std::string& name) {
    auto available = GetAvailablePlugins();
    for (auto& ap : available) {
        if (ap.name == name) {
            fs::path json_path = fs::path(ap.path) / "plugin.json";
            if (fs::exists(json_path)) {
                try {
                    std::ifstream f(json_path);
                    nlohmann::json data = nlohmann::json::parse(f);
                    std::string entry_point = data.value("entry_point", "");
                    if (!entry_point.empty()) {
                        fs::path plugin_file = fs::path(ap.path) / entry_point;
                        if (fs::exists(plugin_file)) {
                            return plugin_loader.Load(plugin_file.string());
                        }
                    }
                } catch(...) {}
            }
        }
    }
    return false;
}

bool Runtime::DisablePlugin(const std::string& name) {
    return plugin_loader.Unload(name);
}

void Runtime::SetAllPluginsState(bool enabled) {
    if (!enabled) {
        plugin_loader.UnloadAll();
    } else {
        ReloadPlugins();
    }
}

} // namespace opendriver::core
