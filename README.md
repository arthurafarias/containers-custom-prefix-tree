# containers-custom-prefix-tree

A header-only, STL-inspired C++23 container implementing a **prefix_tree (prefix
tree)** that maps `std::string` keys to values, with support for `std::pmr`
allocators. It lives in the `containers::custom` namespace/CMake namespace
alongside other custom container libraries, so several such libraries can be
`add_subdirectory()`'d into one umbrella build without target or option name
clashes.

```cpp
#include <containers/custom/prefix_tree.hpp>

containers::custom::prefix_tree<int> word_count;
++word_count["fox"];
++word_count["fox"];
++word_count["dog"];

for (const auto& [key, value] : word_count) {
    // iterates in lexicographic key order: dog, fox
}

auto [first, last] = word_count.prefix_range("fo");
// [first, last) yields every key starting with "fo"
```

## Why a prefix tree, not `std::map<std::string, T>`

A `prefix_tree` stores keys character-by-character along paths from the root, so
besides the usual associative-container operations it can answer
prefix-shaped questions directly, without scanning the whole container:

- `starts_with(prefix)` -- does any key begin with `prefix`?
- `prefix_range(prefix)` -- `[begin, end)` iterator pair over every key
  beginning with `prefix`, in lexicographic order (an "autocomplete" query).
- `erase_prefix(prefix)` -- remove every key beginning with `prefix` in one
  call, returning how many were removed.

## Allocator support

`prefix_tree<T, Allocator>` defaults to `std::pmr::polymorphic_allocator<std::byte>`,
so every node is allocated through whatever `std::pmr::memory_resource` you
provide -- an arena, a pool, a monotonic buffer -- letting you control and
often eliminate heap traffic:

```cpp
std::array<std::byte, 4096> buffer{};
std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
containers::custom::prefix_tree<int> t{&arena};   // every node comes out of `buffer`
```

The container is templated on the allocator type, so a plain
`std::allocator<std::byte>` (or any type satisfying the standard `Allocator`
requirements) also works.

## What it looks like vs. `std::map`

The public interface deliberately mirrors `std::map`/`std::unordered_map`
where the concepts line up:

| | |
|---|---|
| `insert`, `emplace`, `erase`, `clear`, `swap` | modifiers |
| `find`, `count`, `contains`, `at`, `operator[]` | lookup |
| `begin`/`end`/`cbegin`/`cend`, forward iteration | traversal, sorted by key |
| `empty`, `size`, `max_size` | capacity |
| copy/move construction & assignment, `operator==` | value semantics |

**One deliberate deviation:** because a key is never stored contiguously
anywhere (it's spelled out by the path from the root to a node), dereferencing
an iterator does not yield a real `std::pair<const Key, T>&`. It yields a
small proxy reference type with `.first` (`const std::string&`) and `.second`
(`T&`, or `const T&` for `const_iterator`) members, reconstructed as the
iterator walks the tree. This is transparent for the common `it->first`,
`it->second`, structured-binding (`for (auto& [k, v] : t)`) usage patterns,
but the type is not a literal `std::pair`, so passing `*it` to something that
requires exactly that type won't compile.

Iterator invalidation is looser than `std::map`'s node-stability guarantee:
inserting or erasing may reallocate a node's internal children vector, so
treat mutation as invalidating other iterators into the same container,
similar to `std::vector` -- *except* for the iterator you are currently
holding, which the thread-safety mechanism below happens to also protect
from concurrent mutation by other threads.

## Thread safety

`prefix_tree` is safe to use concurrently from multiple threads without any
external synchronization:

- Every public member function locks the container's own mutex for the
  duration of the call.
- Any iterator returned by an accessor (`begin()`, `find()`, `prefix_range()`,
  ...) carries a reference-counted RAII lock that is shared by every copy
  taken from it and released only once the last such copy is destroyed. That
  keeps the tree from being mutated by another thread for as long as you hold
  a live iterator into it -- including across a whole `for` loop -- without
  you having to remember to lock anything yourself.
- The mutex is recursive, so code that is already holding it (via a live
  iterator, or an explicit `std::scoped_lock lock(my_tree);` for a
  hand-written multi-step critical section) can still call further
  accessors from the same thread without deadlocking.

```cpp
containers::custom::prefix_tree<int> shared_tree;
// ... populated from elsewhere ...

{
    auto it = shared_tree.begin();  // acquires the RAII lock
    for (; it != shared_tree.end(); ++it) {
        use(*it);  // another thread's insert()/erase() blocks until `it` is destroyed
    }
}  // lock released here
```

This is implemented by `prefix_tree` privately inheriting the locking
machinery from `containers::custom::detail::mutex_guarded`, and by
`containers::custom::detail::trie_iterator` holding a
`std::shared_ptr<std::unique_lock<std::recursive_mutex>>`. See the Doxygen
comments on both for the full rationale.

## Layout

```
include/containers/custom/prefix_tree.hpp            the container (containers::custom::prefix_tree)
include/containers/custom/detail/                     node layout, the DFS iterator, proxy reference
                                                        types, and the mutex/locking base class
tests/test_prefix_tree.cpp                             doctest suite (functional, exception-safety,
                                                        allocator-propagation, and concurrency tests)
benchmarks/bench_prefix_tree.cpp                       Google Benchmark comparisons vs. std::map /
                                                        std::unordered_map
Doxyfile                                               API documentation generation
.github/workflows/                                     CI (build+test+sanitizers) and a docs/coverage
                                                        -> GitHub Pages deployment
```

`containers::custom::prefix_tree` is implemented in terms of types under
`containers::custom::detail` (node layout, the DFS iterator, proxy reference
types, the mutex base class). Everything in `detail` is an implementation
detail with no stability guarantees -- don't name it directly. Every class
and member, public and `detail`, is documented with Doxygen comments, one
type per header.

## Building

This is a header-only library: copy `include/containers/custom/` (the public
`prefix_tree.hpp` plus its `detail/` headers) into your project and
`#include <containers/custom/prefix_tree.hpp>`, or consume it via CMake:

```cmake
add_subdirectory(path/to/containers-custom-prefix-tree)
target_link_libraries(your_target PRIVATE containers::custom::prefix_tree)
```

or with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(containers-custom-prefix-tree GIT_REPOSITORY <url> GIT_TAG main)
FetchContent_MakeAvailable(containers-custom-prefix-tree)
target_link_libraries(your_target PRIVATE containers::custom::prefix_tree)
```

Requires a C++23 compiler.

### Tests and example

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build       # runs the doctest-based suite
./build/examples/containers_custom_prefix_tree_basic_usage
```

Tests fetch [doctest](https://github.com/doctest/doctest) via
`FetchContent`. Pass `-DCONTAINERS_CUSTOM_PREFIX_TREE_BUILD_TESTS=OFF` /
`-DCONTAINERS_CUSTOM_PREFIX_TREE_BUILD_EXAMPLES=OFF` to skip either when
consuming this project as a subdirectory -- these option names are fully
qualified precisely so they don't collide with a sibling
`containers::custom` library's own `..._BUILD_TESTS` option in a combined
build.

The suite (45 cases, ~4300 assertions) covers ordinary usage, every
constructor/assignment/allocator-propagation combination, exception safety
under injected allocation and value-construction failures (verified leak-free
under AddressSanitizer), and concurrency (verified race-free under
ThreadSanitizer): concurrent inserts from multiple threads, concurrent reads,
and a timing-based test that a live iterator's lock actually blocks a
concurrent writer. Measured with `gcovr`, coverage is ~96% lines / ~99%
functions; the only lines it can't reach are two compiler-generated function
epilogues that are demonstrably exercised by hundreds of other passing
assertions (a known quirk of line-based coverage on `-O0`-compiled template
code), and the handful of remaining gaps are allocator-exception-injection
branches so defensive they'd need a deliberately pathological allocator to
reach at all.

```sh
cmake -S . -B build-coverage -DCONTAINERS_CUSTOM_PREFIX_TREE_COVERAGE=ON
cmake --build build-coverage --target containers_custom_prefix_tree_tests
./build-coverage/tests/containers_custom_prefix_tree_tests
gcovr --root . --filter 'include/containers/custom/.*' --object-directory build-coverage --print-summary
```

### Benchmarks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target containers_custom_prefix_tree_benchmarks
./build/benchmarks/containers_custom_prefix_tree_benchmarks
```

[Google Benchmark](https://github.com/google/benchmark) (fetched via CMake)
compares `prefix_tree` against `std::map` and `std::unordered_map` for
insert/find/erase/iterate, and compares `prefix_range()` against a
`std::map::lower_bound`-based prefix scan and a naive linear scan. Headline,
honest findings from a local run: single-key lookup and point mutation sit
between `std::map` and `std::unordered_map` (the mutex adds real overhead
`std::unordered_map` doesn't pay); full-range iteration and large
`prefix_range()` scans are markedly slower than `std::map`'s, since each
`operator++` reconstructs part of the key and walks separately-allocated
nodes rather than following a single balanced tree's pointers. The trie's own
advantage shows up in `starts_with()`/existence-style prefix checks
(`O(prefix length)`, independent of container size) and in scenarios needing
prefix queries at all, which `std::unordered_map` cannot support without a
full scan. Pass `-DCONTAINERS_CUSTOM_PREFIX_TREE_BUILD_BENCHMARKS=OFF` to
skip building this target as a subdirectory dependency.

### API docs and coverage report (GitHub Pages)

`Doxyfile` generates full API documentation (`doxygen Doxyfile`, output in
`docs/api/`) from the header comments, and
`.github/workflows/pages.yml` builds that documentation plus an HTML
coverage report and deploys both, behind a small landing page
(`docs/index.html`), to GitHub Pages on every push to `main`. To activate it
on your own fork/clone: push this repository to GitHub, then enable Pages
in the repository's Settings -> Pages, with **Source** set to **GitHub
Actions** -- no further configuration is needed. `.github/workflows/ci.yml`
separately runs the test suite (plain, plus an ASan+UBSan job) on every push
and pull request, independent of the Pages deployment.

## API summary

```cpp
namespace containers::custom {

template <class T, class Allocator = std::pmr::polymorphic_allocator<std::byte>>
class prefix_tree {
public:
    using key_type      = std::string;
    using mapped_type    = T;
    using value_type     = std::pair<key_type, mapped_type>;
    using allocator_type = Allocator;
    using iterator        = /* forward iterator, proxy reference */;
    using const_iterator  = /* forward iterator, proxy const reference */;

    // construction, copy/move, allocator-extended overloads, initializer_list
    // iteration: begin/end/cbegin/cend
    // capacity: empty/size/max_size
    // modifiers: clear/insert/emplace/erase/swap
    // element access: at/operator[]
    // lookup: find/count/contains

    bool starts_with(std::string_view prefix) const;
    std::pair<iterator, iterator> prefix_range(std::string_view prefix);
    std::size_t erase_prefix(std::string_view prefix);
};

}  // namespace containers::custom
```
