# libgputc -- GPU task collection prototype
# (c) 2026 D. Brian Larkins
#
# Requires CUDA >= 12 and NVSHMEM >= 2.9.  Set NVSHMEM_HOME and CUDA_HOME, or
# override on the command line:
#
#   make NVSHMEM_HOME=/opt/nvshmem CUDA_HOME=/usr/local/cuda
#
# NOTE: relocatable device code (-rdc=true) is mandatory.  NVSHMEM's device
# library is linked separately, and the persistent kernel calls across
# translation units.

CUDA_HOME    ?= /usr/local/cuda
NVSHMEM_HOME ?= /opt/nvshmem

# Hopper and Blackwell.  sm_70 is the practical floor: the steal path uses
# __nanosleep and relies on independent thread scheduling for the block-scoped
# head lock to be safe.
GENCODE  ?= -gencode arch=compute_90,code=sm_90 \
            -gencode arch=compute_80,code=sm_80

NVCC      = $(CUDA_HOME)/bin/nvcc
NVCCFLAGS = -O3 -std=c++14 -rdc=true $(GENCODE) \
            -I$(NVSHMEM_HOME)/include -I$(CUDA_HOME)/include -I.

# Uncomment for protocol tracing; very noisy.
# NVCCFLAGS += -DGPUTC_DEBUG

LIB     = libgputc.a
SRCS    = gputc_host.cu collection.cu
OBJS    = $(SRCS:.cu=.o)
HEADERS = gputc.h gputc_types.cuh gpu_shrb.cuh gpu_task.cuh \
          gpu_termination.cuh gputc_internal.cuh

all: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

%.o: %.cu $(HEADERS)
	$(NVCC) $(NVCCFLAGS) -dc $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
