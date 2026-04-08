// _GNU_SOURCE is already defined by command line or other headers
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <opendriver/core/ipc.h>
#include <opendriver/core/mapper.h>
#include <opendriver/core/platform.h>
#include <opendriver/core/process_utils.h>
#include <openvr_driver.h>
#include <string>
#include <thread>
#include <vector>

#if defined(OD_PLATFORM_LINUX)
#include <dlfcn.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(OD_PLATFORM_WINDOWS)
#include "video/video_encoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#endif

#if HAVE_X264
#include <x264.h>
extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace fs = std::filesystem;

using namespace vr;
using namespace opendriver::core;

#if defined(_WIN32)
#define HMD_DLL_EXPORT __declspec(dllexport)
#else
#define HMD_DLL_EXPORT __attribute__((visibility("default")))
#endif

extern "C" HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName,
                                                 int *pReturnCode);

// ============================================================================
// DEVICE IMPLEMENTATION
// ============================================================================

class COpenDriverDevice : public ITrackedDeviceServerDriver {
public:
  COpenDriverDevice(const std::string &serial) : serial_number(serial) {}

  EVRInitError Activate(uint32_t unObjectId) override {
    object_id = unObjectId;
    property_container =
        VRProperties()->TrackedDeviceToPropertyContainer(object_id);

    VRProperties()->SetStringProperty(property_container,
                                      Prop_ModelNumber_String,
                                      "OpenDriver Modular Device");
    VRProperties()->SetStringProperty(
        property_container, Prop_SerialNumber_String, serial_number.c_str());
    VRProperties()->SetStringProperty(
        property_container, Prop_ManufacturerName_String, "OpenDriver Project");

    // Rejestracja wibracji (domyślnie dla każdego drajwera OpenDriver)
    VRDriverInput()->CreateHapticComponent(property_container,
                                           "/outputs/haptic", &haptic_handle);

    return VRInitError_None;
  }

  void Deactivate() override { object_id = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *pchComponentNameAndVersion) override {
    return nullptr;
  }
  void DebugRequest(const char *pchRequest, char *pchResponseBuffer,
                    uint32_t unResponseBufferSize) override {}
  DriverPose_t GetPose() override { return Mapper::CreateDefaultPose(); }

  void UpdatePose(const DriverPose_t &new_pose) {
    if (object_id != k_unTrackedDeviceIndexInvalid) {
      VRServerDriverHost()->TrackedDevicePoseUpdated(object_id, new_pose,
                                                     sizeof(DriverPose_t));
    }
  }

  void RegisterInput(const std::string &name, InputType type) {
    VRInputComponentHandle_t handle;
    if (type == InputType::BOOLEAN) {
      VRDriverInput()->CreateBooleanComponent(property_container, name.c_str(),
                                              &handle);
    } else {
      VRDriverInput()->CreateScalarComponent(property_container, name.c_str(),
                                             &handle, VRScalarType_Absolute,
                                             VRScalarUnits_NormalizedTwoSided);
    }
    input_handles[name] = handle;
  }

  void UpdateInput(const std::string &name, float value) {
    auto it = input_handles.find(name);
    if (it != input_handles.end()) {
      if (value > 0.5f) {
        VRDriverInput()->UpdateBooleanComponent(it->second, true, 0.0);
      } else if (value < -0.5f && value > -1.5f) {
        VRDriverInput()->UpdateBooleanComponent(it->second, false, 0.0);
      } else {
        VRDriverInput()->UpdateScalarComponent(it->second, value, 0.0);
      }
    }
  }

  PropertyContainerHandle_t GetPropertyContainer() const {
    return property_container;
  }
  const std::string &GetSerial() const { return serial_number; }

private:
  std::string serial_number;
  uint32_t object_id = k_unTrackedDeviceIndexInvalid;
  PropertyContainerHandle_t property_container = k_ulInvalidPropertyContainer;
  VRInputComponentHandle_t haptic_handle = k_ulInvalidInputComponentHandle;
  std::map<std::string, VRInputComponentHandle_t> input_handles;
};

// ============================================================================
// HMD DEVICE IMPLEMENTATION — CPU H264 Pipeline
// ============================================================================

class COpenDriverHMD : public COpenDriverDevice,
                       public IVRDisplayComponent,
                       public IVRVirtualDisplay {
public:
  COpenDriverHMD(const std::string &serial, IIPCClient *ipc)
      : COpenDriverDevice(serial), m_ipc(ipc) {
    LoadVideoConfig();
  }

  ~COpenDriverHMD() { CleanupEncoder(); }

  // Wczytaj ustawienia video z config.json
  void LoadVideoConfig() {
    try {
      const char *home_env = std::getenv("HOME");
      if (!home_env)
        return;

      std::string config_file =
          std::string(home_env) + "/.config/opendriver/config.json";
      if (!fs::exists(config_file))
        return;

      std::ifstream f(config_file);
      nlohmann::json j = nlohmann::json::parse(f);

      if (j.contains("video_encoding")) {
        auto &ve = j["video_encoding"];
        m_bitrate_mbps = ve.value("bitrate_mbps", 30);
        m_quality_preset = ve.value("preset", "ultrafast");

        if (VRDriverLog()) {
          char buf[128];
          snprintf(
              buf, sizeof(buf),
              "OpenDriver: Loaded video config — bitrate=%d Mbps, preset=%s",
              m_bitrate_mbps, m_quality_preset.c_str());
          VRDriverLog()->Log(buf);
        }
      }
    } catch (const std::exception &e) {
      if (VRDriverLog()) {
        VRDriverLog()->Log(
            std::string("OpenDriver: Failed to load video config: ")
                .append(e.what())
                .c_str());
      }
    }
  }

  void CleanupEncoder() {
#if HAVE_X264
    if (m_encoder) {
      x264_encoder_close(m_encoder);
      m_encoder = nullptr;
    }
    if (m_pic_allocated) {
      x264_picture_clean(&m_pic_in);
      m_pic_allocated = false;
    }
    if (m_sws_ctx) {
      sws_freeContext(m_sws_ctx);
      m_sws_ctx = nullptr;
    }
#endif
    m_is_encoder_ready = false;
  }

  void InitEncoder() {
#if defined(OD_PLATFORM_WINDOWS)
    // ─── WINDOWS HARDWARE ENCODING (MEDIA FOUNDATION / D3D11 ZERO COPY) ───
    video::VideoConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.refresh_rate = refresh_rate;
    cfg.bitrate_mbps = m_bitrate_mbps;
    cfg.preset = m_quality_preset;

    m_winEncoder = video::CreateMediaFoundationEncoder();
    m_is_encoder_ready = m_winEncoder->Initialize(cfg);

    if (m_is_encoder_ready && VRDriverLog()) {
      VRDriverLog()->Log("OpenDriver: Native Windows HW Encoder initialized "
                         "via Media Foundation (DXGI)");
    }
    return;
#endif

#if !HAVE_X264
    if (VRDriverLog())
      VRDriverLog()->Log(
          "OpenDriver: H264 encoding not available (x264 not installed)");
    m_is_encoder_ready = false;
    return;
#else
    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency");

    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = (int)refresh_rate;
    param.i_fps_den = 1;
    param.i_csp = X264_CSP_I420;
    param.i_threads = 4;        // Use 4 CPU threads for encoding
    param.b_sliced_threads = 1; // Sliced threading for lower latency
    param.i_log_level = X264_LOG_WARNING;

    // Rate control — target reasonable bitrate for VR streaming
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = m_bitrate_mbps * 1000;         // Convert Mbps to kbps
    param.rc.i_vbv_max_bitrate = m_bitrate_mbps * 1667; // Max burst ~1.67x
    param.rc.i_vbv_buffer_size = m_bitrate_mbps * 1667;

    // Low-latency settings
    param.i_keyint_max = (int)refresh_rate * 2; // IDR every 2 seconds
    param.i_keyint_min = (int)refresh_rate;
    param.b_intra_refresh = 0;
    param.i_bframe = 0;         // No B-frames for lowest latency
    param.b_repeat_headers = 1; // SPS/PPS with each IDR

    if (x264_param_apply_profile(&param, "baseline") < 0) {
      if (VRDriverLog())
        VRDriverLog()->Log("OpenDriver: x264 profile apply failed!");
      return;
    }

    m_encoder = x264_encoder_open(&param);
    if (!m_encoder) {
      if (VRDriverLog())
        VRDriverLog()->Log("OpenDriver: x264_encoder_open failed!");
      return;
    }

    if (x264_picture_alloc(&m_pic_in, X264_CSP_I420, width, height) < 0) {
      x264_encoder_close(m_encoder);
      m_encoder = nullptr;
      if (VRDriverLog())
        VRDriverLog()->Log("OpenDriver: x264_picture_alloc failed!");
      return;
    }
    m_pic_allocated = true;

    // SwsContext: RGBA → YUV420P
    m_sws_ctx =
        sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height,
                       AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);

    m_is_encoder_ready = (m_encoder != nullptr && m_sws_ctx != nullptr);

    if (m_is_encoder_ready && VRDriverLog()) {
      char buf[256];
      snprintf(
          buf, sizeof(buf),
          "OpenDriver: H264 CPU encoder ready — %ux%u @ %dHz, ABR %d kbps, %d "
          "threads, preset=%s",
          width, height, (int)refresh_rate, m_bitrate_mbps * 1000,
          param.i_threads, m_quality_preset.c_str());
      VRDriverLog()->Log(buf);
    }
#endif
  }

  void SetDisplayParams(uint32_t w, uint32_t h, float refresh, float fovL,
                        float fovR) {
    bool changed = (width != w || height != h || refresh_rate != refresh);
    width = w;
    height = h;
    refresh_rate = refresh;
    fov_left = fovL;
    fov_right = fovR;

    if (changed || !m_encoder) {
      CleanupEncoder();
      InitEncoder();
    }
  }

  EVRInitError Activate(uint32_t unObjectId) override {
    EVRInitError err = COpenDriverDevice::Activate(unObjectId);
    if (err != VRInitError_None)
      return err;

    auto container = GetPropertyContainer();
    VRProperties()->SetStringProperty(container, Prop_ManufacturerName_String,
                                      manufacturer.c_str());
    VRProperties()->SetStringProperty(container, Prop_ModelNumber_String,
                                      model.c_str());
    VRProperties()->SetStringProperty(container, Prop_RenderModelName_String,
                                      model.c_str());

    VRProperties()->SetFloatProperty(container, Prop_DisplayFrequency_Float,
                                     refresh_rate);
    VRProperties()->SetBoolProperty(container, Prop_DisplayDebugMode_Bool,
                                    false);

    // CRITICAL for Virtual Display mode on Linux:
    // We set HasDisplayComponent to false so SteamVR doesn't look for a
    // physical monitor via DRM Lease. We keep VirtualDisplayComponent as true
    // to use the software H264 pipeline.
    VRProperties()->SetBoolProperty(container, Prop_HasDisplayComponent_Bool,
                                    false);
    VRProperties()->SetBoolProperty(
        container, Prop_HasDriverDirectModeComponent_Bool, false);
    VRProperties()->SetBoolProperty(container,
                                    Prop_HasVirtualDisplayComponent_Bool, true);
    VRProperties()->SetBoolProperty(container, Prop_IsOnDesktop_Bool, false);

    VRProperties()->SetBoolProperty(
        container, Prop_DriverDirectModeSendsVsyncEvents_Bool, false);
    VRProperties()->SetBoolProperty(container, Prop_ReportsTimeSinceVSync_Bool,
                                    true);

    // Universe ID 31 is often used by virtual drivers (like ALVR)
    VRProperties()->SetUint64Property(container, Prop_CurrentUniverseId_Uint64,
                                      31);
    VRProperties()->SetStringProperty(container, Prop_TrackingSystemName_String,
                                      "opendriver");
    VRProperties()->SetStringProperty(container, Prop_RenderModelName_String,
                                      "generic_hmd");

    // Additional flags to make SteamVR happy
    VRProperties()->SetBoolProperty(container, Prop_WillDriftInYaw_Bool, false);
    VRProperties()->SetBoolProperty(
        container, Prop_DeviceProvidesBatteryStatus_Bool, true);

    return VRInitError_None;
  }

  void *GetComponent(const char *pchComponentNameAndVersion) override {
    if (std::string(IVRDisplayComponent_Version) ==
        pchComponentNameAndVersion) {
      return static_cast<IVRDisplayComponent *>(this);
    }
    if (std::string(IVRVirtualDisplay_Version) == pchComponentNameAndVersion) {
      return static_cast<IVRVirtualDisplay *>(this);
    }
    return COpenDriverDevice::GetComponent(pchComponentNameAndVersion);
  }

  // IVRDisplayComponent
  void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth,
                       uint32_t *pnHeight) override {
    *pnX = 0;
    *pnY = 0;
    *pnWidth = width;
    *pnHeight = height;
  }

  bool IsDisplayOnDesktop() override { return false; }
  bool IsDisplayRealDisplay() override { return false; }

  void GetRecommendedRenderTargetSize(uint32_t *pnWidth,
                                      uint32_t *pnHeight) override {
    *pnWidth = width / 2;
    *pnHeight = height;
  }

  void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY,
                            uint32_t *pnWidth, uint32_t *pnHeight) override {
    *pnY = 0;
    *pnWidth = width / 2;
    *pnHeight = height;
    if (eEye == Eye_Left) {
      *pnX = 0;
    } else {
      *pnX = width / 2;
    }
  }

  void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight,
                        float *pfTop, float *pfBottom) override {
    float fov = (eEye == Eye_Left) ? fov_left : fov_right;
    float tanHalfFov = tanf(fov * 0.5f * 3.14159f / 180.0f);
    *pfLeft = -tanHalfFov;
    *pfRight = tanHalfFov;
    *pfTop = -tanHalfFov;
    *pfBottom = tanHalfFov;
  }

  DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU,
                                            float fV) override {
    DistortionCoordinates_t coordinates;
    coordinates.rfRed[0] = fU;
    coordinates.rfRed[1] = fV;
    coordinates.rfGreen[0] = fU;
    coordinates.rfGreen[1] = fV;
    coordinates.rfBlue[0] = fU;
    coordinates.rfBlue[1] = fV;
    return coordinates;
  }

  bool ComputeInverseDistortion(HmdVector2_t *pResult, EVREye eEye,
                                uint32_t unChannel, float fU,
                                float fV) override {
    pResult->v[0] = fU;
    pResult->v[1] = fV;
    return true;
  }

  // ========================================================================
  // IVRVirtualDisplay — the core of the CPU H264 pipeline
  // ========================================================================

  void Present(const PresentInfo_t *pPresentInfo,
               uint32_t unPresentInfoSize) override {
#if defined(OD_PLATFORM_WINDOWS)
    if (!m_is_encoder_ready || !m_winEncoder || !pPresentInfo)
      return;
    if (m_encoding_in_progress.exchange(true)) {
      m_frames_dropped++;
      return;
    }

    std::vector<uint8_t> out_packet;
    auto frame_start = std::chrono::steady_clock::now();

    if (m_winEncoder->EncodeFrame((void *)pPresentInfo->backbufferTextureHandle,
                                  out_packet)) {
      if (!out_packet.empty() && m_ipc) {
        IPCMessage video_msg;
        video_msg.type = IPCMessageType::VIDEO_PACKET;
        video_msg.data = std::move(out_packet);
        m_ipc->Send(video_msg);
      }
    } else {
      m_mmap_failures++;
    }

    m_frame_count++;
    m_encoding_in_progress = false;

    // Stats
    auto now = std::chrono::steady_clock::now();
    m_total_encode_ms +=
        std::chrono::duration<float, std::milli>(now - frame_start).count();

    if (m_frame_count % ((int)refresh_rate * 5) == 0 && VRDriverLog()) {
      float avg_ms = m_total_encode_ms / m_frame_count;
      char buf[256];
      snprintf(buf, sizeof(buf),
               "OpenDriver [DX11]: H264 stats — frame #%lu, avg encode %.1fms, "
               "dropped %lu, fails %lu",
               m_frame_count, avg_ms, m_frames_dropped, m_mmap_failures);
      VRDriverLog()->Log(buf);
      m_total_encode_ms = 0;
    }
    return;

#elif defined(OD_PLATFORM_LINUX)
    // ─── LINUX PIPELINE (CPU X264 + SWSCALE VIA DMABUF) ───────────────────
#if !HAVE_X264
    (void)pPresentInfo;
    (void)unPresentInfoSize;
    return;
#endif
    if (!m_is_encoder_ready || !m_encoder || !pPresentInfo)
      return;
    if (m_encoding_in_progress.exchange(true)) {
      m_frames_dropped++;
      return;
    }

    auto frame_start = std::chrono::steady_clock::now();

    uint8_t *mapped_data = nullptr;
    int mapped_stride = 0;

    // ─── LINUX: DMABUF MMAP
    // ───────────────────────────────────────────────────
    int fd = (int)pPresentInfo->backbufferTextureHandle;
    if (fd < 0) {
      m_encoding_in_progress = false;
      return;
    }

    size_t frame_size = width * height * 4; // RGBA

    struct dma_buf_sync sync_start = {0};
    sync_start.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_start) < 0) {
    }

    void *mapped = mmap(NULL, frame_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
      struct dma_buf_sync sync_end = {0};
      sync_end.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
      ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_end);

      m_mmap_failures++;
      m_encoding_in_progress = false;
      return;
    }
    mapped_data = (uint8_t *)mapped;
    mapped_stride = (int)width * 4;

#endif // OD_PLATFORM

  } // Present()

  void WaitForPresent() override {
    // Prosta symulacja VSync
    auto target = std::chrono::microseconds((int)(1000000.0f / refresh_rate));
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_last_vsync;

    if (elapsed < target) {
      std::this_thread::sleep_for(target - elapsed);
    }
    m_last_vsync = std::chrono::steady_clock::now();
  }

  bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync,
                             uint64_t *pulFrameCounter) override {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - m_last_vsync).count();
    double frame_duration = 1.0 / refresh_rate;
    *pulFrameCounter = m_frame_count;
    *pfSecondsSinceLastVsync = (float)fmod(elapsed, frame_duration);
    return true;
  }

  uint32_t width = 1920;
  uint32_t height = 1080;
  float refresh_rate = 90.0f;
  float fov_left = 100.0f;
  float fov_right = 100.0f;
  std::string manufacturer = "OpenDriver";
  std::string model = "OpenDriver HMD";

  // Video encoding settings (loaded from config)
  int m_bitrate_mbps = 30;
  std::string m_quality_preset = "ultrafast";

#if defined(OD_PLATFORM_WINDOWS)
  std::unique_ptr<video::IVideoEncoder> m_winEncoder;
#endif

private:
  IIPCClient *m_ipc = nullptr;
#if HAVE_X264
  x264_t *m_encoder = nullptr;
  x264_picture_t m_pic_in = {};
  x264_picture_t m_pic_out = {};
  struct SwsContext *m_sws_ctx = nullptr;
#else
  void *m_encoder = nullptr;
  void *m_sws_ctx = nullptr;
#endif
  bool m_is_encoder_ready = false;
  bool m_pic_allocated = false;

  // Frame pacing
  std::atomic<bool> m_encoding_in_progress{false};
  std::chrono::steady_clock::time_point m_last_vsync =
      std::chrono::steady_clock::now();

  // Stats
  uint64_t m_frame_count = 0;
  uint64_t m_frames_dropped = 0;
  uint64_t m_mmap_failures = 0;
  uint64_t m_zero_size_frames = 0;
  float m_total_encode_ms = 0.0f;
};

// ============================================================================
// SERVER DRIVER (The Provider)
// ============================================================================

class COpenDriverServerDriver : public IServerTrackedDeviceProvider {
public:
  EVRInitError Init(IVRDriverContext *pDriverContext) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

#if defined(OD_PLATFORM_LINUX)
    // CRITICAL: Disable LD_PRELOAD recursion — prevents error 303.
    // The DRM lease shim is already loaded; don't inherit it in children.
    unsetenv("LD_PRELOAD");
#elif defined(OD_PLATFORM_WINDOWS)
    // On Windows there is no LD_PRELOAD; nothing to clean up.
    // SteamVR uses DLL injection differently and we don't need a shim.
#endif

    if (VRDriverLog())
      VRDriverLog()->Log(
          "OpenDriver: Initializing server driver " OD_PLATFORM_NAME "...");

    ipc_client = CreateIPCClient();

    // Launch the runner (opendriver_runner) in the background.
    LaunchRuntime();

    // Try to connect to the runner; retry up to 5 times (5 s total).
    ConnectWithRetry(5);

    if (ipc_client->IsConnected()) {
      is_running = true;
      ipc_thread = std::thread(&COpenDriverServerDriver::IPCThreadLoop, this);
    } else {
      if (VRDriverLog())
        VRDriverLog()->Log(
            "OpenDriver: WARNING - Could not connect to runtime. "
            "Driver will run without IPC (no devices will appear).");
    }

    return VRInitError_None;
  }

  void Cleanup() override {
    is_running = false;
    if (ipc_thread.joinable())
      ipc_thread.join();
    if (ipc_client)
      ipc_client->Disconnect();
    // Free all tracked device instances
    for (auto &[serial, dev] : devices) {
      delete dev;
    }
    devices.clear();
    m_hmd = nullptr;
  }

  const char *const *GetInterfaceVersions() override {
    return k_InterfaceVersions;
  }

  void RunFrame() override {
    // Przechwytywanie zdarzeń ze SteamVR (np. wibracje)
    VREvent_t event;
    while (VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
      if (event.eventType == VREvent_Input_HapticVibration) {
        HandleHapticEvent(event.data.hapticVibration);
      }
    }
  }

  bool ShouldBlockStandbyMode() override { return true; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

private:
  void HandleHapticEvent(const VREvent_HapticVibration_t &haptic) {
    for (auto const &[serial, device] : devices) {
      if (device->GetPropertyContainer() == haptic.containerHandle) {
        IPCHapticEvent payload;
        strncpy(payload.device_id, serial.c_str(), sizeof(payload.device_id));
        payload.duration = haptic.fDurationSeconds;
        payload.frequency = haptic.fFrequency;
        payload.amplitude = haptic.fAmplitude;

        IPCMessage msg;
        msg.type = IPCMessageType::HAPTIC_EVENT;
        msg.data.assign((uint8_t *)&payload,
                        (uint8_t *)&payload + sizeof(payload));
        ipc_client->Send(msg);
        break;
      }
    }
  }

  // ── IPCThreadLoop: receive messages AND handle reconnect ──────────────────
  void IPCThreadLoop() {
    constexpr int kHeartbeatIntervalMs = 5000;
    auto last_heartbeat = std::chrono::steady_clock::now();

    while (is_running) {
      // ---- Reconnect if disconnected ----------------------------------------
      if (!ipc_client->IsConnected()) {
        if (VRDriverLog())
          VRDriverLog()->Log(
              "OpenDriver: IPC disconnected, attempting reconnect...");
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        ConnectWithRetry(3);
        if (!ipc_client->IsConnected())
          continue;
        if (VRDriverLog())
          VRDriverLog()->Log("OpenDriver: IPC reconnected!");
      }

      // ---- Receive messages -------------------------------------------------
      IPCMessage msg;
      if (ipc_client->Receive(msg, 100)) {
        HandleMessage(msg);
      }

      // ---- Periodic heartbeat -----------------------------------------------
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - last_heartbeat)
                         .count();
      if (elapsed >= kHeartbeatIntervalMs) {
        IPCMessage hb;
        hb.type = IPCMessageType::HEARTBEAT;
        ipc_client->Send(hb);
        last_heartbeat = now;
      }
    }
  }

  void HandleMessage(const IPCMessage &msg) {
    try {
      if (msg.type == IPCMessageType::DEVICE_ADDED) {
        try {
          std::string json_str(msg.data.begin(), msg.data.end());
          auto data = nlohmann::json::parse(json_str);
          std::string serial = data["serial_number"];

          if (devices.find(serial) == devices.end()) {
            COpenDriverDevice *device = nullptr;
            ETrackedDeviceClass device_class =
                TrackedDeviceClass_GenericTracker;

            if (data["type"] == 0) { // HMD
              device_class = TrackedDeviceClass_HMD;
              auto *hmd = new COpenDriverHMD(serial, ipc_client.get());
              m_hmd = hmd;

              uint32_t w = 1920, h = 1080;
              float refresh = 90.0f, fovL = 100.0f, fovR = 100.0f;

              if (data.contains("display")) {
                auto &d = data["display"];
                if (d.contains("width"))
                  w = d["width"];
                if (d.contains("height"))
                  h = d["height"];
                if (d.contains("refresh_rate"))
                  refresh = d["refresh_rate"];
                if (d.contains("fov_left"))
                  fovL = d["fov_left"];
                if (d.contains("fov_right"))
                  fovR = d["fov_right"];
              }
              hmd->SetDisplayParams(w, h, refresh, fovL, fovR);
              device = hmd;

              if (VRDriverLog()) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "OpenDriver: HMD registered — %ux%u @ %.0fHz, "
                         "preset=%s, bitrate=%d Mbps",
                         w, h, refresh, hmd->m_quality_preset.c_str(),
                         hmd->m_bitrate_mbps);
                VRDriverLog()->Log(buf);
              }
            } else {
              if (data["type"] == 4)
                device_class = TrackedDeviceClass_Controller;
              device = new COpenDriverDevice(serial);
            }

            devices[serial] = device;
            VRServerDriverHost()->TrackedDeviceAdded(serial.c_str(),
                                                     device_class, device);

            if (data.contains("inputs")) {
              for (auto &input : data["inputs"]) {
                try {
                  // Validate enum range before cast
                  int type_int = input.value("type", -1);
                  if (type_int < 0 ||
                      type_int > static_cast<int>(InputType::BOOLEAN)) {
                    if (VRDriverLog()) {
                      VRDriverLog()->Log("OpenDriver: Invalid input type in "
                                         "DEVICE_ADDED, skipping");
                    }
                    continue;
                  }
                  device->RegisterInput(input["name"],
                                        static_cast<InputType>(type_int));
                } catch (const std::exception &e) {
                  if (VRDriverLog()) {
                    VRDriverLog()->Log(
                        std::string("OpenDriver: Failed to register input: ")
                            .append(e.what())
                            .c_str());
                  }
                }
              }
            }
          }
        } catch (const nlohmann::json::exception &je) {
          if (VRDriverLog()) {
            VRDriverLog()->Log(
                std::string("OpenDriver: JSON parse error in DEVICE_ADDED: ")
                    .append(je.what())
                    .c_str());
          }
        } catch (const std::exception &e) {
          if (VRDriverLog()) {
            VRDriverLog()->Log(
                std::string("OpenDriver: Error handling DEVICE_ADDED: ")
                    .append(e.what())
                    .c_str());
          }
        }

      } else if (msg.type == IPCMessageType::POSE_UPDATE) {
        if (msg.data.size() >= sizeof(IPCPoseData)) {
          try {
            IPCPoseData *pose_data = (IPCPoseData *)msg.data.data();
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
          } catch (const std::exception &e) {
            if (VRDriverLog()) {
              VRDriverLog()->Log(
                  std::string("OpenDriver: Error handling POSE_UPDATE: ")
                      .append(e.what())
                      .c_str());
            }
          }
        }
      } else if (msg.type == IPCMessageType::INPUT_UPDATE) {
        if (msg.data.size() >= sizeof(IPCInputUpdate)) {
          try {
            IPCInputUpdate *update = (IPCInputUpdate *)msg.data.data();
            auto it = devices.find(update->device_id);
            if (it != devices.end()) {
              it->second->UpdateInput(update->component_name, update->value);
            }
          } catch (const std::exception &e) {
            if (VRDriverLog()) {
              VRDriverLog()->Log(
                  std::string("OpenDriver: Error handling INPUT_UPDATE: ")
                      .append(e.what())
                      .c_str());
            }
          }
        }
      }
    } catch (const std::exception &e) {
      if (VRDriverLog()) {
        VRDriverLog()->Log(
            std::string("OpenDriver: Critical error in HandleMessage: ")
                .append(e.what())
                .c_str());
      }
    } catch (...) {
      if (VRDriverLog()) {
        VRDriverLog()->Log("OpenDriver: Unknown error in HandleMessage");
      }
    }
  }

  // ── ConnectWithRetry ───────────────────────────────────────────────────────
  void ConnectWithRetry(int attempts) {
    for (int i = 0; i < attempts; ++i) {
      if (ipc_client->Connect(OD_IPC_ADDRESS)) {
        if (VRDriverLog())
          VRDriverLog()->Log(
              (std::string("OpenDriver: Connected to IPC: ") + OD_IPC_ADDRESS)
                  .c_str());
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }

  // ── LaunchRuntime ────────────────────────────────────────────────────────
  void LaunchRuntime() {
    std::string runner_path = "opendriver_runner";
    std::string bin_dir;

    // Locate opendriver_runner next to this shared library
#if defined(OD_PLATFORM_LINUX) || defined(OD_PLATFORM_MACOS)
    // POSIX: use dladdr() to find our own .so path
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(HmdDriverFactory), &info) &&
        info.dli_fname) {
      fs::path so_path(info.dli_fname);
      bin_dir = so_path.parent_path().string();
      fs::path local_runner = so_path.parent_path() / "opendriver_runner";
      if (fs::exists(local_runner))
        runner_path = fs::absolute(local_runner).string();
    }
#elif defined(OD_PLATFORM_WINDOWS)
    // Windows: use GetModuleHandleEx to find our own DLL path
    HMODULE hmod = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(HmdDriverFactory),
                             &hmod)) {
      char dll_path[MAX_PATH] = {};
      if (::GetModuleFileNameA(hmod, dll_path, MAX_PATH) > 0) {
        fs::path so_path(dll_path);
        bin_dir = so_path.parent_path().string();
        fs::path local_runner = so_path.parent_path() / "opendriver_runner.exe";
        if (fs::exists(local_runner))
          runner_path = fs::absolute(local_runner).string();
      }
    }
#endif

    // Guard: only spawn if not already running
    if (runner_process.valid && IsProcessRunning(runner_process)) {
      if (VRDriverLog())
        VRDriverLog()->Log(
            "OpenDriver: runner already running, skipping launch.");
      return;
    }

    std::vector<std::string> args = {runner_path};
    runner_process = SpawnProcess(runner_path, args, /*env_clear=*/true);

    if (runner_process.valid) {
      if (VRDriverLog())
        VRDriverLog()->Log(
            ("OpenDriver: Launched runner: " + runner_path).c_str());
    } else {
      if (VRDriverLog())
        VRDriverLog()->Log(
            ("OpenDriver: Failed to spawn runner at: " + runner_path).c_str());
    }
  }

  std::unique_ptr<IIPCClient> ipc_client;
  std::thread ipc_thread;
  std::atomic<bool> is_running{false};
  std::map<std::string, COpenDriverDevice *>
      devices; // raw — freed in Cleanup()
  COpenDriverHMD *m_hmd = nullptr;
  ProcessHandle runner_process; // track runner PID
};

// ============================================================================
// FACTORY
// ============================================================================

COpenDriverServerDriver g_serverDriverContext;

extern "C" HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName,
                                                 int *pReturnCode) {
  if (std::string(IServerTrackedDeviceProvider_Version) == pInterfaceName) {
    if (pReturnCode)
      *pReturnCode = VRInitError_None;
    return &g_serverDriverContext;
  }

  if (pReturnCode)
    *pReturnCode = VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
