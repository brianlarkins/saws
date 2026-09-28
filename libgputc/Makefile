#   make test       host protocol tests + nvcc compile/runtime checks
#   make test-gpu   device ring smoke (needs GPU)
#   make test-host  g++ only

CXX      ?= c++
NVCC     ?= $(or $(shell command -v nvcc 2>/dev/null),/usr/local/cuda/bin/nvcc)
GENCODE  ?= -arch=native

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -I. -pthread

NVCCFLAGS ?= -O2 -g
NVCCFLAGS += -std=c++17 -I. $(GENCODE)

HOST_TESTS = test_steal_word test_saws_ring
CUDA_TESTS = test_protocol compile_runtime
HEADERS    = $(wildcard *.hpp *.cuh) tests/check.hpp

BUILD = build

.PHONY: all test test-host test-gpu clean

all: test

test: test-host test-gpu

test-host: $(HOST_TESTS:%=$(BUILD)/%)
	@for t in $^; do $$t || exit 1; done
	@if command -v $(NVCC) >/dev/null 2>&1; then \
	  $(NVCC) $(NVCCFLAGS) -c tests/compile_runtime.cu -o $(BUILD)/compile_runtime.o && \
	  echo "compile_runtime: object ok"; \
	else \
	  echo "compile_runtime: skipped (nvcc not found)"; \
	fi

test-gpu: $(BUILD)/test_protocol
	$(BUILD)/test_protocol

$(BUILD)/%: tests/%.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_protocol: tests/test_protocol.cu $(HEADERS)
	@mkdir -p $(BUILD)
	$(NVCC) $(NVCCFLAGS) tests/test_protocol.cu -o $@

clean:
	rm -rf $(BUILD)
