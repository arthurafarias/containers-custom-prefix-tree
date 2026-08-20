/**
 * @file trie_frame.hpp
 * @brief The explicit-stack DFS frame type used by containers::custom::detail::trie_iterator.
 */

#pragma once

#include <cstddef>

#include <containers/custom/detail/trie_node.hpp>

namespace containers::custom::detail {

/**
 * @brief One stack frame used by trie_iterator's explicit-stack DFS walk.
 *
 * Represents a node on the current root-to-iterator path, together with the
 * index of the next child of that node to descend into when backtracking
 * reaches it again.
 *
 * @tparam T         Mapped value type, forwarded to `trie_node`.
 * @tparam Allocator Allocator type, forwarded to `trie_node`.
 */
template <class T, class Allocator>
struct trie_frame {
    /// The node this frame represents.
    trie_node<T, Allocator>* n;
    /// Index into `n->children` of the next child to try when backtracking into `n`.
    std::size_t next_child;
};

}  // namespace containers::custom::detail
