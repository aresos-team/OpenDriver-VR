#include <opendriver/core/config_manager.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace opendriver::core {

bool ConfigManager::Load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    config_path = path;
    
    std::ifstream file(path);
    if (!file.is_open()) {
        // Create initial empty config if it doesn't exist
        config = json::object();
        return true; 
    }
    
    try {
        file >> config;
        return true;
    } catch (const json::parse_error& e) {
        std::cerr << "Config parse error: " << e.what() << std::endl;
        config = json::object();
        return false;
    }
}

bool ConfigManager::Save() {
    std::lock_guard<std::mutex> lock(mutex);
    if (config_path.empty()) return false;
    
    std::ofstream file(config_path);
    if (!file.is_open()) return false;
    
    file << config.dump(4);
    return true;
}

bool ConfigManager::Reload() {
    return Load(config_path);
}

std::string ConfigManager::GetString(const std::string& path, const std::string& default_value) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        auto& val = GetValueRef(path);
        if (val.is_string()) return val.get<std::string>();
    } catch (...) {}
    return default_value;
}

int ConfigManager::GetInt(const std::string& path, int default_value) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        auto& val = GetValueRef(path);
        if (val.is_number_integer()) return val.get<int>();
    } catch (...) {}
    return default_value;
}

float ConfigManager::GetFloat(const std::string& path, float default_value) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        auto& val = GetValueRef(path);
        if (val.is_number()) return val.get<float>();
    } catch (...) {}
    return default_value;
}

bool ConfigManager::GetBool(const std::string& path, bool default_value) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        auto& val = GetValueRef(path);
        if (val.is_boolean()) return val.get<bool>();
    } catch (...) {}
    return default_value;
}

void ConfigManager::SetString(const std::string& path, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex);
    GetValueRef(path, true) = value;
}

void ConfigManager::SetInt(const std::string& path, int value) {
    std::lock_guard<std::mutex> lock(mutex);
    GetValueRef(path, true) = value;
}

void ConfigManager::SetFloat(const std::string& path, float value) {
    std::lock_guard<std::mutex> lock(mutex);
    GetValueRef(path, true) = value;
}

void ConfigManager::SetBool(const std::string& path, bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    GetValueRef(path, true) = value;
}

json& ConfigManager::GetPluginConfig(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!config.contains("plugins")) config["plugins"] = json::object();
    if (!config["plugins"].contains(plugin_name)) {
        config["plugins"][plugin_name] = json::object();
    }
    return config["plugins"][plugin_name];
}

bool ConfigManager::IsPluginEnabled(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!config.contains("plugins")) return true;
    if (!config["plugins"].contains(plugin_name)) return true;
    if (!config["plugins"][plugin_name].contains("enabled")) return true;
    return config["plugins"][plugin_name]["enabled"].get<bool>();
}

void ConfigManager::SetPluginEnabled(const std::string& plugin_name, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!config.contains("plugins")) config["plugins"] = json::object();
    if (!config["plugins"].contains(plugin_name)) {
        config["plugins"][plugin_name] = json::object();
    }
    config["plugins"][plugin_name]["enabled"] = enabled;
}

std::string ConfigManager::Dump() const {
    std::lock_guard<std::mutex> lock(mutex);
    return config.dump(4);
}

json& ConfigManager::GetValueRef(const std::string& path, bool create) {
    std::stringstream ss(path);
    std::string item;
    json* current = &config;
    
    while (std::getline(ss, item, '.')) {
        if (!current->contains(item)) {
            if (create) {
                (*current)[item] = json::object();
            } else {
                throw std::runtime_error("Path not found: " + path);
            }
        }
        current = &((*current)[item]);
    }
    return *current;
}

} // namespace opendriver::core
