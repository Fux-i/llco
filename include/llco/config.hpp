#pragma once

// Emit Chrome Trace slices from the engine; enable with -DLLCO_TRACE=ON.
#ifndef LLCO_TRACE
#define LLCO_TRACE 0
#endif

namespace llco {

// Default capacity of an engine's handle queue.
inline constexpr unsigned QUEUE_CAPACITY = 8192;

} // namespace llco
