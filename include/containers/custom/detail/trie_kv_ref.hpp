/**
 * @file trie_kv_ref.hpp
 * @brief The proxy reference type returned by dereferencing a containers::custom::detail::trie_iterator.
 */

#pragma once

#include <type_traits>

namespace containers::custom::detail {

/**
 * @brief Reference-like proxy returned by dereferencing a trie_iterator.
 *
 * A trie never stores a key contiguously anywhere -- it is spelled out by the
 * path from the root to a node -- so an iterator cannot return a real
 * `std::pair<const Key, T>&`. It returns this proxy instead, giving the same
 * `.first` / `.second` access pattern by reference.
 *
 * @tparam KeyType    The container's key type (`std::string`).
 * @tparam MappedType The container's mapped type `T`.
 * @tparam IsConst    True to make `second` a `const MappedType&` (used by
 *                     `const_iterator`); false for a mutable `MappedType&`
 *                     (used by `iterator`).
 */
template <class KeyType, class MappedType, bool IsConst>
struct trie_kv_ref {
    /// Reference to the reconstructed key at the iterator's current position.
    const KeyType& first;
    /// Reference to the value stored at the iterator's current position.
    std::conditional_t<IsConst, const MappedType&, MappedType&> second;
};

}  // namespace containers::custom::detail
