NVCC ?= nvcc
ARCH ?= sm_70
NVCCFLAGS ?= -O3 -std=c++17 -lineinfo
CPPFLAGS += -I../..

TARGET = gpu-uts
HEADERS = tree.cuh rng.cuh $(wildcard ../../libgputc/*.cuh ../../libgputc/detail/*.cuh ../../libgputc/detail/*.hpp)

.PHONY: all clean
all: $(TARGET)

$(TARGET): uts.cu $(HEADERS)
	$(NVCC) $(CPPFLAGS) $(NVCCFLAGS) -arch=$(ARCH) uts.cu -o $@

clean:
	rm -f $(TARGET)
