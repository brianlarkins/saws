#
# makefile overlord for portals scioto
# author: d. brian larkins
# created: 4/24/18
#

CFLAGS=$(GCFLAGS) -I. -I../include

RANLIB = /usr/bin/ranlib

HDRS =  mutex.h        			 \
				sdc_shr_ring.h 			 \
				tc.h           			 \
				termination.h  			 \
				clod.h							 \
				saws_shrb.h					 \
				# line eater


OBJS =  collection-sdc.o     \
				collection-saws.o	\
				common.o             \
        handle.o             \
        init.o               \
        mutex.o              \
			  sdc_shr_ring.o       \
				task.o               \
			 	termination.o        \
			  util.o               \
				clod.o							 \
				tc-clod.o						 \
				saws_shrb.o
			 	# line eater

.PHONY: all
all: libtc.a

libtc.a: headers $(OBJS)
	mkdir -p ../lib
	$(AR) rcs ../lib/libtc.a $(OBJS)
	$(RANLIB) ../lib/libtc.a

$(OBJS): $(HDRS)

.PHONY: headers
headers:
	mkdir -p ../include
	cp $(HDRS) ../include/.

.PHONY: clean
clean:
	rm -f *~ *.o ../include/tc.h ../lib/libtc.a
