#include "mf_encoder.h"

#if defined(_WIN32) || defined(WIN32)

#include <iostream>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfcore.lib")

namespace opendriver::driver::video {

MediaFoundationEncoder::MediaFoundationEncoder() {
    MFStartup(MF_VERSION);
}

MediaFoundationEncoder::~MediaFoundationEncoder() {
    Shutdown();
    MFShutdown();
}

bool MediaFoundationEncoder::Initialize(const VideoConfig& config) {
    m_config = config;
    m_initialized = true;
    m_frameIndex = 0;
    return true;
}

void MediaFoundationEncoder::Shutdown() {
    m_initialized = false;
    if (m_encoderMFT) {
        m_encoderMFT->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_encoderMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_encoderMFT.Reset();
    }
    m_dxManager.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
}

bool MediaFoundationEncoder::InitializeD3D11AndDXGI(ID3D11Device* pDevice) {
    if (m_dxManager) return true;

    m_d3dDevice = pDevice;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);

    // Setup DXGI Media Foundation Device Manager for zero-copy encode
    HRESULT hr = MFCreateDXGIDeviceManager(&m_dxToken, &m_dxManager);
    if (FAILED(hr)) return false;

    hr = m_dxManager->ResetDevice(m_d3dDevice.Get(), m_dxToken);
    if (FAILED(hr)) return false;

    return SetupEncoderMFT(m_config);
}

bool MediaFoundationEncoder::SetupEncoderMFT(const VideoConfig& config) {
    // 1. Create the H264 Encoder MFT
    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m_encoderMFT));
    if (FAILED(hr)) return false;

    // 2. Enable Direct3D 11 hardware acceleration
    Microsoft::WRL::ComPtr<IMFAttributes> pAttributes;
    hr = m_encoderMFT->GetAttributes(&pAttributes);
    if (SUCCEEDED(hr)) {
        pAttributes->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
    }
    m_encoderMFT->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxManager.Get()));

    // 3. Set output type (H.264)
    Microsoft::WRL::ComPtr<IMFMediaType> pOutType;
    MFCreateMediaType(&pOutType);
    pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOutType->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_mbps * 1000000);
    MFSetAttributeSize(pOutType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(pOutType.Get(), MF_MT_FRAME_RATE, (UINT32)config.refresh_rate, 1);
    MFSetAttributeRatio(pOutType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_encoderMFT->SetOutputType(0, pOutType.Get(), 0);
    if (FAILED(hr)) return false;

    // 4. Set input type (Usually NV12 or ARGB32 based on the input texture)
    Microsoft::WRL::ComPtr<IMFMediaType> pInType;
    MFCreateMediaType(&pInType);
    pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32); // Assuming SteamVR texture is RGBA
    MFSetAttributeSize(pInType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(pInType.Get(), MF_MT_FRAME_RATE, (UINT32)config.refresh_rate, 1);
    MFSetAttributeRatio(pInType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_encoderMFT->SetInputType(0, pInType.Get(), 0);
    if (FAILED(hr)) return false;

    m_encoderMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_encoderMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}

bool MediaFoundationEncoder::EncodeFrame(void* texture_handle, std::vector<uint8_t>& out_packet) {
    if (!m_initialized || !texture_handle) return false;

    ID3D11Texture2D* pTex = static_cast<ID3D11Texture2D*>(texture_handle);
    
    if (!m_dxManager) {
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        pTex->GetDevice(&dev);
        if (!InitializeD3D11AndDXGI(dev.Get())) {
            return false;
        }
    }

    // Wrap the D3D11 texture directly inside an IMFSample
    Microsoft::WRL::ComPtr<IMFMediaBuffer> pBuffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pTex, 0, FALSE, &pBuffer);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IMFSample> pSample;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return false;

    pSample->AddBuffer(pBuffer.Get());

    // Timestamp calculation
    LONGLONG sampleTime = (m_frameIndex * 10000000LL) / (LONGLONG)m_config.refresh_rate;
    pSample->SetSampleTime(sampleTime);
    pSample->SetSampleDuration(10000000LL / (LONGLONG)m_config.refresh_rate);
    m_frameIndex++;

    // Feed to Hardware Encoder
    hr = m_encoderMFT->ProcessInput(0, pSample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Encoder requires us to pull output before taking more input
    } else if (FAILED(hr)) {
        return false;
    }

    // Read encoded NAL units to out_packet
    return ProcessOutput(out_packet);
}

bool MediaFoundationEncoder::ProcessOutput(std::vector<uint8_t>& out_packet) {
    MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
    outputDataBuffer.dwStreamID = 0;
    DWORD status = 0;

    HRESULT hr = m_encoderMFT->ProcessOutput(0, 1, &outputDataBuffer, &status);
    
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { // Needs more frames to produce output
        return true; 
    }

    if (SUCCEEDED(hr) && outputDataBuffer.pSample) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
        outputDataBuffer.pSample->ConvertToContiguousBuffer(&outBuffer);
        
        BYTE* pData = nullptr;
        DWORD cbData = 0;
        if (SUCCEEDED(outBuffer->Lock(&pData, nullptr, &cbData))) {
            out_packet.insert(out_packet.end(), pData, pData + cbData);
            outBuffer->Unlock();
        }
        outputDataBuffer.pSample->Release();
        if (outputDataBuffer.pEvents) {
            outputDataBuffer.pEvents->Release();
        }
    }

    return SUCCEEDED(hr) || hr == MF_E_TRANSFORM_NEED_MORE_INPUT;
}

std::unique_ptr<IVideoEncoder> CreateMediaFoundationEncoder() {
    return std::make_unique<MediaFoundationEncoder>();
}

} // namespace opendriver::driver::video

#endif
