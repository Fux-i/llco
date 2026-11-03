# 性能基线

这份文档只回答一个问题：**改动前后，这台机器上的 llco 变快了还是变慢了。** 它不是对外可引用的绝对性能数字——WSL2 的 TSC 档位、宿主调度、频率调节都会让同一份二进制在不同时间跑出 10% 以上的差异。

唯一的例外是第二节：网络 echo 对照在固定的裸机上测，那节的数字可以引用，但必须连机器条件一起引用。

采集方式：

```bash
taskset -c 2 ./build/bin/llco_bench > bench.csv
```

CSV 到 stdout，`#` 行是环境信息；`--quick` 是 ctest 里的冒烟模式（只校验格式，数字无意义）。

## 环境（2026-10-23 17:25）

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 9 7940H（16 线程） |
| 内核 | 6.18.40.1-microsoft-standard-WSL2 |
| 编译器 | clang 22.1.8 + libc++ |
| 构建 | Release（`-O3 -DNDEBUG`） |
| TSC | 三次运行均为 3629.11 MHz |
| 负载 | load average ≈ 0.8（不是空载机器） |

## 基线（三次运行）

| 指标 | 测量条件 | run 1 | run 2 | run 3 | 中位数 |
|---|---|---|---|---|---|
| `add_latency` | manual engine 直接调用 `try_add`（含唤醒检查的提前返回） | 5.38 ns | 5.40 ns | 5.37 ns | **5.38 ns** |
| `wakeup_latency` | 已阻塞的 engine：提交 → 协程体第一行 | 40.7 µs | 39.1 µs | 37.7 µs | **39.1 µs** |
| `spawn_task` | 提交 + 跑完一个空任务（含满队列重试） | 109.7 ns | 116.1 ns | 116.7 ns | **116.1 ns** |
| `sync_wait_round_trip` | 建帧 + 首次 resume + 信号量往返 | 32.8 ns | 32.9 ns | 34.0 ns | **32.9 ns** |
| `resume_on_cost` | 同一 engine 上一次 `resume_on` 队列往返 | 9.0 ns | 11.3 ns | 11.2 ns | **11.2 ns** |

吞吐换算：`spawn_task` 中位数 ≈ **8.6 M tasks/s**（单 engine）；`resume_on_cost` ≈ **89 M 次恢复/s**（同 engine，无唤醒）。

## 怎么读这组数字

- **同一次 VM 状态内交替跑 A/B**，各三到五次，比较中位数比值。不要拿"上周的 bench.csv"直接对比——本次会话里 `tsc_hz` 从 3992 MHz 漂到 3629 MHz，说明 WSL2 的 TSC 档位本身会变，跨状态的微秒级数字不可比。
- **先看 `tsc_hz` 是否一致**：两行 `# tsc_hz=` 不同，结论直接作废。
- **小于 10–20% 的差异当噪声**。同一份代码连续三次的 `resume_on_cost` 已经差了 25%。
- **唤醒延迟依赖 vCPU 进入多深的空闲状态**：本表测量条件是"每次采样前先让循环阻塞 1 ms"。连续提交、循环来不及进入阻塞时，同一指标会掉到 0.35–7 µs——那是"未进入阻塞"的路径，不是唤醒的成本。改测量条件就改数字，所以测量条件固定在 bench 里。
- **`p10` 比中位数更接近"无干扰"**：WSL2 的调度噪声只会把延迟推高，p10 是下限估计；中位数是典型值；p90 用来判断尾部抖动。
- **ASan / trace 构建不采集基线**：`bench_smoke` 只在四套构建里校验输出格式，数字只在默认 Release 构建下有意义。

## 指标测量条件细节

- `add_latency`：manual engine（不 `start()`），每轮 8191 次 `try_add`，整批用一次 `rdtsc` 包住再除以次数——`rdtsc` 单次约 8 ns，逐次打点会把 5 ns 的 push 翻倍。
- `wakeup_latency`：每次采样前让 engine 线程重新阻塞（1 ms），主线程用 `steady_clock`（跨线程共用一个时钟）打点、自旋等协程体写回时间戳；包含 eventfd 写、内核唤醒、**WSL2 的 vCPU 空闲退出**，所以比裸机上的量级大。
- `spawn_task`：单 engine、10 万个空任务；队列满时重试提交，失败开销计入成本。
- `sync_wait_round_trip`：调用线程上重复 `sync_wait(noop())`。
- `resume_on_cost`：一个协程反复 `co_await resume_on(同一个 engine)`，每次 resume 处取 `rdtsc`，相邻差值即一次队列往返 + 恢复。
## 网络 echo 对照（裸机，2026-11-03）

这一节回答另一个问题：**同一台机器上，llco 的 echo 服务器与一个手写的单线程 epoll echo 相比，吞吐与延迟差多少、差在哪里。** 数字可以与机器条件一起引用，不能外推。

### 机器与固定变量

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 5 PRO 6650U（6 核 12 线程，15 W） |
| 内核 | 6.18.43（NixOS 26.05），`isolcpus=2-11 nohz_full=2-11 rcu_nocbs=2-11` |
| 编译器 | clang 22.1.8 + libc++，Release（`-O3 -DNDEBUG`），`nix develop` 环境 |
| 频率 | `scaling_governor=powersave`（改它需要 sudo，见失真点 1） |
| 绑核 | 服务端固定在物理核 2（2 个 engine 时再加核 4）；负载生成器固定在核 8 与核 10 |
| 协议 | 裸 TCP echo，4 KB 消息，两端 `TCP_NODELAY`，客户端 ping-pong（`depth=1`） |
| 采样 | 每格 3 次、A/B 交替；每次 warmup 2 s + 测量 10 s，取中位数 |
| 客户端 | [bench/echo_load.cpp](../bench/echo_load.cpp)，测量窗口内逐字节校验回显 |

驱动：[scripts/bench_echo.sh](../scripts/bench_echo.sh)，原始行在 `bench-results/results.csv`。

### 结果（三次中位数）

| 服务器 | 连接 | 吞吐 | p50 | p99 | 服务端 CPU | 客户端 CPU |
|---|---|---|---|---|---|---|
| epoll 单线程（对照） | 1 | 46.1 k msg/s | 20.7 µs | 24.0 µs | 31.1% | 42.3% |
| llco，1 engine | 1 | 43.8 k msg/s | 21.7 µs | 25.9 µs | 41.2% | 40.6% |
| llco，2 engines | 1 | 43.6 k msg/s | 21.8 µs | 25.8 µs | 40.9% | 40.6% |
| epoll 单线程（对照） | 64 | 117.4 k msg/s | 262 µs | 5.28 ms | 60.9% | 34.6% |
| llco，1 engine | 64 | 116.4 k msg/s | 265 µs | 5.28 ms | 66.7% | 34.6% |
| llco，2 engines | 64 | 115.2 k msg/s | 268 µs | 5.29 ms | 69.9% | 34.3% |

CPU 列是"占一个核的百分比"：服务端取 `/proc/<pid>/stat` 的 utime+stime 增量除以墙钟，客户端取 `getrusage` 增量除以（墙钟 × 线程数）。

### 怎么读

- **单连接这六行里，差的是框架成本，不是内核成本。** 两者都在 21 µs 上下、llco 慢约 1 µs；但服务端 CPU 从 31.1% 涨到 41.2%——每条消息多烧约 10 个百分点的一个核（≈2.2 µs/消息）。这 2.2 µs 就是协程 resume、有界队列入队、eventfd 唤醒检查的税。
- **64 连接这行谁都没赢。** 两边都停在约 117 k msg/s，差 1%。每消息一次 `recv` + 一次 `send`，客户端与服务端合计约 11 µs CPU/消息——平台（loopback + 两次系统调用）先饱和，框架的差别被淹掉。
- **5.3 ms 的 p99 是客户端的排队，不是服务端延迟。** 64 连接分给 2 个客户端线程、每条连接 ping-pong，排队 ≈ 在途连接数 × 单条往返。
- **第二个 engine 不提速，还多花 3 个百分点 CPU。** 这个配置下瓶颈不在调度：多一个 engine 只多一份唤醒与队列。

### 不要拿这组数字宣称什么

- 不能说"io_uring 比 epoll 快/慢"：这一版每消息一次 one-shot `recv` + 一次 `send`，没有 batching、没有 multishot、没有 registered files，两边走的是同一套系统调用路径。
- 不能说"llco 支持 X M msg/s"：4 KB + loopback + 这颗 15 W CPU 的上限就是约 117 k msg/s，换机器、换消息大小要整表重测。
- 不能与 nginx / Seastar 横向比较：协议（裸 echo vs HTTP）与功能（无 TLS、无超时、IPv4 only）都不同。

### 失真点（诚实清单）

1. **`scaling_governor=powersave`**：切到 performance 需要 sudo，自动化跑不了。频率仍随负载 boost，但没有锁频，重复运行的方差比锁频大。
2. **C-state 没有关**：`max_cstate=1` 同样需要 root。单连接行的 21 µs 里含 CPU 从浅睡眠唤醒的时间。
3. **`net.ipv4.tcp_no_metrics_save=0`**：跨运行的路由/拥塞参数缓存没被清，A/B 交替只能部分抵消。
4. **客户端与服务端同机**：loopback 上客户端提交的上下文里就完成了服务端的输入处理，服务端 `sys` 时间会被低估——这也是 64 连接行两边数字几乎相同的原因之一。
5. **两套实现都不做 batching**：这是"同为每消息两次系统调用"的对等比较，不是"io_uring 的最佳形态"。
