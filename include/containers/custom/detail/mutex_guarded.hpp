/**
 * @file mutex_guarded.hpp
 * @brief The mutex-owning base class that makes containers::custom::prefix_tree thread-safe.
 */

#pragma once

#include <memory>
#include <mutex>

namespace containers::custom::detail {

/**
 * @brief Base class giving a derived container its own recursive mutex and a
 *        `Lockable`-conforming interface.
 *
 * A container derives from `mutex_guarded` to get two things:
 *
 *  - `lock()` / `try_lock()` / `unlock()`, satisfying the standard `Lockable`
 *    named requirements, so the container object itself can guard a
 *    `std::scoped_lock` (e.g. `std::scoped_lock lock(my_container);`) for
 *    hand-written multi-step critical sections.
 *  - `acquire_shared_lock()`, a reference-counted RAII lock used internally
 *    by the container's member accessors and, notably, attached to any
 *    iterator they return so the lock survives for that iterator's whole
 *    lifetime -- see trie_iterator.
 *
 * The mutex is *recursive* specifically so these two mechanisms compose: a
 * caller holding an external `std::scoped_lock` on the container (or an
 * iterator that is keeping the container locked) can still call further
 * member accessors -- which lock internally -- from the same thread without
 * deadlocking.
 *
 * Copying, moving, or assigning a `mutex_guarded` never touches the
 * underlying mutex: a copy/move always starts with its own fresh, unlocked
 * mutex, and assignment leaves the target's existing mutex untouched. A
 * mutex has no meaningful "value" to copy -- only the object that logically
 * owns it does -- so derived containers get ordinary value semantics for
 * everything except locking, which is always local to each object.
 */
class mutex_guarded {
public:
    /// Constructs a fresh, unlocked mutex.
    mutex_guarded() = default;
    /// Leaves the mutex untouched; the new object gets its own fresh, unlocked mutex.
    mutex_guarded(const mutex_guarded&) noexcept {}
    /// Leaves the mutex untouched; the new object gets its own fresh, unlocked mutex.
    mutex_guarded(mutex_guarded&&) noexcept {}
    /**
     * @brief No-op: assignment never touches either object's mutex.
     * @return `*this`.
     */
    mutex_guarded& operator=(const mutex_guarded&) noexcept { return *this; }
    /**
     * @brief No-op: assignment never touches either object's mutex.
     * @return `*this`.
     */
    mutex_guarded& operator=(mutex_guarded&&) noexcept { return *this; }
    ~mutex_guarded() = default;

    /// Locks the mutex, blocking until available. Part of the `Lockable` interface.
    void lock() const { mutex_.lock(); }
    /// @return True and locks the mutex if it was available without blocking; false otherwise.
    bool try_lock() const { return mutex_.try_lock(); }
    /// Unlocks the mutex. Part of the `Lockable` interface.
    void unlock() const { mutex_.unlock(); }

    /**
     * @brief Acquires a reference-counted RAII lock on this object's mutex.
     *
     * The returned handle keeps the mutex locked for as long as it, or any
     * copy of it, is alive -- copies share ownership of the *same*
     * underlying lock rather than each re-locking the mutex. This is what
     * lets a container attach the same lock to an iterator it returns: the
     * iterator (and every copy taken from it, e.g. by `operator++(int)`)
     * keeps the mutex locked until the last such copy is destroyed,
     * preventing concurrent mutation for as long as that iterator is in use.
     * @return An owning handle to a newly locked `std::unique_lock` on this object's mutex.
     */
    std::shared_ptr<std::unique_lock<std::recursive_mutex>> acquire_shared_lock() const {
        return std::make_shared<std::unique_lock<std::recursive_mutex>>(mutex_);
    }

protected:
    /// The mutex backing `lock()`/`try_lock()`/`unlock()`/`acquire_shared_lock()`. Recursive: see class docs.
    mutable std::recursive_mutex mutex_;
};

}  // namespace containers::custom::detail
