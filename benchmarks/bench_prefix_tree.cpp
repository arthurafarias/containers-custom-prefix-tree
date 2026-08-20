// Benchmarks comparing containers::custom::prefix_tree against std::map and
// std::unordered_map for the operations they have in common, plus a
// prefix-query comparison against std::map (via lower_bound) and a naive
// linear scan, to show off the trie's structural advantage for that specific
// query shape.

#include <benchmark/benchmark.h>

#include <containers/custom/prefix_tree.hpp>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Generates `n` unique keys built from a small pool of shared prefixes, so
// that prefix queries have realistic numbers of matches instead of the ~1
// match a purely random string set would give.
std::vector<std::string> generate_keys(std::size_t n) {
    static const std::vector<std::string> roots = {
        "apple", "application", "apply", "banana", "band", "banner",
        "cat", "catalog", "category", "dog", "dodge", "door",
    };
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> root_pick(0, static_cast<int>(roots.size()) - 1);

    std::vector<std::string> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        keys.push_back(roots[static_cast<std::size_t>(root_pick(rng))] + "_" + std::to_string(i));
    }
    return keys;
}

using pt_map = containers::custom::prefix_tree<int>;

}  // namespace

static void BM_Insert_PrefixTree(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        pt_map t;
        for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], static_cast<int>(i));
        benchmark::DoNotOptimize(t);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}
BENCHMARK(BM_Insert_PrefixTree)->Range(1 << 8, 1 << 14);

static void BM_Insert_StdMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        std::map<std::string, int> m;
        for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
        benchmark::DoNotOptimize(m);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}
BENCHMARK(BM_Insert_StdMap)->Range(1 << 8, 1 << 14);

static void BM_Insert_UnorderedMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        std::unordered_map<std::string, int> m;
        for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
        benchmark::DoNotOptimize(m);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}
BENCHMARK(BM_Insert_UnorderedMap)->Range(1 << 8, 1 << 14);

static void BM_Find_Hit_PrefixTree(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    pt_map t;
    for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], static_cast<int>(i));
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(t.find(keys[i]));
        i = (i + 1) % keys.size();
    }
}
BENCHMARK(BM_Find_Hit_PrefixTree)->Range(1 << 8, 1 << 14);

static void BM_Find_Hit_StdMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.find(keys[i]));
        i = (i + 1) % keys.size();
    }
}
BENCHMARK(BM_Find_Hit_StdMap)->Range(1 << 8, 1 << 14);

static void BM_Find_Hit_UnorderedMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::unordered_map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.find(keys[i]));
        i = (i + 1) % keys.size();
    }
}
BENCHMARK(BM_Find_Hit_UnorderedMap)->Range(1 << 8, 1 << 14);

static void BM_Find_Miss_PrefixTree(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    pt_map t;
    for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], static_cast<int>(i));
    for (auto _ : state) {
        benchmark::DoNotOptimize(t.find("definitely-absent-key"));
    }
}
BENCHMARK(BM_Find_Miss_PrefixTree)->Range(1 << 8, 1 << 14);

static void BM_Find_Miss_StdMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.find("definitely-absent-key"));
    }
}
BENCHMARK(BM_Find_Miss_StdMap)->Range(1 << 8, 1 << 14);

static void BM_Find_Miss_UnorderedMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::unordered_map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.find("definitely-absent-key"));
    }
}
BENCHMARK(BM_Find_Miss_UnorderedMap)->Range(1 << 8, 1 << 14);

static void BM_Erase_PrefixTree(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    pt_map t;
    for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], static_cast<int>(i));
    std::size_t i = 0;
    for (auto _ : state) {
        t.erase(keys[i]);
        state.PauseTiming();
        t.insert(keys[i], static_cast<int>(i));
        i = (i + 1) % keys.size();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Erase_PrefixTree)->Range(1 << 8, 1 << 14);

static void BM_Erase_StdMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    std::size_t i = 0;
    for (auto _ : state) {
        m.erase(keys[i]);
        state.PauseTiming();
        m.emplace(keys[i], static_cast<int>(i));
        i = (i + 1) % keys.size();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Erase_StdMap)->Range(1 << 8, 1 << 14);

static void BM_Erase_UnorderedMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::unordered_map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    std::size_t i = 0;
    for (auto _ : state) {
        m.erase(keys[i]);
        state.PauseTiming();
        m.emplace(keys[i], static_cast<int>(i));
        i = (i + 1) % keys.size();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Erase_UnorderedMap)->Range(1 << 8, 1 << 14);

static void BM_Iterate_PrefixTree(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    pt_map t;
    for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], static_cast<int>(i));
    for (auto _ : state) {
        std::int64_t sum = 0;
        for (const auto& kv : t) sum += kv.second;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}
BENCHMARK(BM_Iterate_PrefixTree)->Range(1 << 8, 1 << 14);

static void BM_Iterate_StdMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    for (auto _ : state) {
        std::int64_t sum = 0;
        for (const auto& kv : m) sum += kv.second;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keys.size()));
}
BENCHMARK(BM_Iterate_StdMap)->Range(1 << 8, 1 << 14);

// Prefix query: "give me every key starting with X". prefix_tree does this
// natively in O(prefix length + matches). std::map can approximate it with
// lower_bound + a linear walk while the prefix still matches. A vector with
// no index at all needs a full O(n) scan. This is the trie's headline
// use case, so it gets its own comparison.

static void BM_PrefixQuery_PrefixTree(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    pt_map t;
    for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], static_cast<int>(i));
    for (auto _ : state) {
        std::int64_t matches = 0;
        auto [first, last] = t.prefix_range("application");
        for (; first != last; ++first) ++matches;
        benchmark::DoNotOptimize(matches);
    }
}
BENCHMARK(BM_PrefixQuery_PrefixTree)->Range(1 << 8, 1 << 16);

static void BM_PrefixQuery_StdMap(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    std::map<std::string, int> m;
    for (std::size_t i = 0; i < keys.size(); ++i) m.emplace(keys[i], static_cast<int>(i));
    const std::string prefix = "application";
    for (auto _ : state) {
        std::int64_t matches = 0;
        for (auto it = m.lower_bound(prefix);
             it != m.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
            ++matches;
        }
        benchmark::DoNotOptimize(matches);
    }
}
BENCHMARK(BM_PrefixQuery_StdMap)->Range(1 << 8, 1 << 16);

static void BM_PrefixQuery_LinearScan(benchmark::State& state) {
    auto keys = generate_keys(static_cast<std::size_t>(state.range(0)));
    const std::string prefix = "application";
    for (auto _ : state) {
        std::int64_t matches = 0;
        for (const auto& k : keys) {
            if (k.compare(0, prefix.size(), prefix) == 0) ++matches;
        }
        benchmark::DoNotOptimize(matches);
    }
}
BENCHMARK(BM_PrefixQuery_LinearScan)->Range(1 << 8, 1 << 16);
