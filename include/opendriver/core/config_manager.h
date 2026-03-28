#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>

namespace opendriver::core {

using json = nlohmann::json;

// ============================================================================
// CONFIG MANAGER (czytanie, zapis, validation)
// ============================================================================

class ConfigManager {
public:
    ConfigManager() = default;

    // ────────────────────────────────────────────────────────────────────
    // LOADING & SAVING
    // ────────────────────────────────────────────────────────────────────

    /// Załaduj config z pliku
    /// 
    /// Domyślne lokacje:
    /// - Windows: %APPDATA%/opendriver/config.json
    /// - Linux: ~/.opendriver/config.json
    /// 
    /// @param config_path: pełna ścieżka do config.json
    /// @return true = loaded; false = failed (sprawdź logs)
    bool Load(const std::string& config_path);

    /// Zapisz config do pliku
    /// Utwórz backup jeśli plik już istnieje
    bool Save();

    /// Reload z dysku (undo changes)
    bool Reload();

    // ────────────────────────────────────────────────────────────────────
    // GET/SET VALUES
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie wartości z config'u
    /// 
    /// Przykład:
    ///   std::string encoder = GetString("plugins.h264_encoder.encoder");
    ///   int bitrate = GetInt("plugins.h264_encoder.bitrate");
    ///   bool enabled = GetBool("plugins.leap_motion.enabled");
    /// 
    /// JSON path format: "section.key.subkey"
    /// Zwraca default_value jeśli key nie istnieje
    
    std::string GetString(const std::string& path, const std::string& default_value = "");
    int GetInt(const std::string& path, int default_value = 0);
    float GetFloat(const std::string& path, float default_value = 0.0f);
    bool GetBool(const std::string& path, bool default_value = false);

    /// Ustawienie wartości
    /// Automatycznie tworzy pośrednie keys jeśli nie istnieją
    void SetString(const std::string& path, const std::string& value);
    void SetInt(const std::string& path, int value);
    void SetFloat(const std::string& path, float value);
    void SetBool(const std::string& path, bool value);

    // ────────────────────────────────────────────────────────────────────
    // PLUGIN SPECIFIC CONFIG
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie config dla konkretnego pluginu
    /// Zwraca JSON node: config["plugins"][plugin_name]
    json& GetPluginConfig(const std::string& plugin_name);

    /// Czy plugin jest enabled?
    bool IsPluginEnabled(const std::string& plugin_name) const;

    /// Enable/disable plugin w config'u
    void SetPluginEnabled(const std::string& plugin_name, bool enabled);

    /// Pobranie ścieżki do obecnie załadowanego pliku
    const std::string& GetConfigPath() const { return config_path; }

    // ────────────────────────────────────────────────────────────────────
    // DEBUGGING
    // ────────────────────────────────────────────────────────────────────

    /// Zwróć cały config jako formatted JSON string
    std::string Dump() const;

private:
    json config;
    std::string config_path;
    mutable std::mutex mutex;

    json& GetValueRef(const std::string& path, bool create = false);
};

} // namespace opendriver::core
