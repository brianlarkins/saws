# Host-only tests for the queue protocol. No CUDA required.
#
#   make test        optimized build, run all tests
#   make tsan        ThreadSanitizer build, run all tests

CXX      ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -I. -pthread

TESTS = test_steal_word test_ring test_saws_ring
HEADERS = $(wildcard *.hpp) tests/check.hpp

BUILD = build
TSAN  = build/tsan

all: $(TESTS:%=$(BUILD)/%)

$(BUILD)/%: tests/%.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(TSAN)/%: tests/%.cpp $(HEADERS)
	@mkdir -p $(TSAN)
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=thread $< -o $@

test: $(TESTS:%=$(BUILD)/%)
	@for t in $^; do $$t || exit 1; done

tsan: $(TESTS:%=$(TSAN)/%)
	@for t in $^; do $$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all test tsan clean
