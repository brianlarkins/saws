# libgputc tests (requires nvcc and a CUDA device for the ring smoke test).
#
#   make test

CUDA_HOME ?= /usr/local/cuda
NVCC      ?= $(or $(shell command -v nvcc 2>/dev/null),$(CUDA_HOME)/bin/nvcc)
GENCODE   ?= -arch=native

NVCCFLAGS ?= -O2 -g
NVCCFLAGS += -std=c++17 -I. $(GENCODE)

HEADERS = $(wildcard *.hpp) tests/check.hpp
BUILD   = build

.PHONY: all test clean

all: test

$(BUILD)/test_protocol: tests/test_protocol.cu $(HEADERS)
	@mkdir -p $(BUILD)
	$(NVCC) $(NVCCFLAGS) tests/test_protocol.cu -o $@

test: $(BUILD)/test_protocol
	$(BUILD)/test_protocol

clean:
	rm -rf $(BUILD)
