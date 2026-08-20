/**
 * @file trie_node.hpp
 * @brief The node type underlying containers::custom::trie.
 */

#pragma once

#include <memory>
#include <optional>
#include <vector>

namespace containers::custom::detail {

/**
 * @namespace containers::custom::detail
 * @brief Implementation details backing the containers in containers::custom.
 *
 * Nothing here is part of the public API: types and functions in this
 * namespace exist only to implement the containers in containers::custom and
 * may change shape, move, or disappear between releases without notice.
 * User code should not name anything in this namespace directly.
 */

/**
 * @brief One node of a trie's underlying tree.
 *
 * A node represents the character edge leading to it (trie_node::incoming_char)
 * plus, if some inserted key ends exactly here, that key's associated value.
 * Children are kept sorted by `incoming_char` so lookups can binary-search
 * and so a pre-order walk of the tree visits keys in lexicographic order.
 *
 * @tparam T         Mapped value type stored at key-terminating nodes.
 * @tparam Allocator Allocator (as supplied to the owning trie) that this
 *                     node's children container is rebound to. The node
 *                     itself is allocated by the owning trie, using the same
 *                     allocator rebound to `trie_node`.
 */
template <class T, class Allocator>
struct trie_node {
    /// `Allocator` rebound to `trie_node*`, used by `children`.
    using node_ptr_allocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<trie_node*>;
    /// Storage type for `children`: a vector of child node pointers.
    using child_vector = std::vector<trie_node*, node_ptr_allocator>;

    /// Character on the edge from this node's parent to this node; meaningless for the root.
    char incoming_char = 0;
    /// True iff some inserted key ends exactly at this node.
    bool is_end = false;
    /// The value associated with the key ending here; engaged iff `is_end` is true.
    std::optional<T> value{};
    /// This node's children, kept sorted by `incoming_char` for binary search.
    child_vector children;

    /**
     * @brief Constructs an empty, non-terminating node.
     * @param alloc Allocator to rebind for the `children` vector's storage.
     */
    explicit trie_node(const Allocator& alloc) : children(node_ptr_allocator(alloc)) {}
};

}  // namespace containers::custom::detail
