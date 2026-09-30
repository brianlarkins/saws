#pragma once

#include "warp.cuh"
#include <cuda/atomic>
#include <cstdint>

namespace saws {

enum class Error : unsigned {
    none,
    invalid_options,
    unsupported_device,
    cuda_failure,
    capacity_exceeded,
    counter_overflow,
    attempt_overflow
};

inline const char* error_name(Error error) {
    switch (error) {
    case Error::none: return "success";
    case Error::invalid_options: return "invalid options";
    case Error::unsupported_device: return "cooperative launch is unavailable";
    case Error::cuda_failure: return "CUDA failure";
    case Error::capacity_exceeded: return "deque capacity exceeded";
    case Error::counter_overflow: return "task counter overflow";
    case Error::attempt_overflow: return "steal attempt counter exhausted";
    }
    return "unknown error";
}

namespace detail {

template<class T>
using Atomic = cuda::atomic_ref<T, cuda::thread_scope_device>;

enum class Step : unsigned { ready, empty, retry, stopped };

struct alignas(64) Counters {
    alignas(8) std::uint64_t spawned;
    alignas(8) std::uint64_t executed;
};

struct alignas(64) RunState {
    unsigned done;
    unsigned error;
    unsigned error_worker;
    std::uint64_t error_detail;
};

class Control {
public:
    __device__ Control(RunState* state, Counters* counters, unsigned worker,
                       unsigned workers)
        : state_(state), counters_(counters), worker_(worker), workers_(workers) {}

    __device__ unsigned worker() const { return worker_; }
    __device__ unsigned workers() const { return workers_; }

    // Leader-only. Publishing the code elects the payload writer. Device code
    // reads only the code; the host reads the payload after kernel completion.
    __device__ void fail(Error error, std::uint64_t detail = 0) {
        unsigned expected = 0;
        if (Atomic<unsigned>(state_->error).compare_exchange_strong(
                expected, unsigned(error), cuda::memory_order_acq_rel,
                cuda::memory_order_relaxed)) {
            state_->error_worker = worker_;
            state_->error_detail = detail;
        }
    }

    // Collective, including inside spawn retries. Only worker zero scans.
    __device__ bool poll() {
        unsigned running = 0;
        if (warp::lane() == 0) {
            if (!failed() && !Atomic<unsigned>(state_->done).load(cuda::memory_order_acquire)) {
                if (worker_ == 0 && (++polls_ & 255u) == 0) check_termination();
                running = !failed() &&
                    !Atomic<unsigned>(state_->done).load(cuda::memory_order_acquire);
            }
        }
        warp::join();
        return warp::broadcast(running) != 0;
    }

    __device__ bool count_spawned(unsigned count) {
        warp::join(); // Join all body writes before publishing their births.
        unsigned ok = 0;
        if (warp::lane() == 0) ok = add(counters_[worker_].spawned, count);
        warp::join();
        return warp::broadcast(ok) != 0;
    }

    __device__ bool complete_batch(unsigned count) {
        warp::join(); // Join callbacks and every child commit before completion.
        unsigned ok = 0;
        if (warp::lane() == 0) ok = add(counters_[worker_].executed, count);
        warp::join();
        return warp::broadcast(ok) != 0;
    }

private:
    __device__ bool failed() const {
        return Atomic<unsigned>(state_->error).load(cuda::memory_order_acquire) != 0;
    }

    __device__ bool add(std::uint64_t& counter, std::uint64_t count) {
        if (failed()) return false;
        Atomic<std::uint64_t> atomic(counter);
        const auto old = atomic.load(cuda::memory_order_relaxed);
        if (count > UINT64_MAX - old) {
            fail(Error::counter_overflow);
            return false;
        }
        atomic.store(old + count, cuda::memory_order_release);
        return true;
    }

    __device__ void check_termination() {
        std::uint64_t executed = 0, spawned = 0;
        // Two whole passes. Interleaving these reads per worker is incorrect.
        for (unsigned w = 0; w < workers_; ++w) {
            const auto n = Atomic<std::uint64_t>(counters_[w].executed)
                               .load(cuda::memory_order_acquire);
            if (n > UINT64_MAX - executed) {
                fail(Error::counter_overflow);
                return;
            }
            executed += n;
        }
        for (unsigned w = 0; w < workers_; ++w) {
            const auto n = Atomic<std::uint64_t>(counters_[w].spawned)
                               .load(cuda::memory_order_acquire);
            if (n > UINT64_MAX - spawned) {
                fail(Error::counter_overflow);
                return;
            }
            spawned += n;
        }
        if (!failed() && executed == spawned)
            Atomic<unsigned>(state_->done).store(1, cuda::memory_order_release);
    }

    RunState* state_;
    Counters* counters_;
    unsigned worker_;
    unsigned workers_;
    unsigned polls_ = 0; // Only the leader's value is used.
};

} // namespace detail
} // namespace saws
