#pragma once

#include <ranges>
#include <type_traits>

#include <llco/concepts/task_maker.hpp>

namespace llco {

// A range of tasks that may be consumed: its elements are task<T> and it is an rvalue.
template <class R>
concept TaskBatch = std::ranges::range<R> && Task<std::ranges::range_value_t<R>> &&
                    (!std::is_lvalue_reference_v<R>);

// A batch whose size is known up front, so sync_run can count it before submitting.
template <class R>
concept SizedTaskBatch = TaskBatch<R> && std::ranges::sized_range<R>;

} // namespace llco
