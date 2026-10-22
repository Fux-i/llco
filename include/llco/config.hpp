#pragma once

// Emit Chrome Trace slices from the engine; enable with -DLLCO_TRACE=ON.
#ifndef LLCO_TRACE
#define LLCO_TRACE 0
#endif

namespace llco {

// Default capacity of an engine's handle queue.
inline constexpr unsigned QUEUE_CAPACITY = 8192;

// Entries in an engine's ring; also the cap on in-flight ops per engine.
inline constexpr unsigned RING_ENTRIES = 64;

} // namespace llco
