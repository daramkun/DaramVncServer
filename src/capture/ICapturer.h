#pragma once
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Pixels at `dst` were copied from (srcX, srcY) within the same framebuffer.
struct MoveRect {
    Rect dst;
    int srcX = 0, srcY = 0;
};

struct CursorShape {
    int w = 0, h = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> bgra;  // w*h*4
    std::vector<uint8_t> mask;  // 1 = opaque, w*h bytes
};

struct FrameUpdate {
    std::vector<Rect> dirty;
    std::vector<MoveRect> moves;  // apply before dirty rects
    bool resized = false;         // framebuffer size changed (width()/height() are new)
    bool hasCursor = false;
    CursorShape cursor;
    void clear() { dirty.clear(); moves.clear(); resized = hasCursor = false; }
};

// One monitor inside the framebuffer (RFB "screen"); coordinates are framebuffer pixels.
struct ScreenInfo {
    uint32_t id = 1;
    int x = 0, y = 0, w = 0, h = 0;
};

enum class AcquireResult { Updated, Timeout, Failed };

// Exclusive-locks the frame lock (if any) for as long as a capturer modifies its pixel buffer.
class FrameWriteGuard {
public:
    explicit FrameWriteGuard(std::shared_mutex* m) : m_(m) { if (m_) m_->lock(); }
    ~FrameWriteGuard() { if (m_) m_->unlock(); }
    FrameWriteGuard(const FrameWriteGuard&) = delete;
    FrameWriteGuard& operator=(const FrameWriteGuard&) = delete;

private:
    std::shared_mutex* m_;
};

class ICapturer {
public:
    virtual ~ICapturer() = default;
    // Readers of pixels() hold this lock shared; the capturer holds it exclusive while it changes the pixel
    // buffer or its geometry. That lets consumers use the capturer's buffer directly instead of keeping a copy.
    void setFrameLock(std::shared_mutex* lock) { frameLock_ = lock; }
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Current framebuffer, tightly packed BGRA (stride = width*4). Valid after a successful acquire/grabFull.
    virtual const uint8_t* pixels() const = 0;
    // Monitor layout inside the framebuffer.
    virtual std::vector<ScreenInfo> screens() const { return {{1, 0, 0, width(), height()}}; }
    // Waits up to timeoutMs for changes accumulated since the previous call.
    virtual AcquireResult acquire(int timeoutMs, FrameUpdate& out) = 0;
    // Forces a complete snapshot; out.dirty holds the full-screen rect.
    virtual bool grabFull(FrameUpdate& out) = 0;

protected:
    std::shared_mutex* frameLock_ = nullptr;
};
