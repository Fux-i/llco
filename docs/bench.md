# 性能基线

这份文档只回答一个问题：**改动前后，这台机器上的 llco 变快了还是变慢了。** 它不是对外可引用的绝对性能数字——WSL2 的 TSC 档位、宿主调度、频率调节都会让同一份二进制在不同时间跑出 10% 以上的差异。

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