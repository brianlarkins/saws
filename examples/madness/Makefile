#
# madness sciotwo kernel makefile
#

TC_TOP=../..

include $(TC_TOP)/tc.mk

OBJS =  init.o           \
				math.o           \
				tensor.o         \
				tree.o	         \
        mad_analytics.o  \
        mad3d.o


all: tclibs mad3d

mad3d: tclibs $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(filter-out tclibs, $+) $(TC_LIBS)

analytic_test: mad_analytics.o analytic_test.o
	$(CC) $(CFLAGS) -o $@ $+ -lm 

clean: tcclean
	rm -f *~ *.o gmon.out mad3d analytic_test

