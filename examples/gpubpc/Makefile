# gpubpc -- GPU Bouncing Producer-Consumer benchmark
# (c) 2026 D. Brian Larkins

CUDA_HOME    ?= /usr/local/cuda
NVSHMEM_HOME ?= /opt/nvshmem
GPUTC_HOME   ?= ../libgputc

GENCODE  ?= -gencode arch=compute_90,code=sm_90 \
            -gencode arch=compute_80,code=sm_80

NVCC      = $(CUDA_HOME)/bin/nvcc
NVCCFLAGS = -O3 -std=c++14 -rdc=true $(GENCODE) \
            -I$(GPUTC_HOME) -I$(NVSHMEM_HOME)/include -I$(CUDA_HOME)/include

# NVSHMEM's device library must be linked into the final binary, not the
# archive, and -rdc=true means device linking happens here.
LDFLAGS   = -L$(GPUTC_HOME) -L$(NVSHMEM_HOME)/lib -L$(CUDA_HOME)/lib64
LDLIBS    = -lgputc -lnvshmem_host -lnvshmem_device -lcudart

TARGET = gpubpc

all: $(TARGET)

$(TARGET): gpubpc.o $(GPUTC_HOME)/libgputc.a
	$(NVCC) $(NVCCFLAGS) gpubpc.o -o $@ $(LDFLAGS) $(LDLIBS)

gpubpc.o: gpubpc.cu
	$(NVCC) $(NVCCFLAGS) -dc $< -o $@

$(GPUTC_HOME)/libgputc.a:
	$(MAKE) -C $(GPUTC_HOME)

clean:
	rm -f gpubpc.o $(TARGET)

.PHONY: all clean
