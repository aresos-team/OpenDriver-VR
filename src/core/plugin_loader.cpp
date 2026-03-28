#include <opendriver/core/plugin_loader.h>
#include <dlfcn.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <opendriver/core/logger.h>

namespace fs = std::filesystem;

namespace opendriver::core {

PluginLoader::PluginLoader(IPluginContext* ctx) : context(ctx) {}

PluginLoader::~PluginLoader() {
    UnloadAll();
}

bool PluginLoader::Load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    
    void* handle = OpenLibrary(path);
    if (!handle) {
        return false;
    }
    
    CreatePluginFn create_fn = (CreatePluginFn)dlsym(handle, "CreatePlugin");
    DestroyPluginFn destroy_fn = (DestroyPluginFn)dlsym(handle, "DestroyPlugin");
    
    if (!create_fn || !destroy_fn) {
        std::cerr << "Plugin at " << path << " is missing required exports" << std::endl;
        dlclose(handle);
        return false;
    }
    
    IPlugin* instance = create_fn();
    if (!instance) {
        Logger::GetInstance().Error("PluginLoader", "Failed to create plugin instance from " + path);
        dlclose(handle);
        return false;
    }
    
    std::string name = "Unknown";
    try {
        name = instance->GetName();
        if (!instance->OnInitialize(context)) {
            Logger::GetInstance().Error("PluginLoader", "Plugin " + name + " failed to initialize");
            destroy_fn(instance);
            dlclose(handle);
            return false;
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Critical("PluginLoader", "Crash during OnInitialize for " + path + ": " + e.what());
        destroy_fn(instance);
        dlclose(handle);
        return false;
    }
    
    name = instance->GetName();
    if (plugins.find(name) != plugins.end()) {
        Logger::GetInstance().Error("PluginLoader", "Plugin with name " + name + " already loaded");
        instance->OnShutdown();
        destroy_fn(instance);
        dlclose(handle);
        return false;
    }
    
    LoadedPlugin lp;
    lp.instance = instance;
    lp.library_handle = handle;
    lp.destroy_fn = destroy_fn;
    lp.file_path = path;
    lp.name = name;
    
    plugins[name] = lp;
    load_order.push_back(name);
    
    try {
        if (fs::exists(path)) {
            plugins[name].last_write_time = fs::last_write_time(path);
        }
    } catch (...) {}
    
    return true;
}

int PluginLoader::LoadDirectory(const std::string& plugins_dir, bool recursive) {
    int count = 0;
    if (!fs::exists(plugins_dir)) return 0;
    
    for (const auto& entry : fs::directory_iterator(plugins_dir)) {
        if (entry.is_directory()) {
            std::string json_path = (entry.path() / "plugin.json").string();
            if (fs::exists(json_path)) {
                try {
                    std::ifstream f(json_path);
                    nlohmann::json j;
                    f >> j;
                    
                    if (j.value("enabled", true)) {
                        std::string entry_point = j.at("entry_point").get<std::string>();
                        std::string plugin_path = (entry.path() / entry_point).string();
                        if (Load(plugin_path)) count++;
                    }
                } catch (...) {
                    std::cerr << "Failed to parse " << json_path << std::endl;
                }
            }
        }
    }
    return count;
}

bool PluginLoader::Unload(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = plugins.find(name);
    if (it == plugins.end()) return false;
    
    LoadedPlugin& lp = it->second;
    
    // Wyczyść ewentualne osierocone urządzenia w DeviceRegistry
    context->UnregisterDevicesByPlugin(name.c_str());
    
    try {
        lp.instance->OnShutdown();
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("PluginLoader", "Exception during OnShutdown for " + name + ": " + e.what());
    }
    lp.destroy_fn(lp.instance);
    dlclose(lp.library_handle);
    
    plugins.erase(it);
    load_order.erase(std::remove(load_order.begin(), load_order.end(), name), load_order.end());
    
    return true;
}

void PluginLoader::UnloadAll() {
    std::lock_guard<std::mutex> lock(mutex);
    // Unload in reverse order
    for (auto it = load_order.rbegin(); it != load_order.rend(); ++it) {
        auto pit = plugins.find(*it);
        if (pit != plugins.end()) {
            LoadedPlugin& lp = pit->second;
            lp.instance->OnShutdown();
            lp.destroy_fn(lp.instance);
            dlclose(lp.library_handle);
        }
    }
    plugins.clear();
    load_order.clear();
}

IPlugin* PluginLoader::Get(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = plugins.find(name);
    if (it != plugins.end()) return it->second.instance;
    return nullptr;
}

std::vector<std::string> PluginLoader::GetLoadedPlugins() const {
    std::lock_guard<std::mutex> lock(mutex);
    return load_order;
}

std::vector<IPlugin*> PluginLoader::GetPlugins() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<IPlugin*> result;
    for (auto& [name, lp] : plugins) {
        result.push_back(lp.instance);
    }
    return result;
}

bool PluginLoader::IsLoaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex);
    return plugins.find(name) != plugins.end();
}

size_t PluginLoader::GetCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return plugins.size();
}

void PluginLoader::TickAll(float delta_time) {
    std::vector<std::string> to_unload;
    std::vector<std::string> to_reload;
    std::map<std::string, std::string> paths_to_reload;
    std::vector<std::pair<std::string, void*>> states_to_pass;

    {
        std::lock_guard<std::mutex> lock(mutex);

        for (auto& [name, lp] : plugins) {
            if (lp.instance->IsActive()) {
                try {
                    lp.instance->OnTick(delta_time);
                } catch (const std::exception& e) {
                    Logger::GetInstance().Critical("Plugin", "Exception in [" + name + "] OnTick: " + e.what());
                    Logger::GetInstance().Error("Plugin", "Disabling plugin [" + name + "] due to crash.");
                    to_unload.push_back(name);
                }
            }
        }

        // Hot plug (hot reload na zmianę pliku)
        hot_reload_timer += delta_time;
        if (hot_reload_timer >= 1.0f) {
            hot_reload_timer = 0.0f;
            for (auto& [name, lp] : plugins) {
                try {
                    if (fs::exists(lp.file_path)) {
                        auto current_time = fs::last_write_time(lp.file_path);
                        if (current_time > lp.last_write_time) {
                            to_reload.push_back(name);
                            paths_to_reload[name] = lp.file_path;
                            void* state = nullptr;
                            try { state = lp.instance->ExportState(); } catch(...) {}
                            states_to_pass.push_back({name, state});
                        }
                    }
                } catch (...) {}
            }
        }
    } // unlock mutex

    // Usunięcie wywalonych pluginów
    for (const auto& n : to_unload) {
        Unload(n);
    }

    // Wykonanie przeładowań po zwolnieniu locka
    for (const auto& pair : states_to_pass) {
        std::string name = pair.first;
        void* saved_state = pair.second;
        std::string path_copy = paths_to_reload[name];

        Logger::GetInstance().Info("PluginLoader", "File modification detected. Hot reloading plugin: " + name);
        
        Unload(name);
        if (Load(path_copy)) {
            try { Get(name)->ImportState(saved_state); } catch(...) {}
        }
    }
}

void PluginLoader::ScanDirectory(const std::string& plugins_dir) {
    std::lock_guard<std::mutex> lock(mutex);
    available_plugins.clear();

    if (!fs::exists(plugins_dir)) return;

    for (const auto& entry : fs::directory_iterator(plugins_dir)) {
        if (!entry.is_directory()) continue;

        fs::path json_path = entry.path() / "plugin.json";
        if (!fs::exists(json_path)) continue;

        try {
            std::ifstream f(json_path);
            nlohmann::json data = nlohmann::json::parse(f);

            AvailablePlugin ap;
            ap.name = data.value("name", entry.path().filename().string());
            ap.version = data.value("version", "0.0.0");
            ap.description = data.value("description", "");
            ap.author = data.value("author", "Unknown");
            ap.path = entry.path().string();
            ap.is_enabled = data.value("enabled", true);
            ap.is_loaded = (plugins.find(ap.name) != plugins.end());
            
            available_plugins.push_back(ap);
        } catch (const std::exception& e) {
            // Logger::GetInstance().Error("PluginLoader", "Error parsing plugin.json");
        }
    }
}

std::vector<PluginLoader::AvailablePlugin> PluginLoader::GetAvailablePlugins() {
    std::lock_guard<std::mutex> lock(mutex);
    return available_plugins;
}

void* PluginLoader::OpenLibrary(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        std::cerr << "Failed to load library " << path << ": " << dlerror() << std::endl;
    }
    return handle;
}

void PluginLoader::CloseLibrary(void* handle) {
    if (handle) dlclose(handle);
}

void* PluginLoader::GetSymbol(void* handle, const std::string& name) {
    return dlsym(handle, name.c_str());
}

} // namespace opendriver::core
