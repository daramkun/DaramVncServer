// Independent H.264 decoder (Windows' own Media Foundation decoder MFT) for round-trip tests of the H.264 encoder.
#pragma once
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ref {

class H264Decoder {
public:
    // width/height: the coded (padded) size the encoder used.
    bool open(int width, int height) {
        static const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
        if (!mf) return false;
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)))) return false;
        Microsoft::WRL::ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft_.As(&api))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }
        Microsoft::WRL::ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return false;
        if (!selectNv12()) return false;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // Decodes an Annex-B access unit; on success `bgra` holds the first w x h pixels (rows of w*4 bytes).
    bool decode(const std::vector<uint8_t>& annexB, int w, int h, std::vector<uint8_t>& bgra) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(static_cast<DWORD>(annexB.size()), &buf);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        std::copy(annexB.begin(), annexB.end(), p);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(annexB.size()));
        Microsoft::WRL::ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(time_);
        sample->SetSampleDuration(333333);
        time_ += 333333;
        const HRESULT hin = mft_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hin)) { std::printf("    ProcessInput hr=0x%08lx", static_cast<unsigned long>(hin)); std::puts(""); return false; }

        return pull(w, h, bgra);  // may be empty: without reorder hints the decoder keeps one frame back
    }

    // Flushes the frame the decoder is still holding.
    bool drain(int w, int h, std::vector<uint8_t>& bgra) {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return pull(w, h, bgra);
    }

private:
    bool selectNv12() {
        for (DWORD i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) return SUCCEEDED(mft_->SetOutputType(0, t.Get(), 0));
        }
    }

    bool pull(int w, int h, std::vector<uint8_t>& bgra) {
        for (int attempts = 0; attempts < 8; ++attempts) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(0, &info);
            const bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            MFT_OUTPUT_DATA_BUFFER ob{};
            Microsoft::WRL::ComPtr<IMFSample> own;
            if (!provides) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
                MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1 << 20), &mb);
                MFCreateSample(&own);
                own->AddBuffer(mb.Get());
                ob.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!selectNv12()) return false;
                continue;
            }
            if (FAILED(hr)) { std::printf("    ProcessOutput hr=0x%08lx", static_cast<unsigned long>(hr)); std::puts(""); return false; }

            Microsoft::WRL::ComPtr<IMFMediaType> t;
            mft_->GetOutputCurrentType(0, &t);
            UINT32 cw = 0, ch = 0;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
            LONG stride = static_cast<LONG>(MFGetAttributeUINT32(t.Get(), MF_MT_DEFAULT_STRIDE, cw));
            if (stride <= 0) stride = static_cast<LONG>(cw);

            Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
            ob.pSample->ConvertToContiguousBuffer(&mb);
            BYTE* data = nullptr;
            DWORD len = 0;
            mb->Lock(&data, nullptr, &len);
            bgra.assign(static_cast<size_t>(w) * h * 4, 0);
            const BYTE* yp = data;
            const BYTE* uv = data + static_cast<size_t>(stride) * ch;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int Y = yp[static_cast<size_t>(y) * stride + x] - 16;
                    const BYTE* c = uv + static_cast<size_t>(y / 2) * stride + (x & ~1);
                    const int U = c[0] - 128, V = c[1] - 128;
                    const int r = (298 * Y + 459 * V + 128) >> 8;
                    const int g = (298 * Y - 55 * U - 136 * V + 128) >> 8;
                    const int b = (298 * Y + 541 * U + 128) >> 8;
                    uint8_t* o = &bgra[(static_cast<size_t>(y) * w + x) * 4];
                    o[0] = static_cast<uint8_t>(std::clamp(b, 0, 255));
                    o[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
                    o[2] = static_cast<uint8_t>(std::clamp(r, 0, 255));
                    o[3] = 0;
                }
            mb->Unlock();
            if (provides) ob.pSample->Release();
            if (ob.pEvents) ob.pEvents->Release();
            return true;
        }
        return false;
    }

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    long long time_ = 0;
};

}  // namespace ref
