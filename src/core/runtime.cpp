#include <opendriver/core/runtime.h>
#include <opendriver/core/platform.h>
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
    try {
        if (is_running) {
            Logger::GetInstance().Warn("Core", "Runtime already initialized, ignoring re-initialization attempt");
            return true;
        }
        
        m_configDir = config_dir;
        try {
            if (!fs::exists(m_configDir)) {
                fs::create_directories(m_configDir);
                Logger::GetInstance().Info("Core", "Created config directory: " + m_configDir);
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Critical("Core", "Failed to create config directory: " + std::string(e.what()));
            return false;
        }
        
        std::string config_path = (fs::path(config_dir) / "config.json").string();
        try {
            config_manager.Load(config_path);
            Logger::GetInstance().Info("Core", "Configuration loaded from: " + config_path);
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Failed to load config: " + std::string(e.what()));
        }
        
        std::string log_path = (fs::path(config_dir) / "opendriver.log").string();
        try {
            Logger::GetInstance().Initialize(log_path, LogLevel::Info, true);
            Logger::GetInstance().Info("Core", "Logger initialized to: " + log_path);
        } catch (const std::exception& e) {
            Logger::GetInstance().Critical("Core", "Failed to initialize logger: " + std::string(e.what()));
            return false;
        }
        
        Logger::GetInstance().Info("Core", "Runtime starting up...");
        
        std::string plugins_dir = (fs::path(config_dir) / "plugins").string();
        try {
            if (!fs::exists(plugins_dir)) {
                fs::create_directories(plugins_dir);
                Logger::GetInstance().Info("Core", "Created plugins directory: " + plugins_dir);
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Failed to create plugins directory: " + std::string(e.what()));
        }
        
        int loaded = plugin_loader.LoadDirectory(plugins_dir);
        std::string plugin_msg = "Loaded " + std::to_string(loaded) + " plugin(s) from: " + plugins_dir;
        Logger::GetInstance().Info("Core", plugin_msg);

        bridge = std::make_unique<Bridge>(event_bus, device_registry);
        try {
            if (bridge->Start(OD_IPC_ADDRESS)) {
                event_bus.Subscribe(EventType::DEVICE_CONNECTED, bridge.get());
                event_bus.Subscribe(EventType::POSE_UPDATE, bridge.get());
                event_bus.Subscribe(EventType::INPUT_UPDATE, bridge.get());
                Logger::GetInstance().Info("Core", "Bridge started on " + std::string(OD_IPC_ADDRESS));
            } else {
                Logger::GetInstance().Error("Core", "Failed to start Bridge on " + std::string(OD_IPC_ADDRESS));
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Exception during Bridge initialization: " + std::string(e.what()));
        }
        
        is_running = true;
        
        try {
            Event evt(EventType::CORE_STARTUP, "core");
            event_bus.Publish(evt);
            Logger::GetInstance().Info("Core", "CORE_STARTUP event published");
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Failed to publish CORE_STARTUP event: " + std::string(e.what()));
        }
        
        Logger::GetInstance().Info("Core", "=== Runtime initialization complete ===");
        return true;
    } catch (const std::exception& e) {
        Logger::GetInstance().Critical("Core", "Unexpected error during Initialize: " + std::string(e.what()));
        return false;
    } catch (...) {
        Logger::GetInstance().Critical("Core", "Unknown error during Initialize");
        return false;
    }
}

void Runtime::UpdateInput(const char* device_id, const char* component_name, float value) {
    try {
        if (!is_running) return;
        
        IPCInputUpdate payload;
        memset(&payload, 0, sizeof(payload));
        strncpy(payload.device_id, device_id, sizeof(payload.device_id) - 1);
        strncpy(payload.component_name, component_name, sizeof(payload.component_name) - 1);
        payload.value = value;

        Event evt(EventType::INPUT_UPDATE, "core");
        evt.data = payload;
        event_bus.Publish(evt);
    } catch (...) {}
}

void Runtime::UpdatePose(const char* device_id, 
                         double x, double y, double z, 
                         double qw, double qx, double qy, double qz,
                         double vx, double vy, double vz,
                         double avx, double avy, double avz) {
    try {
        if (!is_running) return;
        
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
    } catch (...) {}
}

void Runtime::Shutdown() {
    try {
        if (!is_running) return;
        
        Logger::GetInstance().Info("Core", "=== Runtime shutdown initiated ===");

        try {
            if (bridge) {
                event_bus.Unsubscribe(EventType::DEVICE_CONNECTED, bridge.get());
                event_bus.Unsubscribe(EventType::POSE_UPDATE, bridge.get());
                event_bus.Unsubscribe(EventType::INPUT_UPDATE, bridge.get());
                bridge->Stop();
            }
        } catch (...) {}
        
        try {
            Event evt(EventType::CORE_SHUTDOWN, "core");
            event_bus.Publish(evt);
        } catch (...) {}
        
        try {
            plugin_loader.UnloadAll();
        } catch (...) {}
        
        is_running = false;
        Logger::GetInstance().Info("Core", "=== Runtime shutdown complete ===");
        Logger::GetInstance().Shutdown();
    } catch (...) {}
}

void Runtime::Tick(float delta_time) {
    try {
        if (!is_running) return;
        
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            while (!main_thread_callbacks.empty()) {
                callbacks.push_back(std::move(main_thread_callbacks.front()));
                main_thread_callbacks.pop();
            }
        }
        
        for (auto& cb : callbacks) try { cb(); } catch (...) {}
        
        try {
            plugin_loader.TickAll(delta_time);
        } catch (...) {}

        static float auto_discover_timer = 0.0f;
        auto_discover_timer += delta_time;
        if (auto_discover_timer >= 1.0f) {
            auto_discover_timer = 0.0f;
            try {
                // Skanujemy folder w poszukiwaniu nowych pluginów (nie ładujemy ich automatycznie!)
                ScanPlugins();
            } catch (...) {}
        }
    } catch (...) {}
}

bool Runtime::LoadPluginRuntime(const std::string& plugin_path) {
    return plugin_loader.Load(plugin_path);
}

bool Runtime::UnloadPluginRuntime(const std::string& plugin_name) {
    return plugin_loader.Unload(plugin_name);
}

void Runtime::Log(int level, const char* message) {
    try {
        Logger::GetInstance().Log(static_cast<LogLevel>(level), "plugin", message);
        
        LogLevelEnum evt_level = static_cast<LogLevelEnum>(level);
        EventType evt_type = EventType::LOG_INFO;
        
        if (evt_level == LogLevelEnum::Trace) evt_type = EventType::LOG_TRACE;
        else if (evt_level == LogLevelEnum::Debug) evt_type = EventType::LOG_DEBUG;
        else if (evt_level == LogLevelEnum::Warn) evt_type = EventType::LOG_WARN;
        else if (evt_level == LogLevelEnum::Error) evt_type = EventType::LOG_ERROR;
        else if (evt_level == LogLevelEnum::Critical) evt_type = EventType::LOG_CRITICAL;
        
        Event evt(evt_type, "core");
        evt.data = LogMessage(evt_level, "plugin", message);
        event_bus.Publish(evt);
    } catch (...) {}
}

void Runtime::RegisterDevice(const Device& device) {
    try {
        if (device_registry.Register(device)) {
            Logger::GetInstance().Info("Core", "Device registered: " + std::string(device.id));
            Event evt(EventType::DEVICE_CONNECTED, "core");
            evt.data = device.id;
            event_bus.Publish(evt);
        }
    } catch (...) {}
}

const Device* Runtime::GetDevice(const char* device_id) const {
    return device_registry.Get(device_id);
}

void Runtime::UnregisterDevice(const char* device_id) {
    try {
        if (device_registry.Unregister(device_id)) {
            Event evt(EventType::DEVICE_DISCONNECTED, "core");
            evt.data = std::string(device_id);
            event_bus.Publish(evt);
        }
    } catch (...) {}
}

void Runtime::UnregisterDevicesByPlugin(const char* plugin_name) {
    try {
        auto removed_ids = device_registry.UnregisterByPlugin(plugin_name);
        for (const auto& id : removed_ids) {
            Event evt(EventType::DEVICE_DISCONNECTED, "core");
            evt.data = id;
            event_bus.Publish(evt);
        }
    } catch (...) {}
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
    try {
        plugin_loader.UnloadAll();
        device_registry.Clear();
        ScanPlugins();
        auto available = plugin_loader.GetAvailablePlugins();
        for (auto& ap : available) {
            if (ap.is_enabled) EnablePlugin(ap.name);
        }
    } catch (...) {}
}

std::vector<PluginLoader::AvailablePlugin> Runtime::GetAvailablePlugins() {
    return plugin_loader.GetAvailablePlugins();
}

bool Runtime::EnablePlugin(const std::string& name) {
    try {
        auto available = GetAvailablePlugins();
        for (auto& ap : available) {
            if (ap.name == name) {
                fs::path json_path = fs::path(ap.path) / "plugin.json";
                if (fs::exists(json_path)) {
                    std::ifstream f(json_path);
                    nlohmann::json data = nlohmann::json::parse(f);
                    std::string entry_point = data.value("entry_point", "");
                    if (!entry_point.empty()) {
                        fs::path plugin_file = fs::path(ap.path) / entry_point;
                        if (fs::exists(plugin_file)) {
                            return plugin_loader.Load(plugin_file.string());
                        }
                    }
                }
            }
        }
        return false;
    } catch (...) { return false; }
}

bool Runtime::DisablePlugin(const std::string& name) {
    return plugin_loader.Unload(name);
}

void Runtime::SetAllPluginsState(bool enabled) {
    if (!enabled) plugin_loader.UnloadAll();
    else ReloadPlugins();
}

} // namespace opendriver::core
