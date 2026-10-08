#pragma once
#include <atomic>
#include <chrono>

namespace rfbenc {

// Cumulative time per Tight encoder phase, for the benchmark tool (cheap enough to leave on).
struct TightProfile {
    std::atomic<long long> planNs{0}, scanNs{0}, buildNs{0}, deflateNs{0}, jpegNs{0};
};

inline TightProfile& tightProfile() {
    static TightProfile p;
    return p;
}

class ScopedNs {
public:
    explicit ScopedNs(std::atomic<long long>& sink) : sink_(sink), start_(std::chrono::steady_clock::now()) {}
    ~ScopedNs() {
        sink_ += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::atomic<long long>& sink_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace rfbenc
