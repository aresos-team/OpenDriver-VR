#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace opendriver::core {

enum class IPCMessageType : uint32_t {
    DEVICE_ADDED = 0x01,
    DEVICE_REMOVED = 0x02,
    POSE_UPDATE = 0x03,
    PROPERTY_UPDATE = 0x04,
    INPUT_UPDATE = 0x05,
    HAPTIC_EVENT = 0x06,
    VIDEO_PACKET = 0x07,
    HEARTBEAT = 0xFF
};

struct IPCMessage {
    IPCMessageType type;
    std::vector<uint8_t> data;
};

// ============================================================================
// BINARY IPC STRUCTURES (Common for Core and Driver)
// ============================================================================

#pragma pack(push, 1)
struct IPCInputUpdate {
    char device_id[64];
    char component_name[64];
    float value;
};

struct IPCHapticEvent {
    char device_id[64];
    float duration;
    float frequency;
    float amplitude;
};

struct IPCPoseData {
    char device_id[64];
    double posX, posY, posZ;
    double rotW, rotX, rotY, rotZ;
    double velX, velY, velZ;
    double angVelX, angVelY, angVelZ;
};
#pragma pack(pop)


/// Interfejs dla serwera IPC (działa w Runnerze)
class IIPCServer {
public:
    virtual ~IIPCServer() = default;
    
    /// Rozpoczęcie nasłuchiwania
    virtual bool Start(const std::string& address) = 0;
    
    /// Zatrzymanie serwera
    virtual void Stop() = 0;
    
    /// Wysłanie wiadomości do wszystkich połączonych klientów
    virtual void Broadcast(const IPCMessage& msg) = 0;
    
    /// Odebranie wiadomości od dowolnego klienta
    virtual bool Receive(IPCMessage& msg, int timeout_ms = -1) = 0;

    /// Sprawdzenie czy są aktywni klienci
    virtual bool HasClients() const = 0;
};

/// Interfejs dla klienta IPC (działa w sterowniku SteamVR)
class IIPCClient {
public:
    virtual ~IIPCClient() = default;
    
    /// Połączenie z serwerem
    virtual bool Connect(const std::string& address) = 0;
    
    /// Rozłączenie
    virtual void Disconnect() = 0;
    
    /// Odebranie wiadomości (blokujące lub nie)
    virtual bool Receive(IPCMessage& msg, int timeout_ms = -1) = 0;
    
    /// Wysłanie wiadomości do serwera
    virtual bool Send(const IPCMessage& msg) = 0;

    /// Czy jesteśmy połączeni?
    virtual bool IsConnected() const = 0;
};

/// Factory do tworzenia instancji zależnych od platformy
std::unique_ptr<IIPCServer> CreateIPCServer();
std::unique_ptr<IIPCClient> CreateIPCClient();

} // namespace opendriver::core
