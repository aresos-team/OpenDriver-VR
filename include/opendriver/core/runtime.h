#pragma once

#include <opendriver/core/plugin_interface.h>
#include <opendriver/core/event_bus.h>
#include <opendriver/core/device_registry.h>
#include <opendriver/core/config_manager.h>
#include <opendriver/core/logger.h>
#include <opendriver/core/plugin_loader.h>
#include <opendriver/core/bridge.h>

#include <string>
#include <memory>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <functional>

namespace opendriver::core {

// ============================================================================
// CORE RUNTIME - serce OpenDriver'a
// ============================================================================

class Runtime : public IPluginContext {
public:
    /// Pobranie singletonu
    static Runtime& GetInstance();

    // ────────────────────────────────────────────────────────────────────
    // INITIALIZATION & SHUTDOWN
    // ────────────────────────────────────────────────────────────────────

    /// Inicjalizacja runtime'u
    /// 
    /// Kroki:
    /// 1. Load config from ~/.opendriver/config.json
    /// 2. Initialize logging
    /// 3. Initialize event bus
    /// 4. Initialize device registry
    /// 5. Load plugins
    /// 
    /// @param config_dir: katalog z config.json
    /// @return true = success; false = init failed
    bool Initialize(const std::string& config_dir);

    /// Shutdown runtime'u
    /// 
    /// Kroki:
    /// 1. Unload all plugins
    /// 2. Clear device registry
    /// 3. Shutdown logging
    void Shutdown();

    // ────────────────────────────────────────────────────────────────────
    // MAIN LOOP
    // ────────────────────────────────────────────────────────────────────

    /// Co-frame tick (wywoływane z głównego loop'a)
    /// ~90 razy na sekundę dla VR
    /// 
    /// @param delta_time: czas od ostatniego frame'a (sekundy)
    void Tick(float delta_time);

    // ────────────────────────────────────────────────────────────────────
    // PLUGIN MANAGEMENT (runtime)
    // ────────────────────────────────────────────────────────────────────

    /// Załaduj plugin w runtime'ie (nie trzeba restartu)
    /// @param plugin_path: ścieżka do .so/.dll
    bool LoadPluginRuntime(const std::string& plugin_path);

    /// Wyladuj plugin w runtime'ie
    bool UnloadPluginRuntime(const std::string& plugin_name);

    // Menedżer Pluginów (dla UI)
    void ScanPlugins();
    std::vector<PluginLoader::AvailablePlugin> GetAvailablePlugins();
    bool EnablePlugin(const std::string& name);
    bool DisablePlugin(const std::string& name);
    void SetAllPluginsState(bool enabled);
    void ReloadPlugins();

    // ────────────────────────────────────────────────────────────────────
    // IPluginContext IMPLEMENTATION (pluginy używają tych metod)
    // ────────────────────────────────────────────────────────────────────

    EventBus& GetEventBus() override { return event_bus; }
    ConfigManager& GetConfig() override { return config_manager; }
    void Log(int level, const char* message) override;
    void RegisterDevice(const Device& device) override;
    const Device* GetDevice(const char* device_id) const override;
    void UnregisterDevice(const char* device_id) override;
    void UnregisterDevicesByPlugin(const char* plugin_name) override;
    void UpdateInput(const char* device_id, const char* component_name, float value) override;
    void UpdatePose(const char* device_id, 
                     double x, double y, double z, 
                     double qw, double qx, double qy, double qz,
                     double vx = 0, double vy = 0, double vz = 0,
                     double avx = 0, double avy = 0, double avz = 0) override;
    IPlugin* GetPlugin(const char* name) override;
    void PostToMainThread(std::function<void()> callback) override;

    // ────────────────────────────────────────────────────────────────────
    // GETTERS
    // ────────────────────────────────────────────────────────────────────

    DeviceRegistry& GetDeviceRegistry() { return device_registry; }
    PluginLoader& GetPluginLoader() { return plugin_loader; }
    EventBus& GetEventBusPublic() { return event_bus; }

    bool IsRunning() const { return is_running; }

private:
    Runtime();
    ~Runtime();

    // Components
    EventBus event_bus;
    DeviceRegistry device_registry;
    ConfigManager config_manager;
    PluginLoader plugin_loader;
    std::unique_ptr<Bridge> bridge;

    // State
    std::atomic<bool> is_running{false};
    std::string m_configDir;

    // Callback queue (dla PostToMainThread)
    std::queue<std::function<void()>> main_thread_callbacks;
    std::mutex callback_mutex;
};

} // namespace opendriver::core
