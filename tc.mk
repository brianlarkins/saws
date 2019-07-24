#
# makefile overlord for task collections
# author: d. brian larkins
# created: 1/22/18
#
#CC = clang
CC    = oshcc
CXX   = oshc++
MPICC = mpicc
#GCFLAGS = -fsanitize=address -fsanitize=undefined -Wall --std=c99 -g3 -rdynamic -O3 -D_POSIX_C_SOURCE=200112L -msse4.2 # development
GCFLAGS = -fsanitize=address -fsanitize=undefined -g3 -Wall -rdynamic -D_POSIX_C_SOURCE=200112L -msse4.2
#GCFLAGS = -g3 -Wall
#GCFLAGS = -g3 -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
#GCFLAGS = --std=c99 -g -pg -O3 -D_POSIX_C_SOURCE=200112L -msse4.2 # profiling
#GCFLAGS = -DNDEBUG --std=c99 -O3 -D_POSIX_C_SOURCE=200112L -msse4.2 # performance
#GCFLAGS = -O3
GCXXFLAGS = -std=gnu++11 -fsanitize=address -fsanitize=undefined -Wall -g -rdynamic -O3 -D_POSIX_C_SOURCE=200112L -msse4.2 # development
#GCXXFLAGS = -pg -g -O3 -D_POSIX_C_SOURCE=200112L -msse4.2 # profiling
CFLAGS    = $(GCFLAGS) -I. -I$(TC_TOP)/include
CXXFLAGS  = $(GCXXFLAGS) -I. -I$(TC_TOP)/include
CFLAGSMPI = $(GCFLAGS) -I. -I$(TC_TOP)/includempi

#LDFLAGS=-L$(PORTALS_LIBDIR)
MATH_LIB            = -lm
RT_LIB              = -lrt
PMI_LIB             = -lpmi
PTHREAD_LIB         = -lpthread
TC_INSTALL_LIBDIR   = $(TC_TOP)/lib
TC_LIB             = $(TC_INSTALL_LIBDIR)/libtc.a

TC_LIBDIRS = $(TC_TOP)/libtc

TC_LIBS = $(TC_LIB) $(PTHREAD_LIB) $(PMI_LIB) $(PORTALS_LIB) $(MATH_LIB) $(RT_LIB)

.PHONY: all

default: all


checkflags:
ifndef TC_TOP
	@echo You must define TC_TOP with the path of the top-level directory
	@false
endif

.PHONY: tclibs $(TC_LIBDIRS)
tclibs: tcheaders $(TC_LIBDIRS)

.PHONY: tcheaders
tcheaders:
	for dir in $(TC_LIBDIRS); do \
    $(MAKE) -C $$dir headers; \
  done; \
	for dir in $(TC_LIBMPIDIRS); do \
		$(MAKE) -C $$dir headers; \
	done

$(TC_LIBDIRS):
	$(MAKE) -C $@ GCFLAGS="$(GCFLAGS)" CC="$(CC)"

$(TC_LIBMPIDIRS):
	$(MAKE) -C $@ GCFLAGS="$(GCFLAGS)" CC="$(MPICC)"

tcclean:
	for dir in $(TC_LIBDIRS); do \
    $(MAKE) -C $$dir clean; \
  done;\
	for dir in $(TC_LIBMPIDIRS); do \
		$(MAKE) -C $$dir clean; \
	done;\
	rm -f *~ *.o gmon.out $(LOCAL_EXECS)
