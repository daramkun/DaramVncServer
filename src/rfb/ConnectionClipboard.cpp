// Clipboard sharing: legacy Latin-1 ClientCutText/ServerCutText and the Extended Clipboard (UTF-8 text).
#include <zlib.h>

#include <algorithm>

#include "input/Clipboard.h"
#include "rfb/Connection.h"
#include "util/Log.h"

using rfbenc::put32;

namespace {

constexpr uint32_t kFlagText = 1u << 0, kFlagCaps = 1u << 24, kFlagRequest = 1u << 25, kFlagPeek = 1u << 26,
                   kFlagNotify = 1u << 27, kFlagProvide = 1u << 28;
constexpr uint32_t kMaxClipboardBytes = 20u * 1024 * 1024;

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

std::string toCrLf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 20);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += '\r';
        out += s[i];
    }
    return out;
}

}  // namespace

void Connection::sendClipboardCaps() {
    std::vector<uint8_t> m{rfb::kMsgServerCutText, 0, 0, 0};
    put32(m, static_cast<uint32_t>(-8));  // extended format: 4 flags + 4 for one size entry
    put32(m, kFlagCaps | kFlagText | kFlagRequest | kFlagPeek | kFlagNotify | kFlagProvide);
    put32(m, 0);  // max unsolicited size for text: 0 -> always announce with "notify" first
    send(m);
}

void Connection::sendClipboardNotify() {
    std::vector<uint8_t> m{rfb::kMsgServerCutText, 0, 0, 0};
    put32(m, static_cast<uint32_t>(-4));
    put32(m, kFlagNotify | kFlagText);
    send(m);
}

void Connection::sendClipboardProvide() {
    std::wstring wide;
    if (!ClipboardMonitor::instance().getText(wide)) return;
    std::string text = toCrLf(wideToUtf8(wide));
    text.push_back('\0');  // text must be NUL-terminated
    if (text.size() > kMaxClipboardBytes) return;

    std::vector<uint8_t> plain;
    put32(plain, static_cast<uint32_t>(text.size()));
    plain.insert(plain.end(), text.begin(), text.end());
    uLongf zlen = compressBound(static_cast<uLong>(plain.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, plain.data(), static_cast<uLong>(plain.size()), 6) != Z_OK) return;
    z.resize(zlen);

    std::vector<uint8_t> m{rfb::kMsgServerCutText, 0, 0, 0};
    put32(m, static_cast<uint32_t>(-static_cast<int32_t>(4 + z.size())));
    put32(m, kFlagProvide | kFlagText);
    m.insert(m.end(), z.begin(), z.end());
    send(m);
}

bool Connection::sendLegacyCutText(const std::wstring& text) {
    std::string latin1;
    for (wchar_t c : text) latin1.push_back(c < 256 ? static_cast<char>(c) : '?');
    std::vector<uint8_t> m{rfb::kMsgServerCutText, 0, 0, 0};
    put32(m, static_cast<uint32_t>(latin1.size()));
    m.insert(m.end(), latin1.begin(), latin1.end());
    return send(m);
}

// Runs on the process-wide clipboard thread, which serves every connection one after the other. If it sent here, a
// client that stopped reading (send() blocked while holding the send lock) would hold up everybody's notifications,
// so it only raises a flag and wakes this connection's own updater thread.
void Connection::onLocalClipboardChanged() {
    if (dead_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        clipboardPending_ = true;
    }
    cv_.notify_all();
}

void Connection::flushClipboardNotify() {
    bool extended;
    {
        std::lock_guard<std::mutex> lk(clipMu_);
        extended = clipExtended_;
    }
    if (extended) {
        sendClipboardNotify();
    } else {
        std::wstring text;
        if (ClipboardMonitor::instance().getText(text) && text.size() < kMaxClipboardBytes) sendLegacyCutText(text);
    }
}

void Connection::applyRemoteText(const std::string& utf8) {
    if (cfg_.viewOnly) return;
    ClipboardMonitor::instance().setText(utf8ToWide(utf8));
}

bool Connection::handleClientCutText() {
    uint8_t h[7];
    if (!tp_->readExact(h, 7)) return false;
    const int32_t len = static_cast<int32_t>(get32(h + 3));

    if (len >= 0) {  // legacy: ISO 8859-1 text
        if (static_cast<uint32_t>(len) > kMaxClipboardBytes) return false;
        std::string latin1(static_cast<size_t>(len), '\0');
        if (len && !tp_->readExact(latin1.data(), latin1.size())) return false;
        std::wstring wide;
        for (unsigned char c : latin1) wide.push_back(c);
        if (!cfg_.viewOnly) ClipboardMonitor::instance().setText(wide);
        return true;
    }

    const uint32_t total = static_cast<uint32_t>(-static_cast<int64_t>(len));
    if (total < 4 || total > kMaxClipboardBytes) return false;
    std::vector<uint8_t> data(total);
    if (!tp_->readExact(data.data(), data.size())) return false;
    const uint32_t flags = get32(data.data());

    if (flags & kFlagCaps) return true;  // we only ever use text; nothing to record
    if (flags & kFlagPeek) {
        std::wstring t;
        if (ClipboardMonitor::instance().getText(t)) sendClipboardNotify();
    } else if (flags & kFlagNotify) {
        if (flags & kFlagText) {  // the client has new text: ask for it
            std::vector<uint8_t> m{rfb::kMsgServerCutText, 0, 0, 0};
            put32(m, static_cast<uint32_t>(-4));
            put32(m, kFlagRequest | kFlagText);
            if (!send(m)) return false;
        }
    } else if (flags & kFlagRequest) {
        if (flags & kFlagText) sendClipboardProvide();
    } else if (flags & kFlagProvide) {
        if (!(flags & kFlagText)) return true;
        // zlib stream with (size, data) pairs for each format present in `flags`; we only handle text.
        std::vector<uint8_t> out;
        z_stream z{};
        if (inflateInit(&z) != Z_OK) return true;
        z.next_in = data.data() + 4;
        z.avail_in = static_cast<uInt>(data.size() - 4);
        uint8_t chunk[16384];
        int rc;
        do {
            z.next_out = chunk;
            z.avail_out = sizeof chunk;
            rc = inflate(&z, Z_NO_FLUSH);
            out.insert(out.end(), chunk, chunk + (sizeof chunk - z.avail_out));
        } while (rc == Z_OK && out.size() <= kMaxClipboardBytes);
        inflateEnd(&z);
        if (out.size() >= 4) {
            const uint32_t size = std::min<uint32_t>(get32(out.data()), static_cast<uint32_t>(out.size() - 4));
            std::string text(reinterpret_cast<char*>(out.data() + 4), size);
            while (!text.empty() && text.back() == '\0') text.pop_back();
            applyRemoteText(text);
        }
    }
    return true;
}
