# TCP 连接的放置归 scheduler，不由内核

> 修订（ADR-0012）：listener 的归属仍是 engine 0，但构造与放置都不再需要 engine 对象——`net::listener listener{port}` 内部取 engine 0 的 ring，`run_at(0, ...)` / `sync_run_at(0, ...)` 代替 `engine_at(0).spawn(...)`。

M13 起，一条连接在 accept 之后由 `scheduler::submit` 决定落在哪个 engine：唯一的 listener 在 engine 0 上，acceptor 用 `listener::accept_fd()` 拿到原始 fd，把 `serve(fd)` 交给 scheduler 的轮转放置；`serve` 在目标 engine 上跑起来后才构造 `net::connection`（ADR-0012 起是 `net::connection{fd}`，取当前 engine 的 ring），此后绑定。llco 不使用 `SO_REUSEPORT`。

这不是省事的默认，而是查完同类实现后的选择：Seastar 当前默认就是“单 listener + 跨核 handoff”（`smp::submit_to`），并且把 reuseport 硬关掉（`posix_reuseport_available()` 直接 `return false`，注释是 “reuseport currently leads to heavy load imbalance”）；Netty、muduo、Folly/Proxygen、Thrift、Node cluster 的 RR 也都是“单 acceptor + 在目标 loop 上首次执行”。

## Considered Options

- **每 engine 一个 listener + SO_REUSEPORT**（Envoy 默认、glommio、monoio）：零 handoff，但内核对 4-tuple 哈希，实测 64 条连接在 2 个 listener 上是 31/33 或 26/38（换源端口基址就变）；放置被内核拿走，`scheduler` 的轮转在 TCP 路径上失效；还要处理 N 个 socket 的端口复用（两个都 bind port 0 会拿到不同端口，必须先 bind 0 拿端口再显式绑）、N 份 backlog、N 处 cancel，以及关 listener 时自己队列里未 accept 的连接被 RST。留作 accept 速率成为瓶颈时的后继方案。
- **共享 listener，每 engine 一个 accept**（nginx 默认、Node `SCHED_NONE`）：省 socket，但要脱掉 `listener` 的 ring 绑定，并引入“每 engine 恰好一个 pending accept”的隐式不变量；只有所有人始终挂着一个 accept 才严格轮转，慢重新提交会倾斜（实测 3:1），一条 stale accept 会把本该属于别人的连接误接收且永远交付不了。两者都有官方倾斜记录（nginx 源码注释 “most of the connections are handled by the first worker process”、Node “over 70% … in just two processes”）。
- **连接迁移**（Asio 的共享 `io_context` + strand、Tokio 的 work stealing）：与 ring 的线程亲和直接冲突，要跨 ring 提交完成、按 fd 取消未完成 op、再保证同一连接的读写顺序；Asio/Tokio 靠“没有每线程 ring”才成立。

## Consequences

- 新增 `listener::accept_fd()`；`accept()` 保留给单 engine 用法。
- 网络层不再假设“listener 所在的 engine 就是连接所在的 engine”；`serve` 里 `current_engine()` 非空是契约（只经 `scheduler::submit` 投递）。
- acceptor 单线程，且与 engine 0 的服务共用一条线程：accept 速率有上限，连接数很多时是已知瓶颈。
- 每连接一次跨线程入队 + 唤醒（队列满按 ADR-0004 丢弃并关 fd）；唤醒针对的是阻塞在 `read(eventfd)` 的循环，不是 `io_uring_wait_cqe`。
- 关停仍只 cancel listener（`io::cancel`，必须在 engine 0 上提交）；仍打开的连接（idle recv）会让 `scheduler::stop()` 等到它们结束，这是 M11/M12 的既有语义，连接级取消另立里程碑。
- trace 里一条连接跨两条线程轨道：accept 在 `engine-0`，echo 在目标 `engine-i`。
