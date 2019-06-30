# 
# makefile for shared ringbuffer tests
# 

TC_TOP=../..

include $(TC_TOP)/tc.mk

TARGETS = test-shrb             \
          test-shrb-contention  \
          test-sdc-shrb         \
          test-sh               \
          test-sn               \
          #end


############################################################
# SHared Ring Buffer Tests
############################################################


.PHONY: all
all: $(TARGETS)

test-shrb: tclibs test-shrb.o ../../libtc/shr_ring.o
	$(CC) $(CFLAGS) -o $@ test-shrb.o ../../libtc/shr_ring.o $(TC_LIBS)

test-sdc-shrb: tclibs test-sdc-shrb.o ../../libtc/sdc_shr_ring.o
	$(CC) $(CFLAGS) -o $@  test-sdc-shrb.o ../../libtc/sdc_shr_ring.o $(TC_LIBS)

test-sh: tclibs test-sh.o ../../libtc/sn_ring.o ../../libtc/sh_ring.o
	$(CC) $(CFLAGS) -o $@  test-sh.o ../../libtc/sn_ring.o ../../libtc/sh_ring.o $(TC_LIBS)

test-sn: tclibs test-sn.o ../../libtc/sn_ring.o
	$(CC) $(CFLAGS) -o $@  test-sn.o ../../libtc/sn_ring.o $(TC_LIBS)

test-shrb-contention: tclibs test-shrb-contention.o ../../libtc/shr_ring.o
	$(CC) $(CFLAGS) -o $@ test-shrb-contention.o  ../../libtc/shr_ring.o $(TC_LIBS)

clean: tcclean
	rm -f *~ *.o gmon.out $(TARGETS)
