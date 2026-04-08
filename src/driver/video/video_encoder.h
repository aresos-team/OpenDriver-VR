#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <string>

namespace opendriver::driver::video {

struct VideoConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    float refresh_rate = 90.0f;
    int bitrate_mbps = 30;
    std::string preset = "ultrafast";
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // Initialize encoder with the required config
    virtual bool Initialize(const VideoConfig& config) = 0;
    
    // Shutdown encoder and free resources
    virtual void Shutdown() = 0;

    // Push frame (accepts different handle types depending on platform).
    // - On Linux: texture_handle is DMABUF fd
    // - On Windows: texture_handle is ID3D11Texture2D*
    // Returns NAL packets ready for IPC
    virtual bool EncodeFrame(void* texture_handle, std::vector<uint8_t>& out_packet) = 0;
};

// Factory methods
#if defined(_WIN32) || defined(WIN32)
std::unique_ptr<IVideoEncoder> CreateMediaFoundationEncoder();
#endif

// Fallback/Linux CPU encoder
std::unique_ptr<IVideoEncoder> CreateX264Encoder();

} // namespace opendriver::driver::video
