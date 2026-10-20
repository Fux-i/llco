# 施工路线图

这份路线图按"一步只加一个东西"来切：每个里程碑都能单独跑、单独看到输出、单独提交一次 commit。每个里程碑都**测试先行**：先把会失败的用例写进去，确认它失败的原因正是"这个功能还不存在"，再动手实现。

**谁写哪一步**：不必全部手写。建议至少亲手写 **M1（队列语义）、M5（线程循环）、M6（ring + eventfd）** 这三步——它们是这个框架真正难的地方；其余步骤可以先让我写、你 review，或者反过来。

**节奏约定**

1. 写测试 → 跑，看它**因为正确的理由**失败（不是编译不过，而是断言不成立；编译不过说明接口还没定，先补声明）。
2. 实现 → 跑，绿。
3. 跑一个 demo，人眼确认行为（这一步别跳，它是"阶段性成果"的来源）。
4. commit。

**里程碑一览**

| # | 里程碑 | 依赖 | 产出 |
|---|---|---|---|
| M0 | 骨架与构建 | — | 能编、能 ctest |
| M1 | 队列适配 + manual engine | M0 | 单线程确定性调度 |
| M2 | spawn / detached / 异常 | M1 | 所有权转移 |
| M3 | resume_on（单 engine hop） | M1 | 显式 hop |
| M4 | trace 模块 | M1 | 泳道图 |
| M5 | threaded engine + eventfd 门铃 + sync_wait | M1–M4 | 真线程、空闲不占 CPU |
| M6 | 接环：每 engine 一个 ring + 共享 eventfd | M5 | 完成事件能唤醒 engine |
| M7 | 门铃优化 + 漏唤醒回归 | M6 | 少一次系统调用 |
| M8 | scheduler（多 engine） | M5–M7 | 线程池 |
| M9 | 性能基线 bench | M6 | 一组可对比的数字 |
| M10 | 同步原语（latch → when_all/when_any → event） | M8 | 编排能力 |
| M11 | io_uring 读写（read/write awaiter） | M6 | 真 IO |
| M12 | TCP connector / echo | M11 | 网络组件 |

---

## M0 · 骨架与构建

**目标**：恢复"能编译 + 能跑测试"的基线，并把目录与文件约定落地。当前这棵树链接不过（`undefined reference to 'main'`）。

**改动**：`CMakeLists.txt`（重写）、`.gitignore`、`include/coro.hpp` → `include/llco/task.hpp`、`include/attr.hpp` → `include/llco/attr.hpp`、`tests/test_transfer.cpp`（改成 gtest 用例）、新增 `examples/` 与 `bench/` 的最小目标、**删除 `src/` 目录**（含空的 `engine.cpp`）。

**先写的失败测试**：`transfer.nested_three_levels`（断言 top/middle/leaf 的执行顺序与返回值 22）与 `transfer.void_chain`。此时它们甚至链接不过——这就是红。

**实现顺序**

1. 迁移并改名：`coro.hpp` → `include/llco/task.hpp`，`attr.hpp` → `include/llco/attr.hpp`；include 改成 `#include <llco/task.hpp>`。
2. `CMakeLists.txt`：`FetchContent` 拉 googletest（固定 tag + `GIT_SHALLOW`）；`add_library(llco INTERFACE)` + `target_include_directories` + `target_compile_features(cxx_std_23)`；clang 下把 `-stdlib=libc++` 挂到 INTERFACE。
3. `enable_testing()` + 每个 `tests/*.cpp` 一个可执行文件 + `gtest_discover_tests`。
4. 删掉 `-fcoroutines`（clang 22 会报 unused 警告）；删 `src/`，从构建里移除原来的 `main` 可执行目标。
5. `examples/` 与 `bench/` 各留一个能编的最小目标。

**完成标志**：`cmake -S . -B build && ninja -C build && ctest --output-on-failure` 全绿。

**坑**：FetchContent 首次 configure 需要网络，产物进 `build/_deps`；别把旧 `src/coro.cpp` 留在树里。

---

## M1 · 队列适配层 + manual engine

**目标**：单线程、无系统调用、确定性地把一批句柄按 FIFO 跑完。

**改动**：`include/llco/queue.decl.hpp` + `queue.impl.hpp`、`engine.decl.hpp` + `engine.impl.hpp`、`config.hpp`、`tests/test_engine.cpp`。

**先写的失败测试**

- `engine.manual_runs_handles_in_fifo_order`
- `engine.add_fails_when_full`（按 `QUEUE_CAPACITY` 填满，第 N+1 个返回 false）
- `engine.drain_returns_count`

**实现顺序**

1. `queue.decl.hpp`：`queue_backend` 别名指向 `atomic_queue::AtomicQueue2<T, Cap>`，外面包一层二十行的适配器（`try_push` / `try_pop` 转发）；engine 的成员直接写 `queue<std::coroutine_handle<>>`，用哪种队列一目了然，将来换实现只改 `queue_backend` 这一处。
2. `config.hpp`：`inline constexpr unsigned QUEUE_CAPACITY = 8192;`（先手写，M0 的 CMake 变量以后再接）。
3. `engine`（具体类，不是模板）：成员一个 `queue<std::coroutine_handle<>> m_queue`，`stopped` 标志留给 M5；manual 模式不建线程、不建 ring。
4. `bool try_add(handle)` / `void add(handle)` / `bool run_once()` / `std::size_t drain()`。
5. 测试用一个最小的协程工厂产出句柄（或直接复用 `task<>`）。

**完成标志**：`ctest -R engine` 全绿；`examples/engine_manual.cpp` 打印三层嵌套的执行顺序。

**坑**：用 `AtomicQueue2` 而不是 `AtomicQueue`（空句柄会 assert/hang）；容量会被向上取整到 2 的幂。

---

## M2 · spawn / detached / 异常

**目标**：engine 接管 `task` 的所有权，跑完自毁；异常不逃逸、不静默。

**改动**：`task.hpp`（加 `release()`、promise 加 detached 位）、`engine.decl.hpp` + `engine.impl.hpp`（`spawn`、exception handler）、测试。

**先写的失败测试**

- `task.release_transfers_ownership`（release 之后 `~task` 不再 destroy）
- `spawn.runs_and_destroys_frame`（配 ASan/LSan 跑）
- `spawn.exception_goes_to_handler`（默认 handler 会终止，测试里替换成计数 handler）

**实现顺序**

1. `promise_base` 加 `bool m_detached`；`final_awaiter` 里若 detached 就 `destroy()`（destroy 之后不得再碰 promise 任何成员）。
2. `task::release()` 返回句柄并把内部句柄置空（`[[nodiscard]]`）。
3. `engine::spawn(task<T>&&)`：release → 置 detached → `try_add`；满载则销毁帧并返回 false（终局，不能重试——ADR-0004）。
4. `engine::set_exception_handler(std::function<void(std::exception_ptr)>)`，默认打印 + `std::terminate`。

**完成标志**：`ctest -R spawn` 全绿；`ASAN_OPTIONS=detect_leaks=1` 下无泄漏、无二次 destroy。

**坑**：detached 自毁只能发生在最后一个触碰该帧的地方；`spawn` 出来的任务不能同时被 `sync_wait` 等。

---

## M3 · resume_on（单 engine hop）

**目标**：`co_await resume_on(e)` 把当前协程的句柄投进目标 engine，而不是内联继续。

**改动**：`include/llco/executor.decl.hpp`（`Executor` 概念，不拆 impl）、`include/llco/resume_on.hpp`（模板 awaiter + 工厂，单文件）、`tests/test_resume_on.cpp`、`examples/resume_on.cpp`、伞头文件。

**先写的失败测试**

- `resume_on.hops_through_queue`（manual engine：`run_once()` 之后函数体停在 await 点没继续，`drain()` 之后才继续，用计数断言）
- `resume_on.full_queue_continues_inline`（队列满时内联继续，钉住 ADR-0004 的协程内路径）

**实现顺序**

1. `executor.decl.hpp`：

```cpp
template <class E>
concept Executor = requires(E& e, std::coroutine_handle<> h) {
    { e.try_add(h) } -> std::same_as<bool>;
};
```

只要求 `try_add` 返回 `bool`（不放松到 `convertible_to`，也不要求 `noexcept`）；概念名按 `.clang-tidy` 的 `ConceptCase` 用 CamelCase。

2. `resume_on.hpp`：`resume_on_awaiter<E>` 持 `E&`；`await_ready()` 返回 false；`await_suspend(h)` 用对称传递表达"进得去就挂起，进不去就继续"：

```cpp
auto await_suspend(std::coroutine_handle<> h) noexcept -> std::coroutine_handle<> {
    return m_executor.try_add(h) ? std::noop_coroutine() : h;   // 满了就返回自己 = 立即继续
}
```

3. 工厂 `resume_on(E&) -> resume_on_awaiter<E>`，约束写作 `template <Executor E>`。概念在实例化时才检查，所以 `resume_on.hpp` 不 include `engine.decl.hpp`，一行 engine 类型都不用知道。

4. 跨线程版本留到 M8（那时才有多个 engine）。

**完成标志**：`ctest -R resume_on` 全绿；`examples/resume_on` 打印"await 点 → drain 时 resume"。

**坑**

- **绝不要**在 `await_suspend` 里直接调 `h.resume()`（此时协程还没真正挂起完）；返回句柄才是正确姿势。
- 满载分支在单线程里**不能**靠 `spawn` 制造：从队列里跑出来的协程在 hop 之前已经腾出一个槽，`try_push` 必然成功。测试要手动 `resume()` 驱动那个协程，或者等 M5 用跨线程生产者撞满队列。

---

## M4 · trace 模块

**目标**：默认零开销的 Chrome Trace JSON 埋点，能在 Perfetto / chrome://tracing 里看到"每线程一条泳道 + 嵌套任务块"。

**改动**：`trace.decl.hpp` + `trace.impl.hpp`、`engine.impl.hpp` 埋点、`config.hpp` 开关、带 trace 的 demo。

**先写的失败测试**：`trace.emits_lane_metadata_and_paired_slices`（打开开关跑一个小 demo，断言 JSON 里有 `ph:"M"` 的 `thread_name`、每条 `B` 都有对应 `E`、同 tid 时间戳单调不减）。

**实现顺序**

1. `trace`：编译期开关（`LLCO_TRACE`）；`begin(name)` / `end()`；每线程一个缓冲，flush 时拼成一个 JSON 数组；`ts` 用微秒。
2. engine 循环埋点：`resume` 前 `B`、返回后 `E`。`resume` 返回时整条 inline 链可能已经跑完或全部挂起——这正是我们要的 lane 语义。
3. （可选）用 `trace_processor` 的 SQL 做自动断言，让 trace 变成回归测试的一部分。

**完成标志**：`LLCO_TRACE=1 ./build/examples/engine_manual` 生成 `trace.json`，浏览器打开可见泳道。

**坑**：同一 tid 上时间戳必须单调不减、区间必须严格嵌套（Perfetto 规范）；跨线程续跑要用 async `b`/`n`/`e`（共享 `id`+`scope`）或 flow `s`/`t`/`f`，不能硬用 `B`/`E`。

---

## M5 · threaded engine + eventfd 门铃 + sync_wait

**目标**：engine 拥有一个 `std::jthread`，空闲时阻塞在 `read(eventfd)` 上（此时还没有 ring）；`sync_wait` 让调用线程等到结果。

**改动**：`engine.decl.hpp` / `engine.impl.hpp`（线程、`start`/`stop`、循环）、`sync_wait.decl.hpp` + `sync_wait.impl.hpp`、测试。

**先写的失败测试**

- `engine.thread_runs_added_tasks`
- `engine.idle_thread_does_not_spin`（空闲 100ms 内循环迭代次数 == 1）
- `engine.stop_wakes_idle_thread`（stop 之后 join 在毫秒级返回）
- `sync_wait.returns_value_and_propagates_exception`

**实现顺序**

1. engine 持有一个 eventfd；`notify()` 写 8 字节，`wait()` `read` 并排空。
2. 循环：`while (!stop_requested()) { drain_queue(); wait(); }`；`std::stop_callback` 里写一次 eventfd。
3. `start()` / `stop()`（`request_stop` + `join`）；析构 = stop。
4. `sync_wait(task<T>&&)`：`std::binary_semaphore` + 结果槽；root promise 在 `final_suspend` 里 set。

**完成标志**：demo 起 4 个 engine、提交 1000 个任务全部完成；空闲时 `top` 里 CPU ≈ 0。

**坑**：read 之后要循环排空队列再决定是否回去 wait（否则丢唤醒）；`stop_callback` 里只能做很小的动作；`sync_wait` 的阻塞是调用线程的事，与 engine 的 blocking point 无关。

---

## M6 · 接环：每 engine 一个 ring + 共享 eventfd

**目标**：把 io_uring 接进 engine 的等待循环。每个 engine 建**一个** ring（不重复构造），用 `io_uring_register_eventfd_async` 把该 engine 的 eventfd 绑进 ring，使"完成事件"和"任务门铃"共用一个阻塞点；此时还不做真正的读写，只用 NOP / 超时验证唤醒。

**改动**：`include/llco/io/io_uring.decl.hpp` + `io_uring.impl.hpp`、`engine.impl.hpp`（循环里加"排空 CQ"这一步）、demo。

**先写的失败测试**

- `io.ring_created_once_per_engine`（重复 `start`/`stop` 不泄漏 ring）
- `io.nop_completion_wakes_engine`（提交一个异步超时，engine 在到期时被唤醒）
- `io.doorbell_wake_with_empty_cq_is_harmless`（只写门铃、没有完成，engine 醒来后照常回去阻塞）
- `io.two_completions_one_notification`（一次通知排空两个 CQE）

**实现顺序**

1. `io_uring` 封装：`init` / `register_eventfd_async(efd)` / `submit(sqe)` / `peek_cqe` / `cqe_seen`；析构 `queue_exit`。
2. engine 循环改为：**排空任务队列 → 提交/排空 CQE → 两处都空才 `read(efd)`**。
3. 提交之后立刻 peek 一次：`_ASYNC` 注册下，同步完成的 op 不会发通知。
4. manual engine 不建 ring（测试里没有环也能跑）。
5. 完成回调先只做"记账 + 打印"，真正的 IO 放 M11。

**完成标志**：demo 提交一个 200ms 超时，打印"提交 → 阻塞 → 200ms 后唤醒 → 收到完成"。

**坑**：**只允许 `read(efd)` 阻塞**，全程不调 `wait_cqe`；ring 只归本线程（liburing 非线程安全）；一次通知可能对应多个完成；醒来也可能什么都没有；`read` 遇到 `-EINTR` 要重试。

---

## M7 · 门铃优化 + 漏唤醒回归

**目标**：只在线程真的睡着时才写 eventfd。

**改动**：`engine.impl.hpp`（`sleeping` + double check）、测试。

**先写的失败测试**：`doorbell.no_lost_wakeup_under_churn`（多个生产者疯狂注入 + 一个 engine；先用可注入时序的测试把窗口固定下来，再加一个 10 万次压力版）。

**实现顺序**

1. 生产者：push 之后 `if (sleeping.exchange(false)) write(efd, 1);`
2. 消费者：`sleeping = true` → **再检查一次队列与 CQ** → 有货就 `sleeping = false` 继续；没货才 `read`。
3. 计数断言：所有任务都跑了（不能少），可选地断言唤醒次数没有暴涨。

**完成标志**：系统调用次数下降（`strace -c -f` 或自己计数）；压力测试无丢失。

**坑**：这个 double check 的顺序写反就是漏唤醒——偶发、只在负载下出现。这个坑值得亲手踩一次，再用测试钉死。

---

## M8 · scheduler（多 engine）

**目标**：N 个 engine + 放置策略 + 满载重试 + 跨 engine hop。

**改动**：`scheduler.decl.hpp` + `scheduler.impl.hpp`、放置策略、`engine.impl.hpp`（暴露粗粒度负载计数）、测试。

**先写的失败测试**

- `scheduler.round_robin_distributes`
- `scheduler.retries_on_full_engine`
- `scheduler.submit_returns_false_when_all_full`
- `resume_on.cross_engine_hop_changes_thread`（记录 `std::this_thread::get_id()`）

**实现顺序**

1. 策略接口（runtime 多态）：`unsigned pick(std::span<engine*>)`；实现 round_robin 与 least_loaded。
2. `scheduler(unsigned n = std::thread::hardware_concurrency())` 拥有 `std::vector<std::unique_ptr<engine>>`，构造即启动。
3. `submit(task<T>&&)`：`pick` → `try_add` → 失败就换下一个（最多绕一圈）→ 全满返回 false。
4. 跨 engine 续跑走 M3 的 `resume_on`；trace 里用 flow 事件表示接力。

**完成标志**：demo 打印每个任务落在哪个 engine / 哪条线程；trace 里能看到跨引擎接力。

**坑**：scheduler 是普通实例，不是单例（ADR-0002）；engine 数量为 1 时满载路径也要可测。

---

## M9 · 性能基线 bench

**目标**：一组可重复的数字，作为后面所有优化的参照。

**改动**：`bench/`（自写计时，不引第三方）。

**先写的失败测试**：bench 不做 TDD，改为"断言 bench 能跑完、输出格式稳定"。

**实现顺序**

1. `bench/timer.hpp`：`steady_clock` + `rdtsc`。
2. 指标：单次 add 延迟、跨线程唤醒延迟（提交→resume 的时间）、spawn+完成吞吐、`sync_wait` 往返、任务切换成本（resume 到下一次 resume）。
3. 固定 CSV 输出，便于前后对比。

**完成标志**：`./build/bench/llco_bench` 输出表格；结果落到 `docs/bench.md`（可选）。

**坑**：WSL2 的时钟/频率不稳，用中位数 + 多次重复；micro-bench 的结论不要当绝对真理。

---

## M10 · 同步原语

**目标**：latch → when_all / when_any → event。

**改动**：`latch.decl/impl.hpp`、`when_all.decl/impl.hpp`、`when_any.decl/impl.hpp`、`event.decl/impl.hpp`、测试。

**先写的失败测试**

- `latch.blocks_until_zero`、`latch.cross_engine_countdown`
- `when_all.waits_for_all_children_concurrently`（断言总耗时接近最长者，而不是之和）
- `when_any.resumes_on_first_completion`
- `event.broadcasts_to_all_waiters`

**实现顺序**

1. `latch`：原子计数 + 等待者句柄集合；归零时逐个 resume。**它是所有"等一等"的基元。**
2. `when_all`：先启动全部子任务（每个都 `resume_on`），再用 latch 等齐。
3. `when_any`：第一个完成者触发父协程，其余结果先丢弃（取消留到以后）。
4. `event` = 一次性广播（本质是 latch(1)）。

**完成标志**：一个 fan-out/fan-in demo：父任务启动 8 个子任务，`when_all` 等齐后汇总打印。

**坑**：`when_all` **不能**写成顺序 `co_await a; co_await b;`（那是串行）；等待者集合先用 `std::vector` + mutex，性能留给 bench 之后再动。

---

## M11 · io_uring 读写

**目标**：真正的 `read` / `write` awaiter，不引入 IO 虚接口（只有一个后端）。

**改动**：`include/llco/io/io_uring.decl/impl.hpp`（加 `read/write`）、`examples/`。

**先写的失败测试**

- `io.read_completes_with_bytes`（管道或 eventfd 上读）
- `io.read_wakes_only_the_right_coroutine`（两个协程各等一个 op，CQE 按 `user_data` 分发）
- `io.failed_op_propagates_negative_errno`

**实现顺序**

1. `co_await io.read(fd, std::span<std::byte>)`：取 sqe、填 buffer+user_data（指向 awaiter）、提交、挂起。
2. CQE 到达：用 `user_data` 找回 awaiter → 写结果 → resume 父协程。
3. 错误：`cqe->res` 为负即 errno，转成 `std::error_code` 或异常（二选一，先选异常）。
4. 提交队列满（`-EBUSY` / SQ 满）时的等待策略：先提交、再 peek，必要时小自旋。

**完成标志**：demo 用管道做一次读，打印收到的字节与耗时。

**坑**：sqe 与 buffer 的生命周期必须覆盖到 CQE 回来（awaiter 必须活在帧里）；`user_data` 指针不能悬空；一个 op 只能对应一个 awaiter。

---

## M12 · TCP connector / echo server

**目标**：网络组件，验证 IO 层可用。

**改动**：`include/llco/net/`、`examples/echo_server.cpp`、测试。

**先写的失败测试**：`tcp.connect_and_echo`（本机 loopback）。

**实现顺序**：socket/bind/listen → accept awaiter → connect awaiter → read/write 循环 → 关闭语义（半关闭、错误码映射）。

**完成标志**：`./build/examples/echo_server` 起服务，`nc` 能连通并回显。

**坑**：fd 生命周期与 engine 关停的配合（别在回调里丢 fd）。

---

## 延伸（不排期）

work stealing、协作式取消（`stop_token` 传播）、优先级队列、手写 MPMC 队列替换 atomic_queue、队列满时的阻塞策略、P2300 兼容层、TCP 之外的网络功能。
