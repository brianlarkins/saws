# 
# makefile for task collection tests
# 

TC_TOP=../..

include $(TC_TOP)/tc.mk

TARGETS     = time-get                  \
              time-get-sdc              \
              time-tc                   \
              time-td                   \
              #end

.PHONY: all
all: $(TARGETS)

############################################################
# Task Collection Micro Bechmarks
############################################################

time-get: tclibs time-get.o
	$(CC) $(CFLAGS) -o $@ time-get.o $(TC_LIBS)

time-get-sdc: tclibs time-get-sdc.o
	$(CC) $(CFLAGS) -o $@ time-get-sdc.o $(TC_LIBS)

time-tc: tclibs time-tc.o
	$(CC) $(CFLAGS) -o $@ time-tc.o $(TC_LIBS)

time-add: tclibs time-add.o
	$(CC) $(CFLAGS) -o $@ time-add.o $(TC_LIBS)

time-add-inplace: tclibs time-add.o
	$(CC) $(CFLAGS) -DINPLACE -o $@ time-add.o $(TC_LIBS)

time-add-drain: tclibs time-add-drain.o
	$(CC) $(CFLAGS) -o $@ time-add-drain.o $(TC_LIBS)

time-add-drain-inline: tclibs time-add-drain.o
	$(CC) $(CFLAGS) -DINLINE -o $@ time-add-drain.o $(TC_LIBS)

time-rrtc: tclibs time-rrtc.o
	$(CC) $(CFLAGS) -o $@ time-rrtc.o $(TC_LIBS)

time-td: tclibs time-td.o
	$(CC) $(CFLAGS) -o $@ time-td.o $(TC_LIBS)

time-dispersion: tclibs time-dispersion.o
	$(CC) $(CFLAGS) -o $@ time-dispersion.o $(TC_LIBS)

clean: tcclean
	rm -f *~ *.o gmon.out $(TARGETS)
