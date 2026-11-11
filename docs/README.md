# 文档索引

仓库的文档都在这里入口；正文各自成篇，本页只做导航。

## [architecture.md](architecture.md)

架构与决策依据：分层、依赖方向、一段任务的生命周期、五条关键机制、网络层、文件布局约定、工程约束，以及末尾的"已知边界"。

## [adr/](adr/)

每份 ADR 记录一个已经定下的决策与当时的取舍，编号顺序即决策顺序。

| 编号 | 标题 | 结论 |
|---|---|---|
| [0001](adr/0001-inline-parent-resume.md) | 控制权默认 inline 交回父协程，只有显式 resume_on 才入队 | `co_await` 子任务零队列往返，换线程必须显式写 `resume_on`。 |
| [0002](adr/0002-scheduler-is-an-instance.md) | scheduler 是普通实例，不提供全局单例 | 调度器不进类型系统、由使用者持有；已被 ADR-0010 推翻，保留原文记录当初的取舍。 |
| [0003](adr/0003-eventfd-blocking-point.md) | engine 的唯一阻塞点是 ring 绑定的 eventfd，阻塞在 read() | 完成通知与跨线程唤醒共用一个阻塞点，不需要 epoll。 |
| [0004](adr/0004-full-queue-policy.md) | 队列满时对外返回失败，resume_on 回退为内联继续 | 不阻塞、不丢帧：外部路径返回 false 由调度层换 engine 重试，协程内部内联继续。 |
| [0005](adr/0005-placement-is-round-robin.md) | 放置先硬编码 round-robin，不做策略抽象 | 一个原子游标决定起点，能否落地由 `try_adopt` 决定；出现第二种需求再抽接口。 |
| [0006](adr/0006-io-one-path.md) | io 的提交、完成与关停各只有一条路径 | SQE 由循环统一提交、CQE 在 `reap()` 内联恢复、`stop()` 等到 `running_io() == 0`。 |
| [0007](adr/0007-sockets-use-send-recv.md) | socket 走 send/recv，关停靠 cancel | `IORING_OP_WRITE` 在 socket 上会投递 SIGPIPE，pending accept 只能由 `io::cancel` 打断。 |
| [0008](adr/0008-connections-are-placed-by-the-scheduler.md) | TCP 连接的放置归 scheduler，不由内核 | 单 listener + 跨线程 handoff，不用 `SO_REUSEPORT`（内核哈希会让轮转失效）。 |
| [0009](adr/0009-io-errors-are-values.md) | io 的稳态错误走返回值，不抛异常 | 内核的负返回值映射成 `std::expected<..., error_code>`，只有启动期与不变量失败才抛。 |
| [0010](adr/0010-scheduler-is-a-singleton.md) | scheduler 是进程内单例 | 公开面全是静态函数，`init(n)` 可重建池；`init` / `stop` 不能与 `submit` 并发。 |
| [0011](adr/0011-task-is-move-only.md) | task 是 move-only | 拷贝删除、移动转移帧所有权，容器批量提交因此成立。 |
| [0012](adr/0012-run-at-pins-a-task.md) | run_at：把任务钉在一个 engine 上 | `submit_at` 不做沿环重试，满载或停机时抛 `std::runtime_error`。 |
| [0013](adr/0013-engine-affinity.md) | engine 可以绑定到一个核 | 亲和性归 engine，`init(count, cpus)` 显式传核表才绑；一个 engine 一个核，绑不上就退化为不绑。 |

## [roadmap.md](roadmap.md)

按里程碑（M0–M13）的施工记录：每个阶段的目标、改动、先行测试、完成标志与踩过的陷阱，取舍写在对应的里程碑里。

## [bench.md](bench.md)

性能基线：微基准与网络 echo 两组对照（epoll / raw io_uring / llco），采集命令、机器条件、指标口径与八条失真点。数字只用于同机对比，不是可引用的绝对性能。

## [examples/README.md](../examples/README.md)

示例导航：七个示例各自演示什么、用到哪些 API、在 trace 里该看哪条轨道。

## [GLOSSARY.md](../GLOSSARY.md)

术语表（仓库根目录）：engine、scheduler、acceptor 等词的固定含义与应当避免的叫法。
