CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror
INCLUDES := -Iinclude
LDFLAGS  ?= -pthread

BUILD    := build
LIB_SRC  := src/order_book.cpp
LIB_OBJ  := $(BUILD)/order_book.o
HEADERS  := $(wildcard include/nanobook/*.hpp)

TEST_BIN  := $(BUILD)/nanobook_tests
BENCH_BIN := $(BUILD)/nanobook_bench
DEMO_BIN  := $(BUILD)/nanobook_demo

.PHONY: all test bench demo snapshot clean

all: $(TEST_BIN) $(BENCH_BIN) $(DEMO_BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(LIB_OBJ): $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(WARNINGS) $(INCLUDES) -c $< -o $@

TEST_SRC := $(wildcard tests/*.cpp)

$(TEST_BIN): $(TEST_SRC) tests/test_framework.hpp $(HEADERS) $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $(WARNINGS) $(INCLUDES) $(TEST_SRC) $(LIB_OBJ) $(LDFLAGS) -o $@

$(BENCH_BIN): benchmarks/bench_latency.cpp $(HEADERS) $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -march=native $(WARNINGS) $(INCLUDES) benchmarks/bench_latency.cpp $(LIB_OBJ) $(LDFLAGS) -o $@

$(DEMO_BIN): demo/nanobook_demo.cpp $(HEADERS) $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $(WARNINGS) $(INCLUDES) demo/nanobook_demo.cpp $(LIB_OBJ) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

snapshot: $(DEMO_BIN)
	./$(DEMO_BIN) --frames 1

clean:
	rm -rf $(BUILD)
