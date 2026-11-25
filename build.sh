#!/usr/bin/env sh
# Builds NanoBook with g++ directly (no make/cmake needed), then runs the selected targets.
# Usage: ./build.sh [all|test|bench|demo|build]   (default: all = tests + demo snapshot + benchmark)
set -eu

CXX="${CXX:-g++}"
FLAGS="-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror -Iinclude"
MODE="${1:-all}"

case "$MODE" in
    all|test|bench|demo|build) ;;
    *) echo "usage: $0 [all|test|bench|demo|build]" >&2; exit 2 ;;
esac

cd "$(dirname "$0")"
mkdir -p build

echo "[build] library"
$CXX $FLAGS -c src/order_book.cpp -o build/order_book.o
echo "[build] tests"
$CXX $FLAGS tests/*.cpp build/order_book.o -pthread -o build/nanobook_tests
echo "[build] benchmark"
$CXX $FLAGS -march=native benchmarks/bench_latency.cpp build/order_book.o -pthread -o build/nanobook_bench
echo "[build] demo"
$CXX $FLAGS demo/nanobook_demo.cpp build/order_book.o -o build/nanobook_demo

if [ "$MODE" = "test" ] || [ "$MODE" = "all" ]; then
    echo; ./build/nanobook_tests
fi
if [ "$MODE" = "all" ]; then
    echo; ./build/nanobook_demo --frames 1
fi
if [ "$MODE" = "demo" ]; then
    ./build/nanobook_demo
fi
if [ "$MODE" = "bench" ] || [ "$MODE" = "all" ]; then
    echo; ./build/nanobook_bench
fi
