#pragma once

#include <llco/attr.hpp>
#include <llco/runtime/engine.decl.hpp>

#include <atomic>
#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>

namespace llco {

namespace detail {

// Value slot behind event<T>; the void case stores nothing.
template <class T>
class event_value {
  private:
    std::optional<T> m_slot;

  public:
    void store(T value) { // TODO: rvalue?
        m_slot.emplace(std::move(value));
    }

    // Copied: every waiter gets its own copy.
    auto load() const -> T {
        return *m_slot;
    }
};

template <>
class event_value<void> {
  public:
    void store() noexcept {}
    void load() const noexcept {}
};

} // namespace detail

// One-shot broadcast: every waiter resumes after set(), each with its own copy of the value.
template <class T = void>
class event {
  private:
    struct node {
        std::coroutine_handle<> handle;
        engine                 *target{};
        node                   *next{};
    };

    inline static node m_set_sentinel{};

    std::atomic<bool>      m_setter{false};
    std::atomic<node *>    m_head{nullptr};
    detail::event_value<T> m_value;

    static void wake(node *entry) noexcept {
        if (entry->target != nullptr && entry->target->try_add(entry->handle))
            return;
        entry->handle.resume(); // no engine to return to, or its queue is full (ADR-0004)
    }

    void publish() noexcept {
        for (auto *entry = m_head.exchange(&m_set_sentinel, std::memory_order_acq_rel);
             entry != nullptr;) {
            auto *next = entry->next; // read before waking: the resumed frame may die
            wake(entry);
            entry = next;
        }
    }

  public:
    class [[LLCO_AWAIT_HINT]] awaiter {
      private:
        event &m_event;
        node   m_node{};

      public:
        explicit awaiter(event &owner) noexcept : m_event(owner) {}

        auto await_ready() const noexcept -> bool {
            return m_event.m_head.load(std::memory_order_acquire) == &m_set_sentinel;
        }

        auto await_suspend(std::coroutine_handle<> handle) noexcept -> bool {
            m_node         = node{.handle = handle, .target = current_engine(), .next = nullptr};
            auto *expected = m_event.m_head.load(std::memory_order_acquire);
            while (expected != &m_set_sentinel) {
                m_node.next = expected;
                if (m_event.m_head.compare_exchange_weak(
                        expected, &m_node, std::memory_order_acq_rel, std::memory_order_acquire))
                    return true;
            }
            return false; // set() won the race: this node was never published
        }

        auto await_resume() -> T {
            if constexpr (!std::is_void_v<T>)
                return m_event.m_value.load();
        }
    };

    event() = default;
    CLASS_NO_COPY_MOVE(event);

    template <class U = T>
    void set(std::type_identity_t<U> value)
        requires(!std::is_void_v<U>)
    {
        if (m_setter.exchange(true, std::memory_order_relaxed))
            return; // one-shot: repeated set is a no-op
        m_value.store(std::move(value));
        publish();
    }

    template <class U = T>
    void set()
        requires std::is_void_v<U>
    {
        if (m_setter.exchange(true, std::memory_order_relaxed))
            return;
        publish();
    }

    auto wait() -> awaiter {
        return awaiter{*this};
    }
};

} // namespace llco
