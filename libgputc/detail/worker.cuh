#pragma once

#include "../context.cuh"

namespace saws::detail {

enum class StealResult { received, miss, retry, stopped };

template<class Task>
__device__ StealResult steal_once(DequeOwner<Task>& local, DequeState* states,
                                  Task* bodies, Control& control, unsigned& random) {
    if (control.workers() == 1) return StealResult::miss;

    const unsigned chunks = local.capacity() / chunk_size;
    const unsigned maximum_share = chunks > 1 ? chunks / 2 : 1;
    const Step space = local.prepare_receive(maximum_share * chunk_size);
    if (space == Step::retry) return StealResult::retry;
    if (space == Step::stopped || !control.poll()) return StealResult::stopped;

    unsigned victim = 0;
    if (warp::lane() == 0) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        victim = random % (control.workers() - 1);
        if (victim >= control.worker()) ++victim;
    }
    victim = warp::broadcast(victim);
    RemoteDeque<Task> remote(states, bodies, local.capacity(), victim);
    const Claim claim = remote.claim(control);
    if (claim.stopped) return StealResult::stopped;
    if (claim.chunks == 0) return StealResult::miss;

    // From here through retire: no waits, execution, or second claim.
    const auto destination = local.receive(claim.chunks * chunk_size);
    for (unsigned i = warp::lane(); i < destination.count; i += chunk_size)
        local.write(destination, i, remote.read(claim, i));
    remote.retire(claim);
    local.try_release();
    return StealResult::received;
}

template<class Task, class Execute>
__global__ void worker_kernel(DequeState* states, Task* bodies, Counters* counters,
                              RunState* run, unsigned capacity, unsigned workers,
                              Task root, Execute execute) {
    const unsigned worker = (blockIdx.x * blockDim.x + threadIdx.x) / chunk_size;
    Control control(run, counters, worker, workers);
    DequeOwner<Task> local(states, bodies, capacity, control);
    Context<Task> context(local, control);
    unsigned random = (0x9e3779b9u * (worker + 1)) | 1u;
    // Root supplies a defined inactive-lane value without requiring Task to
    // have a default constructor. Only valid lanes may produce effects.
    Batch<Task> batch{root, 0};

    while (control.poll()) {
        switch (local.take_batch(batch)) {
        case Step::ready:
            execute(context, batch.body, batch.valid());
            if (!control.poll() || !control.complete_batch(batch.count)) return;
            local.try_release();
            break;

        case Step::retry:
            // A blocked owner acquire must not fall through to stealing.
            warp::backoff();
            break;

        case Step::empty:
            switch (steal_once(local, states, bodies, control, random)) {
            case StealResult::received: break;
            case StealResult::miss:
            case StealResult::retry: warp::backoff(); break;
            case StealResult::stopped: return;
            }
            break;

        case Step::stopped:
            return;
        }
    }
}

// Separate launch, ordered before the persistent kernel on the same stream.
template<class Task>
__global__ void initialize(DequeState* states, Task* bodies, Counters* counters,
                           RunState* run, unsigned workers, Task root) {
    const unsigned w = blockIdx.x * blockDim.x + threadIdx.x;
    if (w >= workers) return;
    DequeState state{};
    state.descriptor = closed_bit;
    state.current = -1;
    state.head = w == 0 ? 1 : 0;
    states[w] = state;
    counters[w].spawned = w == 0 ? 1 : 0;
    counters[w].executed = 0;
    if (w == 0) {
        *run = RunState{};
        bodies[0] = root;
    }
}

} // namespace saws::detail
