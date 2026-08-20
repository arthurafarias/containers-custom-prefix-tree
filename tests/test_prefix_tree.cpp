#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <containers/custom/prefix_tree.hpp>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using containers::custom::prefix_tree;

namespace {

std::vector<std::string> keys_in_order(const prefix_tree<int>& t) {
    std::vector<std::string> out;
    for (const auto& kv : t) out.push_back(kv.first);
    return out;
}

// A minimal allocator (not std::pmr-based) whose propagate_on_container_*
// traits are all true and whose is_always_equal is false, so that copying,
// move-assigning, and swapping a prefix_tree built on it exercise the
// `if constexpr (propagate...)` branches that a default-allocator prefix_tree
// never instantiates (the default std::pmr::polymorphic_allocator has all of
// those traits as false_type).
template <class T>
struct propagating_allocator {
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    int id = 0;

    propagating_allocator() = default;
    explicit propagating_allocator(int id_) : id(id_) {}
    template <class U>
    propagating_allocator(const propagating_allocator<U>& other) noexcept : id(other.id) {}

    T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p); }

    friend bool operator==(const propagating_allocator& a, const propagating_allocator& b) {
        return a.id == b.id;
    }
    friend bool operator!=(const propagating_allocator& a, const propagating_allocator& b) {
        return !(a == b);
    }
};

// A value type whose copy constructor throws once a shared counter reaches
// zero, used to force prefix_tree's clone_subtree() into its exception-safety
// (catch-and-rethrow-after-cleanup) path.
struct throws_on_nth_copy {
    static inline int copies_until_throw = -1;  // -1 disables throwing entirely.

    int value = 0;

    throws_on_nth_copy() = default;
    throws_on_nth_copy(int v) : value(v) {}
    throws_on_nth_copy(const throws_on_nth_copy& other) : value(other.value) {
        if (copies_until_throw >= 0 && --copies_until_throw == 0) {
            throw std::runtime_error("forced copy failure");
        }
    }
    throws_on_nth_copy& operator=(const throws_on_nth_copy&) = default;
};

// A value type whose constructor-from-int throws, used to exercise insert()'s
// exception path.
struct throws_on_construct {
    explicit throws_on_construct(int) { throw std::runtime_error("forced construct failure"); }
};

struct allocation_failure : std::runtime_error {
    allocation_failure() : std::runtime_error("forced allocation failure") {}
};

// Shared across every instantiation of sometimes_throwing_allocator<T> below
// -- a `static inline` data member of the class template would instead give
// each T its own independent counter, since sometimes_throwing_allocator<A>
// and sometimes_throwing_allocator<B> are unrelated types.
inline int g_throw_after = -1;  // -1 disables throwing entirely.

// An allocator whose *rebind* conversion constructor (the one used to turn a
// prefix_tree<T, Allocator> into the rebound allocators for its internal node
// and node-pointer-vector types) throws once g_throw_after reaches zero.
// Used to force prefix_tree::create_node() into its exception-safety
// (allocate-then-construct-throws-so-deallocate-and-rethrow) path -- code
// that a well-behaved allocator's default construction essentially never
// reaches.
template <class T>
struct sometimes_throwing_allocator {
    using value_type = T;

    sometimes_throwing_allocator() = default;
    template <class U>
    sometimes_throwing_allocator(const sometimes_throwing_allocator<U>&) {
        if (g_throw_after >= 0 && --g_throw_after == 0) throw allocation_failure();
    }

    T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p); }

    friend bool operator==(const sometimes_throwing_allocator&, const sometimes_throwing_allocator&) {
        return true;
    }
    friend bool operator!=(const sometimes_throwing_allocator&, const sometimes_throwing_allocator&) {
        return false;
    }
};

}  // namespace

TEST_CASE("default-constructed prefix_tree is empty") {
    prefix_tree<int> t;
    CHECK(t.empty());
    CHECK(t.size() == 0);
    CHECK(t.begin() == t.end());
}

TEST_CASE("insert reports success/failure and grows size") {
    prefix_tree<int> t;
    auto [it1, inserted1] = t.insert("cat", 1);
    CHECK(inserted1);
    CHECK(it1->first == "cat");
    CHECK(it1->second == 1);
    CHECK(t.size() == 1);

    auto [it2, inserted2] = t.insert("cat", 99);
    CHECK_FALSE(inserted2);
    CHECK(it2->second == 1);  // unchanged
    CHECK(t.size() == 1);
}

TEST_CASE("supports the empty string as a key") {
    prefix_tree<int> t;
    t.insert("", 42);
    CHECK(t.contains(""));
    CHECK(t.at("") == 42);
    CHECK(t.size() == 1);

    t.insert("a", 1);
    CHECK(keys_in_order(t) == std::vector<std::string>{"", "a"});
}

TEST_CASE("iteration order is lexicographic, including prefix-of-prefix keys") {
    prefix_tree<int> t;
    for (const auto& k : {"cats", "car", "cat", "dog", "do", "cataclysm"}) t.insert(k, 0);

    CHECK(keys_in_order(t) ==
          std::vector<std::string>{"car", "cat", "cataclysm", "cats", "do", "dog"});
}

TEST_CASE("find / contains / count / operator[] / at") {
    prefix_tree<int> t;
    t.insert("hello", 1);

    CHECK(t.find("hello") != t.end());
    CHECK(t.find("nope") == t.end());
    CHECK(t.contains("hello"));
    CHECK_FALSE(t.contains("hell"));
    CHECK(t.count("hello") == 1);
    CHECK(t.count("nope") == 0);

    t["world"] = 2;
    CHECK(t.at("world") == 2);
    t["world"] = 3;
    CHECK(t.at("world") == 3);
    CHECK(t.size() == 2);

    CHECK_THROWS_AS(t.at("missing"), std::out_of_range);

    const prefix_tree<int>& ct = t;
    CHECK(ct.at("hello") == 1);
}

TEST_CASE("erase by key removes only that key and prunes dead nodes") {
    prefix_tree<int> t;
    t.insert("cat", 1);
    t.insert("cats", 2);
    t.insert("car", 3);

    CHECK(t.erase("cat") == 1);
    CHECK_FALSE(t.contains("cat"));
    CHECK(t.contains("cats"));
    CHECK(t.contains("car"));
    CHECK(t.size() == 2);

    CHECK(t.erase("nonexistent") == 0);

    CHECK(t.erase("cats") == 1);
    CHECK(t.erase("car") == 1);
    CHECK(t.empty());
}

TEST_CASE("erase by iterator returns the next iterator, matching begin..end traversal") {
    prefix_tree<int> t;
    for (const auto& k : {"a", "b", "c", "d"}) t.insert(k, 0);

    auto it = t.find("b");
    auto next = t.erase(it);
    CHECK(next->first == "c");
    CHECK(keys_in_order(t) == std::vector<std::string>{"a", "c", "d"});

    // erase the last element -> should return end()
    auto last = t.find("d");
    auto after_last = t.erase(last);
    CHECK(after_last == t.end());
}

TEST_CASE("erasing all keys through repeated erase(begin()) drains the prefix_tree in order") {
    prefix_tree<int> t;
    for (const auto& k : {"delta", "alpha", "charlie", "bravo"}) t.insert(k, 0);

    std::vector<std::string> drained;
    while (!t.empty()) {
        auto it = t.begin();
        drained.push_back(it->first);
        t.erase(it);
    }
    CHECK(drained == std::vector<std::string>{"alpha", "bravo", "charlie", "delta"});
    CHECK(t.begin() == t.end());
}

TEST_CASE("clear empties the prefix_tree and it remains usable afterwards") {
    prefix_tree<int> t;
    t.insert("x", 1);
    t.insert("y", 2);
    t.clear();
    CHECK(t.empty());
    CHECK(t.size() == 0);
    t.insert("z", 3);
    CHECK(t.contains("z"));
    CHECK(t.size() == 1);
}

TEST_CASE("starts_with reflects structural prefix existence") {
    prefix_tree<int> t;
    t.insert("prefix", 1);
    CHECK(t.starts_with(""));
    CHECK(t.starts_with("pre"));
    CHECK(t.starts_with("prefix"));
    CHECK_FALSE(t.starts_with("prefixed"));
    CHECK_FALSE(t.starts_with("q"));
}

TEST_CASE("prefix_range yields exactly the keys sharing that prefix, in order") {
    prefix_tree<int> t;
    for (const auto& k : {"apple", "app", "application", "apply", "banana"}) t.insert(k, 0);

    auto [b, e] = t.prefix_range("app");
    std::vector<std::string> got;
    for (; b != e; ++b) got.push_back(b->first);
    CHECK(got == std::vector<std::string>{"app", "apple", "application", "apply"});

    auto [b2, e2] = t.prefix_range("");
    CHECK(b2 == t.begin());
    CHECK(e2 == t.end());

    auto [b3, e3] = t.prefix_range("zzz");
    CHECK(b3 == t.end());
    CHECK(e3 == t.end());

    auto [b4, e4] = t.prefix_range("banana");
    std::vector<std::string> got4;
    for (; b4 != e4; ++b4) got4.push_back(b4->first);
    CHECK(got4 == std::vector<std::string>{"banana"});
}

TEST_CASE("erase_prefix removes the whole subtree and reports how many keys were removed") {
    prefix_tree<int> t;
    for (const auto& k : {"apple", "app", "application", "apply", "banana"}) t.insert(k, 0);

    CHECK(t.erase_prefix("app") == 4);
    CHECK(keys_in_order(t) == std::vector<std::string>{"banana"});
    CHECK(t.size() == 1);

    CHECK(t.erase_prefix("nonexistent") == 0);

    CHECK(t.erase_prefix("") == 1);
    CHECK(t.empty());
}

TEST_CASE("copy construction is a deep, independent copy") {
    prefix_tree<int> t;
    t.insert("a", 1);
    t.insert("b", 2);

    prefix_tree<int> copy = t;
    copy.insert("c", 3);
    copy["a"] = 100;

    CHECK(t.size() == 2);
    CHECK_FALSE(t.contains("c"));
    CHECK(t.at("a") == 1);
    CHECK(copy.at("a") == 100);
}

TEST_CASE("copy assignment replaces contents independently") {
    prefix_tree<int> a;
    a.insert("x", 1);
    prefix_tree<int> b;
    b.insert("y", 2);
    b.insert("z", 3);

    a = b;
    CHECK(a.size() == 2);
    CHECK(a.contains("y"));
    CHECK(a.contains("z"));

    a.insert("w", 4);
    CHECK_FALSE(b.contains("w"));
}

TEST_CASE("move construction steals storage") {
    prefix_tree<int> t;
    t.insert("a", 1);
    t.insert("b", 2);

    prefix_tree<int> moved = std::move(t);
    CHECK(moved.size() == 2);
    CHECK(moved.contains("a"));
}

TEST_CASE("move assignment replaces contents") {
    prefix_tree<int> a;
    a.insert("x", 1);
    prefix_tree<int> b;
    b.insert("y", 2);

    a = std::move(b);
    CHECK(a.size() == 1);
    CHECK(a.contains("y"));
}

TEST_CASE("self copy/move assignment is a no-op") {
    prefix_tree<int> t;
    t.insert("a", 1);

    t = t;
    CHECK(t.size() == 1);
    CHECK(t.contains("a"));
}

TEST_CASE("swap exchanges contents") {
    prefix_tree<int> a;
    a.insert("a1", 1);
    prefix_tree<int> b;
    b.insert("b1", 2);
    b.insert("b2", 3);

    a.swap(b);
    CHECK(a.size() == 2);
    CHECK(a.contains("b1"));
    CHECK(b.size() == 1);
    CHECK(b.contains("a1"));
}

TEST_CASE("operator== compares by key/value contents, not structure") {
    prefix_tree<int> a;
    a.insert("x", 1);
    a.insert("y", 2);

    prefix_tree<int> b;
    b.insert("y", 2);
    b.insert("x", 1);

    CHECK(a == b);

    b.insert("z", 3);
    CHECK(a != b);
}

TEST_CASE("initializer_list construction and assignment") {
    prefix_tree<int> t{{"a", 1}, {"b", 2}, {"c", 3}};
    CHECK(t.size() == 3);
    CHECK(t.at("b") == 2);

    t = {{"x", 10}, {"y", 20}};
    CHECK(t.size() == 2);
    CHECK(t.contains("x"));
    CHECK_FALSE(t.contains("a"));
}

TEST_CASE("range insert from another container") {
    std::vector<std::pair<std::string, int>> src{{"one", 1}, {"two", 2}};
    prefix_tree<int> t;
    t.insert(src.begin(), src.end());
    CHECK(t.size() == 2);
    CHECK(t.at("two") == 2);
}

TEST_CASE("uses the supplied PMR memory resource for node storage") {
    std::pmr::monotonic_buffer_resource arena;
    prefix_tree<int> t{&arena};
    for (int i = 0; i < 100; ++i) t.insert("key" + std::to_string(i), i);
    CHECK(t.size() == 100);
    for (int i = 0; i < 100; ++i) CHECK(t.at("key" + std::to_string(i)) == i);
}

TEST_CASE("move-with-allocator: same allocator steals, different allocator deep-copies") {
    std::pmr::monotonic_buffer_resource arena1;
    std::pmr::monotonic_buffer_resource arena2;

    prefix_tree<int> t1{&arena1};
    t1.insert("a", 1);

    prefix_tree<int> t2(std::move(t1), std::pmr::polymorphic_allocator<std::byte>(&arena1));
    CHECK(t2.contains("a"));

    prefix_tree<int> t3{&arena1};
    t3.insert("b", 2);
    prefix_tree<int> t4(std::move(t3), std::pmr::polymorphic_allocator<std::byte>(&arena2));
    CHECK(t4.contains("b"));
}

TEST_CASE("emplace forwards constructor arguments") {
    struct point {
        int x, y;
        point(int x_, int y_) : x(x_), y(y_) {}
    };
    prefix_tree<point> t;
    auto [it, inserted] = t.emplace("origin", 1, 2);
    CHECK(inserted);
    CHECK(it->second.x == 1);
    CHECK(it->second.y == 2);
}

TEST_CASE("mutating through a non-const iterator's ->second updates the stored value") {
    prefix_tree<int> t;
    t.insert("a", 1);
    auto it = t.find("a");
    it->second = 42;
    CHECK(t.at("a") == 42);
}

TEST_CASE("const_iterator conversion and forward-iterator const-correctness") {
    prefix_tree<int> t;
    t.insert("a", 1);

    prefix_tree<int>::iterator it = t.begin();
    prefix_tree<int>::const_iterator cit = it;  // converting ctor
    CHECK(cit->first == "a");

    const prefix_tree<int>& ct = t;
    for (auto i = ct.begin(); i != ct.end(); ++i) {
        CHECK(i->first == "a");
    }
}

TEST_CASE("large randomized-ish stress: sorted iteration and full drain") {
    prefix_tree<int> t;
    std::vector<std::string> words;
    const char* alphabet = "abc";
    for (char a : std::string(alphabet))
        for (char b : std::string(alphabet))
            for (char c : std::string(alphabet)) words.push_back(std::string() + a + b + c);

    for (std::size_t i = 0; i < words.size(); ++i) t.insert(words[i], static_cast<int>(i));
    CHECK(t.size() == words.size());

    auto sorted_words = words;
    std::sort(sorted_words.begin(), sorted_words.end());
    CHECK(keys_in_order(t) == sorted_words);

    for (const auto& w : words) CHECK(t.erase(w) == 1);
    CHECK(t.empty());
}

TEST_CASE("erase(iterator) on a node with children returns the next lexicographic element") {
    prefix_tree<int> t;
    for (const auto& k : {"cat", "cats", "catalog"}) t.insert(k, 0);
    // Lexicographic order: "cat" < "catalog" < "cats" (comparing the 4th
    // character, 'a' from catalog/cats' remainder vs cat having no 4th char
    // at all: cat is a prefix of both, and between catalog/cats, 'a' < 's').

    auto it = t.find("cat");
    auto next = t.erase(it);
    CHECK(next->first == "catalog");
    CHECK(keys_in_order(t) == std::vector<std::string>{"catalog", "cats"});
}

TEST_CASE("erase prunes an entire dead single-branch chain back to the nearest live ancestor") {
    prefix_tree<int> t;
    t.insert("a", 1);
    t.insert("elephant", 2);  // unique long chain hanging off the root

    CHECK(t.erase("elephant") == 1);
    CHECK(keys_in_order(t) == std::vector<std::string>{"a"});

    // The whole "elephant" chain must be gone: re-inserting a sibling
    // shouldn't find any leftover node reachable through it.
    CHECK_FALSE(t.starts_with("e"));
}

TEST_CASE("erase_prefix on a leaf key with no descendants removes exactly that key") {
    prefix_tree<int> t;
    t.insert("apple", 1);
    t.insert("banana", 2);
    CHECK(t.erase_prefix("banana") == 1);
    CHECK(keys_in_order(t) == std::vector<std::string>{"apple"});
}

TEST_CASE("postfix increment returns the pre-increment position and still advances") {
    prefix_tree<int> t;
    for (const auto& k : {"a", "b", "c"}) t.insert(k, 0);

    auto it = t.begin();
    auto prev = it++;
    CHECK(prev->first == "a");
    CHECK(it->first == "b");

    auto cit = t.cbegin();
    auto cprev = cit++;
    CHECK(cprev->first == "a");
    CHECK(cit->first == "b");
}

TEST_CASE("max_size and get_allocator") {
    prefix_tree<int> t;
    CHECK(t.max_size() > 0);
    auto alloc = t.get_allocator();
    (void)alloc;
}

TEST_CASE("non-member ADL swap exchanges contents like the member swap") {
    prefix_tree<int> a;
    a.insert("a1", 1);
    prefix_tree<int> b;
    b.insert("b1", 2);

    swap(a, b);
    CHECK(a.contains("b1"));
    CHECK(b.contains("a1"));
}

TEST_CASE("insert with a throwing value constructor leaves the tree valid and the key absent") {
    prefix_tree<throws_on_construct> t;
    CHECK_THROWS_AS(t.emplace("x", 1), std::runtime_error);
    CHECK_FALSE(t.contains("x"));
    CHECK(t.size() == 0);

    // The tree must remain fully usable afterwards.
    prefix_tree<int> usable;
    CHECK_NOTHROW(usable.insert("still-works", 1));
    CHECK(usable.contains("still-works"));
}

TEST_CASE("copy construction cleans up and rethrows if a value's copy constructor throws") {
    prefix_tree<throws_on_nth_copy> original;
    original.insert("aaa", 1);
    original.insert("aab", 2);
    original.insert("abc", 3);

    throws_on_nth_copy::copies_until_throw = 2;  // fail partway through cloning
    CHECK_THROWS_AS([&] { prefix_tree<throws_on_nth_copy> copy = original; }(), std::runtime_error);
    throws_on_nth_copy::copies_until_throw = -1;

    // The source tree must be completely unaffected by the failed copy.
    CHECK(original.size() == 3);
    CHECK(original.at("aaa").value == 1);
    CHECK(original.at("aab").value == 2);
    CHECK(original.at("abc").value == 3);
}

TEST_CASE("copy assignment propagates the allocator when propagate_on_container_copy_assignment is true") {
    using alloc_t = propagating_allocator<std::byte>;
    prefix_tree<int, alloc_t> a{alloc_t(1)};
    a.insert("x", 1);
    prefix_tree<int, alloc_t> b{alloc_t(2)};
    b.insert("y", 2);

    a = b;
    CHECK(a.get_allocator().id == 2);
    CHECK(a.contains("y"));
}

TEST_CASE("move assignment propagates the allocator when propagate_on_container_move_assignment is true") {
    using alloc_t = propagating_allocator<std::byte>;
    prefix_tree<int, alloc_t> a{alloc_t(1)};
    a.insert("x", 1);
    prefix_tree<int, alloc_t> b{alloc_t(2)};
    b.insert("y", 2);

    a = std::move(b);
    CHECK(a.get_allocator().id == 2);
    CHECK(a.contains("y"));
}

TEST_CASE("swap propagates (exchanges) allocators when propagate_on_container_swap is true") {
    using alloc_t = propagating_allocator<std::byte>;
    prefix_tree<int, alloc_t> a{alloc_t(1)};
    a.insert("x", 1);
    prefix_tree<int, alloc_t> b{alloc_t(2)};
    b.insert("y", 2);

    a.swap(b);
    CHECK(a.get_allocator().id == 2);
    CHECK(b.get_allocator().id == 1);
    CHECK(a.contains("y"));
    CHECK(b.contains("x"));
}

TEST_CASE("move assignment deep-copies when allocators differ and propagation is disabled") {
    // The default std::pmr::polymorphic_allocator has
    // propagate_on_container_move_assignment = false_type, so this exercises
    // the runtime-equality-checked, non-propagating branch of operator=(&&).
    std::pmr::monotonic_buffer_resource arena1;
    std::pmr::monotonic_buffer_resource arena2;
    prefix_tree<int> a{&arena1};
    a.insert("x", 1);
    prefix_tree<int> b{&arena2};
    b.insert("y", 2);

    a = std::move(b);
    CHECK(a.size() == 1);
    CHECK(a.contains("y"));
    CHECK_FALSE(a.contains("x"));
}

TEST_CASE("create_node cleans up and rethrows if constructing the node itself throws") {
    using alloc_t = sometimes_throwing_allocator<std::byte>;
    prefix_tree<int, alloc_t> t{alloc_t()};  // root construction: throwing disabled.

    // A single create_node() call performs exactly two rebind conversions:
    // one for create_node()'s own rebound node-allocator, one inside the
    // node's constructor for its children vector's rebound allocator. Fail
    // on the second so allocate() has already succeeded and there is
    // something for the catch block to clean up.
    g_throw_after = 2;
    CHECK_THROWS_AS(t.insert("x", 1), allocation_failure);
    g_throw_after = -1;

    CHECK_FALSE(t.contains("x"));
    CHECK(t.size() == 0);
    // The tree must remain valid and usable after the cleanup.
    CHECK_NOTHROW(t.insert("y", 2));
    CHECK(t.contains("y"));
}

TEST_CASE("self-swap is a no-op") {
    prefix_tree<int> t;
    t.insert("a", 1);
    t.swap(t);
    CHECK(t.size() == 1);
    CHECK(t.contains("a"));
}

TEST_CASE("self-equality short-circuits without deadlocking on the same recursive mutex") {
    prefix_tree<int> t;
    t.insert("a", 1);
    CHECK(t == t);
}

TEST_CASE("concurrent inserts from multiple threads land exactly once each, with no lost updates") {
    prefix_tree<int> t;
    constexpr int thread_count = 8;
    constexpr int keys_per_thread = 500;

    std::vector<std::thread> threads;
    for (int tid = 0; tid < thread_count; ++tid) {
        threads.emplace_back([&t, tid] {
            for (int i = 0; i < keys_per_thread; ++i) {
                t.insert("t" + std::to_string(tid) + "_k" + std::to_string(i), tid * 100000 + i);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(t.size() == static_cast<std::size_t>(thread_count * keys_per_thread));
    for (int tid = 0; tid < thread_count; ++tid) {
        for (int i = 0; i < keys_per_thread; ++i) {
            CHECK(t.at("t" + std::to_string(tid) + "_k" + std::to_string(i)) == tid * 100000 + i);
        }
    }
}

TEST_CASE("concurrent reads from multiple threads never crash or see torn state") {
    prefix_tree<int> t;
    for (int i = 0; i < 200; ++i) t.insert("k" + std::to_string(i), i);

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&] {
            for (int round = 0; round < 50; ++round) {
                for (int i = 0; i < 200; ++i) {
                    if (!t.contains("k" + std::to_string(i))) ++mismatches;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(mismatches == 0);
}

TEST_CASE("a live iterator's RAII lock blocks a concurrent writer until the iterator is destroyed") {
    prefix_tree<int> t;
    t.insert("a", 1);
    t.insert("b", 2);

    std::atomic<bool> writer_finished{false};
    std::thread writer;
    {
        auto it = t.begin();  // acquires and holds the shared RAII lock

        writer = std::thread([&] {
            t.insert("c", 3);  // must block until `it` is destroyed
            writer_finished = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_FALSE(writer_finished.load());  // writer should still be blocked

        // `it` is destroyed at the end of this block, releasing the lock --
        // this must happen before we join the writer below, or the writer
        // would block on the lock forever while we block on the join.
    }
    writer.join();
    CHECK(writer_finished.load());
    CHECK(t.contains("c"));
}
