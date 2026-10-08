#pragma once

// Per-thread CPU accounting for the server's own threads (capture, updater, reader, JPEG workers, ...), so that
// "where does the CPU go" can be answered from a log instead of guessed.
namespace threadcpu {

// Registers the calling thread under `name` for the lifetime of the returned scope.
class Scope {
public:
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// Logs CPU use per thread name since the previous call (user+kernel, percent of one core) and the share of
// the process total that belongs to threads we did not register.
void logReport();

}  // namespace threadcpu
