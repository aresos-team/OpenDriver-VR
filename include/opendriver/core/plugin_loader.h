#pragma once

#include <opendriver/core/plugin_interface.h>
#include <opendriver/core/dynlib.h>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <filesystem>

namespace opendriver::core {

// ============================================================================
// PLUGIN LOADER (ładuje .so/.dll i zarządza lifecycle'em)
// ============================================================================

class PluginLoader {
public:
    PluginLoader(IPluginContext* context);
    ~PluginLoader();

    // ────────────────────────────────────────────────────────────────────
    // LOADING
    // ────────────────────────────────────────────────────────────────────

    /// Załaduj plugin z pliku
    /// 
    /// Co się dzieje:
    /// 1. dlopen() na .so
    /// 2. dlsym(CreatePlugin, DestroyPlugin)
    /// 3. CreatePlugin() - tworzy instancję
    /// 4. plugin->OnInitialize(context) - inicjalizacja
    /// 
    /// @param plugin_path: pełna ścieżka do .so
    /// @return true = success; false = failed (sprawdź logs)
    bool Load(const std::string& plugin_path);

    /// Załaduj wszystkie pluginy z folderu
    /// Skanuje folder i ładuje każdy plugin.json które ma "enabled": true
    /// 
    /// @param plugins_dir: ścieżka do folderu
    /// @param recursive: czy szukać w subfolderach
    int LoadDirectory(const std::string& plugins_dir, bool recursive = true);

    // ────────────────────────────────────────────────────────────────────
    // UNLOADING
    // ────────────────────────────────────────────────────────────────────

    /// Wyladuj pojedynczy plugin
    /// 
    /// Kroki:
    /// 1. plugin->OnShutdown()
    /// 2. DestroyPlugin(plugin)
    /// 3. dlclose(handle)
    /// 
    /// @param plugin_name: nazwa pluginu (z GetName())
    /// @return true = success; false = not found
    bool Unload(const std::string& plugin_name);

    /// Wyladuj WSZYSTKIE pluginy
    /// Odwrotna kolejność ładowania (LIFO)
    void UnloadAll();

    // ────────────────────────────────────────────────────────────────────
    // QUERIES
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie załadowanego pluginu
    IPlugin* Get(const std::string& plugin_name);

    /// Lista wszystkich załadowanych pluginów (nazwy)
    std::vector<std::string> GetLoadedPlugins() const;

    /// Bezpośredni dostęp do instancji pluginów (dla UI)
    std::vector<IPlugin*> GetPlugins();

    /// Czy plugin jest załadowany?
    bool IsLoaded(const std::string& plugin_name) const;

    /// Liczba załadowanych pluginów
    size_t GetCount() const;

    // ────────────────────────────────────────────────────────────────────
    // PLUGIN MANAGEMENT (dla UI)
    // ────────────────────────────────────────────────────────────────────

    struct AvailablePlugin {
        std::string name;
        std::string version;
        std::string description;
        std::string author;
        std::string path;      // ścieżka do folderu z pluginem
        bool is_loaded;        // czy jest obecnie wczytany w pamięci
        bool is_enabled;       // czy "enabled" w plugin.json
        std::string error;     // błąd ładowania, jeśli wystąpił
    };

    /// Skanuje folder w poszukiwaniu plugin.json (nie ładuje bibliotek)
    void ScanDirectory(const std::string& plugins_dir);

    /// Pobiera listę wszystkich znalezionych pluginów
    std::vector<AvailablePlugin> GetAvailablePlugins();

    // ────────────────────────────────────────────────────────────────────
    // TICK (co-frame updates)
    // ────────────────────────────────────────────────────────────────────

    /// Wywoł OnTick() na wszystkich pluginach
    /// Zwykle wywoływane co frame (90 razy na sekundę dla VR)
    /// 
    /// @param delta_time: czas od ostatniego frame'a (sekundy)
    void TickAll(float delta_time);

private:
    struct LoadedPlugin {
        IPlugin*       instance;
        LibHandle      library_handle;
        DestroyPluginFn destroy_fn;
        std::string    file_path;
        std::string    name;
        std::filesystem::file_time_type last_write_time;
    };

    std::map<std::string, LoadedPlugin> plugins;
    std::vector<std::string> load_order;
    std::vector<AvailablePlugin> available_plugins;
    IPluginContext* context;
    mutable std::mutex mutex;
    float hot_reload_timer = 0.0f;

    LibHandle OpenLibrary(const std::string& path);
    void      CloseLibrary(LibHandle handle);
    void*     GetSymbol(LibHandle handle, const std::string& name);
};

} // namespace opendriver::core
