#pragma once
#include <cstdint>
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

class ICapturer {
public:
    virtual ~ICapturer() = default;
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
};
