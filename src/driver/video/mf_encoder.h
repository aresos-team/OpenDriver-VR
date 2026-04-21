#pragma once

#include "video_encoder.h"

#if defined(_WIN32) || defined(WIN32)

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <vector>

namespace opendriver::driver::video {

class MediaFoundationEncoder : public IVideoEncoder {
public:
    MediaFoundationEncoder();
    ~MediaFoundationEncoder() override;

    bool Initialize(const VideoConfig& config) override;
    void SetLogger(std::function<void(const char*)> logger) override;
    void Shutdown() override;
    bool EncodeFrame(void* texture_handle, std::vector<uint8_t>& out_packet) override;
    std::string GetLastError() const override;

private:
    void SetLastError(const std::string& error);
    bool EnsureSharedD3D11Device();
    bool InitializeD3D11AndDXGI(ID3D11Device* pDevice);
    bool SetupEncoderMFT(const VideoConfig& config);
    bool CreateSampleFromTexture(ID3D11Texture2D* texture, IMFSample** sampleOut);
    bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc);
    bool ConvertTextureToNV12Sample(ID3D11Texture2D* sourceTexture,
                                    DXGI_FORMAT sourceFormat,
                                    IMFSample** sampleOut);
    bool ProcessOutput(std::vector<uint8_t>& out_packet);
    bool RefreshOutputMediaTypeState();
    bool AppendSampleAsAnnexB(IMFSample* sample, std::vector<uint8_t>& out_packet);
    void ResetDeviceResources();

    VideoConfig m_config;
    bool m_initialized = false;
    uint64_t m_frameIndex = 0;

    // DXGI / MFT Resources
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11Device1> m_d3dDevice1;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxManager;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    Microsoft::WRL::ComPtr<ID3D11Query> m_query;
    UINT m_dxToken = 0;

    // The H264 Encoder Transform
    Microsoft::WRL::ComPtr<IMFTransform> m_encoderMFT;
    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;
    std::string m_lastError;
    std::vector<uint8_t> m_nv12Buffer;
    std::vector<uint8_t> m_sequenceHeaderAnnexB;
    uint32_t m_avccLengthFieldBytes = 4;
    bool m_sentSequenceHeader = false;
    bool m_isHardwareMFT = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12StagingTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12DefaultTexture;
    uint64_t m_inputFramesSubmitted = 0;
    uint64_t m_outputSamplesProduced = 0;
    std::function<void(const char*)> m_logger;
};

} // namespace opendriver::driver::video

#endif
