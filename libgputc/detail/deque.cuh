#pragma once

#include "claim.hpp"
#include "control.cuh"
#include <cstddef>

namespace saws::detail {

struct Epoch {
    unsigned retired; // Atomic: written by thieves, observed by the owner.
    unsigned claimed;
    std::uint64_t end;
    bool used;
};

// Only descriptor and Epoch::retired are accessed by remote workers.
// Everything else belongs exclusively to this deque's lane zero.
struct alignas(64) DequeState {
    alignas(8) std::uint64_t descriptor;
    std::uint64_t head, split, pub_base, tail;
    Epoch epochs[epoch_slots];
    unsigned pending[epoch_slots];
    unsigned pending_begin, pending_count;
    int current;
};

struct Reservation {
    Step status = Step::retry;
    std::uint64_t begin = 0;
    unsigned count = 0;
};

struct Claim {
    unsigned source = 0; // Physical task index; subsequent indexing wraps.
    unsigned chunks = 0;
    unsigned slot = 0;
    bool stopped = false;
};

template<class Task>
struct Batch {
    Task body;
    unsigned count;
    __device__ bool valid() const { return warp::lane() < count; }
};

template<class Task>
class DequeOwner {
public:
    __device__ DequeOwner(DequeState* states, Task* bodies, unsigned capacity,
                          Control& control)
        : state_(states[control.worker()]),
          bodies_(bodies + std::size_t(control.worker()) * capacity),
          capacity_(capacity), control_(control) {}

    __device__ unsigned capacity() const { return capacity_; }

    // Collective: returns only after all lanes have loaded their task bodies.
    __device__ Step take_batch(Batch<Task>& batch) {
        Reservation pop;
        if (warp::lane() == 0) {
            reclaim();
            pop.status = Step::ready;
            if (state_.head == state_.split) pop.status = acquire();
            if (pop.status == Step::ready) {
                const auto private_count = state_.head - state_.split;
                pop.count = private_count < chunk_size ? unsigned(private_count) : chunk_size;
                state_.head -= pop.count;
                pop.begin = state_.head;
            }
        }
        warp::join();
        pop = warp::broadcast(pop);
        batch.count = pop.count;
        if (pop.status == Step::ready && batch.valid())
            batch.body = bodies_[physical(pop.begin + warp::lane())];
        warp::join(); // Popped slots may be overwritten during the callback.
        return pop.status;
    }

    // Collective. A successful reservation pins its range until the caller
    // writes and joins. Do not publish or make another reservation meanwhile.
    __device__ Reservation reserve(unsigned want, unsigned minimum) {
        Reservation result;
        if (warp::lane() == 0) {
            result.status = make_space(minimum);
            if (result.status == Step::ready) {
                const unsigned free = capacity_ - unsigned(state_.head - state_.tail);
                result.count = want < free ? want : free;
                result.begin = state_.head;
                state_.head += result.count;
            }
        }
        warp::join();
        return warp::broadcast(result);
    }

    // Collective, before issuing a remote claim. Only this owner can allocate
    // private slots, so no allocation is needed to preserve this guarantee.
    __device__ Step prepare_receive(unsigned minimum) {
        Step result = Step::retry;
        if (warp::lane() == 0) result = make_space(minimum);
        warp::join();
        return warp::broadcast(result);
    }

    // Collective, after a successful claim. prepare_receive already guaranteed
    // capacity: this step must never wait, roll, truncate, or abandon the claim.
    __device__ Reservation receive(unsigned count) {
        Reservation result;
        if (warp::lane() == 0) {
            result = {Step::ready, state_.head, count};
            state_.head += count;
        }
        warp::join();
        return warp::broadcast(result);
    }

    __device__ void write(const Reservation& reservation, unsigned offset,
                           const Task& body) {
        bodies_[physical(reservation.begin + offset)] = body;
    }

    // Collective, best effort. All reserved bodies must already be written and
    // counted (or transferred) before entering here.
    __device__ void try_release() {
        warp::join();
        if (warp::lane() == 0) {
            const unsigned full = unsigned(state_.head - state_.split) / chunk_size;
            if (full && unclaimed() == 0) {
                const int slot = free_epoch();
                if (slot >= 0) {
                    close();
                    state_.split += std::uint64_t((full + 1) / 2) * chunk_size;
                    publish(unsigned(slot));
                    reclaim();
                }
            }
        }
        warp::join();
    }

private:
    __device__ unsigned physical(std::uint64_t position) const {
        return unsigned(position) & (capacity_ - 1);
    }

    __device__ unsigned unclaimed() const {
        if (state_.current < 0) return 0;
        const Descriptor word{Atomic<std::uint64_t>(state_.descriptor)
                                  .load(cuda::memory_order_acquire)};
        return remaining(word.chunks(), word.attempts());
    }

    __device__ void reclaim() {
        while (state_.pending_count) {
            const unsigned slot = state_.pending[state_.pending_begin];
            Epoch& epoch = state_.epochs[slot];
            if (Atomic<unsigned>(epoch.retired).load(cuda::memory_order_acquire)
                    != epoch.claimed) break;
            state_.tail = epoch.end;
            epoch.used = false;
            state_.pending_begin = (state_.pending_begin + 1) % epoch_slots;
            --state_.pending_count;
        }
        if (state_.pending_count == 0) state_.tail = state_.pub_base;
    }

    __device__ int free_epoch() {
        reclaim();
        for (unsigned slot = 0; slot < epoch_slots; ++slot)
            if (!state_.epochs[slot].used) return int(slot);
        return -1;
    }

    // The returned pre-close word determines which claims won the race.
    __device__ void close() {
        if (state_.current < 0) return;
        const Descriptor word{Atomic<std::uint64_t>(state_.descriptor)
                                  .fetch_or(closed_bit, cuda::memory_order_acq_rel)};
        const unsigned claimed = word.chunks() - remaining(word.chunks(), word.attempts());
        const unsigned slot = unsigned(state_.current);
        Epoch& epoch = state_.epochs[slot];
        state_.pub_base += std::uint64_t(claimed) * chunk_size;
        if (claimed) {
            epoch.claimed = claimed;
            epoch.end = state_.pub_base;
            // Retirement may have happened before close. Never reset it here.
            const unsigned end = (state_.pending_begin + state_.pending_count) % epoch_slots;
            state_.pending[end] = slot;
            ++state_.pending_count;
        } else {
            epoch.used = false;
        }
        state_.current = -1;
    }

    __device__ void publish(unsigned slot) {
        const unsigned chunks = unsigned(state_.split - state_.pub_base) / chunk_size;
        if (chunks == 0) return;
        Epoch& epoch = state_.epochs[slot];
        epoch.used = true;
        Atomic<unsigned>(epoch.retired).store(0, cuda::memory_order_relaxed);
        state_.current = int(slot);
        const auto word = Descriptor::open(slot, chunks, physical(state_.pub_base) / chunk_size);
        Atomic<std::uint64_t>(state_.descriptor).store(word.bits, cuda::memory_order_release);
    }

    __device__ Step acquire() {
        if (state_.current < 0) return Step::empty;
        // Reserve replacement bookkeeping before closing a live epoch.
        const int slot = free_epoch();
        if (slot < 0) return Step::retry;
        close();
        const unsigned rest = unsigned(state_.split - state_.pub_base) / chunk_size;
        state_.split -= std::uint64_t((rest + 1) / 2) * chunk_size;
        publish(unsigned(slot));
        reclaim();
        return state_.head == state_.split ? Step::empty : Step::ready;
    }

    __device__ Step roll() {
        if (state_.current >= 0) {
            const int slot = free_epoch();
            if (slot < 0) return Step::retry;
            close();
            publish(unsigned(slot)); // Preserve split: rolling does not acquire.
        }
        reclaim();
        return Step::ready;
    }

    __device__ Step make_space(unsigned minimum) {
        reclaim();
        if (state_.head - state_.tail + minimum <= capacity_) return Step::ready;
        const Step rolled = roll();
        // Logical occupancy excludes claimed work, even if copies pin storage.
        const auto queued = state_.head - state_.split +
                            std::uint64_t(unclaimed()) * chunk_size;
        if (queued + minimum > capacity_) {
            control_.fail(Error::capacity_exceeded, queued + minimum);
            return Step::stopped;
        }
        if (rolled == Step::retry) return Step::retry;
        return state_.head - state_.tail + minimum <= capacity_ ? Step::ready : Step::retry;
    }

    DequeState& state_;
    Task* bodies_;
    unsigned capacity_;
    Control& control_;
};

template<class Task>
class RemoteDeque {
public:
    __device__ RemoteDeque(DequeState* states, Task* bodies, unsigned capacity,
                           unsigned victim)
        : state_(states[victim]), bodies_(bodies + std::size_t(victim) * capacity),
          capacity_(capacity) {}

    // Collective. The fetch-add alone authorizes a claim; no discovery load.
    __device__ Claim claim(Control& control) {
        Claim result;
        if (warp::lane() == 0) {
            const Descriptor word{Atomic<std::uint64_t>(state_.descriptor)
                                      .fetch_add(attempt_increment, cuda::memory_order_acq_rel)};
            if (word.attempts() >= attempt_limit - 1) {
                control.fail(Error::attempt_overflow, word.attempts());
                result.stopped = true;
            } else if (!word.closed()) {
                const Share share = half_share(word.chunks(), word.attempts());
                result = {(word.base() + share.first) * chunk_size,
                          share.count, word.slot(), false};
            }
        }
        warp::join(); // Propagate lane zero's acquire before source reads.
        return warp::broadcast(result);
    }

    __device__ Task read(const Claim& claim, unsigned offset) const {
        return bodies_[(claim.source + offset) & (capacity_ - 1)];
    }

    __device__ void retire(const Claim& claim) {
        warp::join(); // Every source read AND destination write precedes reuse.
        if (warp::lane() == 0)
            Atomic<unsigned>(state_.epochs[claim.slot].retired)
                .fetch_add(claim.chunks, cuda::memory_order_release);
        warp::join();
    }

private:
    DequeState& state_;
    Task* bodies_;
    unsigned capacity_;
};

} // namespace saws::detail
