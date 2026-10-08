#pragma once
#include <atomic>
#include <cstdint>
#include <unordered_map>

// Turns RFB pointer/key events into SendInput calls. Not thread-safe: use from one thread (the reader).
class InputInjector {
public:
    InputInjector(int screenW, int screenH) : w_(screenW), h_(screenH) {}
    ~InputInjector() { releaseAll(); }

    void setSize(int w, int h) { w_ = w; h_ = h; }
    void pointer(uint8_t buttonMask, int x, int y);
    void key(bool down, uint32_t keysym);
    // QEMU Extended Key Event: keysym plus the XT scancode of the physical key (0 = unknown, 0x80 flag = 0xE0 prefix).
    // Printable characters keep going through the keysym path (works with the client's IME); shortcuts and
    // non-printable keys use the physical key so they behave the same whatever the keyboard layout/IME state.
    void keyExt(bool down, uint32_t keysym, uint32_t keycode);
    // Releases every key and button still held (called when the client goes away so nothing stays stuck).
    void releaseAll();

private:
    struct Pressed {
        bool unicode = false;
        uint16_t vk = 0;       // virtual key (VK path)
        uint16_t unit = 0;     // UTF-16 code unit (Unicode path)
        bool extended = false;
        bool scan = false;     // inject by hardware scancode instead of virtual key
        uint16_t scanCode = 0;
    };
    void send(const Pressed& p, bool down);

    std::atomic<int> w_, h_;  // updated by the updater thread on resolution changes
    uint8_t lastMask_ = 0;
    int curX_ = 0, curY_ = 0;
    std::unordered_map<uint32_t, Pressed> down_;  // keysym -> how it was injected, so release matches press
    bool shiftDown_ = false, ctrlDown_ = false, altDown_ = false;
};
