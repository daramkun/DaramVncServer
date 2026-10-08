#include "encode/H264Encoder.h"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <oleauto.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace rfbenc {

namespace {

struct ComScope {
    bool ok;
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() { if (ok) CoUninitialize(); }
};

bool startMediaFoundation() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    return ok;
}

void setU32(ICodecAPI* api, const GUID& key, ULONG value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);  // optional tuning knob: ignore "not supported"
}

void setBool(ICodecAPI* api, const GUID& key, bool value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &v);
}

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

void bgraToNv12(const FrameView& fb, const Rect& r, int cw, int ch, uint8_t* out) {
    uint8_t* yPlane = out;
    uint8_t* uvPlane = out + static_cast<size_t>(cw) * ch;
    auto px = [&](int x, int y) { return fb.at(r.x + std::min(x, r.w - 1), r.y + std::min(y, r.h - 1)); };
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const uint8_t* p = px(x, y);  // BGRA
            yPlane[static_cast<size_t>(y) * cw + x] = clamp8(((47 * p[2] + 157 * p[1] + 16 * p[0]) >> 8) + 16);
        }
    }
    for (int y = 0; y < ch; y += 2) {
        for (int x = 0; x < cw; x += 2) {
            int rs = 0, gs = 0, bs = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = px(x + dx, y + dy);
                    rs += p[2]; gs += p[1]; bs += p[0];
                }
            rs >>= 2; gs >>= 2; bs >>= 2;
            uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * cw + x;
            uv[0] = clamp8(((-26 * rs - 87 * gs + 112 * bs) >> 8) + 128);  // Cb
            uv[1] = clamp8(((112 * rs - 102 * gs - 10 * bs) >> 8) + 128);  // Cr
        }
    }
}

std::unique_ptr<H264Encoder> H264Encoder::create(int width, int height, int fps, int bitrateKbps) {
    thread_local ComScope com;
    if (width < 16 || height < 16 || !startMediaFoundation()) return nullptr;
    std::unique_ptr<H264Encoder> e(new H264Encoder());
    return e->init(width, height, fps, bitrateKbps) ? std::move(e) : nullptr;
}

bool H264Encoder::init(int width, int height, int fps, int bitrateKbps) {
    w_ = width;
    h_ = height;
    cw_ = (width + 15) & ~15;
    ch_ = (height + 15) & ~15;
    fps_ = fps;
    if (FAILED(CoCreateInstance(CLSID_MSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)))) return false;
    if (FAILED(mft_.As(&api_))) return false;

    setBool(api_.Get(), CODECAPI_AVLowLatencyMode, true);
    setU32(api_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    setU32(api_.Get(), CODECAPI_AVEncCommonMeanBitRate, static_cast<ULONG>(bitrateKbps) * 1000);
    setU32(api_.Get(), CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(fps) * 60);
    setU32(api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    setU32(api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 35);

    ComPtr<IMFMediaType> out;
    if (FAILED(MFCreateMediaType(&out))) return false;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateKbps) * 1000);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(cw_), static_cast<UINT32>(ch_));
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (FAILED(mft_->SetOutputType(0, out.Get(), 0))) return false;

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(cw_), static_cast<UINT32>(ch_));
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return false;

    MFT_OUTPUT_STREAM_INFO info{};
    if (FAILED(mft_->GetOutputStreamInfo(0, &info))) return false;
    mftAllocates_ = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    outBufSize_ = std::max<DWORD>(info.cbSize, static_cast<DWORD>(cw_ * ch_ * 3 / 2));

    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    nv12_.resize(static_cast<size_t>(cw_) * ch_ * 3 / 2);
    return true;
}

void H264Encoder::setBitrate(int kbps) {
    if (api_) setU32(api_.Get(), CODECAPI_AVEncCommonMeanBitRate, static_cast<ULONG>(std::max(kbps, 100)) * 1000);
}

bool H264Encoder::encode(const FrameView& fb, const Rect& r, bool keyframe, std::vector<uint8_t>& out) {
    if (r.w != w_ || r.h != h_) return false;
    bgraToNv12(fb, r, cw_, ch_, nv12_.data());
    if (keyframe) setU32(api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_.size()), &buf))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    std::memcpy(dst, nv12_.data(), nv12_.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(nv12_.size()));
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(sampleTime_);
    sample->SetSampleDuration(10000000LL / fps_);
    sampleTime_ += 10000000LL / fps_;
    if (FAILED(mft_->ProcessInput(0, sample.Get(), 0))) return false;

    const size_t before = out.size();
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!mftAllocates_) {
            ComPtr<IMFMediaBuffer> ob2;
            if (FAILED(MFCreateMemoryBuffer(outBufSize_, &ob2)) || FAILED(MFCreateSample(&own))) return false;
            own->AddBuffer(ob2.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, 0, &t)) || FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;
            continue;
        }
        if (FAILED(hr)) return false;
        if (ob.pSample) {
            ComPtr<IMFMediaBuffer> mb;
            if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&mb))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
                    out.insert(out.end(), data, data + len);
                    mb->Unlock();
                }
            }
            if (mftAllocates_) ob.pSample->Release();
        }
        if (ob.pEvents) ob.pEvents->Release();
    }
    return out.size() > before;
}

}  // namespace rfbenc
