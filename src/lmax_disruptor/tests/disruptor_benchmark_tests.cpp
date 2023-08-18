#include <benchmark/benchmark.h>

#include "test_helpers.hpp"

constexpr size_t NoOfWriters = 3;
constexpr size_t NoOfWritesPerWriter =200;

static void BM_Queue3Readers1Writer(benchmark::State& state) {
    for (auto _ : state) {
       auto queue_stats = tests::TimedQueueTask ( NoOfWriters,  NoOfWritesPerWriter);
    }
}

static void BM_LMAX3Readers1Writer(benchmark::State& state) {
    for (auto _ : state) {
        auto disruptor_stats = tests::TimedDisruptorTask ( NoOfWriters,  NoOfWritesPerWriter);
    }
}


BENCHMARK(BM_Queue3Readers1Writer);
BENCHMARK(BM_LMAX3Readers1Writer);

BENCHMARK_MAIN();