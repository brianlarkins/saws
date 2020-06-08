# 
# makefile for shared ringbuffer tests
# 

TC_TOP=../..

include $(TC_TOP)/tc.mk

TARGETS = test-sdc-shrb         \
          #end


############################################################
# SHared Ring Buffer Tests
############################################################


.PHONY: all
all: $(TARGETS)

test-sdc-shrb: tclibs test-sdc-shrb.o ../../libtc/sdc_shr_ring.o
	$(CC) $(CFLAGS) -o $@  test-sdc-shrb.o ../../libtc/sdc_shr_ring.o $(TC_LIBS)

clean: tcclean
	rm -f *~ *.o gmon.out $(TARGETS)
