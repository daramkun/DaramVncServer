#pragma once
#include <cstdint>
#include <cstring>

namespace rfbenc {

// Small open-addressing set mapping 32-bit colors to palette indices (insertion order). Gives up past `limit`.
class ColorSet {
public:
    static constexpr int kMax = 256;

    explicit ColorSet(int limit = kMax) : limit_(limit) { reset(); }
    void reset() {
        count_ = 0;
        overflow_ = false;
        lastIdx_ = -1;
        std::memset(slot_, 0xFF, sizeof(slot_));
    }
    bool overflow() const { return overflow_; }
    int size() const { return count_; }
    uint32_t color(int i) const { return colors_[i]; }

    // Returns the palette index, or -1 on overflow.
    int add(uint32_t c) {
        if (lastIdx_ >= 0 && c == last_) return lastIdx_;  // runs of the same colour skip the hash lookup
        const int idx = addSlow(c);
        if (idx >= 0) { last_ = c; lastIdx_ = idx; }
        return idx;
    }

private:
    int addSlow(uint32_t c) {
        uint32_t h = (c * 2654435761u) >> 23 & (kSlots - 1);
        for (;; h = (h + 1) & (kSlots - 1)) {
            int16_t s = slot_[h];
            if (s < 0) break;
            if (colors_[s] == c) return s;
        }
        if (count_ >= limit_) {
            overflow_ = true;
            return -1;
        }
        colors_[count_] = c;
        slot_[h] = static_cast<int16_t>(count_);
        return count_++;
    }

    static constexpr int kSlots = 512;
    uint32_t last_ = 0;
    int lastIdx_ = -1;
    int limit_;
    int count_ = 0;
    bool overflow_ = false;
    int16_t slot_[kSlots];
    uint32_t colors_[kMax];
};

}  // namespace rfbenc
