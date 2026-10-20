#pragma once

#define LLCO_INLINE __attribute__((always_inline))

#define LLCO_AWAIT_HINT nodiscard("Don't forget to co_await")
#define LLCO_DISCARD_HINT nodiscard("Check the return value")
#define LLCO_ALIGN alignas(64)

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
