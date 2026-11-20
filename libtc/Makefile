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
				locality.h					 \
				laws_shrb.h					 \
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
				saws_shrb.o						 \
				locality.o						 \
				laws.o							 \
				laws_shrb.o						 \
				collection-laws.o
			 	# line eater

.PHONY: all
all: libtc.a

libtc.a: headers $(OBJS)
	mkdir -p ../lib
	$(AR) rcs ../lib/libtc.a $(OBJS)
	$(RANLIB) ../lib/libtc.a

$(OBJS): $(HDRS)

# Build provenance: libtc revision and the OpenSHMEM install behind $(CC).
# Regenerated on every build but only rewritten when it changes.
.PHONY: FORCE
buildinfo.h: FORCE
	@printf '#define GTC_BUILD_REV   "%s"\n#define GTC_BUILD_SHMEM "%s"\n' \
	  "$$(git -C .. describe --always --dirty --abbrev=10 2>/dev/null || echo unknown)" \
	  "$$(dirname $$(dirname $$(command -v $(CC) 2>/dev/null || echo ./unknown/x)))" > buildinfo.h.tmp
	@cmp -s buildinfo.h.tmp buildinfo.h || mv buildinfo.h.tmp buildinfo.h
	@rm -f buildinfo.h.tmp

init.o: buildinfo.h

.PHONY: headers
headers:
	mkdir -p ../include
	cp $(HDRS) ../include/.

.PHONY: clean
clean:
	rm -f *~ *.o buildinfo.h ../include/tc.h ../lib/libtc.a
