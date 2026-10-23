#pragma once

#include <llco/attr.hpp>

#include <string_view>

namespace llco::trace {

// One nested slice: emits a begin event on construction and an end event on destruction.
class scope {
  private:
    const char *m_name;

  public:
    explicit scope(const char *name) noexcept;
    ~scope();
    CLASS_NO_COPY_MOVE(scope);
};

// Names the calling thread's lane; defaults to thread-<tid>.
void set_thread_name(std::string_view name);

// Writes the events collected so far to path as a Chrome Trace JSON array, then drains them.
[[LLCO_DISCARD_HINT]]
auto flush(std::string_view path) -> bool;

} // namespace llco::trace

#include <llco/trace/trace.impl.hpp>
