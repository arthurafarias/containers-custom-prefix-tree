/**
 * @file trie_iterator.hpp
 * @brief The forward iterator type used by containers::custom::prefix_tree.
 */

#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <containers/custom/detail/trie_arrow_proxy.hpp>
#include <containers/custom/detail/trie_frame.hpp>
#include <containers/custom/detail/trie_kv_ref.hpp>
#include <containers/custom/detail/trie_node.hpp>

namespace containers::custom {

// Forward declaration only (not a definition), so trie_iterator can grant it
// friendship without pulling in the rest of prefix_tree's declaration.
template <class T, class Allocator>
class prefix_tree;

}  // namespace containers::custom

namespace containers::custom::detail {

/**
 * @brief Forward iterator over a `prefix_tree`, visiting keys in ascending (lexicographic) order.
 *
 * Performs a pre-order depth-first walk of the node tree using an explicit
 * stack of trie_frame entries (rather than recursion, to avoid unbounded
 * call-stack depth on tries with very long keys). The key at the current
 * position is rebuilt incrementally in `key_buffer_` as the walk descends and
 * backtracks, rather than being stored anywhere in the tree itself.
 *
 * Thread safety: an iterator returned by one of the owning `prefix_tree`'s
 * accessors (`begin()`, `find()`, ...) carries a reference-counted RAII lock
 * (`lock_`) on that prefix_tree's mutex, acquired via
 * detail::mutex_guarded::acquire_shared_lock(). The lock is shared by every
 * copy taken from that iterator (assignment, `operator++(int)`'s snapshot,
 * the iterator-to-const_iterator conversion, ...) and is only released once
 * the last such copy is destroyed, which keeps the tree from being mutated
 * out from under the iterator for as long as the iterator -- in any of its
 * copies -- remains alive. A default-constructed (`end()`) iterator holds no
 * lock.
 *
 * @tparam T         Mapped value type.
 * @tparam Allocator Allocator type used by the owning prefix_tree.
 * @tparam IsConst   True for `prefix_tree::const_iterator`, false for `prefix_tree::iterator`.
 */
template <class T, class Allocator, bool IsConst>
class trie_iterator {
    /// The node type this iterator walks.
    using node = trie_node<T, Allocator>;
    /// The DFS stack frame type this iterator maintains.
    using frame = trie_frame<T, Allocator>;

public:
    /// Satisfies `std::forward_iterator`.
    using iterator_category = std::forward_iterator_tag;
    /// Conceptual value type (see class docs: `operator*` returns a proxy, not this type by reference).
    using value_type = std::pair<std::string, T>;
    /// Signed distance type, required by the iterator named requirements.
    using difference_type = std::ptrdiff_t;
    /// The proxy type returned by `operator*`.
    using reference = trie_kv_ref<std::string, T, IsConst>;
    /// The proxy type returned by `operator->`.
    using pointer = trie_arrow_proxy<std::string, T, IsConst>;

    /// Constructs a past-the-end iterator (equal to the owning prefix_tree's `end()`).
    trie_iterator() = default;

    /**
     * @brief Converting constructor from `iterator` to `const_iterator`.
     *
     * Participates in overload resolution only when constructing a
     * const_iterator (`IsConst == true`) from a non-const iterator
     * (`WasConst == false`), matching `std::map`'s iterator/const_iterator
     * relationship.
     * @param other The (non-const) iterator to copy the position of.
     */
    template <bool WasConst, class = std::enable_if_t<IsConst && !WasConst>>
    trie_iterator(const trie_iterator<T, Allocator, WasConst>& other)
        : stack_(other.stack_), key_buffer_(other.key_buffer_), lock_(other.lock_) {}

    /**
     * @brief Dereferences the iterator.
     * @return A trie_kv_ref proxy referring to the current key and value.
     * @pre The iterator is not past-the-end.
     */
    reference operator*() const {
        node* n = stack_.back().n;
        return reference{key_buffer_, n->value.value()};
    }

    /**
     * @brief Member-access into the current key/value pair.
     * @return A trie_arrow_proxy wrapping the result of `operator*`.
     * @pre The iterator is not past-the-end.
     */
    pointer operator->() const { return pointer{operator*()}; }

    /**
     * @brief Advances to the next key in lexicographic order (pre-increment).
     * @return `*this`, now referring to the next element, or `end()`.
     */
    trie_iterator& operator++() {
        advance();
        return *this;
    }

    /**
     * @brief Advances to the next key in lexicographic order (post-increment).
     * @return A copy of the iterator's position before advancing.
     */
    trie_iterator operator++(int) {
        trie_iterator tmp = *this;
        advance();
        return tmp;
    }

    /**
     * @brief Equality comparison.
     * @return True iff both iterators refer to the same node (or both are past-the-end).
     */
    friend bool operator==(const trie_iterator& a, const trie_iterator& b) {
        return a.current() == b.current();
    }

private:
    friend class containers::custom::prefix_tree<T, Allocator>;
    template <class, class, bool>
    friend class trie_iterator;

    /// Root-to-current-node path; `stack_.back().n` is the current node, or empty means `end()`.
    std::vector<frame> stack_;
    /// The key at the current position, rebuilt incrementally as the walk proceeds.
    std::string key_buffer_;
    /// RAII lock on the owning prefix_tree's mutex, shared with every copy of this iterator; null for `end()`.
    std::shared_ptr<std::unique_lock<std::recursive_mutex>> lock_;

    /// @return The current node, or `nullptr` if this iterator is past-the-end.
    node* current() const { return stack_.empty() ? nullptr : stack_.back().n; }

    /**
     * @brief Tries to move to the current node's first not-yet-visited child.
     * @return True and pushes a new frame if a child was available; false otherwise.
     */
    bool descend_first_child() {
        frame& f = stack_.back();
        if (f.next_child < f.n->children.size()) {
            node* c = f.n->children[f.next_child++];
            stack_.push_back({c, 0});
            key_buffer_.push_back(c->incoming_char);
            return true;
        }
        return false;
    }

    /**
     * @brief Pops the current node and walks up ancestors until an unvisited sibling is found.
     * @return True and lands on that sibling if one exists; false if the walk reached the root
     *         with nothing left to visit (the iterator becomes `end()`-equivalent: empty stack).
     */
    bool backtrack() {
        stack_.pop_back();
        if (!key_buffer_.empty()) key_buffer_.pop_back();
        while (!stack_.empty()) {
            frame& f = stack_.back();
            if (f.next_child < f.n->children.size()) {
                node* c = f.n->children[f.next_child++];
                stack_.push_back({c, 0});
                key_buffer_.push_back(c->incoming_char);
                return true;
            }
            stack_.pop_back();
            if (!key_buffer_.empty()) key_buffer_.pop_back();
        }
        return false;
    }

    /**
     * @brief Moves to the next node in raw pre-order (regardless of whether it holds a value).
     * @return True if a next node was found; false if traversal is exhausted.
     */
    bool step() { return descend_first_child() || backtrack(); }

    /// Advances (via `step()`) until landing on a value-bearing node, or traversal is exhausted.
    void seek_value_forward() {
        while (!stack_.empty() && !stack_.back().n->is_end) {
            if (!step()) break;
        }
    }

    /// Moves off the current node by at least one step, then seeks the next value-bearing node.
    void advance() {
        if (step()) seek_value_forward();
    }

    /**
     * @brief Skips the current node's entire subtree and seeks the next value after it.
     *
     * Used to compute the exclusive end of a prefix range: unlike `advance()`,
     * this never descends into the current node's children.
     */
    void skip_subtree_and_seek() {
        if (backtrack()) seek_value_forward();
    }
};

}  // namespace containers::custom::detail
