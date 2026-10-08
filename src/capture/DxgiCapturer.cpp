#include "capture/DxgiCapturer.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "util/Desktop.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kMaxDirtyRects = 16;  // beyond this, merge into one bounding box

Rect clampRect(RECT r, int w, int h) {
    int l = std::max<LONG>(r.left, 0), t = std::max<LONG>(r.top, 0);
    int rr = std::min<LONG>(r.right, w), b = std::min<LONG>(r.bottom, h);
    return {l, t, std::max(rr - l, 0), std::max(b - t, 0)};
}

}  // namespace

std::vector<DxgiCapturer::OutputDesc> DxgiCapturer::enumerateOutputs() {
    std::vector<OutputDesc> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a, adapter.Reset()) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i, o.Reset()) {
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            result.push_back({a, i, d.DesktopCoordinates.left, d.DesktopCoordinates.top, d.DesktopCoordinates.right,
                              d.DesktopCoordinates.bottom});
        }
    }
    return result;
}

std::unique_ptr<DxgiCapturer> DxgiCapturer::tryCreate(const OutputDesc& output) {
    auto c = std::unique_ptr<DxgiCapturer>(new DxgiCapturer());
    c->sel_ = output;
    return c->init() ? std::move(c) : nullptr;
}

bool DxgiCapturer::init() {
    shutdown();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    if (FAILED(factory->EnumAdapters1(sel_.adapter, &adapter)) || FAILED(adapter->EnumOutputs(sel_.output, &output))) return false;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                 static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev_, nullptr, &ctx_)))
        return false;

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(dev_.Get(), &dup_))) {
        shutdown();
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    dup_->GetDesc(&dd);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = dd.ModeDesc.Width;
    td.Height = dd.ModeDesc.Height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) {
        shutdown();
        return false;
    }

    w_ = static_cast<int>(td.Width);
    h_ = static_cast<int>(td.Height);
    fb_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
    needFull_ = true;
    return true;
}

void DxgiCapturer::shutdown() {
    dup_.Reset();
    staging_.Reset();
    ctx_.Reset();
    dev_.Reset();
}

// Desktop switch / mode change / UAC: rebuild duplication. The next acquire delivers a full frame.
AcquireResult DxgiCapturer::recover() {
    const int oldW = w_, oldH = h_;
    shutdown();
    // While the secure desktop is up (and we lack rights for it) DuplicateOutput keeps failing; report "no
    // change" and retry on the next call instead of dropping the client, but give up after 30 s.
    for (int i = 0; i < 3; ++i) {
        if (init()) {
            failSince_ = 0;
            resizedPending_ = resizedPending_ || (w_ != oldW || h_ != oldH);
            return AcquireResult::Timeout;
        }
        Sleep(100);
    }
    if (!failSince_) failSince_ = GetTickCount64();
    if (GetTickCount64() - failSince_ > 30000) return AcquireResult::Failed;
    w_ = oldW;
    h_ = oldH;
    return AcquireResult::Timeout;
}

void DxgiCapturer::applyMove(const MoveRect& m) {
    const Rect& d = m.dst;
    std::vector<uint8_t> tmp(static_cast<size_t>(d.w) * d.h * 4);
    for (int y = 0; y < d.h; ++y)
        std::memcpy(&tmp[static_cast<size_t>(y) * d.w * 4], &fb_[(static_cast<size_t>(m.srcY + y) * w_ + m.srcX) * 4],
                    static_cast<size_t>(d.w) * 4);
    for (int y = 0; y < d.h; ++y)
        std::memcpy(&fb_[(static_cast<size_t>(d.y + y) * w_ + d.x) * 4], &tmp[static_cast<size_t>(y) * d.w * 4],
                    static_cast<size_t>(d.w) * 4);
}

bool DxgiCapturer::readPointerShape(const DXGI_OUTDUPL_FRAME_INFO& info, CursorShape& out) {
    std::vector<uint8_t> buf(info.PointerShapeBufferSize);
    UINT need = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    if (FAILED(dup_->GetFramePointerShape(static_cast<UINT>(buf.size()), buf.data(), &need, &si))) return false;

    const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const int w = static_cast<int>(si.Width), h = mono ? static_cast<int>(si.Height) / 2 : static_cast<int>(si.Height);
    if (w <= 0 || h <= 0) return false;
    out.w = w; out.h = h; out.hotX = si.HotSpot.x; out.hotY = si.HotSpot.y;
    out.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    out.mask.assign(static_cast<size_t>(w) * h, 0);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t o = static_cast<size_t>(y) * w + x;
            uint8_t* px = &out.bgra[o * 4];
            if (mono) {
                const uint8_t* andRow = &buf[static_cast<size_t>(y) * si.Pitch];
                const uint8_t* xorRow = &buf[static_cast<size_t>(y + h) * si.Pitch];
                bool a = (andRow[x / 8] >> (7 - x % 8)) & 1, xr = (xorRow[x / 8] >> (7 - x % 8)) & 1;
                px[0] = px[1] = px[2] = (xr && !a) ? 255 : 0;
                out.mask[o] = !(a && !xr);
            } else {
                const uint8_t* src = &buf[static_cast<size_t>(y) * si.Pitch + x * 4];
                px[0] = src[0]; px[1] = src[1]; px[2] = src[2];
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR)
                    out.mask[o] = src[3] >= 128;
                else  // MASKED_COLOR: alpha 0 = opaque pixel, otherwise XOR (nonzero rgb only)
                    out.mask[o] = src[3] == 0 || (src[0] | src[1] | src[2]) != 0;
            }
        }
    }
    return true;
}

AcquireResult DxgiCapturer::acquire(int timeoutMs, FrameUpdate& out) {
    out.clear();
    // Follow the input desktop (Winlogon/UAC) when we are allowed to; duplication must be recreated after a switch.
    if (desktop::attachToInputDesktop() && dup_) shutdown();
    if (!dup_) {
        AcquireResult r = recover();
        if (r != AcquireResult::Timeout || !dup_) return r;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return AcquireResult::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST) return recover();
    if (FAILED(hr)) return AcquireResult::Failed;

    AcquireResult result = AcquireResult::Timeout;
    ComPtr<ID3D11Texture2D> tex;
    const bool haveImage = info.LastPresentTime.QuadPart != 0 && SUCCEEDED(res.As(&tex));

    if (info.PointerShapeBufferSize > 0 && readPointerShape(info, out.cursor)) {
        out.hasCursor = true;
        result = AcquireResult::Updated;
    }

    if (haveImage) {
        if (needFull_) {
            ctx_->CopyResource(staging_.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                for (int y = 0; y < h_; ++y)
                    std::memcpy(&fb_[static_cast<size_t>(y) * w_ * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch,
                                static_cast<size_t>(w_) * 4);
                ctx_->Unmap(staging_.Get(), 0);
                out.dirty.push_back({0, 0, w_, h_});
                out.resized = resizedPending_;
                resizedPending_ = needFull_ = false;
                result = AcquireResult::Updated;
            }
        } else if (info.TotalMetadataBufferSize > 0) {
            meta_.resize(info.TotalMetadataBufferSize);
            UINT moveBytes = 0, dirtyBytes = 0;
            auto* moves = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
            if (SUCCEEDED(dup_->GetFrameMoveRects(static_cast<UINT>(meta_.size()), moves, &moveBytes))) {
                auto* dirty = reinterpret_cast<RECT*>(meta_.data() + moveBytes);
                if (SUCCEEDED(dup_->GetFrameDirtyRects(static_cast<UINT>(meta_.size()) - moveBytes, dirty, &dirtyBytes))) {
                    for (UINT i = 0; i < moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
                        MoveRect mr;
                        mr.dst = clampRect(moves[i].DestinationRect, w_, h_);
                        mr.srcX = moves[i].SourcePoint.x;
                        mr.srcY = moves[i].SourcePoint.y;
                        if (mr.dst.w <= 0 || mr.dst.h <= 0 || mr.srcX < 0 || mr.srcY < 0 ||
                            mr.srcX + mr.dst.w > w_ || mr.srcY + mr.dst.h > h_)
                            continue;
                        applyMove(mr);
                        out.moves.push_back(mr);
                    }
                    const UINT nDirty = dirtyBytes / sizeof(RECT);
                    for (UINT i = 0; i < nDirty; ++i) {
                        Rect r = clampRect(dirty[i], w_, h_);
                        if (r.w <= 0 || r.h <= 0) continue;
                        D3D11_BOX box{static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0,
                                      static_cast<UINT>(r.x + r.w), static_cast<UINT>(r.y + r.h), 1};
                        ctx_->CopySubresourceRegion(staging_.Get(), 0, r.x, r.y, 0, tex.Get(), 0, &box);
                        out.dirty.push_back(r);
                    }
                    if (!out.dirty.empty()) {
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                            for (const Rect& r : out.dirty)
                                for (int y = r.y; y < r.y + r.h; ++y)
                                    std::memcpy(&fb_[(static_cast<size_t>(y) * w_ + r.x) * 4],
                                                static_cast<uint8_t*>(m.pData) + y * m.RowPitch + r.x * 4,
                                                static_cast<size_t>(r.w) * 4);
                            ctx_->Unmap(staging_.Get(), 0);
                        } else {
                            out.dirty.clear();
                        }
                    }
                    if (out.dirty.size() > kMaxDirtyRects) {
                        Rect bb = out.dirty[0];
                        int r = bb.x + bb.w, b = bb.y + bb.h;
                        for (const Rect& d : out.dirty) {
                            bb.x = std::min(bb.x, d.x); bb.y = std::min(bb.y, d.y);
                            r = std::max(r, d.x + d.w); b = std::max(b, d.y + d.h);
                        }
                        bb.w = r - bb.x; bb.h = b - bb.y;
                        out.dirty.assign(1, bb);
                    }
                    if (!out.dirty.empty() || !out.moves.empty()) result = AcquireResult::Updated;
                }
            }
        }
    }

    dup_->ReleaseFrame();
    return result;
}

bool DxgiCapturer::grabFull(FrameUpdate& out) {
    needFull_ = true;
    for (int i = 0; i < 30; ++i) {
        AcquireResult r = acquire(100, out);
        if (r == AcquireResult::Failed) return false;
        if (r == AcquireResult::Updated && !out.dirty.empty()) return true;
    }
    return false;
}
