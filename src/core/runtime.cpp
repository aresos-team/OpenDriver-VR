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
        
        // 1. Create config dir if not exists
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
        
        // 2. Load config
        std::string config_path = (fs::path(config_dir) / "config.json").string();
        try {
            config_manager.Load(config_path);
            Logger::GetInstance().Info("Core", "Configuration loaded from: " + config_path);
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Failed to load config: " + std::string(e.what()));
            // Continue anyway - use defaults
        }
        
        // 3. Initialize logging
        std::string log_path = (fs::path(config_dir) / "opendriver.log").string();
        try {
            Logger::GetInstance().Initialize(log_path, LogLevel::INFO, true);
            Logger::GetInstance().Info("Core", "Logger initialized to: " + log_path);
        } catch (const std::exception& e) {
            Logger::GetInstance().Critical("Core", "Failed to initialize logger: " + std::string(e.what()));
            return false;
        }
        
        Logger::GetInstance().Info("Core", "Runtime starting up...");
        
        // 4. Initialize event bus (already done in constructor)
        // 5. Initialize device registry (already done in constructor)
        Logger::GetInstance().Debug("Core", "Event bus and device registry initialized");
        
        // 6. Load plugins
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

        // 7. Start Bridge (IPC for SteamVR)
        bridge = std::make_unique<Bridge>(event_bus, device_registry);
        try {
            if (bridge->Start(OD_IPC_ADDRESS)) {
                event_bus.Subscribe(EventType::DEVICE_CONNECTED, bridge.get());
                event_bus.Subscribe(EventType::POSE_UPDATE, bridge.get());
                event_bus.Subscribe(EventType::INPUT_UPDATE, bridge.get());
                Logger::GetInstance().Info("Core", "Bridge started on " + std::string(OD_IPC_ADDRESS));
            } else {
                Logger::GetInstance().Error("Core", "Failed to start Bridge on " + std::string(OD_IPC_ADDRESS));
                // Continue anyway - Bridge is optional
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Exception during Bridge initialization: " + std::string(e.what()));
            // Continue anyway
        }
        
        is_running = true;
        
        // Publish STARTUP event
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
        if (!is_running) {
            Logger::GetInstance().Debug("Core", "UpdateInput called while not running");
            return;
        }
        
        IPCInputUpdate payload;
        memset(&payload, 0, sizeof(payload));
        strncpy(payload.device_id, device_id, sizeof(payload.device_id) - 1);
        strncpy(payload.component_name, component_name, sizeof(payload.component_name) - 1);
        payload.value = value;

        Event evt(EventType::INPUT_UPDATE, "core");
        evt.data = payload;
        event_bus.Publish(evt);
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in UpdateInput: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in UpdateInput");
    }
}

void Runtime::UpdatePose(const char* device_id, 
                         double x, double y, double z, 
                         double qw, double qx, double qy, double qz,
                         double vx, double vy, double vz,
                         double avx, double avy, double avz) {
    try {
        if (!is_running) {
            Logger::GetInstance().Debug("Core", "UpdatePose called while not running");
            return;
        }
        
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
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in UpdatePose: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in UpdatePose");
    }
}

void Runtime::Shutdown() {
    try {
        if (!is_running) {
            Logger::GetInstance().Warn("Core", "Runtime not running, ignoring shutdown request");
            return;
        }
        
        Logger::GetInstance().Info("Core", "=== Runtime shutdown initiated ===");

        try {
            if (bridge) {
                event_bus.Unsubscribe(EventType::DEVICE_CONNECTED, bridge.get());
                event_bus.Unsubscribe(EventType::POSE_UPDATE, bridge.get());
                event_bus.Unsubscribe(EventType::INPUT_UPDATE, bridge.get());
                bridge->Stop();
                Logger::GetInstance().Info("Core", "Bridge stopped");
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Error stopping Bridge: " + std::string(e.what()));
        } catch (...) {
            Logger::GetInstance().Error("Core", "Unknown error stopping Bridge");
        }
        
        // Publish SHUTDOWN event
        try {
            Event evt(EventType::CORE_SHUTDOWN, "core");
            event_bus.Publish(evt);
            Logger::GetInstance().Info("Core", "CORE_SHUTDOWN event published");
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Error publishing CORE_SHUTDOWN event: " + std::string(e.what()));
        }
        
        // Unload all plugins
        try {
            plugin_loader.UnloadAll();
            Logger::GetInstance().Info("Core", "All plugins unloaded");
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Error unloading plugins: " + std::string(e.what()));
        } catch (...) {
            Logger::GetInstance().Error("Core", "Unknown error unloading plugins");
        }
        
        is_running = false;
        
        Logger::GetInstance().Info("Core", "=== Runtime shutdown complete ===");
        Logger::GetInstance().Shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Critical error during Shutdown: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown critical error during Shutdown" << std::endl;
    }
}

void Runtime::Tick(float delta_time) {
    try {
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
            try {
                cb();
            } catch (const std::exception& e) {
                Logger::GetInstance().Error("Core", "Exception in main thread callback: " + std::string(e.what()));
            } catch (...) {
                Logger::GetInstance().Error("Core", "Unknown exception in main thread callback");
            }
        }
        
        // Tick plugins
        try {
            plugin_loader.TickAll(delta_time);
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Exception in PluginLoader::TickAll: " + std::string(e.what()));
        } catch (...) {
            Logger::GetInstance().Error("Core", "Unknown exception in PluginLoader::TickAll");
        }

        // Auto-discover new plugins check (every ~1s)
        static float auto_discover_timer = 0.0f;
        auto_discover_timer += delta_time;
        if (auto_discover_timer >= 1.0f) {
            auto_discover_timer = 0.0f;
            try {
                ScanPlugins();
                auto available = GetAvailablePlugins();
                for (const auto& ap : available) {
                    if (ap.is_enabled && !ap.is_loaded) {
                        Logger::GetInstance().Info("Core", "Auto-discovered new plugin: " + ap.name);
                        EnablePlugin(ap.name);
                    }
                }
            } catch (const std::exception& e) {
                Logger::GetInstance().Error("Core", "Error during auto-discovery: " + std::string(e.what()));
            } catch (...) {
                Logger::GetInstance().Error("Core", "Unknown error during auto-discovery");
            }
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in Tick: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in Tick");
    }
}

bool Runtime::LoadPluginRuntime(const std::string& plugin_path) {
    return plugin_loader.Load(plugin_path);
}

bool Runtime::UnloadPluginRuntime(const std::string& plugin_name) {
    return plugin_loader.Unload(plugin_name);
}

void Runtime::Log(int level, const char* message) {
    try {
        // Log to file/console
        Logger::GetInstance().Log(static_cast<LogLevel>(level), "plugin", message);
        
        // Also publish to event bus for listeners
        LogLevelEnum evt_level = static_cast<LogLevelEnum>(level);
        EventType evt_type = EventType::LOG_INFO;  // default
        
        if (evt_level == LogLevelEnum::TRACE) evt_type = EventType::LOG_TRACE;
        else if (evt_level == LogLevelEnum::DEBUG) evt_type = EventType::LOG_DEBUG;
        else if (evt_level == LogLevelEnum::INFO) evt_type = EventType::LOG_INFO;
        else if (evt_level == LogLevelEnum::WARN) evt_type = EventType::LOG_WARN;
        else if (evt_level == LogLevelEnum::ERROR) evt_type = EventType::LOG_ERROR;
        else if (evt_level == LogLevelEnum::CRITICAL) evt_type = EventType::LOG_CRITICAL;
        
        Event evt(evt_type, "core");
        evt.data = LogMessage(evt_level, "plugin", message);
        event_bus.Publish(evt);
    } catch (const std::exception& e) {
        Logger::GetInstance().Critical("Runtime", "Error in Log(): " + std::string(e.what()));
    }
}

void Runtime::RegisterDevice(const Device& device) {
    try {
        if (device_registry.Register(device)) {
            Logger::GetInstance().Info("Core", "Device registered: " + std::string(device.id) + " (type: " + std::to_string(static_cast<int>(device.type)) + ")");
            
            Event evt(EventType::DEVICE_CONNECTED, "core");
            evt.data = device.id;
            event_bus.Publish(evt);
        } else {
            Logger::GetInstance().Warn("Core", "Failed to register device: " + std::string(device.id) + " (likely duplicate)");
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in RegisterDevice: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in RegisterDevice");
    }
}

const Device* Runtime::GetDevice(const char* device_id) const {
    return device_registry.Get(device_id);
}

void Runtime::UnregisterDevice(const char* device_id) {
    try {
        if (device_registry.Unregister(device_id)) {
            Logger::GetInstance().Info("Core", "Device unregistered: " + std::string(device_id));
            
            Event evt(EventType::DEVICE_DISCONNECTED, "core");
            evt.data = std::string(device_id);
            event_bus.Publish(evt);
        } else {
            Logger::GetInstance().Warn("Core", "Device not found for unregister: " + std::string(device_id));
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in UnregisterDevice: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in UnregisterDevice");
    }
}

void Runtime::UnregisterDevicesByPlugin(const char* plugin_name) {
    try {
        auto removed_ids = device_registry.UnregisterByPlugin(plugin_name);
        for (const auto& id : removed_ids) {
            Event evt(EventType::DEVICE_DISCONNECTED, "core");
            evt.data = id;
            event_bus.Publish(evt);
            Logger::GetInstance().Info("Core", "Auto-unregistered orphaned device: " + id + " (owned by " + std::string(plugin_name) + ")");
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in UnregisterDevicesByPlugin: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in UnregisterDevicesByPlugin");
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
    try {
        Logger::GetInstance().Info("Core", "=== Reloading all plugins ===");
        
        // 1. Unload all
        try {
            plugin_loader.UnloadAll();
            device_registry.Clear();
            Logger::GetInstance().Info("Core", "All plugins unloaded");
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Error unloading plugins: " + std::string(e.what()));
        }
        
        // 2. Rescan and load
        try {
            ScanPlugins();
            auto available = plugin_loader.GetAvailablePlugins();
            int loaded = 0;
            for (auto& ap : available) {
                if (ap.is_enabled) {
                    if (EnablePlugin(ap.name)) {
                        loaded++;
                    }
                }
            }
            Logger::GetInstance().Info("Core", "Reloaded " + std::to_string(loaded) + " plugin(s)");
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("Core", "Error reloading plugins: " + std::string(e.what()));
        }
        
        Logger::GetInstance().Info("Core", "=== Plugin reload complete ===");
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in ReloadPlugins: " + std::string(e.what()));
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in ReloadPlugins");
    }
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
                    try {
                        std::ifstream f(json_path);
                        nlohmann::json data = nlohmann::json::parse(f);
                        std::string entry_point = data.value("entry_point", "");
                        if (!entry_point.empty()) {
                            fs::path plugin_file = fs::path(ap.path) / entry_point;
                            if (fs::exists(plugin_file)) {
                                Logger::GetInstance().Info("Core", "Enabling plugin: " + name);
                                return plugin_loader.Load(plugin_file.string());
                            } else {
                                Logger::GetInstance().Error("Core", "Plugin file not found: " + plugin_file.string());
                            }
                        } else {
                            Logger::GetInstance().Error("Core", "No entry_point specified for plugin: " + name);
                        }
                    } catch (const std::exception& e) {
                        Logger::GetInstance().Error("Core", "Error parsing plugin.json for " + name + ": " + e.what());
                    }
                } else {
                    Logger::GetInstance().Error("Core", "plugin.json not found for " + name);
                }
            }
        }
        Logger::GetInstance().Warn("Core", "Plugin not found in available list: " + name);
        return false;
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in EnablePlugin: " + std::string(e.what()));
        return false;
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in EnablePlugin");
        return false;
    }
}

bool Runtime::DisablePlugin(const std::string& name) {
    try {
        Logger::GetInstance().Info("Core", "Disabling plugin: " + name);
        return plugin_loader.Unload(name);
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("Core", "Exception in DisablePlugin: " + std::string(e.what()));
        return false;
    } catch (...) {
        Logger::GetInstance().Error("Core", "Unknown exception in DisablePlugin");
        return false;
    }
}

void Runtime::SetAllPluginsState(bool enabled) {
    if (!enabled) {
        plugin_loader.UnloadAll();
    } else {
        ReloadPlugins();
    }
}

} // namespace opendriver::core
