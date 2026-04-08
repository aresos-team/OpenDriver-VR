#pragma once

#include "video_encoder.h"

#if defined(_WIN32) || defined(WIN32)

#include <d3d11.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>

namespace opendriver::driver::video {

class MediaFoundationEncoder : public IVideoEncoder {
public:
    MediaFoundationEncoder();
    ~MediaFoundationEncoder() override;

    bool Initialize(const VideoConfig& config) override;
    void Shutdown() override;
    bool EncodeFrame(void* texture_handle, std::vector<uint8_t>& out_packet) override;

private:
    bool InitializeD3D11AndDXGI(ID3D11Device* pDevice);
    bool SetupEncoderMFT(const VideoConfig& config);
    bool ProcessOutput(std::vector<uint8_t>& out_packet);

    VideoConfig m_config;
    bool m_initialized = false;
    uint64_t m_frameIndex = 0;

    // DXGI / MFT Resources
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxManager;
    UINT m_dxToken = 0;

    // The H264 Encoder Transform
    Microsoft::WRL::ComPtr<IMFTransform> m_encoderMFT;
    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;
};

} // namespace opendriver::driver::video

#endif
