#pragma once
#include <algorithm>

namespace rfbenc {

// Picks the JPEG quality from the delivery delay the client reports (Fence round trips beyond the base latency).
//   * delay above kHighMs      -> quality drops quickly (every kStepMs, by kDownStep)
//   * delay between the limits -> hold
//   * delay below kLowMs for at least kRiseAfterMs -> quality rises slowly (every kStepMs, by kUpStep)
// Fast down / slow up keeps it from oscillating around the threshold. The result always stays inside [lo, hi].
class QualityController {
public:
    static constexpr double kHighMs = 70.0;
    static constexpr double kLowMs = 30.0;
    static constexpr double kStepMs = 200.0;
    static constexpr double kRiseAfterMs = 600.0;
    static constexpr int kDownStep = 10;
    static constexpr int kUpStep = 3;

    // `nowMs` is any monotonic clock in milliseconds; `excessMs` < 0 means "no measurement available".
    int update(double nowMs, double excessMs, int lo, int hi) {
        if (!started_) {  // begin at the best quality allowed and let the measurements pull it down
            quality_ = hi;
            started_ = true;
            lastStepMs_ = nowMs;
        }
        quality_ = std::clamp(quality_, lo, hi);  // limits (or the client's request) may have changed
        if (excessMs < 0) return quality_;

        if (excessMs >= kLowMs) lowSinceMs_ = -1;
        else if (lowSinceMs_ < 0) lowSinceMs_ = nowMs;

        if (nowMs - lastStepMs_ >= kStepMs) {
            if (excessMs > kHighMs && quality_ > lo) {
                quality_ = std::max(lo, quality_ - kDownStep);
                lastStepMs_ = nowMs;
            } else if (lowSinceMs_ >= 0 && nowMs - lowSinceMs_ >= kRiseAfterMs && quality_ < hi) {
                quality_ = std::min(hi, quality_ + kUpStep);
                lastStepMs_ = nowMs;
            }
        }
        return quality_;
    }

    int current() const { return quality_; }

private:
    bool started_ = false;
    int quality_ = 85;
    double lastStepMs_ = 0, lowSinceMs_ = -1;
};

}  // namespace rfbenc
