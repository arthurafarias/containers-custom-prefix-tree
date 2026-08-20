/**
 * @file prefix_tree.hpp
 * @brief Header-only, PMR-aware prefix_tree (prefix tree) container.
 *
 * Defines containers::custom::prefix_tree<T, Allocator>, an STL-inspired associative
 * container mapping `std::string` keys to `T` values via a character-indexed
 * prefix tree. Node layout and iterator machinery live in
 * containers::custom::detail (see containers/custom/detail/) and are not
 * part of the public API: they may change between releases without notice.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <containers/custom/detail/mutex_guarded.hpp>
#include <containers/custom/detail/trie_frame.hpp>
#include <containers/custom/detail/trie_iterator.hpp>
#include <containers/custom/detail/trie_node.hpp>

/**
 * @namespace containers::custom
 * @brief Custom (non-standard-library), STL-inspired containers.
 */
namespace containers::custom {

/**
 * @brief An ordered, associative, STL-inspired container mapping `std::string`
 *        keys to `T` values, structured as a character-indexed prefix tree.
 *
 * Iteration visits keys in lexicographic order. Beyond the usual
 * associative-container operations, `prefix_tree` exposes prefix-native queries --
 * starts_with(), prefix_range(), erase_prefix() -- that a hash- or tree-based
 * map cannot offer efficiently, since it stores keys along shared root-to-node
 * paths rather than as opaque, independently-hashed/ordered blobs.
 *
 * `prefix_tree` is *not* a strict implementation of the Container/AssociativeContainer
 * named requirements: because a key is not stored contiguously anywhere (it is
 * spelled out by the path from the root), dereferencing an iterator yields a
 * small proxy reference type (detail::trie_kv_ref) rather than a real
 * `std::pair<const Key, T>&`. Everything else -- insert/erase/find semantics,
 * allocator-awareness, iterator invalidation rules similar to node-based
 * containers -- follows STL conventions.
 *
 * Thread safety: `prefix_tree` inherits from detail::mutex_guarded, which
 * gives it its own recursive mutex. Every public member function locks it
 * for the duration of the call, so individual calls are safe to make
 * concurrently from multiple threads. Additionally, any iterator returned by
 * an accessor (`begin()`, `find()`, `prefix_range()`, ...) carries a
 * reference-counted RAII lock that keeps the tree from being mutated by
 * another thread for as long as that iterator -- or any copy taken from it
 * -- remains alive, which is what makes it safe to iterate a shared
 * prefix_tree without separately synchronizing the loop. Because the mutex
 * is recursive, code already holding it (whether via a live iterator or an
 * explicit `std::scoped_lock lock(my_prefix_tree);`) can still call further
 * accessors from the same thread without deadlocking.
 *
 * @tparam T         Mapped value type associated with each key.
 * @tparam Allocator An allocator satisfying the standard Allocator named
 *                     requirements, used (after rebinding) to allocate nodes
 *                     and their children storage. Defaults to
 *                     `std::pmr::polymorphic_allocator<std::byte>`, so node
 *                     storage is driven by whatever `std::pmr::memory_resource`
 *                     is supplied at construction.
 */
template <class T, class Allocator = std::pmr::polymorphic_allocator<std::byte>>
class prefix_tree : public detail::mutex_guarded {
public:
    /// The key type: sequences of `char`.
    using key_type = std::string;
    /// The value type associated with each key.
    using mapped_type = T;
    /// Key/value pair type accepted by `insert`/`emplace`-adjacent overloads and initializer lists.
    using value_type = std::pair<key_type, mapped_type>;
    /// The allocator type used for node storage.
    using allocator_type = Allocator;
    /// Unsigned type used for sizes and counts.
    using size_type = std::size_t;
    /// Signed type used for iterator distances.
    using difference_type = std::ptrdiff_t;

private:
    /// This trie's node type.
    using node = detail::trie_node<T, Allocator>;
    /// `Allocator` rebound to `node`, used for all node allocation/deallocation.
    using node_allocator_type =
        typename std::allocator_traits<Allocator>::template rebind_alloc<node>;
    /// `std::allocator_traits` for `node_allocator_type`.
    using node_alloc_traits = std::allocator_traits<node_allocator_type>;

public:
    /// Mutable forward iterator; see the `prefix_tree` class docs for how dereferencing differs from `std::map`.
    using iterator = detail::trie_iterator<T, Allocator, false>;
    /// Read-only forward iterator counterpart of `iterator`.
    using const_iterator = detail::trie_iterator<T, Allocator, true>;
    /// The proxy type yielded by `*iterator{}`.
    using reference = typename iterator::reference;
    /// The proxy type yielded by `*const_iterator{}`.
    using const_reference = typename const_iterator::reference;

    /// Constructs an empty prefix_tree using a default-constructed `Allocator`.
    prefix_tree() : prefix_tree(Allocator()) {}

    /**
     * @brief Constructs an empty prefix_tree using the given allocator.
     * @param alloc Allocator (or, for the default `Allocator`, anything
     *               convertible to `std::pmr::polymorphic_allocator<std::byte>`,
     *               such as a `std::pmr::memory_resource*`) to use for all
     *               node allocations.
     */
    explicit prefix_tree(const Allocator& alloc) : allocator_(alloc), root_(create_node()) {}

    /**
     * @brief Copy constructor. Deep-copies `other`'s tree using `other`'s allocator.
     * @param other The prefix_tree to copy.
     */
    prefix_tree(const prefix_tree& other) : prefix_tree(other, other.allocator_) {}

    /**
     * @brief Allocator-extended copy constructor.
     * @param other The prefix_tree to copy.
     * @param alloc Allocator to use for the new prefix_tree's nodes (may differ from `other`'s).
     */
    prefix_tree(const prefix_tree& other, const Allocator& alloc)
        : allocator_(alloc), root_(nullptr), size_(0) {
        std::scoped_lock other_lock(other);
        root_ = clone_subtree(other.root_);
        size_ = other.size_;
    }

    /**
     * @brief Move constructor. Takes ownership of `other`'s storage in constant time.
     * @param other The prefix_tree to move from; left empty and safe to destroy or reassign.
     */
    prefix_tree(prefix_tree&& other) noexcept
        : allocator_(std::move(other.allocator_)), root_(other.root_), size_(other.size_) {
        other.root_ = nullptr;
        other.size_ = 0;
    }

    /**
     * @brief Allocator-extended move constructor.
     *
     * If `alloc` compares equal to `other`'s allocator, storage is stolen in
     * constant time; otherwise the tree is deep-copied using `alloc`, since
     * nodes allocated from a different resource cannot simply be adopted.
     * @param other The prefix_tree to move (or copy) from.
     * @param alloc Allocator to use for the new prefix_tree's nodes.
     */
    prefix_tree(prefix_tree&& other, const Allocator& alloc) : allocator_(alloc), root_(nullptr), size_(0) {
        if (alloc == other.allocator_) {
            root_ = other.root_;
            size_ = other.size_;
            other.root_ = nullptr;
            other.size_ = 0;
        } else {
            root_ = clone_subtree(other.root_);
            size_ = other.size_;
        }
    }

    /**
     * @brief Constructs a prefix_tree from a brace-enclosed list of key/value pairs.
     * @param init  Key/value pairs to insert; later duplicates of an earlier key are ignored.
     * @param alloc Allocator to use for all node allocations.
     */
    prefix_tree(std::initializer_list<value_type> init, const Allocator& alloc = Allocator())
        : prefix_tree(alloc) {
        insert(init.begin(), init.end());
    }

    /// Destroys every node in the tree.
    ~prefix_tree() {
        if (root_) destroy_subtree(root_);
    }

    /**
     * @brief Copy assignment. Replaces the contents with a deep copy of `other`.
     * @param other The prefix_tree to copy.
     * @return `*this`.
     */
    prefix_tree& operator=(const prefix_tree& other) {
        if (this == &other) return *this;
        constexpr bool propagate =
            std::allocator_traits<Allocator>::propagate_on_container_copy_assignment::value;
        // `tmp`'s constructor locks `other` for its own duration (see the
        // allocator-extended copy constructor); `*this` is locked separately
        // below, so the two mutexes are never held at once here.
        prefix_tree tmp(other, propagate ? other.allocator_ : allocator_);
        std::scoped_lock lock(*this);
        if constexpr (propagate) allocator_ = other.allocator_;
        swap_contents(tmp);
        return *this;
    }

    /**
     * @brief Move assignment. Replaces the contents by taking ownership of `other`'s storage.
     * @param other The prefix_tree to move from; left empty.
     * @return `*this`.
     */
    prefix_tree& operator=(prefix_tree&& other) noexcept(
        std::allocator_traits<Allocator>::propagate_on_container_move_assignment::value ||
        std::allocator_traits<Allocator>::is_always_equal::value) {
        if (this == &other) return *this;
        std::scoped_lock lock(*this);
        if (root_) destroy_subtree(root_);
        if constexpr (std::allocator_traits<
                          Allocator>::propagate_on_container_move_assignment::value) {
            allocator_ = std::move(other.allocator_);
            root_ = other.root_;
            size_ = other.size_;
            other.root_ = nullptr;
            other.size_ = 0;
        } else {
            if (allocator_ == other.allocator_) {
                root_ = other.root_;
                size_ = other.size_;
                other.root_ = nullptr;
                other.size_ = 0;
            } else {
                root_ = clone_subtree(other.root_);
                size_ = other.size_;
            }
        }
        return *this;
    }

    /**
     * @brief Replaces the contents with a brace-enclosed list of key/value pairs.
     * @param init Key/value pairs to insert; later duplicates of an earlier key are ignored.
     * @return `*this`.
     */
    prefix_tree& operator=(std::initializer_list<value_type> init) {
        std::scoped_lock lock(*this);
        clear();
        insert(init.begin(), init.end());
        return *this;
    }

    /// @return A copy of the allocator used by this prefix_tree.
    allocator_type get_allocator() const noexcept {
        std::scoped_lock lock(*this);
        return allocator_;
    }

    /**
     * @brief Returns an iterator to the first key in lexicographic order.
     *
     * The returned iterator holds a reference-counted lock on this
     * prefix_tree (see the class docs' Thread safety section) that is only
     * released once it, and every copy taken from it, is destroyed.
     * @return An iterator to the first key, or `end()` if empty.
     */
    iterator begin() {
        auto guard = acquire_shared_lock();
        iterator it;
        it.stack_.push_back({root_, 0});
        it.seek_value_forward();
        it.lock_ = std::move(guard);
        return it;
    }
    /// @return The past-the-end iterator. Holds no lock: it references no node.
    iterator end() noexcept { return iterator(); }
    /// @copydoc begin()
    const_iterator begin() const { return cbegin(); }
    /// @return The past-the-end const_iterator. Holds no lock: it references no node.
    const_iterator end() const noexcept { return cend(); }
    /// @copydoc begin()
    const_iterator cbegin() const {
        auto guard = acquire_shared_lock();
        const_iterator it;
        it.stack_.push_back({root_, 0});
        it.seek_value_forward();
        it.lock_ = std::move(guard);
        return it;
    }
    /// @return The past-the-end const_iterator. Holds no lock: it references no node.
    const_iterator cend() const noexcept { return const_iterator(); }

    /// @return True iff the prefix_tree contains no keys.
    [[nodiscard]] bool empty() const noexcept {
        std::scoped_lock lock(*this);
        return size_ == 0;
    }
    /// @return The number of keys currently stored.
    size_type size() const noexcept {
        std::scoped_lock lock(*this);
        return size_;
    }
    /// @return An upper bound on the number of nodes this prefix_tree could allocate.
    size_type max_size() const noexcept {
        std::scoped_lock lock(*this);
        return std::allocator_traits<node_allocator_type>::max_size(node_allocator_type(allocator_));
    }

    /// Removes every key, leaving the prefix_tree empty. Invalidates all iterators.
    void clear() noexcept {
        std::scoped_lock lock(*this);
        for (node* c : root_->children) destroy_subtree(c);
        root_->children.clear();
        root_->is_end = false;
        root_->value.reset();
        size_ = 0;
    }

    /**
     * @brief Inserts `key` if absent, constructing its value in place from `args`.
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param key  Key to insert.
     * @param args Arguments forwarded to `T`'s constructor if `key` is not already present.
     * @return A pair of an iterator to `key`'s element and whether insertion took place.
     */
    template <class... Args>
    std::pair<iterator, bool> emplace(const key_type& key, Args&&... args) {
        auto guard = acquire_shared_lock();
        auto result = emplace_impl(key, std::forward<Args>(args)...);
        result.first.lock_ = std::move(guard);
        return result;
    }

    /**
     * @brief Inserts a copy of `value` under `key` if `key` is not already present.
     * @param key   Key to insert.
     * @param value Value to copy in if `key` is not already present.
     * @return A pair of an iterator to `key`'s element and whether insertion took place.
     */
    std::pair<iterator, bool> insert(const key_type& key, const T& value) {
        auto guard = acquire_shared_lock();
        auto result = emplace_impl(key, value);
        result.first.lock_ = std::move(guard);
        return result;
    }
    /**
     * @brief Inserts `value` under `key` if `key` is not already present.
     * @param key   Key to insert.
     * @param value Value to move in if `key` is not already present.
     * @return A pair of an iterator to `key`'s element and whether insertion took place.
     */
    std::pair<iterator, bool> insert(const key_type& key, T&& value) {
        auto guard = acquire_shared_lock();
        auto result = emplace_impl(key, std::move(value));
        result.first.lock_ = std::move(guard);
        return result;
    }
    /**
     * @brief Inserts a copy of a key/value pair if its key is not already present.
     * @param kv Key/value pair to insert.
     * @return A pair of an iterator to the element and whether insertion took place.
     */
    std::pair<iterator, bool> insert(const value_type& kv) {
        auto guard = acquire_shared_lock();
        auto result = emplace_impl(kv.first, kv.second);
        result.first.lock_ = std::move(guard);
        return result;
    }
    /**
     * @brief Inserts a key/value pair, moving the value, if its key is not already present.
     * @param kv Key/value pair to insert.
     * @return A pair of an iterator to the element and whether insertion took place.
     */
    std::pair<iterator, bool> insert(value_type&& kv) {
        auto guard = acquire_shared_lock();
        auto result = emplace_impl(kv.first, std::move(kv.second));
        result.first.lock_ = std::move(guard);
        return result;
    }

    /**
     * @brief Inserts every key/value pair in `[first, last)` whose key is not already present.
     *
     * Locks once for the whole range rather than once per element, so the
     * insertion is atomic with respect to other threads.
     * @tparam InputIt Input iterator type dereferencing to something convertible to `value_type`.
     * @param first Beginning of the range to insert.
     * @param last  End of the range to insert.
     */
    template <class InputIt>
    void insert(InputIt first, InputIt last) {
        std::scoped_lock lock(*this);
        for (; first != last; ++first) insert(*first);
    }
    /**
     * @brief Inserts every key/value pair in a brace-enclosed list whose key is not already present.
     * @param init Key/value pairs to insert.
     */
    void insert(std::initializer_list<value_type> init) {
        std::scoped_lock lock(*this);
        insert(init.begin(), init.end());
    }

    /**
     * @brief Erases the element at `pos`, pruning any nodes left with neither a value nor children.
     * @param pos Iterator to the element to erase; must be dereferenceable (not `end()`).
     * @return An iterator to the element following the erased one, or `end()`.
     */
    iterator erase(const_iterator pos) {
        auto guard = acquire_shared_lock();
        iterator next = to_iterator(pos);
        next.advance();
        node* target = pos.current();
        target->is_end = false;
        target->value.reset();
        --size_;
        prune_dead_path(pos.stack_);
        next.lock_ = std::move(guard);
        return next;
    }

    /**
     * @brief Erases the element with the given key, if present.
     * @param key Key to erase.
     * @return 1 if `key` was present and erased, 0 otherwise.
     */
    size_type erase(const key_type& key) {
        std::scoped_lock lock(*this);
        const_iterator it = find_const(key);
        if (it == cend()) return 0;
        erase(it);
        return 1;
    }

    /**
     * @brief Exchanges the contents of `*this` and `other` in constant time.
     * @param other The prefix_tree to swap with.
     */
    void swap(prefix_tree& other) noexcept(std::allocator_traits<Allocator>::propagate_on_container_swap::value ||
                                     std::allocator_traits<Allocator>::is_always_equal::value) {
        if (this == &other) return;
        std::scoped_lock lock(*this, other);
        using std::swap;
        if constexpr (std::allocator_traits<Allocator>::propagate_on_container_swap::value) {
            swap(allocator_, other.allocator_);
        }
        swap(root_, other.root_);
        swap(size_, other.size_);
    }

    /**
     * @brief Accesses the value for `key`, throwing if absent.
     * @param key Key to look up.
     * @return A reference to the value associated with `key`.
     * @throws std::out_of_range if `key` is not present.
     */
    T& at(const key_type& key) {
        std::scoped_lock lock(*this);
        node* n = find_node(key);
        if (!n || !n->is_end) throw std::out_of_range("containers::custom::prefix_tree::at: key not found");
        return n->value.value();
    }
    /// @copydoc at(const key_type&)
    const T& at(const key_type& key) const {
        std::scoped_lock lock(*this);
        node* n = find_node(key);
        if (!n || !n->is_end) throw std::out_of_range("containers::custom::prefix_tree::at: key not found");
        return n->value.value();
    }

    /**
     * @brief Accesses the value for `key`, default-constructing and inserting one if absent.
     * @param key Key to look up or insert.
     * @return A reference to `key`'s (possibly newly default-constructed) value.
     */
    T& operator[](const key_type& key) {
        std::scoped_lock lock(*this);
        auto [it, inserted] = emplace_impl(key);
        return it.current()->value.value();
    }

    /**
     * @brief Counts how many elements match `key` (0 or 1, since keys are unique).
     * @param key Key to look up.
     * @return 1 if `key` is present, 0 otherwise.
     */
    size_type count(const key_type& key) const {
        std::scoped_lock lock(*this);
        return contains(key) ? 1 : 0;
    }

    /**
     * @brief Looks up `key`.
     * @param key Key to look up.
     * @return An iterator to `key`'s element, or `end()` if not present.
     */
    iterator find(const key_type& key) {
        auto guard = acquire_shared_lock();
        node* n = find_node(key);
        if (!n || !n->is_end) return end();
        iterator it = build_iterator_for_path<false>(key);
        it.lock_ = std::move(guard);
        return it;
    }
    /// @copydoc find(const key_type&)
    const_iterator find(const key_type& key) const {
        auto guard = acquire_shared_lock();
        node* n = find_node(key);
        if (!n || !n->is_end) return cend();
        const_iterator it = build_iterator_for_path<true>(key);
        it.lock_ = std::move(guard);
        return it;
    }

    /**
     * @brief Checks whether `key` is present.
     * @param key Key to look up.
     * @return True iff `key` is present.
     */
    bool contains(const key_type& key) const {
        std::scoped_lock lock(*this);
        node* n = find_node(key);
        return n && n->is_end;
    }

    /**
     * @brief Checks whether any stored key begins with `prefix`.
     *
     * O(prefix.size()): unlike a hash- or tree-based map, this does not need
     * to scan the container's contents.
     * @param prefix Prefix to test for.
     * @return True iff at least one stored key begins with `prefix` (a key
     *         equal to `prefix` counts).
     */
    bool starts_with(std::string_view prefix) const {
        std::scoped_lock lock(*this);
        return find_prefix_node(prefix) != nullptr;
    }

    /**
     * @brief Returns the range of elements whose keys begin with `prefix`, in lexicographic order.
     *
     * Both returned iterators share one lock (see the class docs' Thread
     * safety section), so the tree cannot be mutated by another thread for
     * as long as either endpoint of the range remains alive.
     * @param prefix Prefix to query.
     * @return A `[first, second)` iterator pair; both are `end()` if no key has this prefix.
     */
    std::pair<iterator, iterator> prefix_range(std::string_view prefix) {
        auto guard = acquire_shared_lock();
        auto result = prefix_range_impl<false>(prefix);
        result.first.lock_ = guard;
        result.second.lock_ = std::move(guard);
        return result;
    }
    /// @copydoc prefix_range(std::string_view)
    std::pair<const_iterator, const_iterator> prefix_range(std::string_view prefix) const {
        auto guard = acquire_shared_lock();
        auto result = prefix_range_impl<true>(prefix);
        result.first.lock_ = guard;
        result.second.lock_ = std::move(guard);
        return result;
    }

    /**
     * @brief Erases every key beginning with `prefix` in a single structural operation.
     *
     * Detaches the subtree rooted at `prefix` (or, for an empty `prefix`, calls `clear()`)
     * rather than erasing keys one at a time.
     * @param prefix Prefix identifying the keys to erase.
     * @return The number of keys removed.
     */
    size_type erase_prefix(std::string_view prefix) {
        std::scoped_lock lock(*this);
        if (prefix.empty()) {
            size_type n = size_;
            clear();
            return n;
        }
        node* parent = root_;
        node* cur = root_;
        std::size_t last_idx = 0;
        for (char c : prefix) {
            std::size_t idx = lower_bound_index(cur, c);
            if (idx >= cur->children.size() || cur->children[idx]->incoming_char != c) return 0;
            parent = cur;
            last_idx = idx;
            cur = cur->children[idx];
        }
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(last_idx));
        size_type removed = count_and_destroy_subtree(cur);
        size_ -= removed;
        return removed;
    }

    /**
     * @brief Compares two tries by content (same set of key/value pairs), not by internal structure.
     * @return True iff `a` and `b` have the same size and every key in `a` maps to an equal value in `b`.
     */
    friend bool operator==(const prefix_tree& a, const prefix_tree& b) {
        if (&a == &b) return true;
        std::scoped_lock lock(a, b);
        if (a.size_ != b.size_) return false;
        auto ai = a.cbegin();
        auto bi = b.cbegin();
        for (; ai != a.cend(); ++ai, ++bi) {
            if ((*ai).first != (*bi).first) return false;
            if (!((*ai).second == (*bi).second)) return false;
        }
        return true;
    }
    /// @return The negation of `operator==`.
    friend bool operator!=(const prefix_tree& a, const prefix_tree& b) { return !(a == b); }

    /// Non-member `swap`, found via ADL; equivalent to `a.swap(b)`.
    friend void swap(prefix_tree& a, prefix_tree& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

private:
    /// Allocator used (after rebinding) for every node allocation.
    Allocator allocator_;
    /// Sentinel root node; never represents a real key itself unless `""` was inserted.
    node* root_;
    /// Cached element count, kept in sync by every mutator.
    size_type size_ = 0;

    /**
     * @brief Allocates and default-constructs one node.
     * @return A newly allocated node owned by the caller.
     */
    node* create_node() {
        node_allocator_type node_alloc(allocator_);
        node* p = node_alloc_traits::allocate(node_alloc, 1);
        try {
            node_alloc_traits::construct(node_alloc, p, allocator_);
        } catch (...) {
            node_alloc_traits::deallocate(node_alloc, p, 1);
            throw;
        }
        return p;
    }

    /**
     * @brief Destroys and deallocates a single node (not its children).
     * @param p Node to destroy; must have been obtained from `create_node()`.
     */
    void destroy_node(node* p) {
        node_allocator_type node_alloc(allocator_);
        node_alloc_traits::destroy(node_alloc, p);
        node_alloc_traits::deallocate(node_alloc, p, 1);
    }

    /**
     * @brief Iteratively destroys `start` and everything below it.
     *
     * Iterative (explicit stack) rather than recursive, so destruction depth
     * is bounded by available memory rather than by call-stack size even for
     * tries holding very long keys.
     * @param start Subtree root to destroy.
     */
    void destroy_subtree(node* start) {
        std::vector<node*> stack{start};
        std::vector<node*> all;
        while (!stack.empty()) {
            node* n = stack.back();
            stack.pop_back();
            all.push_back(n);
            for (node* c : n->children) stack.push_back(c);
        }
        for (node* n : all) destroy_node(n);
    }

    /**
     * @brief Destroys `start` and everything below it, counting value-bearing nodes first.
     * @param start Subtree root to destroy.
     * @return The number of `is_end` (key-bearing) nodes that were removed.
     */
    size_type count_and_destroy_subtree(node* start) {
        std::vector<node*> stack{start};
        std::vector<node*> all;
        size_type count = 0;
        while (!stack.empty()) {
            node* n = stack.back();
            stack.pop_back();
            all.push_back(n);
            if (n->is_end) ++count;
            for (node* c : n->children) stack.push_back(c);
        }
        for (node* n : all) destroy_node(n);
        return count;
    }

    /**
     * @brief Iteratively deep-copies a subtree.
     * @param src Subtree root to copy (from another prefix_tree, possibly using a different allocator).
     * @return A newly allocated, independent copy of `src`'s subtree, owned by the caller.
     * @throws Whatever `T`'s copy constructor or `create_node()` throws; on exception, any
     *         partially-built subtree is destroyed before rethrowing.
     */
    node* clone_subtree(const node* src) {
        node* new_root = create_node();
        new_root->incoming_char = src->incoming_char;
        try {
            new_root->is_end = src->is_end;
            if (src->is_end) new_root->value = src->value;
            struct work_item {
                const node* s;
                node* d;
            };
            std::vector<work_item> stack{{src, new_root}};
            while (!stack.empty()) {
                work_item w = stack.back();
                stack.pop_back();
                w.d->children.reserve(w.s->children.size());
                for (const node* sc : w.s->children) {
                    node* dc = create_node();
                    dc->incoming_char = sc->incoming_char;
                    // Link `dc` into the tree (and onto the work stack)
                    // before copying its value: if that copy throws, `dc`
                    // must already be reachable from `new_root` so the catch
                    // block's destroy_subtree(new_root) below finds and
                    // frees it instead of leaking it.
                    w.d->children.push_back(dc);
                    stack.push_back({sc, dc});
                    dc->is_end = sc->is_end;
                    if (sc->is_end) dc->value = sc->value;
                }
            }
        } catch (...) {
            destroy_subtree(new_root);
            throw;
        }
        return new_root;
    }

    /**
     * @brief Finds the insertion/lookup point for edge character `c` among `n`'s children.
     * @param n Node whose children to search.
     * @param c Edge character to search for.
     * @return The index of the first child whose `incoming_char` is not less than `c`
     *         (i.e. where `c` would need to be inserted to keep `children` sorted).
     */
    static std::size_t lower_bound_index(const node* n, char c) {
        const auto& ch = n->children;
        auto it = std::lower_bound(ch.begin(), ch.end(), c,
                                    [](node* x, char val) { return x->incoming_char < val; });
        return static_cast<std::size_t>(it - ch.begin());
    }

    /**
     * @brief Finds the child of `n` reached via edge character `c`, if any.
     * @param n Node whose children to search.
     * @param c Edge character to search for.
     * @return The matching child, or `nullptr` if `n` has no child for `c`.
     */
    static node* find_child(const node* n, char c) {
        std::size_t idx = lower_bound_index(n, c);
        if (idx < n->children.size() && n->children[idx]->incoming_char == c) return n->children[idx];
        return nullptr;
    }

    /**
     * @brief Walks from the root following `key`, one character per edge.
     * @param key Character sequence to follow from the root.
     * @return The node reached by consuming all of `key`, or `nullptr` if the path doesn't exist.
     */
    node* find_node(std::string_view key) const {
        node* cur = root_;
        for (char c : key) {
            cur = find_child(cur, c);
            if (!cur) return nullptr;
        }
        return cur;
    }

    /**
     * @brief Walks from the root following `prefix`, one character per edge.
     * @param prefix Character sequence to follow from the root.
     * @return The node reached by consuming all of `prefix`, or `nullptr` if the path doesn't exist.
     */
    node* find_prefix_node(std::string_view prefix) const { return find_node(prefix); }

    /**
     * @brief Builds an iterator positioned at the node reached by following `key` from the root.
     *
     * Unlike `find_node()`, this also records, for every ancestor on the path, the index of the
     * child taken -- the bookkeeping `trie_iterator` needs to backtrack correctly later.
     * @tparam IsConst Whether to build an `iterator` or `const_iterator`.
     * @param key Character sequence to follow from the root; must name an existing path.
     * @return An iterator positioned at the node for `key`.
     */
    template <bool IsConst>
    detail::trie_iterator<T, Allocator, IsConst> build_iterator_for_path(std::string_view key) const {
        detail::trie_iterator<T, Allocator, IsConst> it;
        it.stack_.push_back({root_, 0});
        node* cur = root_;
        for (char c : key) {
            std::size_t idx = lower_bound_index(cur, c);
            node* child = cur->children[idx];
            it.stack_.back().next_child = idx + 1;
            it.stack_.push_back({child, 0});
            it.key_buffer_.push_back(c);
            cur = child;
        }
        return it;
    }

    /**
     * @brief Looks up `key`, returning a const_iterator suitable for use with `erase()`.
     * @param key Key to look up.
     * @return A const_iterator to `key`'s element, or `cend()` if not present.
     */
    const_iterator find_const(const key_type& key) const {
        node* n = find_node(key);
        if (!n || !n->is_end) return cend();
        return build_iterator_for_path<true>(key);
    }

    /**
     * @brief Converts a const_iterator's position into an equivalent (mutable) iterator.
     * @param ci const_iterator to convert; consumed (its internals are moved from).
     * @return An iterator at the same position as `ci`.
     */
    iterator to_iterator(const_iterator ci) {
        iterator it;
        it.stack_ = std::move(ci.stack_);
        it.key_buffer_ = std::move(ci.key_buffer_);
        return it;
    }

    /**
     * @brief Shared implementation of `insert`/`emplace`: walks or grows the path for `key`,
     *        then constructs its value from `args` if not already present.
     * @tparam Args Constructor argument types forwarded to `T`.
     * @param key  Key to insert.
     * @param args Arguments forwarded to `T`'s constructor if `key` is not already present.
     * @return A pair of an iterator to `key`'s element and whether insertion took place.
     */
    template <class... Args>
    std::pair<iterator, bool> emplace_impl(const key_type& key, Args&&... args) {
        iterator it;
        it.stack_.push_back({root_, 0});
        node* cur = root_;
        for (char c : key) {
            std::size_t idx = lower_bound_index(cur, c);
            node* child;
            if (idx < cur->children.size() && cur->children[idx]->incoming_char == c) {
                child = cur->children[idx];
            } else {
                child = create_node();
                child->incoming_char = c;
                cur->children.insert(cur->children.begin() + static_cast<std::ptrdiff_t>(idx), child);
            }
            it.stack_.back().next_child = idx + 1;
            it.stack_.push_back({child, 0});
            it.key_buffer_.push_back(c);
            cur = child;
        }
        bool inserted = false;
        if (!cur->is_end) {
            cur->value.emplace(std::forward<Args>(args)...);
            cur->is_end = true;
            ++size_;
            inserted = true;
        }
        return {it, inserted};
    }

    /**
     * @brief Shared implementation of the two `prefix_range` overloads.
     * @tparam IsConst Whether to build `iterator`s or `const_iterator`s.
     * @param prefix Prefix to query.
     * @return A `[first, second)` iterator pair; both are `end()`-equivalent if no key has this prefix.
     */
    template <bool IsConst>
    std::pair<detail::trie_iterator<T, Allocator, IsConst>, detail::trie_iterator<T, Allocator, IsConst>>
    prefix_range_impl(std::string_view prefix) const {
        using it_t = detail::trie_iterator<T, Allocator, IsConst>;
        it_t start;
        start.stack_.push_back({root_, 0});
        node* cur = root_;
        for (char c : prefix) {
            std::size_t idx = lower_bound_index(cur, c);
            if (idx >= cur->children.size() || cur->children[idx]->incoming_char != c) {
                return {it_t(), it_t()};
            }
            start.stack_.back().next_child = idx + 1;
            node* child = cur->children[idx];
            start.stack_.push_back({child, 0});
            start.key_buffer_.push_back(c);
            cur = child;
        }
        it_t finish = start;
        start.seek_value_forward();
        finish.skip_subtree_and_seek();
        return {start, finish};
    }

    /**
     * @brief Removes now-useless (no value, no children) nodes walking up from the deepest entry
     *        of `path` towards, but excluding, the root.
     * @param path Root-to-erased-node path, as captured by the erased element's iterator
     *             before the element's `is_end`/`value` were cleared.
     */
    void prune_dead_path(const std::vector<detail::trie_frame<T, Allocator>>& path) {
        for (std::size_t i = path.size(); i-- > 1;) {
            node* n = path[i].n;
            node* parent = path[i - 1].n;
            if (n->is_end || !n->children.empty()) break;
            std::size_t child_idx = path[i - 1].next_child - 1;
            parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(child_idx));
            destroy_node(n);
        }
    }

    /**
     * @brief Swaps just `root_` and `size_` (not the allocator) with `other`.
     * @param other The prefix_tree to swap storage with.
     */
    void swap_contents(prefix_tree& other) noexcept {
        std::swap(root_, other.root_);
        std::swap(size_, other.size_);
    }
};

}  // namespace containers::custom
