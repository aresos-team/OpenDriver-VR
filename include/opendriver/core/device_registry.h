#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace opendriver::core {

// ============================================================================
// DEVICE TYPES (co typy urządzeń może mieć system)
// ============================================================================

enum class DeviceType : uint32_t {
  HMD = 0,             // Headset
  HAND_TRACKER = 1,    // Hand tracking
  GENERIC_TRACKER = 2, // Generic 3-axis tracker
  LIGHTHOUSE = 3,      // Base station
  CONTROLLER = 4,      // Input device
};

// ============================================================================
// INPUT COMPONENT DEFINITION
// ============================================================================

enum class InputType : uint32_t {
  BOOLEAN = 0, // Click, Touch, etc.
  SCALAR = 1,  // Trigger, Stick axis, etc.
};

struct InputComponent {
    std::string name = "unknown";       // "trigger", "thumbstick_x", etc.
    InputType type = InputType::BOOLEAN; // Boolean or Scalar
    uint64_t handle = 0;    // Internal OpenVR handle (set by driver)
};

// ============================================================================
// DEVICE STRUCTURE
// ============================================================================

struct Device {
  std::string id;            // "phone_hmd", "left_hand", etc.
  DeviceType type = DeviceType::GENERIC_TRACKER; // HMD, HAND_TRACKER, etc.
  std::string name = "OpenDriver Device";           // "Phone VR Headset"
  std::string manufacturer = "Generic DIY";  // "OpenDriver"
  std::string serial_number; // unique
  std::string owner_plugin;  // nazwa wtyczki, ktora jest wlascicielem (idiot-proof cleanup)

  // Input components
  std::vector<InputComponent> inputs;


  // Properties (dla SteamVR)
  uint32_t vendor_id = 0;         // USB vendor ID (optional)
  uint32_t product_id = 0;        // USB product ID (optional)
  float battery_percent = 100.0f; // 0-100
  bool connected = true;

  // Display properties (dla HMD)
  struct {
    uint32_t width = 1920;
    uint32_t height = 1200;
    float refresh_rate = 90.0f;
    float fov_left = 100.0f;
    float fov_right = 100.0f;
  } display;

  // Tracking properties (dla trackerów)
  struct {
    bool has_gyro = true;
    bool has_accel = true;
    bool has_compass = false;
    uint32_t update_rate = 90;
  } tracking;
};

// ============================================================================
// DEVICE REGISTRY (central storage for all devices)
// ============================================================================

class DeviceRegistry {
public:
  DeviceRegistry() = default;
  ~DeviceRegistry() = default;

  // Nie kopiowalny
  DeviceRegistry(const DeviceRegistry &) = delete;
  DeviceRegistry &operator=(const DeviceRegistry &) = delete;

  // ────────────────────────────────────────────────────────────────────
  // REGISTRATION
  // ────────────────────────────────────────────────────────────────────

  /// Rejestracja nowego device'a
  ///
  /// Przykład:
  ///   Device hmd;
  ///   hmd.id = "phone_hmd";
  ///   hmd.type = DeviceType::HMD;
  ///   hmd.name = "Phone VR Headset";
  ///   registry.Register(hmd);
  ///
  /// @param device: struktura z informacjami
  /// @return true = OK; false = już istnieje device z tym ID
  bool Register(const Device &device);

  /// Unregister device
  /// @return true = OK; false = nie znaleziono
  bool Unregister(const std::string &device_id);

  /// Wyczyść osierocone urządzenia danego pluginu
  /// @return lista usuniętych ID
  std::vector<std::string> UnregisterByPlugin(const std::string &plugin_name);

  /// Czyści cały rejestr (dla ReloadPlugins)
  void Clear();

  // ────────────────────────────────────────────────────────────────────
  // LOOKUP
  // ────────────────────────────────────────────────────────────────────

  /// Pobranie device'a po ID
  /// @return Device pointer, lub nullptr jeśli nie znaleziono
  Device *Get(const std::string &device_id);
  const Device *Get(const std::string &device_id) const;

  /// Pobranie wszystkich device'ów danego typu
  /// Przykład: GetByType(DeviceType::HMD) zwraca wszystkie HMD'y
  std::vector<Device *> GetByType(DeviceType type);

  // ────────────────────────────────────────────────────────────────────
  // UPDATE STATUS
  // ────────────────────────────────────────────────────────────────────

  /// Update statusu device'a (np. battery, connection state)
  /// Zwraca false jeśli device nie istnieje
  bool UpdateDevice(const std::string &device_id, const Device &updated);

  /// Update connected status
  bool SetConnected(const std::string &device_id, bool connected);

  // ────────────────────────────────────────────────────────────────────
  // QUERIES
  // ────────────────────────────────────────────────────────────────────

  /// Czy device istnieje?
  bool Exists(const std::string &device_id) const;

  /// Liczba wszystkich device'ów
  size_t GetCount() const;

  /// Liczba device'ów typu
  size_t GetCountByType(DeviceType type) const;

  // ────────────────────────────────────────────────────────────────────
  // ENUMERATION
  // ────────────────────────────────────────────────────────────────────

  /// Iteracja po wszystkich device'ach
  ///
  /// Przykład:
  ///   for (auto& device : registry.GetAll()) {
  ///       printf("%s: %s\n", device.id.c_str(), device.name.c_str());
  ///   }
  std::vector<Device *> GetAll();

private:
  std::map<std::string, Device> devices;
  mutable std::mutex mutex;
};

} // namespace opendriver::core
