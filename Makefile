# 
# makefile for task collection tests
# 
#
TC_TOP=.

include $(TC_TOP)/tc.mk

EXE_DIRS=tests

.PHONY: all clean
all: $(EXE_DIRS)
	for dir in $(EXE_DIRS); do \
		$(MAKE) -C $$dir; \
	done;\

clean: $(EXE_DIRS)
	for dir in $(EXE_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done;\
