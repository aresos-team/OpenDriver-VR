#pragma once

#include <opendriver/core/ipc.h>
#include <opendriver/core/event_bus.h>
#include <opendriver/core/device_registry.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <iostream>
#include <thread>
#include <atomic>
#include <cstring>

namespace opendriver::core {


class Bridge : public IEventListener {
public:
    Bridge(EventBus& bus, DeviceRegistry& registry) 
        : event_bus(bus), device_registry(registry) {
        ipc_server = CreateIPCServer();
    }
    
    ~Bridge() {
        Stop();
    }

    bool Start(const std::string& address) {
        if (ipc_server->Start(address)) {
            is_running = true;
            receive_thread = std::thread(&Bridge::ReceiveLoop, this);
            return true;
        }
        return false;
    }

    void Stop() {
        is_running = false;
        if (receive_thread.joinable()) receive_thread.join();
        ipc_server->Stop();
    }

    void OnEvent(const Event& event) override {
        if (!ipc_server->HasClients()) return;

        IPCMessage msg;
        switch (event.type) {
            case EventType::DEVICE_CONNECTED: {
                msg.type = IPCMessageType::DEVICE_ADDED;
                std::string device_id = std::any_cast<std::string>(event.data);
                
                auto* device = device_registry.Get(device_id);
                if (device) {
                    nlohmann::json j;
                    j["serial_number"] = device->serial_number;
                    j["type"] = static_cast<uint32_t>(device->type);
                    j["name"] = device->name;
                    
                    // Parametry wyświetlacza (kluczowe dla HMD!)
                    if (device->type == DeviceType::HMD) {
                        nlohmann::json disp;
                        disp["width"] = device->display.width;
                        disp["height"] = device->display.height;
                        disp["refresh_rate"] = device->display.refresh_rate;
                        disp["fov_left"] = device->display.fov_left;
                        disp["fov_right"] = device->display.fov_right;
                        j["display"] = disp;
                    }
                    
                    auto inputs_arr = nlohmann::json::array();
                    for (const auto& in : device->inputs) {
                        nlohmann::json in_j;
                        in_j["name"] = in.name;
                        in_j["type"] = static_cast<uint32_t>(in.type);
                        inputs_arr.push_back(in_j);
                    }
                    j["inputs"] = inputs_arr;
                    
                    std::string s = j.dump();
                    msg.data.assign(s.begin(), s.end());
                    ipc_server->Broadcast(msg);
                }
                break;
            }
                
            case EventType::POSE_UPDATE: {
                msg.type = IPCMessageType::POSE_UPDATE;
                try {
                    auto update = std::any_cast<IPCPoseData>(event.data);
                    msg.data.assign((uint8_t*)&update, (uint8_t*)&update + sizeof(update));
                    ipc_server->Broadcast(msg);
                } catch(...) {}
                break;
            }

            case EventType::INPUT_UPDATE: {
                msg.type = IPCMessageType::INPUT_UPDATE;
                try {
                    auto update = std::any_cast<IPCInputUpdate>(event.data);
                    msg.data.assign((uint8_t*)&update, (uint8_t*)&update + sizeof(update));
                    ipc_server->Broadcast(msg);
                } catch(...) {}
                break;
            }
                
            default:
                break;
        }
    }

private:
    void ReceiveLoop() {
        bool had_clients = false;
        while (is_running) {
            bool has_clients_now = ipc_server->HasClients();
            
            // Nowy klient się podłączył - wyślij mu snapshot wszystkich urządzeń
            if (has_clients_now && !had_clients) {
                SendExistingDevices();
            }
            had_clients = has_clients_now;
            
            IPCMessage msg;
            if (ipc_server->Receive(msg, 100)) {
                if (msg.type == IPCMessageType::HAPTIC_EVENT) {
                    if (msg.data.size() >= sizeof(IPCHapticEvent)) {
                        IPCHapticEvent* haptic = (IPCHapticEvent*)msg.data.data();
                        
                        Event evt(EventType::HAPTIC_ACTION, "bridge");
                        evt.data = *haptic; 
                        event_bus.Publish(evt);
                    }
                }
            }
        }
    }

    void SendExistingDevices() {
        auto all_devices = device_registry.GetAll();
        for (auto* device : all_devices) {
            if (!device) continue;
            
            IPCMessage msg;
            msg.type = IPCMessageType::DEVICE_ADDED;
            
            nlohmann::json j;
            j["serial_number"] = device->serial_number;
            j["type"] = static_cast<uint32_t>(device->type);
            j["name"] = device->name;
            
            if (device->type == DeviceType::HMD) {
                nlohmann::json disp;
                disp["width"] = device->display.width;
                disp["height"] = device->display.height;
                disp["refresh_rate"] = device->display.refresh_rate;
                disp["fov_left"] = device->display.fov_left;
                disp["fov_right"] = device->display.fov_right;
                j["display"] = disp;
            }
            
            auto inputs_arr = nlohmann::json::array();
            for (const auto& in : device->inputs) {
                nlohmann::json in_j;
                in_j["name"] = in.name;
                in_j["type"] = static_cast<uint32_t>(in.type);
                inputs_arr.push_back(in_j);
            }
            j["inputs"] = inputs_arr;
            
            std::string s = j.dump();
            msg.data.assign(s.begin(), s.end());
            ipc_server->Broadcast(msg);
        }
    }

    EventBus& event_bus;
    DeviceRegistry& device_registry;
    std::unique_ptr<IIPCServer> ipc_server;
    std::thread receive_thread;
    std::atomic<bool> is_running{false};
};

} // namespace opendriver::core
