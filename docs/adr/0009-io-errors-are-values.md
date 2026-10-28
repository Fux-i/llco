# io 的稳态错误走返回值，不抛异常

M11 起 `io::*` 的 awaiter 在 `await_resume` 里抛 `std::system_error`。这把两类完全不同的事塞进了同一个机制：**对端重置、EPIPE、ECANCELED 这类每次 I/O 都可能发生的错误**，和 **ring 起不来、fd 非法这类不变量失败**。前者在服务端是常态，后者一辈子一次。

## 1. 边界：内核返回值可能是负数的稳态操作 → 值

| awaiter | 之前 | 现在 |
|---|---|---|
| `rw_awaiter` | `std::size_t`，负数抛 | `expected<std::size_t, error_code>` |
| `accept_awaiter` | `int`，负数抛 | `expected<int, error_code>` |
| `connect_awaiter` | `void`，负数抛 | `expected<void, error_code>` |
| `cancel_awaiter` | `bool`，非 0 / 非 -ENOENT 抛 | `expected<bool, error_code>` |
| `listener::accepting` | `connection` | `expected<connection, error_code>` |

五个 `await_resume` 都加 `noexcept`。用 `error_code` 而不是 `std::errc`：它和原来的 `std::system_error{rc, std::system_category()}` 是同一套语义，迁移是机械的；24 字节、无分配。

`cancel` 的三态保留：`true` 取消成功、`false` 没有在飞 op（不是错误）、`has_value() == false` 才是错误。注意 `expected<bool>` 的 `operator bool` 表示"有值"而不是那个 bool，判断要写 `ASSERT_TRUE(r); EXPECT_TRUE(*r);`。

## 2. 边界之外仍然抛

启动期与不变量失败继续抛：`ring` 构造、`register/unregister_eventfd`、`submit`、`tcp_socket` / `listen_tcp` / `local_port`、`eventfd`、`scheduler` 满、`task::result()` 的"返回值没设过"。全库 16 处 `throw` 只改了 4 处。

## 3. 未捕获的异常 = 终止，这是设计

四条逃逸路径实测（clang 22 / libc++，2026-10-28）：

| 场景 | 输出 | 退出码 |
|---|---|---|
| detached 任务体里 throw | `llco: unhandled exception in detached task: ...` | 134 |
| `await_suspend` 里 throw（`submit()` 失败） | 同上 | 134 |
| `sync_wait` 的任务 throw、外面没接 | 运行库的 terminate 消息 | 134 |
| 用户显式 catch | 正常返回，库继续可用 | 0 |

所以使用方**不需要写 try/catch**：稳态错误是值，剩下的异常只可能来自启动期或程序 bug，终止是预期结果。默认 handler 先打印 `what()` 再 `terminate`；想"每连接隔离"的人可以用 `engine::set_exception_handler` 换掉它——实测换成只打印的 handler 后，出事的任务被丢弃、engine 继续跑。

## 4. 顺带量到的体积

`.eh_frame` / `.gcc_except_table` 只降了几十到两百字节（`echo_server`：clang 8312→8252 / 2052→1948，gcc 6360→6312 / 1338→1278）。因为 `await_suspend` 仍然调 `next_sqe()` → `submit()`，而 `submit()` 会抛，每个 await 点的 landing pad 还在。要再降得让 `submit()` 不抛（失败即终止），那是独立的一步，没有测量支撑之前不做。

## Consequences

- 测试断言要显式看 `has_value()`。`std::expected` 从 `T` 隐式构造，所以**旧写法的 `co_return <值>` 仍能编译**——类型系统挡不住"忘了返回错误"，只有测试挡得住。
- `connection::recv/send` 的返回类型不变（仍是 awaiter），改的只是 `await_resume`。
- `task` 的 `exception_ptr` 通道保留：它现在只承载启动期错误和程序 bug。
