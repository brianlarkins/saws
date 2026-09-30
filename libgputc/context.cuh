#pragma once

#include "detail/deque.cuh"
#include <type_traits>

namespace saws {

// Every lane calls collective methods in the same sequence. A false spawn
// result is warp-uniform and means the run is stopping, not temporary fullness.
template<class Task>
class Context {
public:
    __device__ Context(detail::DequeOwner<Task>& deque, detail::Control& control)
        : deque_(deque), control_(control) {}

    __device__ unsigned lane() const { return detail::warp::lane(); }
    __device__ unsigned warp() const { return control_.worker(); }

    template<class T>
    __device__ T warp_max(T value) const { return detail::warp::maximum(value); }

    __device__ bool spawn(const Task& body, bool predicate = true) {
        if (!control_.poll()) return false;
        const unsigned ballot = __ballot_sync(detail::warp::mask, predicate);
        const unsigned count = __popc(ballot);
        if (count == 0) return true;
        const auto reservation = reserve(count, count);
        if (reservation.status == detail::Step::stopped) return false;
        const unsigned preceding = (1u << lane()) - 1;
        if (predicate) deque_.write(reservation, __popc(ballot & preceding), body);
        return commit(reservation);
    }

    // Concatenate each lane's children in lane order, then distribute their
    // construction across the warp. Parent carries lane-specific generator
    // state; Make must have uniform state and must not use warp collectives.
    template<class Parent, class Make>
    __device__ bool spawn_n(unsigned count, const Parent& parent, Make make) {
        static_assert(std::is_trivially_copyable_v<Parent>);
        if (!control_.poll()) return false;
        const std::uint64_t end = detail::warp::inclusive_sum(count);
        const std::uint64_t total = detail::warp::broadcast(end, 31);
        std::uint64_t committed = 0;
        while (committed < total) {
            const auto left = total - committed;
            const unsigned want = left < deque_.capacity() ? unsigned(left) : deque_.capacity();
            const unsigned minimum = left < 32 ? unsigned(left) : 32;
            const auto reservation = reserve(want, minimum);
            if (reservation.status == detail::Step::stopped) return false;

            for (unsigned base = 0; base < reservation.count; base += 32) {
                const unsigned offset = base + lane();
                const std::uint64_t child = committed + offset;
                unsigned source = 0, index = 0;
                std::uint64_t start = 0;
                // All lanes perform every shuffle, including lanes outside the
                // final partial segment. This simple scan avoids per-lane arrays.
                for (unsigned p = 0; p < 32; ++p) {
                    const auto limit = detail::warp::broadcast(end, p);
                    if (child >= start && child < limit) {
                        source = p;
                        index = unsigned(child - start);
                    }
                    start = limit;
                }
                const Parent selected = detail::warp::broadcast(parent, source);
                if (offset < reservation.count)
                    deque_.write(reservation, offset, make(selected, index));
            }

            if (!commit(reservation)) return false;
            committed += reservation.count;
        }
        return true;
    }

private:
    __device__ detail::Reservation reserve(unsigned want, unsigned minimum) {
        while (control_.poll()) {
            const auto reservation = deque_.reserve(want, minimum);
            if (reservation.status != detail::Step::retry) return reservation;
            detail::warp::backoff();
        }
        return {detail::Step::stopped, 0, 0};
    }

    __device__ bool commit(const detail::Reservation& reservation) {
        // count_spawned joins body writes before the release counter store.
        if (!control_.count_spawned(reservation.count)) return false;
        deque_.try_release();
        return true;
    }

    detail::DequeOwner<Task>& deque_;
    detail::Control& control_;
};

} // namespace saws
