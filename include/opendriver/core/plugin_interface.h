#pragma once

#include <string>
#include <memory>
#include <any>
#include <cstdint>
#include <functional>
#include <opendriver/core/iui_provider.h>

namespace opendriver::core {

// ============================================================================
// PLUGIN INTERFACE - każdy plugin musi implementować to
// ============================================================================

class IPluginContext;  // forward declaration

class IPlugin {
public:
    virtual ~IPlugin() = default;

    // ────────────────────────────────────────────────────────────────────
    // METADATA (dla GUI, logging, dependency resolution)
    // ────────────────────────────────────────────────────────────────────

    /// Unikatowa nazwa: "gyroscope_tracker", "leap_motion", etc.
    /// Musi być lowercase, bez spacji
    virtual const char* GetName() const = 0;

    /// Semantic version: "1.0.0", "2.1.3-beta"
    virtual const char* GetVersion() const = 0;

    /// Krótki opis dla GUI
    virtual const char* GetDescription() const = 0;

    /// Autor/organizacja
    virtual const char* GetAuthor() const = 0;

    // ────────────────────────────────────────────────────────────────────
    // LIFECYCLE
    // ────────────────────────────────────────────────────────────────────

    /// Inicjalizacja pluginu
    /// Wywoływane RAZ na startup, ZANIM plugin zacznie działać
    /// 
    /// @param context: dostęp do core (event bus, config, device registry, logging)
    /// @return true = init OK; false = init failed, plugin nie będzie ładowany
    virtual bool OnInitialize(IPluginContext* context) = 0;

    /// Wyłączenie pluginu
    /// Wywoływane RAZ na shutdown, plugin powinien zwolnić zasoby
    /// 
    /// Gwarancja: Wszystkie eventy wysłane przez ten plugin
    /// powinny być odebranych PRZED OnShutdown()
    virtual void OnShutdown() = 0;

    // ────────────────────────────────────────────────────────────────────
    // HOT RELOAD (zachowanie stanu)
    // ────────────────────────────────────────────────────────────────────

    /// Eksportuje stan wewnętrzny wskaźnika przed przeładowaniem wtyczki (.so)
    virtual void* ExportState() { return nullptr; }

    /// Importuje stan po ponownym załadowaniu i OnInitialize() nowej wersji .so
    virtual void ImportState(void* state) {}

    // ────────────────────────────────────────────────────────────────────
    // PER-FRAME UPDATES (opcjonalnie)
    // ────────────────────────────────────────────────────────────────────

    /// Co-frame tick (wywoływane ~90x na sekundę dla VR)
    /// 
    /// @param delta_time: czas od ostatniego frame'a (sekundy)
    /// 
    /// DEFAULT: no-op (nie musisz implementować jeśli nie potrzebujesz)
    virtual void OnTick(float delta_time) {}

    // ────────────────────────────────────────────────────────────────────
    // EVENT HANDLING (subscriber part)
    // ────────────────────────────────────────────────────────────────────

    /// Odbieranie eventów z event bus'a
    /// 
    /// Plugin subskrybuje się do eventu w OnInitialize()
    /// Wtedy OnEvent() jest wywoływane każdy raz gdy event jest publishowany
    /// 
    /// @param event: event do obsłużenia
    virtual void OnEvent(const class Event& event) {}

    // ────────────────────────────────────────────────────────────────────
    // STATUS & DEBUGGING
    // ────────────────────────────────────────────────────────────────────

    /// Czy plugin jest aktywny?
    /// Zwraca false jeśli plugin napotkał niereparowalny błąd
    /// 
    /// Core będzie co frame sprawdzać - jeśli false, wyłączy plugin
    virtual bool IsActive() const = 0;

    /// Opis statusu pluginu dla dashboard'u
    /// Może zawierać:
    /// - "Connected: yes"
    /// - "FPS: 90"
    /// - "Latency: 5ms"
    /// Format: każda linia to \n
    virtual std::string GetStatus() const { return "OK"; }

    /// Opcjonalny interfejs UI dla pluginu
    virtual IUIProvider* GetUIProvider() { return nullptr; }
};

// ============================================================================
// PLUGIN CONTEXT - co plugin może robić z core'em
// ============================================================================

class IPluginContext {
public:
    virtual ~IPluginContext() = default;

    // ────────────────────────────────────────────────────────────────────
    // EVENT BUS (publikowanie i subskrybowanie)
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie event bus'a
    /// Używaj: context->GetEventBus().Subscribe(...)
    virtual class EventBus& GetEventBus() = 0;

    // ────────────────────────────────────────────────────────────────────
    // CONFIGURATION (czytanie i zapis config'u)
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie konfiguracji
    /// Zwraca JSON node dla tego pluginu
    /// Np. dla "leap_motion" plugin, zwraca config.json["plugins"]["leap_motion"]
    virtual class ConfigManager& GetConfig() = 0;

    // ────────────────────────────────────────────────────────────────────
    // LOGGING (pisanie do centralized log'u)
    // ────────────────────────────────────────────────────────────────────

    /// Zalogowanie wiadomości
    /// Automatycznie dodaje timestamp i plugin name
    /// Output: "[2024-01-15 14:23:45.123] [INFO] [leap_motion] Hand detected"
    virtual void Log(int level, const char* message) = 0;

    // ────────────────────────────────────────────────────────────────────
    // DEVICE REGISTRY (rejestracja virtual devices)
    // ────────────────────────────────────────────────────────────────────

    /// Rejestracja virtual device do SteamVR
    /// Np. "phone_hmd", "left_hand", "right_hand"
    /// 
    /// @param device: struktura z ID, type, name, properties
    virtual void RegisterDevice(const struct Device& device) = 0;

    /// Pobranie zarejestrowanego device'a
    virtual const Device* GetDevice(const char* device_id) const = 0;

    /// Unregistration (jeśli plugin się wyłączy, powinien odrejestrować)
    virtual void UnregisterDevice(const char* device_id) = 0;

    /// Usunięcie wszystkich urządzeń zarejestrowanych przez dany plugin
    virtual void UnregisterDevicesByPlugin(const char* plugin_name) = 0;

    // ────────────────────────────────────────────────────────────────────
    // PLUGIN DEPENDENCIES (jeśli jeden plugin chce użyć innego)
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie innego pluginu (dependency)
    /// Zwraca nullptr jeśli plugin nie załadowany
    /// 
    /// Używaj: auto leap = context->GetPlugin("leap_motion");
    virtual IPlugin* GetPlugin(const char* name) = 0;

    // ────────────────────────────────────────────────────────────────────
    // SCHEDULING / THREADING
    // ────────────────────────────────────────────────────────────────────

    /// Wysyłanie statusu inputu (przycisk, trigger, axis)
    /// @param device_id: ID urządzenia (np. "left_hand")
    /// @param component_name: nazwa wejścia (np. "/input/trigger/value")
    /// @param value: wartość (0.0 - 1.0 dla analogów, 0/1 dla bool)
    virtual void UpdateInput(const char* device_id, const char* component_name, float value) = 0;

    /// Wysyłanie statusu pozycji i rotacji (Pose)
    /// @param device_id: ID urządzenia
    /// @param x, y, z: pozycja (metry)
    /// @param qw, qx, qy, qz: rotacja (quaternion)
    /// @param vx, vy, vz: prędkość liniowa (m/s) (używane dla predykcji)
    /// @param avx, avy, avz: prędkość kątowa (rad/s)
    virtual void UpdatePose(const char* device_id, 
                             double x, double y, double z, 
                             double qw, double qx, double qy, double qz,
                             double vx = 0, double vy = 0, double vz = 0,
                             double avx = 0, double avy = 0, double avz = 0) = 0;

    /// Zarejestrowanie callback'u do wykonania na głównym thread'a (thread-safe)
    virtual void PostToMainThread(std::function<void()> callback) = 0;
};

// ============================================================================
// FACTORY FUNCTIONS (wymagane dla każdego pluginu)
// ============================================================================

/// Ta funkcja MUSI być w każdym plugin .so/.dll
/// Zwraca nową instancję pluginu
extern "C" {
    using CreatePluginFn = IPlugin*(*)();
}

/// Ta funkcja MUSI być w każdym plugin .so/.dll
/// Niszczy instancję pluginu (żeby core nie musiał wiedzeć o destruktorze)
extern "C" {
    using DestroyPluginFn = void(*)(IPlugin*);
}

} // namespace opendriver::core
