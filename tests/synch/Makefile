# 
# makefile for transparent memory adaptor
# 
# id: $Id: Makefile 614 2007-03-06 21:28:26Z brian $
# 

TC_TOP=../..

include $(TC_TOP)/tc.mk


TARGETS     = test-mutex

.PHONY: all
all: $(TARGETS)

test-mutex: tclibs test-mutex.o
	$(CC) $(CFLAGS) -o $@ test-mutex.o $(TC_LIBS)

test-barrier: tclibs test-barrier.o
	$(CC) $(CFLAGS) -o $@ test-barrier.o $(TC_LIBS)

test-incr: test-incr.o
	$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

test-swap: test-swap.o
	$(CC) $(CFLAGS) -o $@ $+ $(LIBS)

clean: tcclean
	rm -f *~ *.o gmon.out $(TARGETS)
