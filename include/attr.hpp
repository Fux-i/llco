#pragma once

#define INLINE __attribute__((always_inline))

#define AWAIT_HINT nodiscard("Don't forget to co_await")
#define DISCARD_HINT nodiscard("Don't discard this")
#define ALIGN alignas(64)

#define CLASS_NO_COPY_MOVE(classname)                                                              \
    classname(classname &&)                 = delete;                                              \
    classname(const classname &)            = delete;                                              \
    classname &operator=(classname &&)      = delete;                                              \
    classname &operator=(const classname &) = delete;

#define CLASS_MOVE_ONLY(classname)                                                                 \
    classname(classname &&)                 = default;                                             \
    classname &operator=(classname &&)      = default;                                             \
    classname(const classname &)            = delete;                                              \
    classname &operator=(const classname &) = delete;
