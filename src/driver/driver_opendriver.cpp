#define _GNU_SOURCE
#include <opendriver/core/ipc.h>
#include <opendriver/core/mapper.h>
#include <openvr_driver.h>
#include <vector>
#include <memory>
#include <thread>
#include <map>
#include <atomic>
#include <unistd.h>
#include <sys/types.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <dlfcn.h>
#include <filesystem>
#include <fcntl.h>
#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>

namespace fs = std::filesystem;

using namespace vr;
using namespace opendriver::core;

extern "C" void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode);

// Struktury binarne są teraz pobierane z opendriver/core/ipc.h

// ============================================================================
// DEVICE IMPLEMENTATION
// ============================================================================

class COpenDriverDevice : public ITrackedDeviceServerDriver {
public:
    COpenDriverDevice(const std::string& serial) : serial_number(serial) {}

    EVRInitError Activate(uint32_t unObjectId) override {
        object_id = unObjectId;
        property_container = VRProperties()->TrackedDeviceToPropertyContainer(object_id);

        VRProperties()->SetStringProperty(property_container, Prop_ModelNumber_String, "OpenDriver Modular Device");
        VRProperties()->SetStringProperty(property_container, Prop_SerialNumber_String, serial_number.c_str());
        VRProperties()->SetStringProperty(property_container, Prop_ManufacturerName_String, "OpenDriver Project");
        
        // Rejestracja wibracji (domyślnie dla każdego drajwera OpenDriver)
        VRDriverInput()->CreateHapticComponent(property_container, "/outputs/haptic", &haptic_handle);

        return VRInitError_None;
    }

    void Deactivate() override { object_id = k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char* pchComponentNameAndVersion) override { return nullptr; }
    void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {}
    DriverPose_t GetPose() override { return Mapper::CreateDefaultPose(); }

    void UpdatePose(const DriverPose_t& new_pose) {
        if (object_id != k_unTrackedDeviceIndexInvalid) {
            VRServerDriverHost()->TrackedDevicePoseUpdated(object_id, new_pose, sizeof(DriverPose_t));
        }
    }

    void RegisterInput(const std::string& name, InputType type) {
        VRInputComponentHandle_t handle;
        if (type == InputType::BOOLEAN) {
            VRDriverInput()->CreateBooleanComponent(property_container, name.c_str(), &handle);
        } else {
            VRDriverInput()->CreateScalarComponent(property_container, name.c_str(), &handle, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        }
        input_handles[name] = handle;
    }

    void UpdateInput(const std::string& name, float value) {
        auto it = input_handles.find(name);
        if (it != input_handles.end()) {
            // W OpenVR BooleanComponent oczekuje bool, ScalarComponent oczekuje float
            // Dla uproszczenia w protokole IPC używamy float 0/1 dla bool
            if (value > 0.5f) {
                VRDriverInput()->UpdateBooleanComponent(it->second, true, 0.0);
            } else if (value < -0.5f && value > -1.5f) { // Specyficzne dla bool (toggle off)
                 VRDriverInput()->UpdateBooleanComponent(it->second, false, 0.0);
            } else {
                // Scalar update
                VRDriverInput()->UpdateScalarComponent(it->second, value, 0.0);
            }
        }
    }

    PropertyContainerHandle_t GetPropertyContainer() const { return property_container; }
    const std::string& GetSerial() const { return serial_number; }

private:
    std::string serial_number;
    uint32_t object_id = k_unTrackedDeviceIndexInvalid;
    PropertyContainerHandle_t property_container = k_ulInvalidPropertyContainer;
    VRInputComponentHandle_t haptic_handle = k_ulInvalidInputComponentHandle;
    std::map<std::string, VRInputComponentHandle_t> input_handles;
};

// ============================================================================
// HMD DEVICE IMPLEMENTATION
// ============================================================================

class COpenDriverHMD : public COpenDriverDevice, public IVRDisplayComponent, public IVRVirtualDisplay {
public:
    COpenDriverHMD(const std::string& serial) : COpenDriverDevice(serial) {}

    void SetDisplayParams(uint32_t w, uint32_t h, float refresh, float fovL, float fovR) {
        width = w;
        height = h;
        refresh_rate = refresh;
        fov_left = fovL;
        fov_right = fovR;
    }

    EVRInitError Activate(uint32_t unObjectId) override {
        EVRInitError err = COpenDriverDevice::Activate(unObjectId);
        if (err != VRInitError_None) return err;

        auto container = GetPropertyContainer();
        VRProperties()->SetFloatProperty(container, Prop_DisplayFrequency_Float, refresh_rate);
        VRProperties()->SetBoolProperty(container, Prop_IsOnDesktop_Bool, false); // Używany IVRVirtualDisplay!
        VRProperties()->SetBoolProperty(container, Prop_ReportsTimeSinceVSync_Bool, true);
        VRProperties()->SetUint64Property(container, Prop_CurrentUniverseId_Uint64, 2);
        VRProperties()->SetStringProperty(container, Prop_TrackingSystemName_String, "opendriver");
        VRProperties()->SetStringProperty(container, Prop_RenderModelName_String, "generic_hmd");

        return VRInitError_None;
    }

    void* GetComponent(const char* pchComponentNameAndVersion) override {
        if (std::string(IVRDisplayComponent_Version) == pchComponentNameAndVersion) {
            return static_cast<IVRDisplayComponent*>(this);
        }
        if (std::string(IVRVirtualDisplay_Version) == pchComponentNameAndVersion) {
            return static_cast<IVRVirtualDisplay*>(this);
        }
        return COpenDriverDevice::GetComponent(pchComponentNameAndVersion);
    }

    // IVRDisplayComponent
    void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
        *pnX = 0;
        *pnY = 0;
        *pnWidth = width;
        *pnHeight = height;
    }

    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }

    void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override {
        *pnWidth = width / 2;
        *pnHeight = height;
    }

    void GetEyeOutputViewport(EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
        *pnY = 0;
        *pnWidth = width / 2;
        *pnHeight = height;
        if (eEye == Eye_Left) {
            *pnX = 0;
        } else {
            *pnX = width / 2;
        }
    }

    void GetProjectionRaw(EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override {
        float fov = (eEye == Eye_Left) ? fov_left : fov_right;
        float tanHalfFov = tanf(fov * 0.5f * 3.14159f / 180.0f);
        *pfLeft = -tanHalfFov;
        *pfRight = tanHalfFov;
        *pfTop = -tanHalfFov;
        *pfBottom = tanHalfFov;
    }

    DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU, float fV) override {
        DistortionCoordinates_t coordinates;
        coordinates.rfRed[0] = fU;
        coordinates.rfRed[1] = fV;
        coordinates.rfGreen[0] = fU;
        coordinates.rfGreen[1] = fV;
        coordinates.rfBlue[0] = fU;
        coordinates.rfBlue[1] = fV;
        return coordinates;
    }

    bool ComputeInverseDistortion(HmdVector2_t* pResult, EVREye eEye, uint32_t unChannel, float fU, float fV) override {
        pResult->v[0] = fU;
        pResult->v[1] = fV;
        return true;
    }

    // IVRVirtualDisplay
    void Present(const PresentInfo_t* pPresentInfo, uint32_t unPresentInfoSize) override {
        // Ignorujemy bufor wejściowy, to dummy!
    }

    void WaitForPresent() override {
        // Prosta symulacja VSync (11ms dla 90Hz)
        std::this_thread::sleep_for(std::chrono::milliseconds((int)(1000.0f / refresh_rate)));
    }

    bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) override {
        static auto start_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();
        double frame_duration = 1.0 / refresh_rate;
        *pulFrameCounter = (uint64_t)(elapsed / frame_duration);
        *pfSecondsSinceLastVsync = (float)fmod(elapsed, frame_duration);
        return true;
    }

private:
    uint32_t width = 1920;
    uint32_t height = 1080;
    float refresh_rate = 90.0f;
    float fov_left = 100.0f;
    float fov_right = 100.0f;
};

// ============================================================================
// SERVER DRIVER (The Provider)
// ============================================================================

class COpenDriverServerDriver : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* pDriverContext) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        
        if (VRDriverLog()) VRDriverLog()->Log("OpenDriver: Initializing server driver...");

        ipc_client = CreateIPCClient();
        
        // Próba połączenia z Runtime (GUI) - 3 próby z auto-startem
        for (int i = 0; i < 3; ++i) {
            if (ipc_client->Connect("/tmp/opendriver.sock")) {
                if (VRDriverLog()) VRDriverLog()->Log("OpenDriver: Connected to runtime socket.");
                break;
            }
            
            if (i == 0) {
                if (VRDriverLog()) VRDriverLog()->Log("OpenDriver: Runtime not found. Attempting to launch GUI...");
                LaunchGUI();
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        if (ipc_client->IsConnected()) {
            is_running = true;
            ipc_thread = std::thread(&COpenDriverServerDriver::IPCThreadLoop, this);
        } else {
            if (VRDriverLog()) VRDriverLog()->Log("OpenDriver: CRITICAL - Failed to connect to runtime after 3 attempts.");
        }

        return VRInitError_None;
    }

    void Cleanup() override {
        is_running = false;
        if (ipc_thread.joinable()) ipc_thread.join();
        ipc_client->Disconnect();
    }

    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }

    void RunFrame() override {
        // Przechwytywanie zdarzeń ze SteamVR (np. wibracje)
        VREvent_t event;
        while (VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == VREvent_Input_HapticVibration) {
                HandleHapticEvent(event.data.hapticVibration);
            }
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    void HandleHapticEvent(const VREvent_HapticVibration_t& haptic) {
        // Znajdź urządzenie po containerze
        for (auto const& [serial, device] : devices) {
            if (device->GetPropertyContainer() == haptic.containerHandle) {
                IPCHapticEvent payload;
                strncpy(payload.device_id, serial.c_str(), sizeof(payload.device_id));
                payload.duration = haptic.fDurationSeconds;
                payload.frequency = haptic.fFrequency;
                payload.amplitude = haptic.fAmplitude;

                IPCMessage msg;
                msg.type = IPCMessageType::HAPTIC_EVENT;
                msg.data.assign((uint8_t*)&payload, (uint8_t*)&payload + sizeof(payload));
                ipc_client->Send(msg); // W drajwerze Send wysyła do serwera (Runtime)
                break;
            }
        }
    }

    void IPCThreadLoop() {
        while (is_running) {
            IPCMessage msg;
            if (ipc_client->Receive(msg, 100)) {
                HandleMessage(msg);
            }
        }
    }

    void HandleMessage(const IPCMessage& msg) {
        if (msg.type == IPCMessageType::DEVICE_ADDED) {
            // Zakładamy payload jako JSON z pełną konfiguracją
            try {
                std::string json_str(msg.data.begin(), msg.data.end());
                auto data = nlohmann::json::parse(json_str);
                std::string serial = data["serial_number"];
                
                if (devices.find(serial) == devices.end()) {
                    COpenDriverDevice* device = nullptr;
                    ETrackedDeviceClass device_class = TrackedDeviceClass_GenericTracker;

                    if (data["type"] == 0) { // HMD
                        device_class = TrackedDeviceClass_HMD;
                        auto* hmd = new COpenDriverHMD(serial);
                        
                        // Pobierz parametry wyświetlacza z JSON (jeśli są)
                        uint32_t w = 1920, h = 1080;
                        float refresh = 90.0f, fovL = 100.0f, fovR = 100.0f;
                        
                        if (data.contains("display")) {
                            auto& d = data["display"];
                            if (d.contains("width")) w = d["width"];
                            if (d.contains("height")) h = d["height"];
                            if (d.contains("refresh_rate")) refresh = d["refresh_rate"];
                            if (d.contains("fov_left")) fovL = d["fov_left"];
                            if (d.contains("fov_right")) fovR = d["fov_right"];
                        }
                        hmd->SetDisplayParams(w, h, refresh, fovL, fovR);
                        device = hmd;
                    } else {
                        if (data["type"] == 4) device_class = TrackedDeviceClass_Controller;
                        device = new COpenDriverDevice(serial);
                    }

                    devices[serial] = device;
                    VRServerDriverHost()->TrackedDeviceAdded(serial.c_str(), device_class, device);
                    
                    // Rejestracja inputów
                    if (data.contains("inputs")) {
                        for (auto& input : data["inputs"]) {
                            device->RegisterInput(input["name"], (InputType)input["type"]);
                        }
                    }
                }
            } catch (...) {}


        } else if (msg.type == IPCMessageType::POSE_UPDATE) {
            if (msg.data.size() >= sizeof(IPCPoseData)) {
                IPCPoseData* pose_data = (IPCPoseData*)msg.data.data();
                auto it = devices.find(pose_data->device_id);
                if (it != devices.end()) {
                    vr::DriverPose_t vpose = Mapper::CreateDefaultPose();
                    
                    vpose.vecPosition[0] = pose_data->posX;
                    vpose.vecPosition[1] = pose_data->posY;
                    vpose.vecPosition[2] = pose_data->posZ;
                    
                    vpose.qRotation.w = pose_data->rotW;
                    vpose.qRotation.x = pose_data->rotX;
                    vpose.qRotation.y = pose_data->rotY;
                    vpose.qRotation.z = pose_data->rotZ;
                    
                    vpose.vecVelocity[0] = pose_data->velX;
                    vpose.vecVelocity[1] = pose_data->velY;
                    vpose.vecVelocity[2] = pose_data->velZ;
                    
                    vpose.vecAngularVelocity[0] = pose_data->angVelX;
                    vpose.vecAngularVelocity[1] = pose_data->angVelY;
                    vpose.vecAngularVelocity[2] = pose_data->angVelZ;
                    
                    it->second->UpdatePose(vpose);
                }
            }
        } else if (msg.type == IPCMessageType::INPUT_UPDATE) {
            if (msg.data.size() >= sizeof(IPCInputUpdate)) {
                IPCInputUpdate* update = (IPCInputUpdate*)msg.data.data();
                auto it = devices.find(update->device_id);
                if (it != devices.end()) {
                    it->second->UpdateInput(update->component_name, update->value);
                }
            }
        }
    }

    void LaunchGUI() {
        std::string exe_path = "opendriver_gui"; 
        
        Dl_info info;
        if (dladdr((void*)HmdDriverFactory, &info)) {
            fs::path so_path(info.dli_fname);
            fs::path local_gui_path = so_path.parent_path() / "opendriver_gui";
            
            if (fs::exists(local_gui_path)) {
                exe_path = fs::absolute(local_gui_path).string();
            }
        }
        
        // SteamVR uruchamia drajwery w izolowanym kontenerze 'Steam Linux Runtime (sniper)',
        // który ma własne /usr i nie widzi systemowego Qt6. Host jest jednak zamontowany pod /run/host.
        // Używamy ścieżek z /run/host + wywalamy LD_PRELOAD Steama, by uniknąć crashy nakładki.
        std::string cmd = "LD_PRELOAD=\"\" LD_LIBRARY_PATH=\"/run/host/usr/lib/x86_64-linux-gnu:/run/host/usr/lib:/usr/lib/x86_64-linux-gnu:/usr/lib\" QT_PLUGIN_PATH=\"/run/host/usr/lib/x86_64-linux-gnu/qt6/plugins:/usr/lib/x86_64-linux-gnu/qt6/plugins\" \"" + exe_path + "\" >> /tmp/opendriver_gui_launch.log 2>&1 &";
        system(cmd.c_str());
        
        if (VRDriverLog()) {
            VRDriverLog()->Log(("OpenDriver: Wykonano: " + cmd).c_str());
        }
    }

    std::unique_ptr<IIPCClient> ipc_client;
    std::thread ipc_thread;
    std::atomic<bool> is_running{false};
    std::map<std::string, COpenDriverDevice*> devices;
};

// ============================================================================
// FACTORY
// ============================================================================

COpenDriverServerDriver g_serverDriverContext;

extern "C" __attribute__((visibility("default"))) 
void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (std::string(IServerTrackedDeviceProvider_Version) == pInterfaceName) {
        return &g_serverDriverContext;
    }
    
    if (pReturnCode) *pReturnCode = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
