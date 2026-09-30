#include "tree.cuh"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

void usage(const char* program) {
    std::printf(
        "Usage: %s [UTS parameters] [SAWS options]\n"
        "  -t N  tree: 0 binomial, 1 geometric, 2 hybrid, 3 balanced\n"
        "  -a N  geometric shape: 0 linear, 1 exponential, 2 cyclic, 3 fixed\n"
        "  -b X  root branching factor (default 4)\n"
        "  -d N  shape/depth parameter (default 6)\n"
        "  -r N  root seed (default 0)\n"
        "  -q X  nonleaf probability (default 15/64)\n"
        "  -m N  nonleaf children (default 4)\n"
        "  -f X  hybrid transition as a fraction of depth (default 0.5)\n"
        "  -g N  hash repetitions per child (default 1)\n"
        "  --capacity N  tasks per worker (default 4096)\n"
        "  --workers N   worker count (default 0: automatic)\n"
        "  --threads N   threads per block (default 128)\n", program);
}

bool integer(const char* text, unsigned& value) {
    if (!*text || *text == '-') return false;
    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(text, &end, 10);
    if (errno || *end || parsed > std::numeric_limits<unsigned>::max()) return false;
    value = unsigned(parsed);
    return true;
}

bool real(const char* text, double& value) {
    if (!*text) return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtod(text, &end);
    return !errno && !*end && std::isfinite(value);
}

bool cuda_ok(cudaError_t error) {
    if (error == cudaSuccess) return true;
    std::fprintf(stderr, "CUDA: %s\n", cudaGetErrorString(error));
    return false;
}

} // namespace

int main(int argc, char** argv) {
    uts::Parameters parameters;
    saws::Options options;
    unsigned seed = 0;
    for (int i = 1; i < argc; ++i) {
        const char* flag = argv[i];
        if (!std::strcmp(flag, "-h") || !std::strcmp(flag, "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (++i == argc) {
            std::fprintf(stderr, "Missing value for %s\n", flag);
            return 2;
        }
        const char* value = argv[i];
        unsigned enum_value = 0;
        bool valid = false;
        if (!std::strcmp(flag, "-t")) {
            valid = integer(value, enum_value) && enum_value <= 3;
            parameters.tree = static_cast<uts::Tree>(enum_value);
        } else if (!std::strcmp(flag, "-a")) {
            valid = integer(value, enum_value) && enum_value <= 3;
            parameters.shape = static_cast<uts::Shape>(enum_value);
        } else if (!std::strcmp(flag, "-b")) valid = real(value, parameters.branching);
        else if (!std::strcmp(flag, "-d")) valid = integer(value, parameters.depth);
        else if (!std::strcmp(flag, "-r")) valid = integer(value, seed);
        else if (!std::strcmp(flag, "-q")) valid = real(value, parameters.nonleaf_probability);
        else if (!std::strcmp(flag, "-m")) valid = integer(value, parameters.nonleaf_children);
        else if (!std::strcmp(flag, "-f")) valid = real(value, parameters.shift_depth);
        else if (!std::strcmp(flag, "-g")) valid = integer(value, parameters.granularity);
        else if (!std::strcmp(flag, "--capacity")) valid = integer(value, options.queue_capacity);
        else if (!std::strcmp(flag, "--workers")) valid = integer(value, options.workers);
        else if (!std::strcmp(flag, "--threads")) valid = integer(value, options.threads_per_block);
        if (!valid) {
            std::fprintf(stderr, "Invalid option or value: %s %s\n", flag, value);
            return 2;
        }
    }

    if (parameters.branching < 0 || parameters.branching > std::numeric_limits<unsigned>::max() ||
        parameters.depth == 0 || parameters.granularity == 0 ||
        parameters.nonleaf_probability < 0 || parameters.nonleaf_probability > 1 ||
        parameters.shift_depth < 0 || parameters.shift_depth > 1 ||
        (parameters.shape == uts::Shape::exponential && parameters.depth < 2)) {
        std::fprintf(stderr, "Invalid tree parameters; use --help for the available options.\n");
        return 2;
    }

    uts::Statistics* device_statistics = nullptr;
    if (!cuda_ok(cudaMalloc(reinterpret_cast<void**>(&device_statistics), sizeof(uts::Statistics))))
        return 1;
    if (!cuda_ok(cudaMemset(device_statistics, 0, sizeof(uts::Statistics)))) {
        cudaFree(device_statistics);
        return 1;
    }

    const uts::Node root{uts::rng::root(seed), 0};
    const auto start = std::chrono::steady_clock::now();
    const auto result = saws::run(root, uts::Execute{parameters, device_statistics}, options);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    uts::Statistics statistics{};
    const bool copied = result.ok() && cuda_ok(cudaMemcpy(
        &statistics, device_statistics, sizeof(statistics), cudaMemcpyDeviceToHost));
    const bool freed = cuda_ok(cudaFree(device_statistics));
    if (!result.ok()) {
        std::fprintf(stderr, "SAWS: %s (worker %u, detail %llu)\n", result.message,
                     result.error_worker, static_cast<unsigned long long>(result.error_detail));
        return 1;
    }
    if (!copied || !freed) return 1;

    std::printf("GPU UTS: tree=%u shape=%u branching=%.8g depth=%u seed=%u\n",
                unsigned(parameters.tree), unsigned(parameters.shape), parameters.branching,
                parameters.depth, seed);
    std::printf("Workers: %u; queue capacity: %u tasks/worker\n", result.workers, options.queue_capacity);
    std::printf("Nodes: %llu; leaves: %llu; maximum depth: %u\n",
                static_cast<unsigned long long>(result.executed), statistics.leaves, statistics.depth);
    std::printf("SAWS run including setup: %.6f s (%.3f million nodes/s)\n",
                seconds, seconds > 0 ? double(result.executed) / seconds / 1e6 : 0.0);
    return 0;
}
