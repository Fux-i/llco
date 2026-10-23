#pragma once

#include <llco/trace/trace.decl.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llco::trace {

namespace detail {

// One slice boundary; names are literals, so no string is copied per event.
struct event {
    const char  *name;
    char         phase;
    std::int64_t ts;
};

// Everything one thread buffered, plus the lane label used in the JSON.
struct lane_data {
    std::string        name;
    std::vector<event> events;
    std::uint32_t      tid{};

    void append_json(std::string &out) const;
};

class lane;

struct registry {
    std::mutex             mutex;
    std::vector<lane *>    live;     // threads still running
    std::vector<lane_data> finished; // threads that already exited
};

// Per-thread buffer; registers itself while the thread lives and hands its
// data to the finished list at exit, so a later flush still writes it.
class lane {
  private:
    lane_data m_data;

  public:
    lane();
    ~lane();
    CLASS_NO_COPY_MOVE(lane);

    void set_name(std::string_view name);
    void push(const char *name, char phase);
    void append_json(std::string &out) const;
    void clear();
};

auto registry_ref() -> registry &;
auto current_lane() -> lane &;

// monotonic clock in microseconds, the unit Chrome Trace expects
inline auto now_us() -> std::int64_t {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// leaked on purpose: lanes outlive static destruction at exit
inline auto registry_ref() -> registry & {
    static auto *instance = new registry;
    return *instance;
}

inline lane::lane() {
    static std::atomic<std::uint32_t> next_tid{1};
    m_data.tid  = next_tid++;
    m_data.name = "thread-" + std::to_string(m_data.tid);

    auto                  &reg = registry_ref();
    const std::scoped_lock lock{reg.mutex};
    reg.live.push_back(this);
}

inline lane::~lane() {
    auto                  &reg = registry_ref();
    const std::scoped_lock lock{reg.mutex};
    std::erase(reg.live, this);
    reg.finished.push_back(std::move(m_data));
}

inline void lane::set_name(std::string_view name) {
    m_data.name = name;
}

inline void lane::push(const char *name, char phase) {
    m_data.events.push_back(event{.name = name, .phase = phase, .ts = now_us()});
}

inline void lane::append_json(std::string &out) const {
    m_data.append_json(out);
}

inline void lane::clear() {
    m_data.events.clear();
}

inline void lane_data::append_json(std::string &out) const {
    out += R"({"name":"thread_name","ph":"M","pid":1,"tid":)";
    out += std::to_string(tid);
    out += R"(,"args":{"name":")";
    out += name;
    out += "\"}},\n";

    for (const auto &event : events) {
        out += R"({"name":")";
        out += event.name;
        out += R"(","ph":")";
        out += event.phase;
        out += R"(","ts":)";
        out += std::to_string(event.ts);
        out += R"(,"pid":1,"tid":)";
        out += std::to_string(tid);
        out += "},\n";
    }
}

inline auto current_lane() -> lane & {
    static thread_local lane instance;
    return instance;
}

} // namespace detail

inline scope::scope(const char *name) noexcept : m_name(name) {
    detail::current_lane().push(m_name, 'B');
}

inline scope::~scope() {
    detail::current_lane().push(m_name, 'E');
}

inline void set_thread_name(std::string_view name) {
    detail::current_lane().set_name(name);
}

inline auto flush(std::string_view path) -> bool {
    auto       &reg  = detail::registry_ref();
    std::string json = "[\n";
    {
        const std::scoped_lock lock{reg.mutex};
        for (auto *lane : reg.live) {
            lane->append_json(json);
            lane->clear();
        }
        for (const auto &data : reg.finished)
            data.append_json(json);
        reg.finished.clear();
    }
    if (json.ends_with(",\n"))
        json.resize(json.size() - 2);
    json += "\n]\n";

    const std::string file_name{path};
    std::FILE        *file = std::fopen(file_name.c_str(), "w");
    if (file == nullptr)
        return false;
    const auto written = std::fwrite(json.data(), 1, json.size(), file);
    const bool closed  = std::fclose(file) == 0;
    return written == json.size() && closed;
}

} // namespace llco::trace
