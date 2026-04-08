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
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
            return false;
        }
        
        CreatePluginFn create_fn = (CreatePluginFn)DynSym(handle, "CreatePlugin");
        DestroyPluginFn destroy_fn = (DestroyPluginFn)DynSym(handle, "DestroyPlugin");
        
        if (!create_fn || !destroy_fn) {
            std::string err_msg = "Plugin at " + path + " is missing required exports (CreatePlugin/DestroyPlugin)";
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
            DynClose(handle);
            return false;
        }
        
        IPlugin* instance = nullptr;
        try {
            instance = create_fn();
        } catch (const std::exception& e) {
            std::string err_msg = "Exception during CreatePlugin for " + path + ": " + e.what();
            Logger::GetInstance().Critical("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
            DynClose(handle);
            return false;
        }
        
        if (!instance) {
            std::string err_msg = "CreatePlugin returned nullptr for " + path;
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
            dlclose(handle);
            return false;
        }
        
        std::string name = "Unknown";
        try {
            name = instance->GetName();
            
            // Logowanie wczytania pluginu
            std::string load_msg = "Initializing plugin: " + name + " (version " + instance->GetVersion() + ")";
            Logger::GetInstance().Info("PluginLoader", load_msg);
            context->Log(static_cast<int>(LogLevelEnum::INFO), load_msg.c_str());
            
            if (!instance->OnInitialize(context)) {
                std::string err_msg = "Plugin " + name + " failed to initialize";
                Logger::GetInstance().Error("PluginLoader", err_msg);
                context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
                
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
            context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
            
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
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
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
        context->Log(static_cast<int>(LogLevelEnum::INFO), success_msg.c_str());
        
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
        context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
        return false;
    } catch (...) {
        std::string err_msg = "Unknown error loading plugin " + path;
        Logger::GetInstance().Critical("PluginLoader", err_msg);
        context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
        return false;
    }
}

int PluginLoader::LoadDirectory(const std::string& plugins_dir, bool recursive) {
    int count = 0;
    if (!fs::exists(plugins_dir)) {
        Logger::GetInstance().Warn("PluginLoader", "Plugins directory does not exist: " + plugins_dir);
        context->Log(static_cast<int>(LogLevelEnum::WARN), ("Plugins directory not found: " + plugins_dir).c_str());
        return 0;
    }
    
    try {
        for (const auto& entry : fs::directory_iterator(plugins_dir)) {
            try {
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
                                
                                if (Load(plugin_path)) {
                                    count++;
                                }
                            } else {
                                Logger::GetInstance().Debug("PluginLoader", "Plugin disabled in config: " + entry.path().filename().string());
                            }
                        } catch (const nlohmann::json::exception& je) {
                            std::string err_msg = "JSON parse error in " + json_path + ": " + je.what();
                            Logger::GetInstance().Error("PluginLoader", err_msg);
                            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
                        } catch (const std::exception& e) {
                            std::string err_msg = "Error loading plugin from " + entry.path().string() + ": " + e.what();
                            Logger::GetInstance().Error("PluginLoader", err_msg);
                            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
                        }
                    }
                }
            } catch (const std::exception& e) {
                Logger::GetInstance().Warn("PluginLoader", "Error scanning directory entry: " + std::string(e.what()));
            }
        }
        
        if (count > 0) {
            std::string msg = "Loaded " + std::to_string(count) + " plugin(s) from " + plugins_dir;
            Logger::GetInstance().Info("PluginLoader", msg);
            context->Log(static_cast<int>(LogLevelEnum::INFO), msg.c_str());
        }
    } catch (const std::exception& e) {
        std::string err_msg = "Error scanning plugins directory: " + std::string(e.what());
        Logger::GetInstance().Error("PluginLoader", err_msg);
        context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
    }
    
    return count;
}

bool PluginLoader::Unload(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = plugins.find(name);
    if (it == plugins.end()) {
        Logger::GetInstance().Warn("PluginLoader", "Plugin not found for unload: " + name);
        context->Log(static_cast<int>(LogLevelEnum::WARN), ("Plugin not loaded: " + name).c_str());
        return false;
    }
    
    try {
        LoadedPlugin& lp = it->second;
        
        Logger::GetInstance().Info("PluginLoader", "Unloading plugin: " + name);
        context->Log(static_cast<int>(LogLevelEnum::INFO), ("Unloading plugin: " + name).c_str());
        
        // Wyczyść ewentualne osierocone urządzenia w DeviceRegistry
        context->UnregisterDevicesByPlugin(name.c_str());
        
        try {
            lp.instance->OnShutdown();
        } catch (const std::exception& e) {
            std::string err_msg = "Exception during OnShutdown for " + name + ": " + e.what();
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
            
            PluginErrorData error_data(name, err_msg, "SHUTDOWN_EXCEPTION");
            Event error_evt(EventType::PLUGIN_ERROR, "core");
            error_evt.data = error_data;
            try {
                context->GetEventBus().Publish(error_evt);
            } catch (...) {}
        } catch (...) {
            Logger::GetInstance().Critical("PluginLoader", "Unknown error during OnShutdown for " + name);
            context->Log(static_cast<int>(LogLevelEnum::CRITICAL), ("Critical error during shutdown: " + name).c_str());
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
        context->Log(static_cast<int>(LogLevelEnum::INFO), msg.c_str());
        
        // Publikowanie PLUGIN_UNLOADED event
        Event unload_evt(EventType::PLUGIN_UNLOADED, "core");
        unload_evt.data = name;
        try {
            context->GetEventBus().Publish(unload_evt);
        } catch (...) {}
        
        return true;
    } catch (const std::exception& e) {
        std::string err_msg = "Unexpected error unloading plugin " + name + ": " + e.what();
        Logger::GetInstance().Critical("PluginLoader", err_msg);
        context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
        return false;
    }
}

void PluginLoader::UnloadAll() {
    std::lock_guard<std::mutex> lock(mutex);
    // Unload in reverse order of loading (LIFO)
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
                    context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
                    
                    PluginErrorData error_data(name, err_msg, "TICK_EXCEPTION");
                    Event error_evt(EventType::PLUGIN_ERROR, "core");
                    error_evt.data = error_data;
                    try {
                        context->GetEventBus().Publish(error_evt);
                    } catch (...) {}
                    
                    Logger::GetInstance().Error("Plugin", "Disabling plugin [" + name + "] due to crash.");
                    context->Log(static_cast<int>(LogLevelEnum::ERROR), ("Disabling plugin: " + name).c_str());
                    to_unload.push_back(name);
                } catch (...) {
                    std::string err_msg = "Unknown exception in [" + name + "] OnTick";
                    Logger::GetInstance().Critical("Plugin", err_msg);
                    context->Log(static_cast<int>(LogLevelEnum::CRITICAL), err_msg.c_str());
                    
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

        // Hot reload (hot reload na zmianę pliku)
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
                            } catch (const std::exception& e) {
                                Logger::GetInstance().Warn("PluginLoader", "ExportState failed for " + name + ": " + e.what());
                            } catch(...) {
                                Logger::GetInstance().Warn("PluginLoader", "Unknown error in ExportState for " + name);
                            }
                            states_to_pass.push_back({name, state});
                        }
                    }
                } catch (const std::exception& e) {
                    Logger::GetInstance().Warn("PluginLoader", "Error checking modification time for " + lp.file_path + ": " + e.what());
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

        try {
            Logger::GetInstance().Info("PluginLoader", "File modification detected. Hot reloading plugin: " + name);
            context->Log(static_cast<int>(LogLevelEnum::INFO), ("Hot reloading plugin: " + name).c_str());
            
            Unload(name);
            if (Load(path_copy)) {
                try { 
                    IPlugin* reloaded = Get(name);
                    if (reloaded) {
                        reloaded->ImportState(saved_state); 
                    }
                } catch (const std::exception& e) {
                    Logger::GetInstance().Warn("PluginLoader", "ImportState failed for " + name + ": " + e.what());
                } catch(...) {
                    Logger::GetInstance().Warn("PluginLoader", "Unknown error in ImportState for " + name);
                }
                
                Logger::GetInstance().Info("PluginLoader", "Plugin hot reloaded successfully: " + name);
                context->Log(static_cast<int>(LogLevelEnum::INFO), ("Hot reload success: " + name).c_str());
            } else {
                std::string err_msg = "Failed to reload plugin: " + name;
                Logger::GetInstance().Error("PluginLoader", err_msg);
                context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
            }
        } catch (const std::exception& e) {
            std::string err_msg = "Exception during hot reload for " + name + ": " + e.what();
            Logger::GetInstance().Error("PluginLoader", err_msg);
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err_msg.c_str());
        }
    }
}

void PluginLoader::ScanDirectory(const std::string& plugins_dir) {
    std::lock_guard<std::mutex> lock(mutex);
    available_plugins.clear();

    if (!fs::exists(plugins_dir)) {
        Logger::GetInstance().Warn("PluginLoader", "Plugins directory does not exist: " + plugins_dir);
        return;
    }

    try {
        for (const auto& entry : fs::directory_iterator(plugins_dir)) {
            try {
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
                } catch (const nlohmann::json::exception& je) {
                    Logger::GetInstance().Warn("PluginLoader", "JSON parse error in " + json_path.string() + ": " + je.what());
                } catch (const std::exception& e) {
                    Logger::GetInstance().Warn("PluginLoader", "Error scanning " + entry.path().string() + ": " + e.what());
                }
            } catch (const std::exception& e) {
                Logger::GetInstance().Warn("PluginLoader", "Error processing directory entry: " + std::string(e.what()));
            }
        }
    } catch (const std::exception& e) {
        Logger::GetInstance().Error("PluginLoader", "Error scanning plugins directory: " + std::string(e.what()));
    }
}

std::vector<PluginLoader::AvailablePlugin> PluginLoader::GetAvailablePlugins() {
    std::lock_guard<std::mutex> lock(mutex);
    return available_plugins;
}

LibHandle PluginLoader::OpenLibrary(const std::string& path) {
    try {
        if (!fs::exists(path)) {
            std::string err = "Plugin file does not exist: " + path;
            Logger::GetInstance().Error("PluginLoader", err);
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err.c_str());
            return kNullHandle;
        }
        
        LibHandle handle = DynOpen(path);
        if (!handle) {
            std::string err = std::string("Failed to load library ") + path + ": " + DynError();
            Logger::GetInstance().Error("PluginLoader", err);
            context->Log(static_cast<int>(LogLevelEnum::ERROR), err.c_str());
            return kNullHandle;
        }
        
        return handle;
    } catch (const std::exception& e) {
        std::string err = "Exception in OpenLibrary for " + path + ": " + e.what();
        Logger::GetInstance().Error("PluginLoader", err);
        context->Log(static_cast<int>(LogLevelEnum::ERROR), err.c_str());
        return kNullHandle;
    }
}

void PluginLoader::CloseLibrary(LibHandle handle) {
    DynClose(handle);
}

void* PluginLoader::GetSymbol(LibHandle handle, const std::string& name) {
    return DynSym(handle, name);
}

} // namespace opendriver::core
