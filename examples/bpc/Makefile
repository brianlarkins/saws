#
# makefile for bouncing producer consumer
# 
#
TC_TOP=../..

include $(TC_TOP)/tc.mk

.PHONY: all
all: bpc

bpc: tclibs bpc.c busy_wait.c
	$(CC) $(CFLAGS) -o $@ bpc.c busy_wait.c  $(TC_LIBS)

busy_wait: busy_wait.c
	$(CC) $(CFLAGS) -DBW_MAIN -o $@ busy_wait.c $(TC_LIBS)

clean: tcclean
	rm -f *~ *.o gmon.out bpc


