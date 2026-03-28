#pragma once

#include <cstdint>
#include <string>
#include <any>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <chrono>

namespace opendriver::core {

// ============================================================================
// EVENT TYPES (centralna enumeracja)
// ============================================================================

enum class EventType : uint32_t {
    // Core lifecycle
    CORE_STARTUP = 0x0000,
    CORE_SHUTDOWN = 0x0001,

    // Plugin lifecycle
    PLUGIN_LOADED = 0x1000,
    PLUGIN_UNLOADED = 0x1001,
    PLUGIN_ERROR = 0x1002,

    // Configuration
    CONFIG_CHANGED = 0x2000,

    // Device events
    DEVICE_CONNECTED = 0x3000,
    DEVICE_DISCONNECTED = 0x3001,
    POSE_UPDATE = 0x3002,
    INPUT_UPDATE = 0x3003,
    HAPTIC_ACTION = 0x3004,

    // Generic plugin events (pluginy mogą definiować swoje własne)
    // Plugin może publikować EventType(0x8000 + custom_id)
    USER_DEFINED_BASE = 0x8000,
};

// ============================================================================
// EVENT STRUCTURE
// ============================================================================

struct Event {
    EventType type;
    uint64_t timestamp;        // ms since epoch
    const char* source_plugin; // który plugin opublikował
    std::any data;             // payload (parametry)
    
    Event() : type(EventType::CORE_STARTUP), timestamp(0), source_plugin(nullptr) {}
    
    Event(EventType t, const char* plugin)
        : type(t), timestamp(std::chrono::system_clock::now().time_since_epoch().count() / 1000000),
          source_plugin(plugin) {}
};

// ============================================================================
// EVENT LISTENER INTERFACE
// ============================================================================

class IEventListener {
public:
    virtual ~IEventListener() = default;
    virtual void OnEvent(const Event& event) = 0;
};

// ============================================================================
// EVENT BUS (centralna magistrala zdarzeń)
// ============================================================================

class EventBus {
public:
    EventBus() = default;
    ~EventBus() = default;

    // Nie kopiowalny, nie moveable (singleton pattern)
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // ────────────────────────────────────────────────────────────────────
    // SUBSCRIPTION
    // ────────────────────────────────────────────────────────────────────

    /// Subskrybowanie do eventu
    /// 
    /// Przykład:
    ///   EventBus& bus = context->GetEventBus();
    ///   bus.Subscribe(EventType::DEVICE_CONNECTED, my_plugin);
    /// 
    /// @param event_type: typ eventu do nasłuchiwania
    /// @param listener: obiekt implementujący IEventListener
    void Subscribe(EventType event_type, IEventListener* listener);

    /// Unsubscribe
    void Unsubscribe(EventType event_type, IEventListener* listener);

    // ────────────────────────────────────────────────────────────────────
    // PUBLISHING
    // ────────────────────────────────────────────────────────────────────

    /// Publikowanie eventu
    /// 
    /// Wszystkie subskrybenci dostaną OnEvent(event) callback
    /// Thread-safe - możesz publikować z wielu threadów
    /// 
    /// Przykład:
    ///   Event evt(EventType::DEVICE_CONNECTED, "gyroscope_tracker");
    ///   evt.data = device_info;
    ///   bus.Publish(evt);
    void Publish(const Event& event);

    // ────────────────────────────────────────────────────────────────────
    // POLLING (alternatywa do subscription)
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie ostatniego eventu danego typu (jeśli istnieje)
    /// Zwraca nullptr jeśli nigdy nie było tego eventu
    /// 
    /// Przydatne dla pluginów które chcą "zapytać" zamiast "słuchać"
    /// 
    /// Przykład:
    ///   auto evt = bus.GetLatestEvent(EventType::DEVICE_CONNECTED);
    ///   if (evt) { process(*evt); }
    const Event* GetLatestEvent(EventType event_type) const;

    // ────────────────────────────────────────────────────────────────────
    // DEBUGGING & MONITORING
    // ────────────────────────────────────────────────────────────────────

    /// Pobranie liczby subskrybentów eventu (dla diagnostyki)
    int GetSubscriberCount(EventType event_type) const;

    /// Czyszczenie starego eventu (jeśli callback ma big payload)
    void ClearEventCache(EventType event_type);

private:
    std::map<EventType, std::vector<IEventListener*>> subscribers;
    std::map<EventType, Event> event_cache;
    mutable std::mutex mutex;
};

} // namespace opendriver::core
