### UTS - Unbalanced Tree Search  ###
TC_TOP=../..

include $(TC_TOP)/tc.mk

COMMON_SRCS = uts.c

TARGETS     = uts-iter-sequential uts-scioto uts-scioto-strict-dfs \
              uts-scioto-initial-bfs uts-scioto-initial-bfs-strict-dfs

RNG_SRC = rng/brg_sha1.c
RNG_INCL= rng/brg_sha1.h
RNG_DEF = -DBRG_RNG

CFLAGS   += $(RNG_DEF)
CXXFLAGS += $(RNG_DEF) 

CXXFLAGS   += -DLDBAL_SCIOTO

# Number of initial work sources for Initial-BFS scheme
INITIAL_WORK_SOURCES = 1

NOINPLACE = 1
ifndef NOINPLACE
CFLAGS  += -DINPLACE
CXXFLAGS+= -DINPLACE
endif

# ------------------------------------- #
# Targets:
# ------------------------------------- #

.PHONY: clean

all: tclibs $(TARGETS)

tags:
	ctags --recurse --language-force=C rng *.c *.h

uts-iter-sequential: tclibs uts_iter_driver.cc UTSIterator.o uts.o $(RNG_SRC:%.c=%.o) SequentialRecursiveLoadBalancer.o
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out tclibs, $+) -DLDBAL_SEQUENTIAL $(TC_LIBS)

#uts-scioto: uts_iter_driver.cc SciotoRecursiveLoadBalancer.cc UTSIterator.o uts.o $(RNG_SRC:%.c=%.o)
uts-scioto: tclibs uts_iter_driver.o SciotoRecursiveLoadBalancer.o UTSIterator.o uts.o $(RNG_SRC:%.c=%.o)
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out tclibs, $+) -DLDBAL_SCIOTO $(TC_LIBS)

uts-scioto-strict-dfs: tclibs uts_iter_driver.cc SciotoRecursiveLoadBalancer.cc UTSIterator.o uts.o $(RNG_SRC:%.c=%.o)
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out tclibs, $+) -DSTRICT_DFS -DLDBAL_SCIOTO $(TC_LIBS)

uts-scioto-initial-bfs: tclibs uts_iter_driver.cc SciotoRecursiveLoadBalancer.cc UTSIterator.o uts.o $(RNG_SRC:%.c=%.o)
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out tclibs, $+) -DINITIAL_BFS -DLDBAL_SCIOTO -DINITIAL_SOURCES=$(INITIAL_WORK_SOURCES) $(TC_LIBS)

uts-scioto-initial-bfs-strict-dfs: tclibs uts_iter_driver.cc SciotoRecursiveLoadBalancer.cc UTSIterator.o uts.o $(RNG_SRC:%.c=%.o)
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out tclibs, $+) -DSTRICT_DFS -DINITIAL_BFS -DLDBAL_SCIOTO -DINITIAL_SOURCES=$(INITIAL_WORK_SOURCES) $(TC_LIBS)

clean: tcclean
	rm -f *.o rng/*.o $(TARGETS) tags

