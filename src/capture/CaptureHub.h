#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "capture/ICapturer.h"

// One capture thread shared by every client. The hub keeps the current framebuffer and, per client, the damage
// that client has not received yet, so a client can ask for any region and slow clients simply accumulate
// (bounded) damage instead of slowing anyone down.
class CaptureHub {
public:
    class Subscription {
    public:
        uint64_t id() const { return id_; }

    private:
        friend class CaptureHub;
        uint64_t id_ = 0;
        // Everything below is guarded by CaptureHub::mu_.
        std::vector<Rect> dirty;      // not yet delivered, bounded in count
        std::vector<MoveRect> moves;  // only recorded while the client was fully in sync (see distribute())
        bool resized = false;
        int resizeReason = 0;         // ExtendedDesktopSize reason: 0 server, 1 this client asked, 2 another client
        bool hasCursor = false;
        CursorShape cursor;
        std::chrono::steady_clock::time_point damageSince{};  // when `dirty` last went from empty to non-empty
    };

    struct Delta {
        std::vector<Rect> dirty;
        std::vector<MoveRect> moves;
        bool resized = false;
        int resizeReason = 0;
        bool hasCursor = false;
        CursorShape cursor;
        double damageAgeMs = 0;  // how long the oldest delivered damage had been waiting
    };

    struct Layout {
        int w = 0, h = 0;
        std::vector<ScreenInfo> screens;
    };

    // Read access to the framebuffer; encoders hold it while they read pixels (the capture thread waits).
    struct View {
        std::shared_lock<std::shared_mutex> lock;
        const uint8_t* bgra = nullptr;
        int w = 0, h = 0;
    };

    CaptureHub() = default;
    ~CaptureHub();
    CaptureHub(const CaptureHub&) = delete;
    CaptureHub& operator=(const CaptureHub&) = delete;

    // Starts capturing on the first subscriber. Returns nullptr if no capture backend works.
    std::shared_ptr<Subscription> subscribe();
    void unsubscribe(const std::shared_ptr<Subscription>& sub);

    // Waits until `sub` has something to deliver inside `region` (or a resize / cursor change). false = capture died.
    bool wait(Subscription& sub, int timeoutMs, const Rect& region);
    // Moves the deliverable part of the pending damage into `out`. true if anything was delivered.
    bool take(Subscription& sub, const Rect& region, Delta& out);
    // Marks `region` (clipped to the screen) as needing a full resend.
    void markDamage(Subscription& sub, const Rect& region);

    View view();
    Layout layout();
    bool alive() const { return !failed_; }

    // Switches the primary display to w x h if the hardware offers that exact mode. Returns an
    // ExtendedDesktopSize status code: 0 ok, 1 prohibited, 3 invalid layout.
    int requestResize(Subscription& by, int w, int h);

private:
    bool startLocked();
    void captureLoop();
    void distribute(FrameUpdate& u);
    static void addDirty(std::vector<Rect>& list, const Rect& r);

    std::mutex lifecycleMu_;  // serialises subscribe()/unsubscribe() so the capture thread starts and stops cleanly
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Subscription>> subs_;
    uint64_t nextId_ = 1;
    Layout layout_;
    uint64_t resizeInitiator_ = 0;
    unsigned long long resizeTime_ = 0;

    std::shared_mutex fbMu_;  // guards the capturer's pixel buffer (see ICapturer::setFrameLock)

    std::unique_ptr<ICapturer> capturer_;  // used by the capture thread only (after start)
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    bool originalModeSaved_ = false;
};
