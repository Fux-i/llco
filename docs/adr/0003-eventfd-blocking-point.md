# engine 的唯一阻塞点是 ring 绑定的 eventfd，阻塞在 read()

每个 engine 在第一次 `start()` 时创建一个 io_uring 实例（每个 engine 一个，之后跨 `start`/`stop` 复用，不重复构造，并且只由本 engine 线程使用），再用 `io_uring_register_eventfd` 把该 engine 的 eventfd 绑进 ring。engine 线程在没有任务、也没有完成事件时阻塞在 `read(eventfd)` 上：内核在 CQE 完成时写这个 fd，任何线程投递任务后也写同一个 fd——于是"完成通知"和"跨线程唤醒"共用一个阻塞点。被唤醒之后分别排空任务队列与 CQ ring（`io_uring_peek_cqe` / `io_uring_cqe_seen`，全程不调 `io_uring_wait_cqe`）。

## 实测（2026-10-22，本机 WSL2 内核 6.18.40；liburing 头 2.10 / 运行库 2.5）

| 场景 | 注册方式 | 结果 |
|---|---|---|
| 200 ms 超时后阻塞在 `read(efd)` | `io_uring_register_eventfd` | 203 ms 返回，counter=1，peek 到 `res=-62`（`-ETIME`） |
| 200 ms 超时后阻塞在 `read(efd)` | `io_uring_register_eventfd_async` | **完全不通知**：poll 600 ms 超时、counter=0，之后 peek 仍能得到 `res=-62` |
| 空管道 `read`，另一线程 100 ms 后写入 | `io_uring_register_eventfd` | 107 ms 返回，`res=2` |
| 同上 | `io_uring_register_eventfd_async` | **完全不通知**：600 ms 无事件 |
| 同步完成的 `NOP`（`submit` 内完成） | `io_uring_register_eventfd` | `submit` 返回后 fd 立刻可读，counter=1，`res=0` |
| 两个超时（100 / 200 ms） | `io_uring_register_eventfd` | 一次 `read` 返回，counter=1，一次排空两个 CQE |
| 阻塞期间另一线程写 fd（无 pending op） | — | `read` 返回，peek 得到 `-EAGAIN` |
| ring 已注册一个 eventfd，再注册另一个 | — | `-EBUSY`（-16），必须先 `io_uring_unregister_eventfd` |
| 超时完成时刻单独观察 ring fd 与 eventfd | 两种注册 | ring fd 在 205 ms 时可读；`_ASYNC` 的 eventfd 始终不可读——内核确实完成了，只是跳过了这次通知 |

## `_ASYNC` 变体为什么不能用

上游 `io_uring/io_uring.c` 的 `io_eventfd_signal()` 里：

```c
if (ev_fd->eventfd_async && !io_wq_current_is_worker())
    goto out;
```

`_ASYNC` 只在**完成发生在 io-wq worker 线程里**时才写通知。超时由 hrtimer 完成、阻塞 `read` 靠 poll 唤醒，都不在 io-wq worker 上下文（本机实测这两种 op 一次都不唤醒），于是永远不发通知——阻塞点会永久阻塞。本 ADR 初稿写的"用 `_ASYNC` 变体"经实测是错的，改为普通注册。

## Considered Options

- **`io_uring_register_eventfd_async`**：本想省掉同步完成那一次写，但实测它把该唤醒的异步完成也一并省掉了（超时与管道 read 都不唤醒），排除。
- **`std::condition_variable_any`**：更快（futex、零系统调用），但阻塞点的形状与 IO 阶段不一致，等于把同一个东西写两遍、教两遍。
- **阻塞在 `io_uring_wait_cqe`**：注册给 ring 的 eventfd 是单向的内核→用户提示，往它写入**不会**唤醒 `wait_cqe` 的等待者，跨线程注入任务因此失效；而且 `wait_cqe` 会返回 `-EINTR` 且不重试。
- **epoll + 双 eventfd**（一个收完成、一个收唤醒）：也能工作，但多一个阻塞原语、多一个必须排空的 fd，而单 fd 已被实测证明够用。

## Consequences

- ring 与 eventfd 都只能由 engine 线程触碰；跨线程投递只走"队列 + `write(efd)`"，绝不跨线程 `io_uring_submit`。`engine::ring()` 也只该在 engine 线程上调用。
- 通知是"有完成"的提示而不是计数：被唤醒但 CQ 与任务队列都空，属于正常情况，直接回去阻塞。
- 普通注册对**同步完成**的 op 也会写 fd（`_ASYNC` 本来省掉的就是这一次写）；所以每次 `submit` 之后仍要 peek 一次再回去阻塞，这一步不花系统调用。
- 一个 ring 同时只能注册一个 eventfd（重复注册 `-EBUSY`）：`stop()` 先 unregister 再 close fd，`start()` 重新注册新 fd；ring 本体复用，不重建。
- `read` 可能返回 `-EINTR`，循环重试。