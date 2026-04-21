#include <opendriver/core/plugin_loader.h>
#include <opendriver/core/event_bus.h>
#include <opendriver/core/dynlib.h>
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
    
    try {
        LibHandle handle = OpenLibrary(path);
        if (!handle) {
            std::string err_msg = std::string("Failed to load library: ") + DynError();
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Error), err_msg.c_str());
            return false;
        }
        
        CreatePluginFn create_fn = (CreatePluginFn)DynSym(handle, "CreatePlugin");
        DestroyPluginFn destroy_fn = (DestroyPluginFn)DynSym(handle, "DestroyPlugin");
        
        if (!create_fn || !destroy_fn) {
            std::string err_msg = "Plugin at " + path + " is missing required exports (CreatePlugin/DestroyPlugin)";
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Error), err_msg.c_str());
            DynClose(handle);
            return false;
        }
        
        IPlugin* instance = nullptr;
        try {
            instance = create_fn();
        } catch (const std::exception& e) {
            std::string err_msg = "Exception during CreatePlugin for " + path + ": " + e.what();
            Logger::GetInstance().Critical("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
            DynClose(handle);
            return false;
        }
        
        if (!instance) {
            std::string err_msg = "CreatePlugin returned nullptr for " + path;
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Error), err_msg.c_str());
            DynClose(handle);
            return false;
        }
        
        std::string name = "Unknown";
        try {
            name = instance->GetName();
            
            // Logowanie wczytania pluginu
            std::string load_msg = "Initializing plugin: " + name + " (version " + instance->GetVersion() + ")";
            Logger::GetInstance().Info("PluginLoader", load_msg);
            context->Log(static_cast<int>(LogLevelEnum::Info), load_msg.c_str());
            
            if (!instance->OnInitialize(context)) {
                std::string err_msg = "Plugin " + name + " failed to initialize";
                Logger::GetInstance().Error("PluginLoader", err_msg);
                context->Log(static_cast<int>(LogLevelEnum::Error), err_msg.c_str());
                
                // Publikowanie PLUGIN_ERROR event
                PluginErrorData error_data(name, err_msg, "INIT_FAILED");
                Event error_evt(EventType::PLUGIN_ERROR, "core");
                error_evt.data = error_data;
                try {
                    context->GetEventBus().Publish(error_evt);
                } catch (...) {}
                
                destroy_fn(instance);
                DynClose(handle);
                return false;
            }
        } catch (const std::exception& e) {
            std::string err_msg = "Crash during OnInitialize for " + name + ": " + e.what();
            Logger::GetInstance().Critical("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
            
            PluginErrorData error_data(name, err_msg, "INIT_EXCEPTION");
            Event error_evt(EventType::PLUGIN_ERROR, "core");
            error_evt.data = error_data;
            try {
                context->GetEventBus().Publish(error_evt);
            } catch (...) {}
            
            destroy_fn(instance);
            DynClose(handle);
            return false;
        }
        
        // Sprawdzenie duplikatu
        if (plugins.find(name) != plugins.end()) {
            std::string err_msg = "Plugin with name " + name + " already loaded";
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Error), err_msg.c_str());
            instance->OnShutdown();
            destroy_fn(instance);
            DynClose(handle);
            return false;
        }
        
        LoadedPlugin lp;
        lp.instance       = instance;
        lp.library_handle = handle;
        lp.destroy_fn     = destroy_fn;
        lp.file_path      = path;
        lp.name           = name;
        
        plugins[name] = lp;
        load_order.push_back(name);
        
        try {
            if (fs::exists(path)) {
                plugins[name].last_write_time = fs::last_write_time(path);
            }
        } catch (...) {}
        
        std::string success_msg = "Plugin loaded successfully: " + name;
        Logger::GetInstance().Info("PluginLoader", success_msg);
        context->Log(static_cast<int>(LogLevelEnum::Info), success_msg.c_str());
        
        // Publikowanie PLUGIN_LOADED event
        Event load_evt(EventType::PLUGIN_LOADED, "core");
        load_evt.data = name;
        try {
            context->GetEventBus().Publish(load_evt);
        } catch (...) {}
        
        return true;
    } catch (const std::exception& e) {
        std::string err_msg = "Unexpected error loading plugin " + path + ": " + e.what();
        Logger::GetInstance().Critical("PluginLoader", err_msg);
        context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
        return false;
    } catch (...) {
        std::string err_msg = "Unknown error loading plugin " + path;
        Logger::GetInstance().Critical("PluginLoader", err_msg);
        context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
        return false;
    }
}

int PluginLoader::LoadDirectory(const std::string& plugins_dir, bool recursive) {
    int count = 0;
    if (!fs::exists(plugins_dir)) {
        Logger::GetInstance().Warn("PluginLoader", "Plugins directory does not exist: " + plugins_dir);
        return 0;
    }
    
    for (const auto& entry : fs::directory_iterator(plugins_dir)) {
        if (!entry.is_directory()) continue;

        std::string json_path = (entry.path() / "plugin.json").string();
        if (!fs::exists(json_path)) continue;

        try {
            std::ifstream f(json_path);
            nlohmann::json j = nlohmann::json::parse(f);
            
            bool enabled = j.value("enabled", true);
            if (!enabled) {
                Logger::GetInstance().Debug("PluginLoader", "Plugin disabled in json: " + entry.path().filename().string());
                continue;
            }

            std::string entry_point = j.value("entry_point", "");
            if (entry_point.empty()) {
                Logger::GetInstance().Error("PluginLoader", "No entry_point in " + json_path);
                continue;
            }

            std::string plugin_path = (entry.path() / entry_point).string();
            
            // Check if already loaded by path
            bool already_loaded = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [name, lp] : plugins) {
                    if (fs::path(lp.file_path) == fs::path(plugin_path)) {
                        already_loaded = true;
                        break;
                    }
                }
            }

            if (!already_loaded && Load(plugin_path)) {
                count++;
            }
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("PluginLoader", "Error processing " + entry.path().string() + ": " + e.what());
        }
    }
    return count;
}

bool PluginLoader::Unload(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = plugins.find(name);
    if (it == plugins.end()) {
        Logger::GetInstance().Warn("PluginLoader", "Plugin not found for unload: " + name);
        context->Log(static_cast<int>(LogLevelEnum::Warn), ("Plugin not loaded: " + name).c_str());
        return false;
    }
    
    try {
        LoadedPlugin& lp = it->second;
        
        Logger::GetInstance().Info("PluginLoader", "Unloading plugin: " + name);
        context->Log(static_cast<int>(LogLevelEnum::Info), ("Unloading plugin: " + name).c_str());
        
        context->UnregisterDevicesByPlugin(name.c_str());
        
        try {
            lp.instance->OnShutdown();
        } catch (const std::exception& e) {
            std::string err_msg = "Exception during OnShutdown for " + name + ": " + e.what();
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::Error), err_msg.c_str());
            
            PluginErrorData error_data(name, err_msg, "SHUTDOWN_EXCEPTION");
            Event error_evt(EventType::PLUGIN_ERROR, "core");
            error_evt.data = error_data;
            try {
                context->GetEventBus().Publish(error_evt);
            } catch (...) {}
        } catch (...) {
            Logger::GetInstance().Critical("PluginLoader", "Unknown error during OnShutdown for " + name);
            context->Log(static_cast<int>(LogLevelEnum::Critical), ("Critical error during shutdown: " + name).c_str());
        }
        
        try {
            lp.destroy_fn(lp.instance);
        } catch (const std::exception& e) {
            Logger::GetInstance().Error("PluginLoader", "Exception during DestroyPlugin for " + name + ": " + e.what());
        }
        
        try {
            DynClose(lp.library_handle);
        } catch (...) {
            Logger::GetInstance().Warn("PluginLoader", "Error closing library handle for " + name);
        }
        
        plugins.erase(it);
        load_order.erase(std::remove(load_order.begin(), load_order.end(), name), load_order.end());
        
        std::string msg = "Plugin unloaded: " + name;
        Logger::GetInstance().Info("PluginLoader", msg);
        context->Log(static_cast<int>(LogLevelEnum::Info), msg.c_str());
        
        Event unload_evt(EventType::PLUGIN_UNLOADED, "core");
        unload_evt.data = name;
        try {
            context->GetEventBus().Publish(unload_evt);
        } catch (...) {}
        
        return true;
    } catch (const std::exception& e) {
        std::string err_msg = "Unexpected error unloading plugin " + name + ": " + e.what();
        Logger::GetInstance().Critical("PluginLoader", err_msg);
        context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
        return false;
    }
}

void PluginLoader::UnloadAll() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = load_order.rbegin(); it != load_order.rend(); ++it) {
        auto pit = plugins.find(*it);
        if (pit != plugins.end()) {
            LoadedPlugin& lp = pit->second;
            try { lp.instance->OnShutdown(); } catch (...) {}
            try { lp.destroy_fn(lp.instance); } catch (...) {}
            DynClose(lp.library_handle);
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
                    std::string err_msg = "Exception in [" + name + "] OnTick: " + e.what();
                    Logger::GetInstance().Critical("Plugin", err_msg);
                    context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
                    
                    PluginErrorData error_data(name, err_msg, "TICK_EXCEPTION");
                    Event error_evt(EventType::PLUGIN_ERROR, "core");
                    error_evt.data = error_data;
                    try {
                        context->GetEventBus().Publish(error_evt);
                    } catch (...) {}
                    
                    Logger::GetInstance().Error("Plugin", "Disabling plugin [" + name + "] due to crash.");
                    context->Log(static_cast<int>(LogLevelEnum::Error), ("Disabling plugin: " + name).c_str());
                    to_unload.push_back(name);
                } catch (...) {
                    std::string err_msg = "Unknown exception in [" + name + "] OnTick";
                    Logger::GetInstance().Critical("Plugin", err_msg);
                    context->Log(static_cast<int>(LogLevelEnum::Critical), err_msg.c_str());
                    
                    PluginErrorData error_data(name, err_msg, "TICK_UNKNOWN_EXCEPTION");
                    Event error_evt(EventType::PLUGIN_ERROR, "core");
                    error_evt.data = error_data;
                    try {
                        context->GetEventBus().Publish(error_evt);
                    } catch (...) {}
                    
                    to_unload.push_back(name);
                }
            }
        }

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
                            try { 
                                state = lp.instance->ExportState(); 
                            } catch (...) {}
                            states_to_pass.push_back({name, state});
                        }
                    }
                } catch (...) {}
            }
        }
    }

    for (const auto& n : to_unload) {
        Unload(n);
    }

    for (const auto& pair : states_to_pass) {
        std::string name = pair.first;
        void* saved_state = pair.second;
        std::string path_copy = paths_to_reload[name];

        if (Unload(name)) {
            if (Load(path_copy)) {
                IPlugin* reloaded = Get(name);
                if (reloaded) {
                    try { reloaded->ImportState(saved_state); } catch (...) {}
                }
            }
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
        } catch (...) {}
    }
}

std::vector<PluginLoader::AvailablePlugin> PluginLoader::GetAvailablePlugins() {
    std::lock_guard<std::mutex> lock(mutex);
    return available_plugins;
}

LibHandle PluginLoader::OpenLibrary(const std::string& path) {
    if (!fs::exists(path)) return kNullHandle;
    return DynOpen(path);
}

void PluginLoader::CloseLibrary(LibHandle handle) {
    DynClose(handle);
}

void* PluginLoader::GetSymbol(LibHandle handle, const std::string& name) {
    return DynSym(handle, name);
}

} // namespace opendriver::core
