#pragma once

#include <concepts>
#include <type_traits>

#include <llco/core/task.hpp>

namespace llco {

namespace detail {

template <class>
struct is_task : std::false_type {};

template <class T>
struct is_task<task<T>> : std::true_type {};

} // namespace detail

// A task<T> specialization, however it is spelled (value, reference, const).
template <class T>
concept Task = detail::is_task<std::remove_cvref_t<T>>::value;

// A callable that hands back a task when invoked, e.g. a coroutine lambda.
template <class F>
concept TaskMaker = requires(F &make) {
    { make() } -> Task;
};

} // namespace llco
