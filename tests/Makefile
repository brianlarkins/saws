# 
# makefile for task collection tests
# 
#
TC_TOP=..

include $(TC_TOP)/tc.mk

TARGETS=test-hello          \
				test-task           \
				test-rrtask         \
				test-simple         \
				test-tasktree       \
				test-tasktree-twotc \
				test-termination    \
				test-sdc-shrb		    \
				test-saws-shrb		  \
				threadtest          \
				shmem_latency	    \
				# line eater

TEST_DIRS=clod microbenchmarks shrb synch

############################################################
# Task Collection Tests
############################################################
.PHONY: all subs
all: $(TARGETS) subs
#all: test-hello test-task test-rrtask

subs:
	for dir in $(TEST_DIRS); do \
		$(MAKE) -C $$dir; \
	done;\

test-hello: tclibs test-hello.o
	$(CC) $(CFLAGS) -o $@ test-hello.o $(TC_LIBS)

test-task: tclibs test-task.o
	$(CC) $(CFLAGS) -o $@ test-task.o $(TC_LIBS)

test-simple: tclibs test-simple.o
	$(CC) $(CFLAGS) -o $@ test-simple.o $(TC_LIBS)

test-rrtask: tclibs test-rrtask.o
	$(CC) $(CFLAGS) -o $@ test-rrtask.o $(TC_LIBS)

test-tasktree: tclibs test-tasktree.o
	$(CC) $(CFLAGS) -o $@ test-tasktree.o $(TC_LIBS)

test-tasktree-twotc: tclibs test-tasktree-twotc.o
	$(CC) $(CFLAGS) -o $@ test-tasktree-twotc.o $(TC_LIBS)

test-termination: tclibs test-termination.o
	$(CC) $(CFLAGS) -o $@ test-termination.o $(TC_LIBS)

test-sdc-shrb: tclibs test_sdc_shrb.o
	$(CC) $(CFLAGS) -o $@ test_sdc_shrb.o $(TC_LIBS)

test-saws-shrb: tclibs test_saws_shrb.o
	$(CC) $(CFLAGS) -o $@ test_saws_shrb.o $(TC_LIBS)

test-timer: tclibs test-timer.o
	$(CC) $(CFLAGS) -o $@ test-timer.o $(TC_LIBS)

threadtest: tclibs threadtest.o
	$(CC) $(CFLAGS) -o $@ threadtest.o $(TC_LIBS)

shmem_latency: tclibs shmem_latency.o
	$(CC) $(CFLAGS) -o $@ shmem_latency.o $(TC_LIBS)

clean: tcclean
	for dir in $(TEST_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done;\
	rm -f *~ *.o gmon.out $(TARGETS)

