# UTS on GPU SAWS (libgputc V1).
#
#   make                 build gpu-uts (needs nvcc, sm_80+) and uts-seq
#   make test-uts-dev    host-only check of the ported tree code (no CUDA)
#
#   source ../uts/sample_trees.sh && ./gpu-uts $T1
#
# GENCODE defaults to the GPU in this machine; override for cross-builds, e.g.
#   make GENCODE="-gencode arch=compute_90,code=sm_90"

CUDA_HOME ?= /usr/local/cuda
NVCC      ?= $(or $(shell command -v nvcc 2>/dev/null),$(CUDA_HOME)/bin/nvcc)
GENCODE   ?= -arch=native

GPUTC = ../../libgputc
UTS   = ../uts

DEFS      = -DBRG_RNG
INCLUDES  = -I$(GPUTC) -I$(UTS)
NVCCFLAGS ?= -O3 -lineinfo
NVCCFLAGS += -std=c++17 $(GENCODE) $(INCLUDES) $(DEFS)
CFLAGS    ?= -O3
CFLAGS    += $(INCLUDES) $(DEFS)
CXXFLAGS  ?= -O3
CXXFLAGS  += -std=c++17 -Wall $(INCLUDES) $(DEFS)

GPUTC_HEADERS = $(wildcard $(GPUTC)/*.hpp $(GPUTC)/*.cuh)
UTS_OBJS      = uts.o brg_sha1.o

all: gpu-uts uts-seq

gpu-uts: gpu_uts.o $(UTS_OBJS)
	$(NVCC) $(NVCCFLAGS) $^ -o $@ -lm

gpu_uts.o: gpu_uts.cu uts_dev.cuh uts_params.h $(GPUTC_HEADERS)
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

uts-seq: uts_seq.cpp uts_dev.cuh uts_params.h $(UTS_OBJS)
	$(CXX) $(CXXFLAGS) uts_seq.cpp $(UTS_OBJS) -o $@ -lm

test-uts-dev: test_uts_dev.cpp uts_dev.cuh uts_params.h $(UTS_OBJS)
	$(CXX) $(CXXFLAGS) test_uts_dev.cpp $(UTS_OBJS) -o $@ -lm

uts.o: $(UTS)/uts.c
	$(CC) $(CFLAGS) -c $< -o $@

brg_sha1.o: $(UTS)/rng/brg_sha1.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f gpu-uts uts-seq test-uts-dev *.o

.PHONY: all clean
