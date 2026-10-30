#pragma once

#include <ranges>
#include <type_traits>

#include <llco/concepts/task_batch.hpp>
#include <llco/concepts/task_maker.hpp>
#include <llco/core/task.hpp>

namespace llco {

// Submits every task and returns; throws std::runtime_error on the first rejection (ADR-0004).
template <class... T>
void run(task<T> &&...tasks);

// Runs what the callable returns, e.g. llco::run([] -> task<> { co_return; });
template <TaskMaker F>
void run(F &&make);

// Submits every task in the batch and returns; the batch is consumed, so only an rvalue fits.
template <TaskBatch R>
void run(R &&tasks);

// Submits every task and blocks until all of them have finished.
template <class... T>
void sync_run(task<T> &&...tasks);

// Runs what the callable returns, then blocks.
template <TaskMaker F>
void sync_run(F &&make);

// Submits every task in the batch, then blocks; the batch is consumed, so only an rvalue fits.
template <SizedTaskBatch R>
void sync_run(R &&tasks);

} // namespace llco

#include <llco/runtime/run.impl.hpp>