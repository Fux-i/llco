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

**改动**：`include/llco/trace.decl.hpp` + `trace.impl.hpp`（`namespace llco::trace`，不是类/单例）、`engine.impl.hpp` 埋点（`#if LLCO_TRACE` 包住）、`config.hpp` 给 `LLCO_TRACE` 默认值、`CMakeLists.txt` 的 `option(LLCO_TRACE)`、`tests/test_trace.cpp`、`examples/trace.cpp`、`scripts/trace.sh`（一条命令跑完配置 → 编译 → 运行 → 校验）、`CMakePresets.json`（`default`/`asan`/`trace`/`gcc` 四棵树）、`CMakeLists.txt`（可执行文件统一输出到 `build/bin`）。

**先写的失败测试**

- `trace.pairs_slices_and_names_lanes`（一次 `flush` 后断言 JSON 是数组、有 `ph:"M"` 的 `thread_name`、`B`/`E` 各两个且严格嵌套、ts 单调不减）
- `trace.keeps_one_lane_per_thread`（另起一个 `std::thread` 打一个 slice，线程退出后 flush 仍要看到两条泳道 → 钉住"线程退出时把缓冲搬进 finished 列表"）
- `trace.engine_emits_resume_slices`（`#if LLCO_TRACE` 包住，**只在 trace-on 树里存在**，默认树不编译）

**实现顺序**

1. `trace`：`namespace llco::trace` + `set_thread_name` / `flush` 自由函数；`trace::scope` 是 RAII（构造发 `B`、析构发 `E`），`B/E` 只作为 detail 存在，从外面无法写出不配对的区间。
2. 每线程一个 `thread_local lane`（events = `{const char* name, char phase, int64 ts}`，名字必须是字面量，不拷贝字符串）；lane 构造时把自己登记进全局注册表、析构时把数据 move 进 `finished` 列表。注册表在堆上**故意 leak**：主线程的 thread_local 可能在静态析构之后才析构。
3. `flush(path)`：加锁遍历 `live` + `finished` 拼 JSON，写完**排空**（清空 events、清空 finished），返回是否写成功。`ts` 用 `steady_clock` 微秒。
4. engine 埋点：`run_once()` 里 `resume()` 前后各一个 `trace::scope{"resume"}`，整段包在 `#if LLCO_TRACE` 里——默认构建里 `run_once()` 一行都不多。
5. 另开一棵 trace-on 树验证埋点：`cmake --preset trace`（**必须整棵树一致**，见坑），由 `CMakePresets.json` 而不是命令行记住 `-DLLCO_TRACE=ON`。

**完成标志**：`./scripts/trace.sh [demo]` 一条命令跑完（`cmake --preset trace` → 编译该 demo → 用 `build/_trace/traces/<demo>.json` 作为输出路径运行 → `json.tool` 校验 → 打印路径），`demo` 缺省为 `trace`；四个 preset 的 `ctest` 全绿（default 16、asan 16、trace 17、gcc 16）；拖进 ui.perfetto.dev 能看到 `resume` 包住 `work` 的嵌套块。

**坑**

- **开关必须整棵树一致**：header-only + INTERFACE 库下，给单个 TU 单独加 `-DLLCO_TRACE=1` 会让同一个 `inline` 函数在不同 TU 里定义不同 → ODR 违反（IFNDR）。只允许 CMake option 全局打开。
- **`flush` 必须排空**：同一进程里多个用例共用主线程的 lane，不排空的话后一个用例会看到前面用例的事件（这条是写测试时才发现的）。
- 同一 tid 上时间戳必须单调不减、区间必须严格嵌套（Perfetto 规范）；跨线程续跑要用 async `b`/`n`/`e`（共享 `id`+`scope`）或 flow `s`/`t`/`f`，不能硬用 `B`/`E`——M8 跨引擎接力时才需要。
- 本机**没有** `trace_processor`（也没有 `perfetto` Python 模块），所以自动断言只能用 `python3` + `json` 做结构校验；可视化靠人工把 `trace.json` 拖进 ui.perfetto.dev。

---

## M5 · threaded engine + eventfd 门铃 + sync_wait

**目标**：engine 拥有一个 `std::jthread`，空闲时阻塞在 `read(eventfd)` 上（此时还没有 ring）；`sync_wait` 让调用线程等到结果。

**改动**：`engine.decl.hpp` / `engine.impl.hpp`（`start`/`stop`、循环、eventfd）、`task.hpp`（promise 的 completion 钩子）、`sync_wait.decl.hpp` + `sync_wait.impl.hpp`、`tests/test_threaded_engine.cpp`、`tests/test_sync_wait.cpp`、`examples/threaded_engine.cpp`、`CMakeLists.txt`（测试 30s 超时）。

**先写的失败测试**

- `threaded_engine.runs_added_tasks`（100 个任务全部跑完）
- `threaded_engine.idle_does_not_spin`（空闲 100ms 后 `loop_count() == 2`）
- `threaded_engine.stop_wakes_idle_thread`（`stop()` 在 50ms 内返回）
- `threaded_engine.destructor_drains_queued_tasks`（析构 = stop = 跑完已入队的任务）
- `sync_wait.returns_value`、`sync_wait.propagates_exception`
- `sync_wait.waits_for_cross_thread_hop`（任务里 `co_await resume_on(e)`，sync_wait 必须等到 engine 线程跑完才返回）

**实现顺序**

1. `engine` 增 `std::jthread m_thread`、`std::atomic<int> m_event_fd`（初值 -1）、`std::size_t m_loops`；`start()` 里 `eventfd(0, EFD_CLOEXEC)` 失败抛 `std::system_error`，然后起线程。没调 `start()` 的引擎仍然是 manual 的。
2. 循环：`std::stop_callback` 里只写一次门铃；主体是 `for (;;) { ++m_loops; drain(); if (stop_requested()) return; wait(); }`。`wait()` 阻塞读 8 字节、`-EINTR` 重试；`notify()` 写 8 字节，失败视为"循环已经醒着"。
3. `try_add` `push` 成功后 `notify()`（M7 再优化成"只在睡着时敲门"）。
4. `stop()`：`joinable()` 时 `request_stop` + `join`，**然后由调用线程再 `drain()` 一次**，兜住"循环在 stop 之前已经越过 drain、之后还有 push"的竞争窗口；最后 close fd。`~engine` = `stop()`。
5. `sync_wait`：`promise_base` 加 `std::binary_semaphore *m_completion`，`final_awaiter::await_suspend` 里**先** `release()` 再处理 continuation/detached；`sync_wait` 在调用线程内联 `resume()` 启动 root，然后阻塞在 semaphore 上，任务中途 `resume_on` 到别的线程也能被唤醒。

**完成标志**：`examples/threaded_engine` 起 4 个 engine、1000 个任务全部完成（实测 ~14ms），空闲 200ms 的进程 CPU 时间 ≈ 0（实测 0 ms）；`sync_wait` 跨引擎拿到 7。

**坑**

- **`~engine` 里不能执行任务**：异常处理函数指针指向 `*this`，那时已经在析构。所以排空必须发生在 `stop()`，对象还活着的时候。
- **`stop()` 之后再提交 = 永久滞留**：fd 已关，没人再唤醒循环。约定"stop 之后不得再提交"。
- manual 引擎（没 `start()`）销毁时队列里未跑的 spawned 帧仍然泄漏——M2 的遗留问题，留到 M8 的 scheduler 关停路径统一解决。
- `std::thread::join()` 对非 joinable 线程会抛 `system_error`，所以 `stop()` 必须用 `joinable()` 守卫，不能只靠 `request_stop()` 的宽松语义（第一次实现就踩了）。
- 阻塞原语让测试有可能挂死：`gtest_discover_tests(... PROPERTIES TIMEOUT 30)`。
- `persistent` 细节：`stop_callback` 在循环体第一行构造，若 `start()` 后立刻 `stop()`，标准保证回调在注册时立即执行一次，所以不会丢唤醒。

---

## M6 · 接环：每 engine 一个 ring + 共享 eventfd

**目标**：把 io_uring 接进 engine 的等待循环。每个 engine 建**一个** ring（不重复构造），用普通 `io_uring_register_eventfd` 把该 engine 的 eventfd 绑进 ring（ring 只建一次，跨 `start`/`stop` 复用），使"完成事件"和"任务门铃"共用一个阻塞点；此时还不做真正的读写，只用 NOP / 超时验证唤醒。

**改动**：`include/llco/io/io_uring.decl.hpp` + `io_uring.impl.hpp`、`engine.impl.hpp`（循环里加"排空 CQ"这一步）、demo。

**先写的失败测试**

- `io.ring_created_once_per_engine`（重复 `start`/`stop` 不泄漏 ring）
- `io.nop_completion_wakes_engine`（提交一个异步超时，engine 在到期时被唤醒）
- `io.doorbell_wake_with_empty_cq_is_harmless`（只写门铃、没有完成，engine 醒来后照常回去阻塞）
- `io.two_completions_one_notification`（一次通知排空两个 CQE）

**实现顺序**

1. `io::ring` 封装：构造即 `queue_init`；`register_eventfd(efd)` + `unregister_eventfd()`（一个 ring 只吃一个注册，重复注册 `-EBUSY`）；`sqe()` / `submit()` / `reap()`（内部 `peek_cqe` + `cqe_seen`）；`completions()` 是给测试与 demo 看的记账；析构 `queue_exit`。
2. engine 循环改为：**排空任务队列 → 提交/排空 CQE → 两处都空才 `read(efd)`**。
3. 提交之后立刻 peek 一次：普通注册下同步完成也会写 fd，但通知只是"有完成"的提示，CQ 必须自己排空。
4. manual engine 不建 ring（测试里没有环也能跑）。
5. 完成先只记账（`ring::completions()`），打印留给 demo；真正的 IO 放 M11。

**完成标志**：demo 提交一个 200ms 超时，打印"提交 → 阻塞 → 200ms 后唤醒 → 收到完成"（实测 `woke after 202 ms, completions: 1`）。

**坑**：**只允许 `read(efd)` 阻塞**，全程不调 `wait_cqe`；ring 只归本线程（liburing 非线程安全）；一次通知可能对应多个完成；醒来也可能什么都没有；`read` 遇到 `-EINTR` 要重试。
- **`_ASYNC` 变体不能用**：内核 `io_eventfd_signal()` 里的 `if (ev_fd->eventfd_async && !io_wq_current_is_worker()) goto out;` 决定它只在 io-wq worker 上下文发通知；超时（hrtimer）与管道 `read`（task work）实测一次都不唤醒，阻塞点会永久睡死（ADR-0003 已订正）。
- **重复注册 `-EBUSY`**：`stop()` 必须先 `unregister_eventfd` 再 close fd，否则同一个 ring 第二次 `start()` 直接抛异常。

---

## M7 · 门铃优化 + 漏唤醒回归

**目标**：只在线程真的睡着时才写 eventfd。

**改动**：`engine.impl.hpp`（`m_sleeping` + double check + `wakeup_count()`）、`tests/test_doorbell.cpp`、`examples/threaded_engine.cpp`（打印 doorbells）。

**先写的失败测试**

- `doorbell.a_busy_engine_is_not_rung`（睡着时的那次 push 必须响一次；随后 50 次落在任务运行期间的 push 一次都不许响）
- `doorbell.no_lost_wakeup_under_churn`（4 生产者 × 25000 次 push 全部跑完；门铃数 < 总数的 1%）

**实现顺序**

1. 生产者：push 之后 `if (!m_sleeping.exchange(false)) return;` 再写 fd——只有真睡着才敲门。
2. 消费者：`m_sleeping = true` → **再看一眼队列与 CQ**（只读检查，不运行任务）→ 有货就 `m_sleeping = false` 继续；没货才 `read`。
3. `stop_requested()` 在**置位之后**还要再查一次：停机回调也走 `notify()`，若 `request_stop()` 落在"查 stop"与"置位"之间，回调看到的是未睡着、不写 fd，线程就睡死了。
4. `wakeup_count()` 给测试与 demo 看真实门铃次数。

**完成标志**：系统调用次数下降（`strace -c -f` 或自己计数）；压力测试无丢失。

- 实测（本机，`strace -c -f ./build/bin/threaded_engine`，1000 个任务）：`write` 1006 → 9，`read` 1009 → 14。
- 饱和注入（4 生产者 × 25000 次 push）：**1 次门铃**。不饱和时（主线程慢慢 push 250 次）仍有 ~88 次——生产慢于消费时线程确实在睡，该响就得响。
- M5 的用例全部照旧：空闲 100ms 后 `loop_count() == 2`，`stop()` 50ms 内返回。

**坑**

- **顺序写反就是漏唤醒**：`m_sleeping = true` 必须在"再排空一次"**之前**。写反以后窗口只有几条指令，本机把顺序故意反转、跑 10 万次压力版**仍然通过**——压力测试抓不住它。所以顺序目前靠注释与 review 保；能测的是门铃契约（睡着才响、响了不丢）与饱和计数。要确定性钉死这个窗口，得加时序注入点（`wait()` 前跑一个测试回调）或把这段状态机抽成可单测的小类，两者都还没做，属于已知缺口。
- **内核完成通知不经过 `m_sleeping`**：被 completion 唤醒时标志仍是 true，此刻到达的 push 会多敲一次门（多一次 `write`、多跑一圈空循环），无害，但门铃数会略高于"必须的次数"。
- **`m_sleeping = false` 只能在 `wait()` 返回之后**：提前置 false 等于整个阻塞期都对外声称"醒着"，生产者不会再敲门——直接睡死。

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
