# engine 的唯一阻塞点是 ring 绑定的 eventfd，阻塞在 read()

每个 engine 在 `start()` 时创建一个 io_uring 实例（每个 engine 一个、不重复构造，并且只由本 engine 线程使用），再用 `io_uring_register_eventfd` 的 `_ASYNC` 变体把该 engine 的 eventfd 绑进 ring。engine 线程在没有任务、也没有完成事件时阻塞在 `read(eventfd)` 上：内核在 CQE 完成时写这个 fd，任何线程投递任务后也写同一个 fd——于是"完成通知"和"跨线程门铃"共用一个阻塞点。醒来之后分别排空任务队列与 CQ ring（`io_uring_peek_cqe` / `io_uring_cqe_seen`，全程不调 `io_uring_wait_cqe`）。

## 实测（2026-10-19，本机 WSL2，liburing）

- 提交一个 300ms 超时后阻塞在 `read(efd)`：300ms 时返回，counter=1，peek 到 `-ETIME` 完成——**内核通知能唤醒 `read`**。
- 阻塞期间另一线程 `write(efd)`：100ms 时 `read` 返回，但 peek 得到 `-EAGAIN`——**门铃有效，且"醒来"不等于"有完成"**。
- 两个完成（100ms / 200ms）只产生一次通知（counter=1），一次 peek 取出两个完成——**通知是提示，必须排空 CQ**。

## Considered Options

- `std::condition_variable_any`：更快（futex、零系统调用），但阻塞点的形状与 IO 阶段不一致，等于把同一个东西写两遍、教两遍。
- 阻塞在 `io_uring_wait_cqe`：注册给 ring 的 eventfd 是单向的 kernel→user 提示，往它写入**不会**唤醒 `wait_cqe` 的等待者（实测），跨线程注入任务因此失效；而且 `wait_cqe` 会返回 `-EINTR` 且不重试。
- epoll + 双 eventfd（一个收完成、一个收门铃）：也能工作，但多一个阻塞原语、多一个必须排空的 fd，而单 fd 已被实测证明够用。

## Consequences

- ring 与 eventfd 都只能由 engine 线程触碰；跨线程投递只走"队列 + `write(efd)`"，绝不跨线程 `io_uring_submit`。
- 每次提交后要立刻 peek 一次：`_ASYNC` 注册下，同步完成的 op 不会发通知。
- `read` 可能返回 `-EINTR`，循环重试；醒来后必须先排空（队列与 CQ）再回去阻塞。
- 判定"通知是提示而非计数"：醒来但两处都空，属于正常情况，直接回去阻塞。
