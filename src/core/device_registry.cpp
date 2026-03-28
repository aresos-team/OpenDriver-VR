#include <opendriver/core/device_registry.h>
#include <algorithm>

namespace opendriver::core {

void DeviceRegistry::Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    devices.clear();
}

bool DeviceRegistry::Register(const Device& device) {
    std::lock_guard<std::mutex> lock(mutex);
    if (devices.find(device.id) != devices.end()) {
        return false;
    }
    devices[device.id] = device;
    return true;
}

bool DeviceRegistry::Unregister(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = devices.find(device_id);
    if (it != devices.end()) {
        devices.erase(it);
        return true;
    }
    return false;
}

std::vector<std::string> DeviceRegistry::UnregisterByPlugin(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> removed_ids;
    for (auto it = devices.begin(); it != devices.end(); ) {
        if (it->second.owner_plugin == plugin_name) {
            removed_ids.push_back(it->first);
            it = devices.erase(it);
        } else {
            ++it;
        }
    }
    return removed_ids;
}

Device* DeviceRegistry::Get(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = devices.find(device_id);
    if (it != devices.end()) {
        return &it->second;
    }
    return nullptr;
}

const Device* DeviceRegistry::Get(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = devices.find(device_id);
    if (it != devices.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<Device*> DeviceRegistry::GetByType(DeviceType type) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Device*> result;
    for (auto& [id, device] : devices) {
        if (device.type == type) {
            result.push_back(&device);
        }
    }
    return result;
}

bool DeviceRegistry::UpdateDevice(const std::string& device_id, const Device& updated) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = devices.find(device_id);
    if (it != devices.end()) {
        it->second = updated;
        return true;
    }
    return false;
}

bool DeviceRegistry::SetConnected(const std::string& device_id, bool connected) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = devices.find(device_id);
    if (it != devices.end()) {
        it->second.connected = connected;
        return true;
    }
    return false;
}

bool DeviceRegistry::Exists(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex);
    return devices.find(device_id) != devices.end();
}

size_t DeviceRegistry::GetCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return devices.size();
}

size_t DeviceRegistry::GetCountByType(DeviceType type) const {
    std::lock_guard<std::mutex> lock(mutex);
    size_t count = 0;
    for (auto& [id, device] : devices) {
        if (device.type == type) {
            count++;
        }
    }
    return count;
}

std::vector<Device*> DeviceRegistry::GetAll() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Device*> result;
    for (auto& [id, device] : devices) {
        result.push_back(&device);
    }
    return result;
}

} // namespace opendriver::core
