#pragma once
#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/CaptureHub.h"
#include "capture/ICapturer.h"
#include "config/Config.h"
#include "encode/H264Encoder.h"
#include "encode/QualityController.h"
#include "encode/RawEncoder.h"
#include "encode/TightEncoder.h"
#include "encode/ZrleEncoder.h"
#include "input/InputInjector.h"
#include "net/Transport.h"
#include "rfb/PixelFormat.h"

// Pseudo-encoding / message numbers (RFB protocol, rfbproto.rst).
namespace rfb {
constexpr int32_t kEncTight = 7, kEncZrle = 16, kEncCopyRect = 1;
constexpr int32_t kEncCursor = -239, kEncDesktopSize = -223, kEncExtDesktopSize = -308, kEncLastRect = -224;
constexpr int32_t kEncQemuKey = -258, kEncH264 = 50;
constexpr int32_t kEncFence = -312, kEncContinuousUpdates = -313;
constexpr int32_t kEncExtClipboard = static_cast<int32_t>(0xC0A1E5CE);
constexpr int32_t kEncQualityFirst = -32, kEncQualityLast = -23, kEncCompressFirst = -256, kEncCompressLast = -247;
constexpr uint8_t kMsgFramebufferUpdate = 0, kMsgServerCutText = 3, kMsgEndOfContinuousUpdates = 150, kMsgFence = 248;
}  // namespace rfb

// One VNC client session: a reader thread (this thread, in run()) handles client messages and input, an updater
// thread captures, encodes and sends framebuffer updates. Must be created with std::make_shared.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(SOCKET s, std::string peer, const ServerConfig& cfg, SSL_CTX* tlsCtx, std::shared_ptr<CaptureHub> hub);
    ~Connection();
    void run();  // blocks until the client disconnects
    void requestStop() { signalStop(); }  // from another thread: disconnect this client

private:
    struct Caps {
        bool tight = false, zrle = false, copyRect = false, cursor = false, desktopSize = false,
             extDesktopSize = false, fence = false, continuous = false, extClipboard = false, qemuKey = false, h264 = false;
        int quality = -1;  // 0..9 from -32..-23
    };
    struct FenceInfo {
        std::chrono::steady_clock::time_point sent;
        size_t bytes = 0;
        bool probe = false;  // sent alone into an empty pipe: measures the network's own round trip
    };
    std::chrono::steady_clock::time_point lastFenceResponse_{};

    // Connection.cpp: session setup and client->server messages
    bool negotiate();
    bool securityNone();
    bool securityVncAuth();
    bool securityVeNCrypt();
    bool verifyPlain(const std::string& user, const std::string& pass);
    bool sendSecurityFailure(const std::string& reason);
    bool serverInit();
    bool messageLoop();
    bool handleSetEncodings();
    bool handleSetDesktopSize();
    bool handleClientCutText();
    void signalStop();  // wakes and unblocks both threads, does not join
    void stop();        // signalStop + join the updater (called from the reader thread)

    // ConnectionUpdate.cpp: framebuffer updates
    enum class UpdateResult { Sent, Idle, Error };
    void updaterMain();
    UpdateResult sendUpdate(bool nonIncremental, const Caps& caps, const PixelFormat& pf, unsigned pfGen);
    bool sendRefine(const Caps& caps, const PixelFormat& pf);
    bool sendDesktopSizeMsg(const Caps& caps, const CaptureHub::Layout& layout, int reason, int errorCode);
    void appendCursor(std::vector<uint8_t>& msg, int& count, const PixelFormat& pf);
    void appendFence(std::vector<uint8_t>& msg, size_t bytes);
    bool sendColourMap();
    bool maybeSendProbe();
    // ConnectionVideo.cpp: H.264 for video-like regions
    void handleVideo(std::vector<Rect>& dirty, const rfbenc::FrameView& fb, const Caps& caps, std::vector<uint8_t>& msg, int& count);
    void maintainVideo();
    int videoBitrateKbps();
    void noteFenceResponse(uint32_t id);
    rfbenc::RectEncoder* pickEncoder(const Caps& caps, const PixelFormat& pf);
    int encodeRects(rfbenc::RectEncoder* enc, const rfbenc::FrameView& fb, const std::vector<Rect>& rects, std::vector<uint8_t>& msg);
    void adaptQuality(const Caps& caps);
    void recordStats(std::chrono::steady_clock::duration encode, std::chrono::steady_clock::duration sendTime, size_t bytes, double ageMs);
    bool send(const std::vector<uint8_t>& data);

    // ConnectionClipboard.cpp
    void sendClipboardCaps();
    void onLocalClipboardChanged();
    void sendClipboardNotify();
    void sendClipboardProvide();
    bool sendLegacyCutText(const std::wstring& text);
    void applyRemoteText(const std::string& utf8);

    std::string peer_;
    ServerConfig cfg_;
    SSL_CTX* tlsCtx_;
    std::unique_ptr<Transport> tp_;
    std::mutex sendMu_;

    std::shared_ptr<CaptureHub> hub_;
    std::shared_ptr<CaptureHub::Subscription> sub_;
    std::unique_ptr<InputInjector> input_;

    // State shared between the reader and updater threads (guarded by mu_).
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    PixelFormat pf_;
    unsigned pfGeneration_ = 0;  // bumped on every SetPixelFormat
    Caps caps_;
    bool requestPending_ = false, requestIncremental_ = false;
    Rect interest_{0, 0, 0, 0};  // region the client asked for (request or continuous-updates rectangle)
    Rect continuousRect_{0, 0, 0, 0};
    bool continuous_ = false;
    bool forceFull_ = true;
    bool cursorDirty_ = true;
    uint32_t fenceNext_ = 1;
    std::map<uint32_t, FenceInfo> fences_;
    double minRttMs_ = 1e9, bandwidth_ = 0;  // bytes/s estimate, 0 = unknown
    double baseRttMs_ = 1e9;                 // network round trip measured by probes (1e9 = not measured yet)
    double probeRtts_[8] = {};               // the most recent probe round trips
    int probeCount_ = 0;
    std::chrono::steady_clock::time_point lastProbe_{};
    double latencyExcessMs_ = -1;            // smoothed (round trip - base): queueing + transfer time of updates
    std::chrono::steady_clock::time_point latencyStamp_{};
    bool announcedFence_ = false, announcedContinuous_ = false;
    bool qemuKeyAnnounced_ = false;
    bool announceQemuKey_ = false;  // pending empty pseudo-rectangle telling the client we accept extended key events

    std::thread updater_;
    std::atomic<bool> dead_{false};

    // Updater-thread-only state.
    std::unique_ptr<rfbenc::TightEncoder> tight_;
    std::unique_ptr<rfbenc::ZrleEncoder> zrle_;
    std::unique_ptr<rfbenc::RawEncoder> raw_;
    std::vector<Rect> lossy_;
    std::vector<uint8_t> msgBuf_;  // outgoing update message, reused
    CursorShape lastCursor_;
    bool haveCursor_ = false;
    unsigned colormapGeneration_ = ~0u;
    int adaptJpeg_ = 100, adaptZlib_ = 3;
    rfbenc::QualityController quality_;
    std::chrono::steady_clock::time_point lastQualityLog_{};
    int lastLoggedQuality_ = -1;
    double lastExcessMs_ = -1;  // for the periodic stats line
    struct Video {
        bool active = false, first = true;
        Rect r;
        std::unique_ptr<rfbenc::H264Encoder> enc;
        std::chrono::steady_clock::time_point lastFrame, lastDamage;
    } video_;
    Rect videoCand_;
    int videoCandStreak_ = 0;
    std::chrono::steady_clock::time_point videoCandLast_;
    bool h264Failed_ = false;
    std::chrono::steady_clock::time_point lastChange_ = std::chrono::steady_clock::now();
    struct Stats {
        int updates = 0;
        double encodeMs = 0, sendMs = 0;
        size_t bytes = 0;
        double ageMs = 0;
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    } stats_;

    // Clipboard (guarded by clipMu_).
    std::mutex clipMu_;
    bool clipExtended_ = false;     // client negotiated Extended Clipboard
    bool clipPeerCanProvide_ = true;
    int clipListener_ = 0;
};
