# 性能基线

这份文档记两类测量：

- **运行时微基准**：`llco_bench`，同机 A/B，用来判断一次改动是变快还是变慢。数字不可跨机比较。
- **网络 echo 对照**：同一台裸机上的三种实现，可以与机器条件一起引用，但不能外推。

## 运行时微基准

`./build/bin/llco_bench` 每个指标输出一行，`#` 开头是环境行（CPU、编译器、构建类型、`tsc_hz`）。

| 指标 | 测量条件 |
|---|---|
| `add_latency` | manual engine 直接调用 `try_add`（含唤醒检查的提前返回） |
| `wakeup_latency` | 已阻塞的 engine：提交 → 协程体第一行 |
| `spawn_task` | 提交 + 跑完一个空任务（含满队列重试） |
| `sync_wait_round_trip` | 建帧 + 首次 resume + 信号量往返 |
| `resume_on_cost` | 同一 engine 上一次 `resume_on` 队列往返 |

裸机实测（AMD Ryzen 5 PRO 6650U，clang 22 + libc++，Release，`taskset -c 2,4`）：

| 指标 | 中位数 | p10 | 说明 |
|---|---|---|---|
| `add_latency` | 5.04 ns | 5.01 ns | 纯入队成本 |
| `wakeup_latency` | 6.45 µs | 6.41 µs | 阻塞到被唤醒的端到端 |
| `spawn_task` | 1.75 µs | 1.50 µs | **含唤醒**：两次提交之间循环睡下时，每条任务都要付一次 eventfd 唤醒 |
| `sync_wait_round_trip` | 34.8 ns | 34.8 ns | 同线程 |
| `resume_on_cost` | 9.28 ns | 9.19 ns | 同 engine 重排队 |

怎么读：

- `add_latency` 与 `resume_on_cost` 是纯队列成本，纳秒级；`spawn_task` 是 1.75 µs，因为它测的是"提交一个任务并等它跑完"，空任务跑完循环就会睡下，下一次提交要付唤醒。生产者压满（连续提交）时这个数字会掉到纳秒级。
- 只做同机 A/B：同一次运行里交替跑两个版本、各三到五次，比中位数比值。差异小于 10–20% 当噪声；`p10` 更接近"无干扰"的下限。
- ASan / trace 构建只校验输出格式（`bench_smoke`），不采集基线。

## 网络 echo 对照

三个实现，同一份消息路径：

| 实现 | 说明 | 代码 |
|---|---|---|
| epoll | 单线程水平触发 epoll，每消息一次 `recv` + 一次 `send` | [bench/epoll_echo.cpp](../bench/epoll_echo.cpp) |
| raw io_uring | 与 llco 相同的 one-shot op 序列，但零框架（无协程、无队列、无 scheduler） | [bench/raw_uring_echo.cpp](../bench/raw_uring_echo.cpp) |
| llco | 1/2/3 个 engine，一连接一协程 | [bench/echo_bench.cpp](../bench/echo_bench.cpp) |

分三层是为了把账切开：epoll→raw 的差是"io_uring + eventfd 这套机制"的钱，raw→llco 的差才是"框架"的钱。

### 机器与固定变量

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 5 PRO 6650U（6 核 12 线程，15 W），单 NUMA 节点 |
| 内核 | 6.18.43（NixOS 26.05），`isolcpus=2-11 nohz_full=2-11 rcu_nocbs=2-11` |
| 编译器 | clang 22.1.8 + libc++，Release（`-O3 -DNDEBUG`），`nix develop` 环境 |
| 频率 | `scaling_governor=performance`、`energy_performance_preference=performance`、`platform_profile=performance`、`scaling_min_freq` 顶到 4.59 GHz |
| 网络 | `net.ipv4.tcp_no_metrics_save=1`；`TCP_NODELAY` 两端开启 |
| perf | `kernel.perf_event_paranoid=1`（syscall 计数需要） |
| 绑核 | 物理核配对 (0,1)(2,3)(4,5)(6,7)(8,9)(10,11)：延迟行服务端 1 核（2）、负载端 3 核（4,6,8）；饱和行服务端 1–3 核、负载端固定 2 线程 |
| 协议 | 裸 TCP echo，4 KB 消息，客户端 ping-pong（`depth=1`） |
| 采样 | 每格 3 次、A/B 交替；每次 warmup 2 s + 测量 10 s，取中位数 |

驱动：[scripts/bench_echo.sh](../scripts/bench_echo.sh)，每次运行的原始行写到 `bench-results/results.csv`（该目录在 `.gitignore` 里，不入库）。

### 单连接：延迟与每消息成本

| 实现 | 吞吐 | p50 | p99 | 服务端 CPU | 系统调用/消息 |
|---|---|---|---|---|---|
| epoll | 46.0 k msg/s | 20.61 µs | 24.23 µs | 31.0% | 3.00 |
| raw io_uring | 44.1 k msg/s | 21.60 µs | 25.43 µs | 40.6% | 5.00 |
| llco，1 engine | 43.5 k msg/s | 21.66 µs | 26.96 µs | 41.1% | 5.00 |

### 64 连接：饱和

| 实现 | 吞吐 | p50 | p99 | 服务端 CPU |
|---|---|---|---|---|
| epoll | 118.7 k msg/s | 259.5 µs | 5.28 ms | 61.1% |
| raw io_uring | 118.3 k msg/s | 260.7 µs | 5.28 ms | 67.3% |
| llco，1 engine | 118.2 k msg/s | 261.1 µs | 5.28 ms | 66.9% |
| llco，2 engines | 117.6 k msg/s | 262.3 µs | 5.28 ms | 69.6% |
| llco，3 engines | 115.8 k msg/s | 258.0 µs | 5.28 ms | 70.8% |

### 归因：每条消息花在哪

`perf stat` 包住服务端进程（[scripts/bench_syscalls.sh](../scripts/bench_syscalls.sh)）：

| 实现 | 系统调用/消息 | 构成 |
|---|---|---|
| epoll | 3.00 | `epoll_wait` 1 + `recvfrom` 1 + `sendto` 1 |
| raw io_uring | 5.00 | `io_uring_enter` 2 + `read(eventfd)` 3 |
| llco | 5.00 | `io_uring_enter` 2 + `read(eventfd)` 3 |

### 三条结论

1. **框架本身几乎免费。** raw io_uring 与 llco 的每消息系统调用完全一致（5.00），p50 差 0.06 µs（21.60 → 21.66），服务端 CPU 差 0.5 个百分点（40.6% → 41.1%）。协程 resume、有界队列入队、scheduler 放置合起来只值这个量级。
2. **贵的是 io_uring + eventfd 这套机制。** epoll 每消息 3 次系统调用，io_uring 每消息 5 次（2 次 `io_uring_enter` 提交 + 3 次 `read(eventfd)` 唤醒），代价是 p50 +1.0 µs、服务端 CPU +9.6 个百分点、吞吐 -5%。
3. **多 engine 会扩展，但前提是给够核、给够负载。** 三个 engine 各绑一核（`--cpus 2,4,6`）、六个负载端进程共 384 连接时：141 → 253 → 325 k msg/s（两次运行的中位数），服务端 CPU 58% → 113% → 173%，客户端 CPU 119% → 228% → 329%。

   两个条件缺一个就看不出扩展；早期版本记的"多 engine 不提速"，是这两条同时没做：
   - **不绑核**。这台机器带 `isolcpus=2-11`，隔离核没有周期性负载均衡，一个进程的所有 engine 线程会全部落在第一个核上（实测 4 个 engine 的 `last_cpu` 都是 2，`cpu2 busy=98%`，`cpu4/6/8` 空闲）。同一台机器上 4 个独立进程各绑一核聚合 282 k，1 个进程 4 个 engine 只有 145 k，差别只在"线程能不能落到别的核"。`--cpus` 就是为此加的（ADR-0013）。
   - **负载不够**。64 连接 + depth 1 + 2 个客户端线程时，客户端自己就是瓶颈：1 engine 已经把服务端 CPU 吃到 61%，2 engine 仍然只有 99%——第三块核拿不到活。要测扩展性，连接数与负载端进程都要跟着 engine 数走。

### 不要拿这组数字宣称什么

- 不能说"io_uring 比 epoll 快/慢"。这里测的是 one-shot op + eventfd 唤醒的实现，没有 batching、没有 multishot、没有 registered files。
- 不能说"llco 支持 X M msg/s"。4 KB + loopback + 这颗 15 W CPU，三引擎三核能到约 325 k msg/s，但这个数随客户端能给出的负载浮动；换协议、换消息大小、换机器都不成立。
- 不能与 nginx / Seastar 横向比较：协议与功能都不同。
- 64 连接行的 p99（5.28 ms）是客户端排队（64 连接 ÷ 2 客户端线程 × 单条往返），不是服务端尾延迟。

### 失真点

1. **客户端与服务端同机**：loopback 上客户端提交的上下文里就完成了服务端的输入处理，服务端 `sys` 时间被低估。
2. **`read(eventfd)` 占 3 次/消息**：recv 完成与 send 完成各唤醒一次，另一次是循环复查。这是实现选择，不是内核必需——`DEFER_TASKRUN` + multishot 可以省掉。
3. **`scaling_cur_freq` 在采样点读到 1.1 GHz**：那是两次突发之间 governor 请求的档位，不是执行频率。把 `scaling_min_freq` 顶到 4.59 GHz 后单连接 p50 只变化 0.3–0.7 µs，说明突发本身已经跑在高频。
4. **C-state 没有禁用**：这台机器没有 `processor.max_cstate` 参数；运行时用 `/dev/cpu_dma_latency` 约束，实测对 p50 无可测影响（两侧都是 20.6 µs），所以没有启用。
5. **两套 io_uring 实现都不做 batching**：这是"同一套系统调用路径"的对等比较，不是 io_uring 的最佳形态。
6. **不绑核时多 engine 的数字没有意义**：`isolcpus` 机器上所有 engine 线程挤在同一个核，测到的是"线程抢一个核"，不是调度器的结果。`scripts/bench_echo.sh` 的 llco 行现在都带 `--cpus`。
7. **扩展性数据是 2–6 个负载端进程喂出来的**：单进程客户端在这台机器上最多给出约 145 k msg/s，更高就要加生成器进程。这是偏移量，不是 llco 的上限。

### 复现

```bash
cmake --preset default && cmake --build --preset default
scripts/bench_echo.sh                  # 三种实现 × 两组负载 × 三次
sudo -v && scripts/bench_syscalls.sh   # 每消息系统调用数
```
