#pragma once

#include "context.cuh"
#include "detail/worker.cuh"
#include <limits>
#include <type_traits>
#include <vector>

namespace saws {

struct Options {
    unsigned queue_capacity = 4096; // Tasks per worker; power of two, 32..131072.
    unsigned workers = 0;          // Zero selects the resident cooperative grid.
    unsigned threads_per_block = 128;
    cudaStream_t stream = nullptr;
};

struct Result {
    Error error = Error::none;
    cudaError_t cuda_error = cudaSuccess;
    const char* message = "success";
    std::uint64_t executed = 0; // Meaningful as a completed tree size on success.
    unsigned workers = 0;
    unsigned error_worker = 0;
    std::uint64_t error_detail = 0;

    bool ok() const { return error == Error::none; }
};

namespace detail {

template<class Task>
struct RunStorage {
    DequeState* deques = nullptr;
    Task* bodies = nullptr;
    Counters* counters = nullptr;
    RunState* control = nullptr;

    RunStorage() = default;
    RunStorage(const RunStorage&) = delete;
    RunStorage& operator=(const RunStorage&) = delete;

    ~RunStorage() {
        if (control) cudaFree(control);
        if (counters) cudaFree(counters);
        if (bodies) cudaFree(bodies);
        if (deques) cudaFree(deques);
    }
};

inline bool product_fits(std::size_t a, std::size_t b) {
    return b == 0 || a <= std::numeric_limits<std::size_t>::max() / b;
}

} // namespace detail

// Blocking host entry point. Execute is a device-callable value of the form:
//   void(Context<Task>&, const Task&, bool valid) const
// Application buffers referenced by Execute must remain alive through this call.
template<class Task, class Execute>
Result run(const Task& root, Execute execute, Options options = {}) {
    static_assert(std::is_trivially_copyable_v<Task>, "Task must be trivially copyable");
    static_assert(std::is_trivially_copy_assignable_v<Task>, "Task must support trivial assignment");
    static_assert(std::is_trivially_copyable_v<Execute>, "Execute must be a device-copyable value");

    Result result;
    const auto invalid = [&](const char* message) {
        result.error = Error::invalid_options;
        result.message = message;
        return result;
    };
    const auto checked = [&](cudaError_t error) {
        if (error == cudaSuccess) return true;
        result.error = Error::cuda_failure;
        result.cuda_error = error;
        result.message = cudaGetErrorString(error);
        return false;
    };

    unsigned capacity = options.queue_capacity;
    if (capacity < detail::chunk_size || capacity > detail::max_queue_capacity ||
        (capacity & (capacity - 1)))
        return invalid("queue_capacity must be a power of two from 32 through 131072 tasks");
    if (options.threads_per_block == 0 || options.threads_per_block % 32)
        return invalid("threads_per_block must be a positive multiple of 32");

    int device = 0;
    cudaDeviceProp properties{};
    if (!checked(cudaGetDevice(&device)) ||
        !checked(cudaGetDeviceProperties(&properties, device))) return result;
    if (!properties.cooperativeLaunch) {
        result.error = Error::unsupported_device;
        result.message = error_name(result.error);
        return result;
    }
    if (options.threads_per_block > unsigned(properties.maxThreadsPerBlock))
        return invalid("threads_per_block exceeds this device's block limit");

    unsigned threads = options.threads_per_block;
    // A small explicit worker count can use a smaller block, including one warp.
    if (options.workers && options.workers < threads / 32) threads = options.workers * 32;
    const unsigned warps_per_block = threads / 32;
    int resident_blocks = 0;
    if (!checked(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &resident_blocks, detail::worker_kernel<Task, Execute>, int(threads), 0)))
        return result;
    if (resident_blocks == 0) return invalid("the task kernel cannot reside with this block size");

    const std::uint64_t max_blocks = std::uint64_t(resident_blocks) * properties.multiProcessorCount;
    const std::uint64_t max_workers = max_blocks * warps_per_block;
    std::uint64_t selected = options.workers ? options.workers : max_workers;
    if (selected == 0 || selected > max_workers || selected % warps_per_block)
        return invalid("workers must fit the resident grid and contain a whole number of blocks");
    // Bound already-in-flight additions after any worker trips the attempt guard.
    if (std::uint64_t(detail::attempt_limit) + selected >= (std::uint64_t{1} << 32))
        return invalid("worker count exceeds the steal-attempt guard bound");
    unsigned workers = unsigned(selected);
    const unsigned blocks = workers / warps_per_block;
    if (blocks > unsigned(properties.maxGridSize[0]))
        return invalid("worker count exceeds the device grid limit");
    result.workers = workers;

    if (!detail::product_fits(workers, capacity)) return invalid("task storage size overflows size_t");
    const std::size_t slots = std::size_t(workers) * capacity;
    if (!detail::product_fits(slots, sizeof(Task)) ||
        !detail::product_fits(workers, sizeof(detail::DequeState)) ||
        !detail::product_fits(workers, sizeof(detail::Counters)))
        return invalid("allocation size overflows size_t");

    detail::RunStorage<Task> storage;
    if (!checked(cudaMalloc(reinterpret_cast<void**>(&storage.deques),
                            std::size_t(workers) * sizeof(detail::DequeState))) ||
        !checked(cudaMalloc(reinterpret_cast<void**>(&storage.bodies), slots * sizeof(Task))) ||
        !checked(cudaMalloc(reinterpret_cast<void**>(&storage.counters),
                            std::size_t(workers) * sizeof(detail::Counters))) ||
        !checked(cudaMalloc(reinterpret_cast<void**>(&storage.control), sizeof(detail::RunState))))
        return result;

    detail::initialize<<<(workers + 255) / 256, 256, 0, options.stream>>>(
        storage.deques, storage.bodies, storage.counters, storage.control, workers, root);
    if (!checked(cudaGetLastError())) return result;

    Task root_value = root;
    void* arguments[] = {&storage.deques, &storage.bodies, &storage.counters,
                         &storage.control, &capacity, &workers, &root_value, &execute};
    const auto launch = cudaLaunchCooperativeKernel(
        reinterpret_cast<const void*>(detail::worker_kernel<Task, Execute>),
        dim3(blocks), dim3(threads), arguments, 0, options.stream);
    if (!checked(launch)) {
        cudaStreamSynchronize(options.stream); // Finish initialization before cleanup.
        return result;
    }
    if (!checked(cudaStreamSynchronize(options.stream))) return result;

    detail::RunState state{};
    if (!checked(cudaMemcpy(&state, storage.control, sizeof(state), cudaMemcpyDeviceToHost)))
        return result;
    // Error wins even if the checker also published done.
    result.error = static_cast<Error>(state.error);
    result.message = error_name(result.error);
    result.error_worker = state.error_worker;
    result.error_detail = state.error_detail;
    if (!result.ok()) return result;

    std::vector<detail::Counters> counters(workers);
    if (!checked(cudaMemcpy(counters.data(), storage.counters,
                            counters.size() * sizeof(detail::Counters), cudaMemcpyDeviceToHost)))
        return result;
    for (const auto& counter : counters) {
        if (counter.executed > UINT64_MAX - result.executed) {
            result.error = Error::counter_overflow;
            result.message = error_name(result.error);
            return result;
        }
        result.executed += counter.executed;
    }
    return result;
}

} // namespace saws
