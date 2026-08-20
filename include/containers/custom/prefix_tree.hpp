/**
 * @file prefix_tree.hpp
 * @brief Header-only, PMR-aware prefix_tree (prefix tree) container.
 *
 * Defines containers::custom::prefix_tree<ElementType, Allocator>, an
 * STL-inspired container storing sequences of `ElementType` (e.g.
 * `prefix_tree<char>` behaves like a `std::set<std::string>`) in a
 * character/element-indexed prefix tree. Node layout and iterator machinery
 * live in containers::custom::detail (see below) and are not part of the
 * public API: they may change between releases without notice.
 */

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <ostream>
#include <ranges>
#include <utility>
#include <vector>

#include <containers/custom/detail/mutex_guarded.hpp>

namespace containers::custom {

template <typename ElementType,
          typename Allocator = std::pmr::polymorphic_allocator<ElementType>>
class prefix_tree;

} // namespace containers::custom

namespace containers::custom::detail {

/**
 * @brief A single node of the tree: one `ElementType` along some stored
 *        sequence's path from the root, plus whether a sequence terminates
 *        here and the (lexicographically sorted) children continuing it.
 *
 * The root node itself carries no element (`element` is `std::nullopt`) --
 * it is only ever the anchor children hang off of.
 */
template <typename ElementType> struct prefix_tree_node {
  std::optional<ElementType> element;
  bool is_end = false;
  std::vector<std::shared_ptr<prefix_tree_node<ElementType>>> children;
};

/**
 * @brief One frame of a root-to-node path: the node itself, plus the index
 *        of the next child of that node still left to explore.
 *
 * `prefix_tree_iterator` keeps a stack of these instead of giving nodes a
 * parent pointer: a parent pointer would form a shared_ptr reference cycle
 * with the `children` vector that already owns the node (parent -> child
 * via `children`, child -> parent would never let refcounts reach zero).
 * The stack is also exactly the "root-to-current-node path", which is what
 * `prefix_tree_element_view` needs to present as a sequence -- so it does
 * double duty as both the iterator's backtracking state and the view's
 * backing storage, at no extra cost.
 */
template <typename ElementType> struct path_frame {
  prefix_tree_node<ElementType> *node;
  std::size_t next_child;
};

/**
 * @brief A read-only, zero-copy view of one stored sequence.
 *
 * Dereferencing a `prefix_tree_iterator` yields one of these: it does not
 * copy any `ElementType` out of the tree, it simply spans the relevant
 * slice of the iterator's own path-frame stack and reads each node's
 * `element` in place. It is therefore only valid for as long as the
 * iterator that produced it is alive and has not been advanced.
 */
template <typename ElementType> class prefix_tree_element_view {
public:
  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = ElementType;
    using difference_type = std::ptrdiff_t;
    using reference = const ElementType &;
    using pointer = const ElementType *;

    iterator() = default;
    explicit iterator(const path_frame<ElementType> *frame) : frame_(frame) {}

    reference operator*() const { return *frame_->node->element; }

    iterator &operator++() {
      ++frame_;
      return *this;
    }
    iterator operator++(int) {
      iterator tmp(*this);
      ++*this;
      return tmp;
    }

    bool operator==(const iterator &) const = default;

  private:
    const path_frame<ElementType> *frame_ = nullptr;
  };

  using const_iterator = iterator;
  using value_type = ElementType;

  prefix_tree_element_view() = default;
  prefix_tree_element_view(const path_frame<ElementType> *first,
                            const path_frame<ElementType> *last)
      : first_(first), last_(last) {}

  iterator begin() const { return iterator(first_); }
  iterator end() const { return iterator(last_); }

  [[nodiscard]] bool empty() const { return first_ == last_; }
  std::size_t size() const {
    return static_cast<std::size_t>(last_ - first_);
  }

private:
  const path_frame<ElementType> *first_ = nullptr;
  const path_frame<ElementType> *last_ = nullptr;
};

template <typename ElementType, std::ranges::input_range Range>
bool operator==(const prefix_tree_element_view<ElementType> &view,
                const Range &other) {
  return std::ranges::equal(view, other);
}

template <typename ElementType>
bool operator==(const prefix_tree_element_view<ElementType> &lhs,
                 const prefix_tree_element_view<ElementType> &rhs) {
  return std::ranges::equal(lhs, rhs);
}

template <typename ElementType>
std::ostream &operator<<(std::ostream &os,
                          const prefix_tree_element_view<ElementType> &view) {
  for (const auto &element : view) {
    os << element;
  }
  return os;
}

/**
 * @brief Forward DFS iterator over the terminal (`is_end`) nodes of a
 *        prefix_tree, in lexicographic order.
 *
 * Holds its own `std::vector<path_frame<ElementType>>` root-to-current-node
 * path. Because that path is owned by the iterator itself (not shared with
 * any other iterator or the tree), copying an iterator copies its path and
 * yields a fully independent, still-valid iterator -- satisfying
 * `std::forward_iterator` despite the proxy `element_view` reference type,
 * which C++20's iterator concepts (unlike the pre-C++20 named
 * requirements) explicitly allow.
 *
 * `min_depth_` bounds backtracking: ordinary whole-tree iteration never
 * pops back past the root frame (`min_depth_ == 1`); `prefix_range()`
 * reuses the exact same mechanism with `min_depth_` set to the depth of the
 * prefix's node, so iteration never escapes that subtree.
 */
template <typename ElementType> class prefix_tree_iterator {
public:
  using node_type = prefix_tree_node<ElementType>;
  using frame_type = path_frame<ElementType>;
  using iterator_category = std::forward_iterator_tag;
  using value_type = prefix_tree_element_view<ElementType>;
  using difference_type = std::ptrdiff_t;
  using reference = value_type;
  using pointer = void;

  prefix_tree_iterator() = default;

  reference operator*() const {
    return value_type(stack_.data() + 1, stack_.data() + stack_.size());
  }

  prefix_tree_iterator &operator++() {
    advance();
    return *this;
  }
  prefix_tree_iterator operator++(int) {
    prefix_tree_iterator tmp(*this);
    advance();
    return tmp;
  }

  bool operator==(const prefix_tree_iterator &other) const {
    if (stack_.empty() || other.stack_.empty()) {
      return stack_.empty() == other.stack_.empty();
    }
    return stack_.back().node == other.stack_.back().node;
  }

private:
  template <typename ET, typename Alloc>
  friend class containers::custom::prefix_tree;

  prefix_tree_iterator(
      std::vector<frame_type> stack, std::size_t min_depth,
      std::shared_ptr<std::unique_lock<std::recursive_mutex>> lock)
      : stack_(std::move(stack)), min_depth_(min_depth), lock_(std::move(lock)) {
    if (!stack_.empty() && !stack_.back().node->is_end) {
      advance();
    }
  }

  void advance() {
    while (!stack_.empty()) {
      frame_type &top = stack_.back();
      if (top.next_child < top.node->children.size()) {
        node_type *child = top.node->children[top.next_child++].get();
        stack_.push_back({child, 0});
        if (child->is_end) {
          return;
        }
      } else if (stack_.size() == min_depth_) {
        stack_.clear();
        return;
      } else {
        stack_.pop_back();
      }
    }
  }

  std::vector<frame_type> stack_;
  std::size_t min_depth_ = 0;
  std::shared_ptr<std::unique_lock<std::recursive_mutex>> lock_;
};

} // namespace containers::custom::detail

namespace containers::custom {

/**
 * @brief A prefix tree (trie) storing unique sequences of `ElementType`.
 *
 * Behaves like an STL associative container in the vein of `std::set`, but
 * over sequences rather than single values -- `prefix_tree<char>` is to
 * `std::set<std::string>` roughly what `std::string` is to
 * `std::vector<char>`. `ElementType` must be `std::totally_ordered` so
 * children can be kept sorted (this is what gives lexicographic iteration
 * order and O(log branching-factor) descent per element).
 *
 * Any `std::ranges::input_range` whose value type is `ElementType` can be
 * inserted/looked up: `std::string`, `std::string_view`, `std::vector<T>`,
 * `std::array<T, N>`, and so on.
 *
 * Dereferencing an iterator does not yield a stored sequence by value; it
 * yields a `prefix_tree_element_view`, a zero-copy read-only view that
 * reads each element directly out of the tree's own nodes as you iterate
 * it. It is valid only transiently -- for as long as the iterator that
 * produced it is alive and not yet advanced. See detail::prefix_tree_iterator
 * and detail::prefix_tree_element_view.
 */
template <typename ElementType, typename Allocator>
class prefix_tree : private detail::mutex_guarded {
  static_assert(std::totally_ordered<ElementType>,
                "prefix_tree requires ElementType to support operator< and "
                "operator==, so children can be kept sorted");

  using node_type = detail::prefix_tree_node<ElementType>;
  using frame_type = detail::path_frame<ElementType>;

  // std::pmr::polymorphic_allocator's operator= is deleted -- it never
  // propagates implicitly, matching every other std::pmr container. Whether
  // the allocator itself is reassigned on copy/move/swap is therefore
  // gated on allocator_traits' propagate_on_container_* traits, exactly as
  // the standard containers do; for an Allocator type without a deleted
  // operator= (e.g. std::allocator), these traits default to propagating.
  using alloc_traits = std::allocator_traits<Allocator>;

public:
  using element_type = ElementType;
  using allocator_type = Allocator;
  using size_type = std::size_t;
  using iterator = detail::prefix_tree_iterator<ElementType>;
  using const_iterator = iterator;
  using element_view = detail::prefix_tree_element_view<ElementType>;

  // Re-exposed from the privately-inherited mutex_guarded base: these make
  // prefix_tree itself satisfy Lockable, so a caller can hold
  // std::scoped_lock lock(my_tree); across a hand-written multi-step
  // critical section (see the class docs on mutex_guarded).
  using detail::mutex_guarded::lock;
  using detail::mutex_guarded::try_lock;
  using detail::mutex_guarded::unlock;

  prefix_tree() : prefix_tree(Allocator()) {}

  explicit prefix_tree(const Allocator &allocator)
      : allocator_(allocator), root_(allocate_node()) {}

  template <std::ranges::input_range Sequence>
  prefix_tree(std::initializer_list<Sequence> init,
              const Allocator &allocator = Allocator())
      : prefix_tree(allocator) {
    for (const auto &sequence : init) {
      insert(sequence);
    }
  }

  prefix_tree(const prefix_tree &other)
      : detail::mutex_guarded(other), allocator_(other.allocator_),
        root_(clone(other.root_)), size_(other.size_) {}

  prefix_tree(prefix_tree &&other) noexcept
      : detail::mutex_guarded(std::move(other)),
        allocator_(std::move(other.allocator_)),
        root_(std::exchange(other.root_, other.allocate_node())),
        size_(std::exchange(other.size_, 0)) {}

  prefix_tree &operator=(const prefix_tree &other) {
    if (this == &other) {
      return *this;
    }
    std::scoped_lock lock(*this);
    if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
      allocator_ = other.allocator_;
    }
    root_ = clone(other.root_);
    size_ = other.size_;
    return *this;
  }

  prefix_tree &operator=(prefix_tree &&other) noexcept(
      alloc_traits::propagate_on_container_move_assignment::value ||
      alloc_traits::is_always_equal::value) {
    if (this == &other) {
      return *this;
    }
    std::scoped_lock lock(*this);
    if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
      allocator_ = std::move(other.allocator_);
      root_ = std::move(other.root_);
    } else if (allocator_ == other.allocator_) {
      root_ = std::move(other.root_);
    } else {
      // Allocators differ and don't propagate: nodes allocated from
      // other's resource can't be adopted, so fall back to a deep copy.
      root_ = clone(other.root_);
    }
    size_ = other.size_;
    other.root_ = other.allocate_node();
    other.size_ = 0;
    return *this;
  }

  ~prefix_tree() = default;

  iterator begin() const {
    std::vector<frame_type> stack{{root_.get(), 0}};
    return iterator(std::move(stack), 1, acquire_shared_lock());
  }
  iterator end() const { return iterator(); }
  iterator cbegin() const { return begin(); }
  iterator cend() const { return end(); }

  [[nodiscard]] bool empty() const {
    std::scoped_lock lock(*this);
    return size_ == 0;
  }
  size_type size() const {
    std::scoped_lock lock(*this);
    return size_;
  }
  size_type max_size() const { return std::numeric_limits<size_type>::max(); }

  void clear() {
    std::scoped_lock lock(*this);
    root_ = allocate_node();
    size_ = 0;
  }

  void swap(prefix_tree &other) noexcept {
    if (this == &other) {
      return;
    }
    std::scoped_lock lock(*this), lock2(other);
    using std::swap;
    if constexpr (alloc_traits::propagate_on_container_swap::value) {
      swap(allocator_, other.allocator_);
    }
    swap(root_, other.root_);
    swap(size_, other.size_);
  }

  template <std::ranges::input_range Sequence>
  std::pair<iterator, bool> insert(const Sequence &sequence) {
    std::scoped_lock lock(*this);
    std::vector<frame_type> stack = locate_or_create(sequence);
    node_type *target = stack.back().node;
    bool inserted = !target->is_end;
    if (inserted) {
      target->is_end = true;
      ++size_;
    }
    return {iterator(std::move(stack), 1, acquire_shared_lock()), inserted};
  }

  template <std::ranges::input_range Sequence>
  size_type erase(const Sequence &sequence) {
    std::scoped_lock lock(*this);
    std::optional<std::vector<frame_type>> found = locate(sequence);
    if (!found || !found->back().node->is_end) {
      return 0;
    }
    found->back().node->is_end = false;
    --size_;
    prune(*found);
    return 1;
  }

  iterator erase(iterator pos) {
    std::scoped_lock lock(*this);
    if (pos == end()) {
      return end();
    }
    iterator next = pos;
    ++next;
    node_type *target = pos.stack_.back().node;
    if (target->is_end) {
      target->is_end = false;
      --size_;
      prune(pos.stack_);
    }
    return next;
  }

  template <std::ranges::input_range Sequence>
  iterator find(const Sequence &sequence) const {
    std::scoped_lock lock(*this);
    std::optional<std::vector<frame_type>> found = locate(sequence);
    if (!found || !found->back().node->is_end) {
      return end();
    }
    return iterator(std::move(*found), 1, acquire_shared_lock());
  }

  template <std::ranges::input_range Sequence>
  bool contains(const Sequence &sequence) const {
    return find(sequence) != end();
  }

  template <std::ranges::input_range Sequence>
  size_type count(const Sequence &sequence) const {
    return contains(sequence) ? 1 : 0;
  }

  template <std::ranges::input_range Sequence>
  bool starts_with(const Sequence &prefix) const {
    std::scoped_lock lock(*this);
    return locate(prefix).has_value();
  }

  template <std::ranges::input_range Sequence>
  std::pair<iterator, iterator> prefix_range(const Sequence &prefix) const {
    std::scoped_lock lock(*this);
    std::optional<std::vector<frame_type>> found = locate(prefix);
    if (!found) {
      return {end(), end()};
    }
    std::size_t min_depth = found->size();
    return {iterator(std::move(*found), min_depth, acquire_shared_lock()),
            end()};
  }

  template <std::ranges::input_range Sequence>
  size_type erase_prefix(const Sequence &prefix) {
    std::scoped_lock lock(*this);
    std::optional<std::vector<frame_type>> found = locate(prefix);
    if (!found) {
      return 0;
    }
    node_type *target = found->back().node;
    size_type removed = count_end_nodes(target);
    if (removed == 0) {
      return 0;
    }
    if (found->size() == 1) {
      root_ = allocate_node();
    } else {
      node_type *parent = (*found)[found->size() - 2].node;
      auto &siblings = parent->children;
      siblings.erase(std::find_if(
          siblings.begin(), siblings.end(),
          [&](const auto &child) { return child.get() == target; }));
    }
    size_ -= removed;
    return removed;
  }

  friend bool operator==(const prefix_tree &lhs, const prefix_tree &rhs) {
    if (&lhs == &rhs) {
      return true;
    }
    std::scoped_lock lock(lhs), lock2(rhs);
    if (lhs.size_ != rhs.size_) {
      return false;
    }
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  }

private:
  std::shared_ptr<node_type> allocate_node() const {
    return std::allocate_shared<node_type>(allocator_);
  }

  std::shared_ptr<node_type> clone(const std::shared_ptr<node_type> &src) const {
    std::shared_ptr<node_type> dst = allocate_node();
    dst->element = src->element;
    dst->is_end = src->is_end;
    dst->children.reserve(src->children.size());
    for (const auto &child : src->children) {
      dst->children.push_back(clone(child));
    }
    return dst;
  }

  template <std::ranges::input_range Sequence>
  std::optional<std::vector<frame_type>> locate(const Sequence &sequence) const {
    std::vector<frame_type> stack{{root_.get(), 0}};
    for (const auto &element : sequence) {
      frame_type &top = stack.back();
      auto &children = top.node->children;
      auto it = std::lower_bound(
          children.begin(), children.end(), element,
          [](const auto &child, const auto &value) {
            return *child->element < value;
          });
      if (it == children.end() || (*it)->element.value() != element) {
        return std::nullopt;
      }
      top.next_child =
          static_cast<std::size_t>(std::distance(children.begin(), it)) + 1;
      stack.push_back({it->get(), 0});
    }
    return stack;
  }

  template <std::ranges::input_range Sequence>
  std::vector<frame_type> locate_or_create(const Sequence &sequence) {
    std::vector<frame_type> stack{{root_.get(), 0}};
    for (const auto &element : sequence) {
      frame_type &top = stack.back();
      auto &children = top.node->children;
      auto it = std::lower_bound(
          children.begin(), children.end(), element,
          [](const auto &child, const auto &value) {
            return *child->element < value;
          });
      std::shared_ptr<node_type> child;
      if (it != children.end() && (*it)->element.value() == element) {
        child = *it;
      } else {
        child = allocate_node();
        child->element = element;
        it = children.insert(it, child);
      }
      top.next_child =
          static_cast<std::size_t>(std::distance(children.begin(), it)) + 1;
      stack.push_back({child.get(), 0});
    }
    return stack;
  }

  void prune(std::vector<frame_type> &stack) {
    for (std::size_t i = stack.size(); i-- > 1;) {
      node_type *node = stack[i].node;
      if (node->is_end || !node->children.empty()) {
        break;
      }
      node_type *parent = stack[i - 1].node;
      auto &siblings = parent->children;
      siblings.erase(std::find_if(
          siblings.begin(), siblings.end(),
          [&](const auto &child) { return child.get() == node; }));
    }
  }

  static size_type count_end_nodes(const node_type *node) {
    size_type count = node->is_end ? 1 : 0;
    for (const auto &child : node->children) {
      count += count_end_nodes(child.get());
    }
    return count;
  }

  Allocator allocator_;
  std::shared_ptr<node_type> root_;
  size_type size_ = 0;
};

template <typename ElementType, typename Allocator>
void swap(prefix_tree<ElementType, Allocator> &lhs,
          prefix_tree<ElementType, Allocator> &rhs) noexcept {
  lhs.swap(rhs);
}

} // namespace containers::custom
