#include "mf_encoder.h"

#if defined(_WIN32) || defined(WIN32)

#include <initguid.h>
#include <algorithm>
#include <iostream>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <sstream>
#include <vector>
#include <thread>


#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfcore.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

namespace opendriver::driver::video {

namespace {

std::string HrToHex(HRESULT hr) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return stream.str();
}

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    std::vector<char> utf8_with_null(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8_with_null.data(), size, nullptr, nullptr);
    return std::string(utf8_with_null.data());
}

std::string GetActivateFriendlyName(IMFActivate* activate) {
    if (!activate) {
        return {};
    }

    WCHAR* wide_name = nullptr;
    UINT32 wide_len = 0;
    if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wide_name, &wide_len)) ||
        !wide_name) {
        return {};
    }

    std::string name = WideToUtf8(wide_name);
    CoTaskMemFree(wide_name);
    return name;
}

void AppendStartCode(std::vector<uint8_t>& output) {
    static constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    output.insert(output.end(), std::begin(kStartCode), std::end(kStartCode));
}

bool LooksLikeAnnexB(const uint8_t* data, size_t size) {
    if (!data || size < 3) {
        return false;
    }

    if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        return true;
    }

    return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01;
}

bool ConvertAvccConfigToAnnexB(const uint8_t* data,
                               size_t size,
                               uint32_t* length_field_bytes,
                               std::vector<uint8_t>& annex_b) {
    if (!data || size < 7 || !length_field_bytes) {
        return false;
    }

    *length_field_bytes = static_cast<uint32_t>(data[4] & 0x03) + 1;
    size_t offset = 5;
    const uint8_t sps_count = data[offset] & 0x1F;
    ++offset;

    annex_b.clear();

    for (uint8_t i = 0; i < sps_count; ++i) {
        if (offset + 2 > size) {
            return false;
        }

        const uint16_t sps_length = (static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]);
        offset += 2;
        if (offset + sps_length > size) {
            return false;
        }

        AppendStartCode(annex_b);
        annex_b.insert(annex_b.end(), data + offset, data + offset + sps_length);
        offset += sps_length;
    }

    if (offset + 1 > size) {
        return false;
    }

    const uint8_t pps_count = data[offset];
    ++offset;

    for (uint8_t i = 0; i < pps_count; ++i) {
        if (offset + 2 > size) {
            return false;
        }

        const uint16_t pps_length = (static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]);
        offset += 2;
        if (offset + pps_length > size) {
            return false;
        }

        AppendStartCode(annex_b);
        annex_b.insert(annex_b.end(), data + offset, data + offset + pps_length);
        offset += pps_length;
    }

    return !annex_b.empty();
}

bool ConvertAvccSampleToAnnexB(const uint8_t* data,
                               size_t size,
                               uint32_t length_field_bytes,
                               std::vector<uint8_t>& annex_b) {
    if (!data || size == 0 || length_field_bytes < 1 || length_field_bytes > 4) {
        return false;
    }

    if (LooksLikeAnnexB(data, size)) {
        annex_b.insert(annex_b.end(), data, data + size);
        return true;
    }

    size_t offset = 0;
    bool appended_any = false;
    while (offset + length_field_bytes <= size) {
        uint32_t nal_size = 0;
        for (uint32_t i = 0; i < length_field_bytes; ++i) {
            nal_size = (nal_size << 8) | static_cast<uint32_t>(data[offset + i]);
        }
        offset += length_field_bytes;

        if (nal_size == 0) {
            continue;
        }
        if (offset + nal_size > size) {
            return false;
        }

        AppendStartCode(annex_b);
        annex_b.insert(annex_b.end(), data + offset, data + offset + nal_size);
        offset += nal_size;
        appended_any = true;
    }

    return appended_any;
}

uint8_t ClampToByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

bool ReadRgbPixel(const uint8_t* pixel, DXGI_FORMAT format, int& r, int& g, int& b) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        b = pixel[0];
        g = pixel[1];
        r = pixel[2];
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        r = pixel[0];
        g = pixel[1];
        b = pixel[2];
        return true;
    default:
        return false;
    }
}

void ConvertPackedRgbToNV12(const uint8_t* source,
                            UINT source_stride,
                            uint32_t width,
                            uint32_t height,
                            DXGI_FORMAT format,
                            std::vector<uint8_t>& output) {
    const size_t y_plane_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t uv_plane_size = y_plane_size / 2;
    output.resize(y_plane_size + uv_plane_size);

    uint8_t* y_plane = output.data();
    uint8_t* uv_plane = output.data() + y_plane_size;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* row0 = source + (static_cast<size_t>(y) * source_stride);
        const uint8_t* row1 = source + (static_cast<size_t>(std::min(y + 1, height - 1)) * source_stride);

        for (uint32_t x = 0; x < width; x += 2) {
            int u_accumulator = 0;
            int v_accumulator = 0;

            for (uint32_t dy = 0; dy < 2; ++dy) {
                const uint8_t* row = (dy == 0) ? row0 : row1;
                const uint32_t sample_y = std::min(y + dy, height - 1);

                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint32_t sample_x = std::min(x + dx, width - 1);
                    const uint8_t* pixel = row + (static_cast<size_t>(sample_x) * 4);

                    int r = 0;
                    int g = 0;
                    int b = 0;
                    if (!ReadRgbPixel(pixel, format, r, g, b)) {
                        // If format is unsupported, fill with a visible diagnostic color 
                        // (magenta) once, then just proceed with black.
                        r = 255; g = 0; b = 255; 
                    }

                    const int y_value = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                    const int u_value = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    const int v_value = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                    y_plane[(static_cast<size_t>(sample_y) * width) + sample_x] = ClampToByte(y_value);
                    u_accumulator += u_value;
                    v_accumulator += v_value;
                }
            }

            const size_t uv_index = (static_cast<size_t>(y / 2) * width) + x;
            uv_plane[uv_index] = ClampToByte(u_accumulator / 4);
            uv_plane[uv_index + 1] = ClampToByte(v_accumulator / 4);
        }
    }
}

}

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
    m_inputStreamId = 0;
    m_outputStreamId = 0;
    m_lastError.clear();
    m_sequenceHeaderAnnexB.clear();
    m_avccLengthFieldBytes = 4;
    m_sentSequenceHeader = false;
    m_inputFramesSubmitted = 0;
    m_outputSamplesProduced = 0;
    return true;
}

void MediaFoundationEncoder::SetLogger(std::function<void(const char*)> logger) {
    m_logger = logger;
}

void MediaFoundationEncoder::Shutdown() {
    m_initialized = false;
    if (m_encoderMFT) {
        m_encoderMFT->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_encoderMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_encoderMFT.Reset();
    }
    m_inputStreamId = 0;
    m_outputStreamId = 0;
    m_dxManager.Reset();
    m_d3dContext.Reset();
    m_d3dDevice1.Reset();
    m_d3dDevice.Reset();
    m_stagingTexture.Reset();
    m_nv12StagingTexture.Reset();
    m_nv12DefaultTexture.Reset();
    m_nv12Buffer.clear();
    m_lastError.clear();
    m_sequenceHeaderAnnexB.clear();
    m_avccLengthFieldBytes = 4;
    m_sentSequenceHeader = false;
    m_isHardwareMFT = false;
    m_inputFramesSubmitted = 0;
    m_outputSamplesProduced = 0;
}

std::string MediaFoundationEncoder::GetLastError() const {
    return m_lastError;
}

void MediaFoundationEncoder::SetLastError(const std::string& error) {
    m_lastError = error;
}

    bool MediaFoundationEncoder::EncodeFrame(void* texture_handle, std::vector<uint8_t>& out_packet) {
        if (!m_initialized || !texture_handle) return false;
        m_lastError.clear();

        if (!EnsureSharedD3D11Device()) {
            return false;
        }

        const HANDLE shared_handle = reinterpret_cast<HANDLE>(texture_handle);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_texture;
        HRESULT hr = m_d3dDevice->OpenSharedResource(
            shared_handle,
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(shared_texture.GetAddressOf()));

        if ((FAILED(hr) || !shared_texture) && m_d3dDevice1) {
            hr = m_d3dDevice1->OpenSharedResource1(
                shared_handle,
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(shared_texture.ReleaseAndGetAddressOf()));
        }

        if (FAILED(hr) || !shared_texture) {
            SetLastError("OpenSharedResource failed " + HrToHex(hr));
            return false;
        }

        if (!m_dxManager || !m_encoderMFT) {
            if (!InitializeD3D11AndDXGI(m_d3dDevice.Get())) {
                return false;
            }
        }

        // 🔥 ZERO COPY SAMPLE
        Microsoft::WRL::ComPtr<IMFSample> sample;
        if (!CreateSampleFromTexture(shared_texture.Get(), sample.GetAddressOf())) {
            return false;
        }

        // Timestamp
        LONGLONG sampleTime = (m_frameIndex * 10000000LL) / (LONGLONG)m_config.refresh_rate;

        sample->SetSampleTime(sampleTime);
        sample->SetSampleDuration(10000000LL / (LONGLONG)m_config.refresh_rate);

        m_frameIndex++;

        // Feed encoder
        hr = m_encoderMFT->ProcessInput(m_inputStreamId, sample.Get(), 0);

        if (hr == MF_E_NOTACCEPTING) {
            std::vector<uint8_t> drained;
            ProcessOutput(drained);
            out_packet.insert(out_packet.end(), drained.begin(), drained.end());

            hr = m_encoderMFT->ProcessInput(m_inputStreamId, sample.Get(), 0);
        }

        if (FAILED(hr)) {
            SetLastError("ProcessInput failed " + HrToHex(hr));
            return false;
        }

        return ProcessOutput(out_packet);
    }

    bool MediaFoundationEncoder::EnsureSharedD3D11Device() {
        if (m_d3dDevice) return true;

        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) {
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            // We could check for LUID here if we had it from the compositor
            break; 
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL requested_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL created_level;
        HRESULT hr = D3D11CreateDevice(
            adapter.Get(),
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            requested_levels,
            ARRAYSIZE(requested_levels),
            D3D11_SDK_VERSION,
            &m_d3dDevice,
            &created_level,
            &m_d3dContext);

        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested_levels,
                                   ARRAYSIZE(requested_levels), D3D11_SDK_VERSION,
                                   &m_d3dDevice, &created_level, &m_d3dContext);
        }

        if (FAILED(hr)) {
            SetLastError("D3D11CreateDevice failed " + HrToHex(hr));
            return false;
        }

        m_d3dDevice.As(&m_d3dDevice1);
        
        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_EVENT;
        hr = m_d3dDevice->CreateQuery(&queryDesc, &m_query);

        return InitializeD3D11AndDXGI(m_d3dDevice.Get());
    }

    bool MediaFoundationEncoder::InitializeD3D11AndDXGI(ID3D11Device* pDevice) {
        if (m_dxManager && m_encoderMFT) return true;
        
        m_d3dDevice = pDevice;
        if (!m_d3dContext) m_d3dDevice->GetImmediateContext(&m_d3dContext);

        if (!m_dxManager) {
            HRESULT hr = MFCreateDXGIDeviceManager(&m_dxToken, &m_dxManager);
            if (FAILED(hr)) return false;
            hr = m_dxManager->ResetDevice(m_d3dDevice.Get(), m_dxToken);
            if (FAILED(hr)) return false;
        }
        return SetupEncoderMFT(m_config);
    }

bool MediaFoundationEncoder::SetupEncoderMFT(const VideoConfig& config) {
    m_encoderMFT.Reset();
    m_inputStreamId = 0;
    m_outputStreamId = 0;

    struct SearchPass {
        DWORD flags;
        const char* label;
        bool set_d3d_manager;
    };

    struct InputSubtype {
        GUID guid;
        const char* name;
    };

    const SearchPass passes[] = {
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT, "sync hardware", true},
        {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT, "async hardware", true},
        {MFT_ENUM_FLAG_SYNCMFT, "sync software", false},
    };

    const InputSubtype input_subtypes[] = {
        {MFVideoFormat_ARGB32, "ARGB32"},
        {MFVideoFormat_RGB32, "RGB32"},
        {MFVideoFormat_NV12, "NV12"}, // fallback
}   ;

    auto append_error = [](std::string& combined, const std::string& entry) {
        if (entry.empty()) {
            return;
        }
        if (!combined.empty()) {
            combined.append(" | ");
        }
        combined.append(entry);
    };

    auto release_activates = [](IMFActivate** activates, UINT32 count) {
        if (!activates) {
            return;
        }
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) {
                activates[i]->Release();
            }
        }
        CoTaskMemFree(activates);
    };

    MFT_REGISTER_TYPE_INFO output_type = {};
    output_type.guidMajorType = MFMediaType_Video;
    output_type.guidSubtype = MFVideoFormat_H264;

    std::string collected_errors;

    for (const SearchPass& pass : passes) {
        IMFActivate** activates = nullptr;
        UINT32 activate_count = 0;
        HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            pass.flags,
            nullptr,
            &output_type,
            &activates,
            &activate_count);

        if (FAILED(hr)) {
            append_error(collected_errors,
                         std::string(pass.label).append(": MFTEnumEx failed ").append(HrToHex(hr)));
            release_activates(activates, activate_count);
            continue;
        }

        if (activate_count == 0) {
            append_error(collected_errors, std::string(pass.label).append(": no encoders"));
            release_activates(activates, activate_count);
            continue;
        }

        for (UINT32 i = 0; i < activate_count; ++i) {
            IMFActivate* activate = activates[i];
            const std::string encoder_name = GetActivateFriendlyName(activate);
            const std::string encoder_label =
                encoder_name.empty()
                    ? std::string(pass.label).append(" encoder #").append(std::to_string(i))
                    : std::string(pass.label).append(" ").append(encoder_name);

            Microsoft::WRL::ComPtr<IMFTransform> candidate;
            hr = activate->ActivateObject(IID_PPV_ARGS(&candidate));
            if (FAILED(hr)) {
                append_error(collected_errors,
                             encoder_label + ": ActivateObject failed " + HrToHex(hr));
                continue;
            }

            Microsoft::WRL::ComPtr<IMFAttributes> attributes;
            if (SUCCEEDED(candidate->GetAttributes(&attributes))) {
                UINT32 is_async = FALSE;
                if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
                    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                }
                attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            }

            // Provide DXGI Device Manager to hardware-accelerated MFTs so they
            // can allocate GPU resources for encoding.  Software MFTs do not
            // support this (returning E_NOTIMPL) and work with plain CPU memory
            // buffers, so we skip this step for software-only passes.
            if (pass.set_d3d_manager && m_dxManager) {
                const HRESULT d3d_mgr_hr = candidate->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(m_dxManager.Get()));
                if (FAILED(d3d_mgr_hr)) {
                    append_error(collected_errors,
                                 encoder_label + ": MFT_MESSAGE_SET_D3D_MANAGER failed " +
                                     HrToHex(d3d_mgr_hr));
                    continue;
                }
                if (m_logger) {
                    m_logger((std::string("OpenDriver [DX11]: ") + encoder_label +
                              " — DXGI Device Manager configured").c_str());
                }
            }

            DWORD input_stream_id = 0;
            DWORD output_stream_id = 0;
            DWORD input_stream_count = 0;
            DWORD output_stream_count = 0;
            hr = candidate->GetStreamCount(&input_stream_count, &output_stream_count);
            if (SUCCEEDED(hr) && input_stream_count == 1 && output_stream_count == 1) {
                DWORD discovered_input_id = 0;
                DWORD discovered_output_id = 0;
                const HRESULT stream_id_hr = candidate->GetStreamIDs(
                    1, &discovered_input_id, 1, &discovered_output_id);
                if (SUCCEEDED(stream_id_hr)) {
                    input_stream_id = discovered_input_id;
                    output_stream_id = discovered_output_id;
                } else if (stream_id_hr != E_NOTIMPL) {
                    append_error(collected_errors,
                                 encoder_label + ": GetStreamIDs fell back to defaults after " +
                                     HrToHex(stream_id_hr));
                }
            } else if (FAILED(hr)) {
                append_error(collected_errors,
                             encoder_label + ": GetStreamCount failed " + HrToHex(hr));
                continue;
            }

            Microsoft::WRL::ComPtr<IMFMediaType> output_media_type;
            hr = MFCreateMediaType(&output_media_type);
            if (FAILED(hr)) {
                append_error(collected_errors,
                             encoder_label + ": MFCreateMediaType(output) failed " +
                                 HrToHex(hr));
                continue;
            }

            output_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            output_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            output_media_type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_mbps * 1000000);
            output_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            output_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            MFSetAttributeSize(output_media_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
            MFSetAttributeRatio(output_media_type.Get(), MF_MT_FRAME_RATE, (UINT32)config.refresh_rate, 1);
            MFSetAttributeRatio(output_media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            // Force periodic keyframes (GOP size) via ICodecAPI
            Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
            if (SUCCEEDED(candidate.As(&codec_api))) {
                VARIANT var;
                VariantInit(&var);
                var.vt = VT_UI4;
                var.ulVal = 30; // Every 30 frames (approx 0.3s)
                codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

                var.ulVal = eAVEncCommonRateControlMode_CBR;
                codec_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

                var.ulVal = 0;
                codec_api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

                /*
                var.vt = VT_UI4;
                var.ulVal = eAVEncH264VProfile_Base;
                codec_api->SetValue(&CODECAPI_AVEncH264VProfile, &var);

                var.ulVal = eAVEncH264VLevel4;
                codec_api->SetValue(&CODECAPI_AVEncH264VLevel, &var);
                */

                var.vt = VT_BOOL;
                var.boolVal = VARIANT_TRUE;
                codec_api->SetValue(&CODECAPI_AVEncCommonLowLatency, &var);
                codec_api->SetValue(&CODECAPI_AVEncCommonRealTime, &var);

                // codec_api->SetValue(&CODECAPI_AVEncCommonFrameDroppingMode, &var);

                var.vt = VT_UI4;
                var.ulVal = 0; // High speed
                codec_api->SetValue(&CODECAPI_AVEncCommonQuality, &var);

                // Resolution-based bitrate optimization
                uint32_t area = config.width * config.height;
                uint32_t target_bitrate = config.bitrate_mbps;
                if (area > 1920 * 1080) {
                    // For 4K/2K, we definitely need more than 30mbps
                    target_bitrate = std::max(target_bitrate, 50u);
                } else if (area < 1280 * 720) {
                    target_bitrate = std::min(target_bitrate, 10u);
                }

                var.ulVal = target_bitrate * 1000000;
                codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

                // Enable multi-slice for better parallelism on higher resolutions
                /*
                if (area >= 1920 * 1080) {
                    var.ulVal = 4; // 4 slices
                    codec_api->SetValue(&CODECAPI_AVEncVideoNumSlices, &var);
                }
                */
            }

            hr = candidate->SetOutputType(output_stream_id, output_media_type.Get(), 0);
            if (FAILED(hr)) {
                append_error(collected_errors,
                             encoder_label + ": SetOutputType(H264) failed " + HrToHex(hr));
                continue;
            }

            HRESULT input_type_hr = E_FAIL;
            std::string input_type_name;
            for (const InputSubtype& input_subtype : input_subtypes) {
                Microsoft::WRL::ComPtr<IMFMediaType> input_media_type;
                hr = MFCreateMediaType(&input_media_type);
                if (FAILED(hr)) {
                    input_type_hr = hr;
                    continue;
                }

                input_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                input_media_type->SetGUID(MF_MT_SUBTYPE, input_subtype.guid);
                input_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(input_media_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
                MFSetAttributeRatio(input_media_type.Get(), MF_MT_FRAME_RATE, (UINT32)config.refresh_rate, 1);
                MFSetAttributeRatio(input_media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

                input_type_hr = candidate->SetInputType(input_stream_id, input_media_type.Get(), 0);
                if (SUCCEEDED(input_type_hr)) {
                    input_type_name = input_subtype.name;
                    break;
                }
            }

            if (FAILED(input_type_hr)) {
                append_error(collected_errors,
                             encoder_label + ": SetInputType failed " + HrToHex(input_type_hr));
                continue;
            }

            hr = candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            if (FAILED(hr)) {
                append_error(collected_errors,
                             encoder_label + ": MFT_MESSAGE_NOTIFY_BEGIN_STREAMING failed " +
                                 HrToHex(hr));
                continue;
            }

            hr = candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            if (FAILED(hr)) {
                append_error(collected_errors,
                             encoder_label + ": MFT_MESSAGE_NOTIFY_START_OF_STREAM failed " +
                                 HrToHex(hr));
                continue;
            }

            m_encoderMFT = candidate;
            m_inputStreamId = input_stream_id;
            m_outputStreamId = output_stream_id;
            m_isHardwareMFT = pass.set_d3d_manager;
            m_sequenceHeaderAnnexB.clear();
            m_avccLengthFieldBytes = 4;
            m_sentSequenceHeader = false;
            m_inputFramesSubmitted = 0;
            m_outputSamplesProduced = 0;
            RefreshOutputMediaTypeState();
            SetLastError("Using encoder " + encoder_label + " with " + input_type_name);
            release_activates(activates, activate_count);
            return true;
        }

        release_activates(activates, activate_count);
    }

    SetLastError("No usable H264 encoder MFT found: " + collected_errors);
    return false;
}

bool MediaFoundationEncoder::EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc) {
    if (sourceDesc.SampleDesc.Count != 1 || sourceDesc.SampleDesc.Quality != 0) {
        SetLastError("MSAA textures are not supported by the Windows encoder path");
        return false;
    }

    if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
        (sourceDesc.Width % 2) != 0 || (sourceDesc.Height % 2) != 0) {
        SetLastError("NV12 conversion requires even texture dimensions");
        return false;
    }

    if (m_stagingTexture) {
        D3D11_TEXTURE2D_DESC stagingDesc{};
        m_stagingTexture->GetDesc(&stagingDesc);
        if (stagingDesc.Width == sourceDesc.Width &&
            stagingDesc.Height == sourceDesc.Height &&
            stagingDesc.Format == sourceDesc.Format) {
            return true;
        }
        m_stagingTexture.Reset();
    }

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
    if (FAILED(hr) || !m_stagingTexture) {
        SetLastError("CreateTexture2D(staging) failed " + HrToHex(hr));
        return false;
    }

    return true;
}

bool MediaFoundationEncoder::ConvertTextureToNV12Sample(ID3D11Texture2D* sourceTexture,
                                                        DXGI_FORMAT sourceFormat,
                                                        IMFSample** sampleOut) {
    if (!sourceTexture || !sampleOut) {
        SetLastError("ConvertTextureToNV12Sample received invalid arguments");
        return false;
    }

    if (sourceFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
        sourceFormat != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        sourceFormat != DXGI_FORMAT_B8G8R8X8_UNORM &&
        sourceFormat != DXGI_FORMAT_B8G8R8X8_UNORM_SRGB &&
        sourceFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
        sourceFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        SetLastError("Unsupported compositor texture format for NV12 conversion");
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    sourceTexture->GetDesc(&textureDesc);

    // Logging for diagnostics (every 1000 frames)
    if (m_frameIndex % 1000 == 0 && m_logger) {
        char diagBuf[256];
        snprintf(diagBuf, sizeof(diagBuf), "OpenDriver [DX11]: Texture — %ux%u, format %u, ArraySize %u, Mips %u, bind 0x%X, misc 0x%X",
                 textureDesc.Width, textureDesc.Height, (uint32_t)sourceFormat, 
                 textureDesc.ArraySize, textureDesc.MipLevels,
                 textureDesc.BindFlags, textureDesc.MiscFlags);
        m_logger(diagBuf);
    }

    if (!EnsureStagingTexture(textureDesc)) {
        return false;
    }

    // Handle Keyed Mutex if present (0x100 = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyedMutex;
    if (textureDesc.MiscFlags & 0x100) {
        if (SUCCEEDED(sourceTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)keyedMutex.GetAddressOf()))) {
            // SteamVR uses key 0 for synchronization between the compositor and driver.
            // Increased timeout to 20ms to ensure we get the frame without stuttering.
            if (FAILED(keyedMutex->AcquireSync(0, 20))) {
                return false; 
            }
        }
    }

    m_d3dContext->CopyResource(m_stagingTexture.Get(), sourceTexture);
    
    // Release Keyed Mutex as soon as CopyResource is queued (it will wait on GPU)
    if (keyedMutex) {
        keyedMutex->ReleaseSync(0);
    }
    
    // Synchronize GPU and CPU: Ensure CopyResource is finished before Map()
    if (m_query) {
        m_d3dContext->End(m_query.Get());
        m_d3dContext->Flush();
        
        BOOL queryData = FALSE;
        int attempts = 0;
        while (attempts < 1000 && 
               m_d3dContext->GetData(m_query.Get(), &queryData, sizeof(BOOL), 0) != S_OK) {
            attempts++;
            std::this_thread::yield(); // Let other threads run while waiting for GPU
        }
        
        if (attempts >= 1000) {
            SetLastError("GPU query timeout waiting for capture");
        }
    } else {
        // Fallback to simple flush if query is missing
        m_d3dContext->Flush();
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        if (hr == 0x887A0005 || hr == 0x887A0006 || hr == 0x887A0007) {
            ResetDeviceResources();
        }
        SetLastError("ID3D11DeviceContext::Map failed " + HrToHex(hr));
        return false;
    }

    // Multi-point pixel diagnostics (every 1000 frames)
    if (m_frameIndex % 1000 == 0 && m_logger) {
        const uint8_t* pData = static_cast<const uint8_t*>(mapped.pData);
        uint32_t w = textureDesc.Width;
        uint32_t h = textureDesc.Height;
        
        auto getPixelStr = [&](uint32_t x, uint32_t y) -> std::string {
            const uint8_t* p = pData + (y * mapped.RowPitch) + (x * 4);
            int r0, g0, b0;
            if (sourceFormat == DXGI_FORMAT_B8G8R8A8_UNORM || sourceFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                b0 = p[0]; g0 = p[1]; r0 = p[2];
            } else {
                r0 = p[0]; g0 = p[1]; b0 = p[2];
            }
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "[%d,%d,%d]", r0, g0, b0);
            return tmp;
        };

        char diagBuf[512];
        snprintf(diagBuf, sizeof(diagBuf), 
                 "OpenDriver [DX11]: Frame Sample — Center:%s, TL:%s, TR:%s, BL:%s, BR:%s",
                 getPixelStr(w/2, h/2).c_str(),
                 getPixelStr(10, 10).c_str(),
                 getPixelStr(w-10, 10).c_str(),
                 getPixelStr(10, h-10).c_str(),
                 getPixelStr(w-10, h-10).c_str());
        m_logger(diagBuf);
    }

    ConvertPackedRgbToNV12(static_cast<const uint8_t*>(mapped.pData),
                           mapped.RowPitch,
                           textureDesc.Width,
                           textureDesc.Height,
                           sourceFormat,
                           m_nv12Buffer);
    m_d3dContext->Unmap(m_stagingTexture.Get(), 0);

    // ── Hardware MFT path: wrap NV12 data in a D3D11 texture for GPU encoding ──
    if (m_isHardwareMFT) {
        const uint32_t w = textureDesc.Width;
        const uint32_t h = textureDesc.Height;

        // Lazily create NV12 staging texture (CPU-writable) for data upload
        if (!m_nv12StagingTexture) {
            D3D11_TEXTURE2D_DESC nd = {};
            nd.Width = w;
            nd.Height = h;
            nd.MipLevels = 1;
            nd.ArraySize = 1;
            nd.Format = DXGI_FORMAT_NV12;
            nd.SampleDesc = {1, 0};
            nd.Usage = D3D11_USAGE_STAGING;
            nd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m_d3dDevice->CreateTexture2D(&nd, nullptr, &m_nv12StagingTexture);
        }

        // Lazily create NV12 default texture (GPU-readable) that the MFT consumes
        if (!m_nv12DefaultTexture) {
            D3D11_TEXTURE2D_DESC nd = {};
            nd.Width = w;
            nd.Height = h;
            nd.MipLevels = 1;
            nd.ArraySize = 1;
            nd.Format = DXGI_FORMAT_NV12;
            nd.SampleDesc = {1, 0};
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            m_d3dDevice->CreateTexture2D(&nd, nullptr, &m_nv12DefaultTexture);
        }

        bool hwPathOk = false;
        if (m_nv12StagingTexture && m_nv12DefaultTexture) {
            D3D11_MAPPED_SUBRESOURCE nm = {};
            hr = m_d3dContext->Map(m_nv12StagingTexture.Get(), 0,
                                   D3D11_MAP_WRITE, 0, &nm);
            if (SUCCEEDED(hr)) {
                // Copy Y plane (width * height luma samples)
                for (uint32_t row = 0; row < h; ++row) {
                    std::memcpy(
                        static_cast<uint8_t*>(nm.pData) + row * nm.RowPitch,
                        m_nv12Buffer.data() + row * w, w);
                }
                // Copy interleaved UV plane (width * height/2 chroma samples)
                const uint8_t* uvSrc = m_nv12Buffer.data()
                                       + static_cast<size_t>(w) * h;
                uint8_t* uvDst = static_cast<uint8_t*>(nm.pData)
                                 + static_cast<size_t>(h) * nm.RowPitch;
                for (uint32_t row = 0; row < h / 2; ++row) {
                    std::memcpy(uvDst + row * nm.RowPitch,
                                uvSrc + row * w, w);
                }
                m_d3dContext->Unmap(m_nv12StagingTexture.Get(), 0);
                m_d3dContext->CopyResource(m_nv12DefaultTexture.Get(),
                                           m_nv12StagingTexture.Get());
                m_d3dContext->Flush(); // Force CPU<->GPU sync before MFT reads it

                Microsoft::WRL::ComPtr<IMFMediaBuffer> dxgiBuf;
                hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                               m_nv12DefaultTexture.Get(),
                                               0, FALSE, &dxgiBuf);
                if (SUCCEEDED(hr)) {
                    // Inform MF about the actual data length
                    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
                    if (SUCCEEDED(dxgiBuf.As(&buf2d))) {
                        DWORD contLen = 0;
                        buf2d->GetContiguousLength(&contLen);
                        dxgiBuf->SetCurrentLength(contLen);
                    }
                    Microsoft::WRL::ComPtr<IMFSample> hwSample;
                    hr = MFCreateSample(&hwSample);
                    if (SUCCEEDED(hr)) {
                        hwSample->AddBuffer(dxgiBuf.Get());
                        *sampleOut = hwSample.Detach();
                        hwPathOk = true;
                    }
                }
            }
        }

        if (hwPathOk) {
            return true;
        }
        // Hardware texture path failed — fall through to CPU memory buffer
        if (m_logger) {
            m_logger("OpenDriver [DX11]: Hardware NV12 texture path failed, "
                     "falling back to CPU buffer");
        }
    }

    // ── Software MFT / fallback path: plain CPU memory buffer ──
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(m_nv12Buffer.size()), &buffer);
    if (FAILED(hr)) {
        SetLastError("MFCreateMemoryBuffer(input) failed " + HrToHex(hr));
        return false;
    }

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&destination, &maxLength, &currentLength);
    if (FAILED(hr)) {
        SetLastError("IMFMediaBuffer::Lock(input) failed " + HrToHex(hr));
        return false;
    }

    std::memcpy(destination, m_nv12Buffer.data(), m_nv12Buffer.size());
    buffer->Unlock();

    hr = buffer->SetCurrentLength(static_cast<DWORD>(m_nv12Buffer.size()));
    if (FAILED(hr)) {
        SetLastError("SetCurrentLength(input) failed " + HrToHex(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        SetLastError("MFCreateSample(input) failed " + HrToHex(hr));
        return false;
    }

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        SetLastError("IMFSample::AddBuffer(input) failed " + HrToHex(hr));
        return false;
    }

    *sampleOut = sample.Detach();
    return true;
}

bool MediaFoundationEncoder::CreateSampleFromTexture(
    ID3D11Texture2D* texture,
    IMFSample** sampleOut)
{
    if (!texture || !sampleOut) return false;

    // Handle Keyed Mutex if present (Problem #2)
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyedMutex;
    if (textureDesc.MiscFlags & 0x100) {
        if (SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)keyedMutex.GetAddressOf()))) {
            if (FAILED(keyedMutex->AcquireSync(0, 0))) {
                return false; 
            }
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
    
    if (keyedMutex) keyedMutex->ReleaseSync(0);

    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    if (FAILED(sample->AddBuffer(buffer.Get()))) return false;

    // Fix Problem #3: Correct length
    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
    if (SUCCEEDED(buffer.As(&buf2d))) {
        DWORD contLen = 0;
        buf2d->GetContiguousLength(&contLen);
        buffer->SetCurrentLength(contLen);
    } else {
        buffer->SetCurrentLength(textureDesc.Width * textureDesc.Height * 4);
    }

    *sampleOut = sample.Detach();
    return true;
}

bool MediaFoundationEncoder::RefreshOutputMediaTypeState() {
    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    HRESULT hr = m_encoderMFT->GetOutputCurrentType(m_outputStreamId, &output_type);
    if (FAILED(hr) || !output_type) {
        SetLastError("GetOutputCurrentType failed " + HrToHex(hr));
        return false;
    }

    UINT32 blob_size = 0;
    hr = output_type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size);
    if (SUCCEEDED(hr) && blob_size > 0) {
        std::vector<uint8_t> blob(blob_size);
        hr = output_type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), blob_size, &blob_size);
        if (FAILED(hr)) {
            SetLastError("GetBlob(MF_MT_MPEG_SEQUENCE_HEADER) failed " + HrToHex(hr));
            return false;
        }

        std::vector<uint8_t> converted_header;
        uint32_t length_field_bytes = m_avccLengthFieldBytes;
        if (ConvertAvccConfigToAnnexB(blob.data(), blob_size, &length_field_bytes, converted_header)) {
            m_sequenceHeaderAnnexB = std::move(converted_header);
            m_avccLengthFieldBytes = length_field_bytes;
            m_sentSequenceHeader = false;
        }
    }

    return true;
}

bool MediaFoundationEncoder::AppendSampleAsAnnexB(IMFSample* sample,
                                                  std::vector<uint8_t>& out_packet) {
    if (!sample) {
        return true;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> out_buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&out_buffer);
    if (FAILED(hr) || !out_buffer) {
        SetLastError("ConvertToContiguousBuffer failed " + HrToHex(hr));
        return false;
    }

    BYTE* data = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    hr = out_buffer->Lock(&data, &max_length, &current_length);
    if (FAILED(hr)) {
        SetLastError("IMFMediaBuffer::Lock(output) failed " + HrToHex(hr));
        return false;
    }

    std::vector<uint8_t> converted;
    bool converted_ok = false;
    if (LooksLikeAnnexB(data, current_length)) {
        converted.insert(converted.end(), data, data + current_length);
        converted_ok = true;
    } else {
        converted_ok = ConvertAvccSampleToAnnexB(
            data, current_length, m_avccLengthFieldBytes, converted);
    }

    out_buffer->Unlock();

    if (!converted_ok) {
        SetLastError("Failed to convert H264 sample to Annex B");
        return false;
    }

    if (!m_sentSequenceHeader && !m_sequenceHeaderAnnexB.empty()) {
        out_packet.insert(out_packet.end(),
                          m_sequenceHeaderAnnexB.begin(),
                          m_sequenceHeaderAnnexB.end());
        m_sentSequenceHeader = true;
    }

    out_packet.insert(out_packet.end(), converted.begin(), converted.end());
    return true;
}

bool MediaFoundationEncoder::ProcessOutput(std::vector<uint8_t>& out_packet) {
    bool produced_output = false;
    bool needs_more_input = false;

    MFT_OUTPUT_STREAM_INFO stream_info = {};
    HRESULT hr = m_encoderMFT->GetOutputStreamInfo(m_outputStreamId, &stream_info);
    if (FAILED(hr)) {
        SetLastError("GetOutputStreamInfo failed " + HrToHex(hr));
        return false;
    }

    // Drain all available output packets from the encoder.
    // Some encoders produce multiple buffers for a single input frame (slices).
    for (int iteration = 0; iteration < 20; ++iteration) {
        MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
        outputDataBuffer.dwStreamID = m_outputStreamId;
        DWORD status = 0;

        Microsoft::WRL::ComPtr<IMFSample> output_sample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> output_buffer;
        if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            hr = MFCreateSample(&output_sample);
            if (FAILED(hr)) {
                SetLastError("MFCreateSample(output) failed " + HrToHex(hr));
                return false;
            }

            const DWORD output_buffer_size = stream_info.cbSize != 0 ? stream_info.cbSize : 1024 * 1024;
            hr = MFCreateMemoryBuffer(output_buffer_size, &output_buffer);
            if (FAILED(hr)) {
                SetLastError("MFCreateMemoryBuffer(output) failed " + HrToHex(hr));
                return false;
            }

            hr = output_sample->AddBuffer(output_buffer.Get());
            if (FAILED(hr)) {
                SetLastError("Output sample AddBuffer failed " + HrToHex(hr));
                return false;
            }

            outputDataBuffer.pSample = output_sample.Get();
        }

        hr = m_encoderMFT->ProcessOutput(0, 1, &outputDataBuffer, &status);

        const bool release_output_sample =
            outputDataBuffer.pSample != nullptr && outputDataBuffer.pSample != output_sample.Get();
        const bool encoder_provides_samples =
            (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            needs_more_input = true;
        } else if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!RefreshOutputMediaTypeState()) {
                if (release_output_sample) {
                    outputDataBuffer.pSample->Release();
                }
                if (outputDataBuffer.pEvents) {
                    outputDataBuffer.pEvents->Release();
                }
                return false;
            }
        } else if (FAILED(hr)) {
            const bool is_warmup_error =
                (hr == E_UNEXPECTED || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) &&
                (encoder_provides_samples || m_inputFramesSubmitted <= 4);

            if (is_warmup_error) {
                needs_more_input = true;
                if (release_output_sample) {
                    outputDataBuffer.pSample->Release();
                }
                if (outputDataBuffer.pEvents) {
                    outputDataBuffer.pEvents->Release();
                }
                break;
            }
            if (hr == 0x887A0005 || hr == 0x887A0006 || hr == 0x887A0007) {
                ResetDeviceResources();
            }
            SetLastError("ProcessOutput failed " + HrToHex(hr) +
                         " (flags=" + std::to_string(stream_info.dwFlags) +
                         ", cbSize=" + std::to_string(stream_info.cbSize) +
                         ", status=" + std::to_string(status) + ")");
            if (release_output_sample) {
                outputDataBuffer.pSample->Release();
            }
            if (outputDataBuffer.pEvents) {
                outputDataBuffer.pEvents->Release();
            }
            return false;
        } else if (outputDataBuffer.pSample) {
            if (!AppendSampleAsAnnexB(outputDataBuffer.pSample, out_packet)) {
                if (release_output_sample) {
                    outputDataBuffer.pSample->Release();
                }
                if (outputDataBuffer.pEvents) {
                    outputDataBuffer.pEvents->Release();
                }
                return false;
            }
            produced_output = true;
            ++m_outputSamplesProduced;
        }

        const bool output_incomplete =
            (outputDataBuffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) != 0;

        if (release_output_sample) {
            outputDataBuffer.pSample->Release();
        }
        if (outputDataBuffer.pEvents) {
            outputDataBuffer.pEvents->Release();
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT && !output_incomplete) {
            break;
        }
        if (!output_incomplete && produced_output) {
            break;
        }
        if (!output_incomplete && hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            break;
        }
    }

    return produced_output || needs_more_input;
}

std::unique_ptr<IVideoEncoder> CreateMediaFoundationEncoder() {
    return std::make_unique<MediaFoundationEncoder>();
}

void MediaFoundationEncoder::ResetDeviceResources() {
    if (m_logger) {
        m_logger("OpenDriver [DX11]: Device lost detected. Resetting D3D11 and Encoder resources for recovery.");
    }

    // Release all MFT and D3D11 resources
    m_encoderMFT.Reset();
    m_dxManager.Reset();
    m_d3dContext.Reset();
    m_d3dDevice1.Reset();
    m_d3dDevice.Reset();
    m_stagingTexture.Reset();
    m_nv12StagingTexture.Reset();
    m_nv12DefaultTexture.Reset();
    m_query.Reset();
    
    // Clear state
    m_sequenceHeaderAnnexB.clear();
    m_sentSequenceHeader = false;
    m_initialized = false;
    // We intentionally do not clear m_config or logger.
    // The driver will call EncodeFrame on the next frame. EnsureSharedD3D11Device
    // will see that m_initialized is false (Wait, EnsureSharedD3D11Device is only called if initialized is true).
    // Actually, EncodeFrame starts with `if (!m_initialized) return false;`. 
    // We need to keep m_initialized = true, but let EnsureSharedD3D11Device/InitializeD3D11AndDXGI rebuild.
    // EnsureSharedD3D11Device checks `if (m_d3dDevice) return true;`
    m_initialized = true; 
}

} // namespace opendriver::driver::video

#endif
