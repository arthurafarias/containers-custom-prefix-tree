# containers-custom-prefix-tree

A header-only, STL-inspired C++23 container implementing a **prefix_tree (prefix
tree)** that stores unique *sequences* of `ElementType`, with support for
`std::pmr` allocators. `prefix_tree<char>` is to `std::set<std::string>`
roughly what `std::string` is to `std::vector<char>`. It lives in the
`containers::custom` namespace/CMake namespace alongside other custom
container libraries, so several such libraries can be `add_subdirectory()`'d
into one umbrella build without target or option name clashes.

```cpp
#include <containers/custom/prefix_tree.hpp>

using namespace std::string_view_literals;

containers::custom::prefix_tree<char> words;
words.insert("fox"sv);
words.insert("dog"sv);

for (const auto& entry : words) {
    // iterates in lexicographic order: dog, fox
    // `entry` is a zero-copy view, not a std::string -- see below
}

auto [first, last] = words.prefix_range("fo"sv);
// [first, last) yields every stored sequence starting with "fo"
```

## Why a prefix tree, not `std::set<std::string>`

A `prefix_tree` stores each sequence element-by-element along a path from the
root, so besides the usual associative-container operations it can answer
prefix-shaped questions directly, without scanning the whole container:

- `starts_with(prefix)` -- does any stored sequence begin with `prefix`?
- `prefix_range(prefix)` -- `[begin, end)` iterator pair over every stored
  sequence beginning with `prefix`, in lexicographic order (an
  "autocomplete" query).
- `erase_prefix(prefix)` -- remove every sequence beginning with `prefix` in
  one call, returning how many were removed.

## Allocator support

`prefix_tree<ElementType, Allocator>` defaults to
`std::pmr::polymorphic_allocator<ElementType>`, so every node is allocated
through whatever `std::pmr::memory_resource` you provide -- an arena, a
pool, a monotonic buffer -- letting you control and often eliminate heap
traffic:

```cpp
std::array<std::byte, 4096> buffer{};
std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
containers::custom::prefix_tree<char> t{&arena};   // every node comes out of `buffer`
```

The container is templated on the allocator type, so a plain
`std::allocator<char>` (or any type satisfying the standard `Allocator`
requirements) also works. Allocator propagation on copy/move
assignment/swap follows `std::allocator_traits`' `propagate_on_container_*`
traits, exactly like the standard `std::pmr` containers -- which is also
why `std::pmr::polymorphic_allocator` (whose `operator=` is deleted) is
usable as the allocator at all.

## What it looks like vs. `std::set`

The public interface deliberately mirrors `std::set` where the concepts
line up, generalized from a single comparable value to a sequence of them:

| | |
|---|---|
| `insert`, `erase`, `clear`, `swap` | modifiers |
| `find`, `count`, `contains` | lookup |
| `begin`/`end`/`cbegin`/`cend`, forward iteration | traversal, lexicographic order |
| `empty`, `size`, `max_size` | capacity |
| copy/move construction & assignment, `operator==` | value semantics |

Any `std::ranges::input_range` whose value type is `ElementType` -- a
`std::string`, `std::string_view`, `std::vector<T>`, `std::array<T, N>`, and
so on -- can be passed to any of these.

**One deliberate deviation:** because a stored sequence is never kept
contiguously anywhere (it's spelled out by the path from the root to a
node), dereferencing an iterator does not yield a real
`const Sequence&`. It yields a `prefix_tree_element_view`, a small
zero-copy proxy that is itself a range over `const ElementType&`,
reconstructed by reading straight out of the tree's own nodes as you
iterate. This is transparent for the common `for (auto element : view)`,
streaming (`os << view`), and range-comparison (`view == "arthur"sv`)
usage patterns, but the type is not a literal `std::string` (or whatever
sequence type you inserted), so passing `*it` to something that requires
exactly that type won't compile -- and the view is only valid for as long
as the iterator that produced it is alive and hasn't been advanced.

Iterator invalidation is looser than `std::set`'s node-stability guarantee:
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
containers::custom::prefix_tree<char> shared_tree;
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
`containers::custom::detail::prefix_tree_iterator` holding a
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

The suite (15 cases, ~55 assertions) covers ordinary usage -- insert, find,
contains, iteration order, `starts_with`/`prefix_range`/`erase_prefix`,
`erase` by key and by iterator, copy/move construction, `operator==`, and
`swap` -- and has been run clean under AddressSanitizer+UndefinedBehaviorSanitizer.
It does not yet include dedicated exception-safety-under-injected-failure or
concurrency/ThreadSanitizer tests despite the thread-safety machinery
described above being implemented; that coverage is a good next addition.

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

`benchmarks/bench_prefix_tree.cpp` is currently a placeholder (it links and
runs, but contains no actual benchmarks yet). The intent, once filled in
with [Google Benchmark](https://github.com/google/benchmark), is to compare
`prefix_tree` against `std::set<std::string>` and `std::unordered_set<std::string>`
for insert/find/erase/iterate, and to compare `prefix_range()` against a
`std::set::lower_bound`-based prefix scan and a naive linear scan -- the
trie's own expected advantage being `starts_with()`/existence-style prefix
checks (`O(prefix length)`, independent of container size) and prefix
queries in general, which `std::unordered_set` cannot support without a
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

template <class ElementType,
          class Allocator = std::pmr::polymorphic_allocator<ElementType>>
class prefix_tree {
public:
    using element_type   = ElementType;
    using allocator_type = Allocator;
    using size_type       = std::size_t;
    using iterator         = /* forward iterator, element_view reference */;
    using const_iterator   = iterator;
    using element_view     = /* zero-copy view over a stored sequence */;

    // construction: default, allocator, initializer_list<Sequence>,
    // copy/move construction & assignment (allocator-propagation aware)

    iterator begin() const; iterator end() const;
    iterator cbegin() const; iterator cend() const;

    bool empty() const; size_type size() const; size_type max_size() const;
    void clear(); void swap(prefix_tree& other) noexcept;

    // Sequence is any std::ranges::input_range<ElementType>
    // (std::string, std::string_view, std::vector<T>, ...)
    template <class Sequence> std::pair<iterator, bool> insert(const Sequence&);
    template <class Sequence> size_type erase(const Sequence&);
    iterator erase(iterator pos);

    template <class Sequence> iterator find(const Sequence&) const;
    template <class Sequence> bool contains(const Sequence&) const;
    template <class Sequence> size_type count(const Sequence&) const;

    template <class Sequence> bool starts_with(const Sequence& prefix) const;
    template <class Sequence>
    std::pair<iterator, iterator> prefix_range(const Sequence& prefix) const;
    template <class Sequence> size_type erase_prefix(const Sequence& prefix);

    friend bool operator==(const prefix_tree&, const prefix_tree&);
};

}  // namespace containers::custom
```
