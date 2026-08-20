/**
 * @file trie_arrow_proxy.hpp
 * @brief The proxy pointer type returned by containers::custom::detail::trie_iterator::operator->.
 */

#pragma once

#include <containers/custom/detail/trie_kv_ref.hpp>

namespace containers::custom::detail {

/**
 * @brief Pointer-like proxy returned by `trie_iterator::operator->`.
 *
 * Holds a trie_kv_ref by value so that `it->first` / `it->second` work even
 * though no real pair object exists for `operator->` to point at.
 *
 * @tparam KeyType    See trie_kv_ref.
 * @tparam MappedType See trie_kv_ref.
 * @tparam IsConst    See trie_kv_ref.
 */
template <class KeyType, class MappedType, bool IsConst>
struct trie_arrow_proxy {
    /// The proxy reference this `operator->` call yields access to.
    trie_kv_ref<KeyType, MappedType, IsConst> ref;
    /// @return A pointer to `ref`, so `operator->` chains as expected.
    const trie_kv_ref<KeyType, MappedType, IsConst>* operator->() const noexcept { return &ref; }
};

}  // namespace containers::custom::detail
