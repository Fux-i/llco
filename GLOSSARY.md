# llco

llco 是一个 C++23 协程编排框架：用协程写 off-hot-path 的多任务流程，由 engine 和 scheduler 决定这些流程跑在哪些线程上。这份词汇表固定我们讨论这套系统时使用的词。

## 执行与调度

**engine**:
一个单线程执行上下文，拥有一个待 resume 的句柄队列，一次只推进一个协程，直到它挂起。
_Avoid_: executor, worker, event loop

**manual engine**:
不拥有线程、由调用方逐步驱动的 engine，用于确定性测试。
_Avoid_: fake engine, mock engine

**scheduler**:
进程内唯一的组件，拥有多个 engine、并按放置策略把任务交给其中一个；`scheduler::init(n)` 建池并可重建（ADR-0010）。
_Avoid_: dispatcher, thread pool, balancer

**acceptor**:
唯一执行 accept、把新连接交给 scheduler 放置的协程；llco 里它跑在 engine 0 上。
_Avoid_: listener, accept thread

**placement policy**:
scheduler 决定"这个任务放进哪个 engine"的规则。当前只有一种实现——轮转（round robin）——直接写在 `scheduler` 里，没有可替换的接口（ADR-0005）。
_Avoid_: strategy, load balancing algorithm

**cross-engine resume（跨 engine 恢复）**:
把一个挂起协程的后续执行交给另一个 engine 的动作，入口是 `co_await resume_on(e)`。
_Avoid_: hop, migration, switch, transfer

## 协程与任务

**task**:
惰性协程类型的实例，代表一件尚未开始、可被 resume 的工作。
_Avoid_: coroutine, job, future

**TaskMaker**:
返回 `task` 的可调用物（协程 lambda、工厂函数）；`run` / `sync_run` 用它约束入参，`Task` 判定一个类型是不是 `task<T>`。
_Avoid_: task factory, producer, callable

**TaskBatch**:
可被消费的 `task` 容器：右值 range，元素是 `task<T>`。`SizedTaskBatch` 另要求 `sized_range`，因为 `sync_run` 要在提交前知道批次大小。
_Avoid_: task list, group, collection

**父协程 (parent)**:
一个协程结束时把控制权交回去的那个协程，也就是 `co_await` 它的那个协程；顶层任务没有父协程，控制权交回 `noop_coroutine`。
_Avoid_: 续体, continuation, caller, 子协程

**symmetric transfer**:
协程结束时把控制权直接交回父协程（或 `noop_coroutine`），而不回到 resume 的调用栈。
_Avoid_: tail call, handoff, trampoline

**detached task**:
没有 await 者、跑完自毁的任务。
_Avoid_: fire-and-forget, background task

**add**:
把一个协程句柄放进 engine 的队列，不转移所有权。
_Avoid_: push, enqueue

**spawn**:
把一个 task 交给 engine 并转移所有权。
_Avoid_: submit, launch, start

**submit**:
把一个 task 交给 scheduler，由它挑选 engine。
_Avoid_: dispatch, route

**sync_wait**:
阻塞当前线程直到某个 task 结束，并取回它的结果。
_Avoid_: block_on, wait_for

## 唤醒与背压

**blocking point**:
engine 线程唯一允许阻塞的调用位置：`read(eventfd)`。
_Avoid_: idle wait, sleep point

**wakeup**:
生产者写 eventfd，让阻塞在 `read(eventfd)` 上的 engine 循环继续运行；IO 完成通知也写同一个 fd，两者共用一个阻塞点。
_Avoid_: doorbell, signal, interrupt

**queue-full policy**:
队列满时 engine 采取的行为。
_Avoid_: full policy, overload policy, backpressure mode

**backpressure**:
下游处理不过来时向上游传回的"满了"信号。
_Avoid_: throttling

## 同步

**event**:
一次性广播原语：`set` 之后所有等待者恢复，各自拿到一份值的拷贝；同步层的基元。
_Avoid_: condition variable, flag

**latch**:
计数 + event：归零时 set 一次，唤醒所有等待者。
_Avoid_: counter, barrier

**when_all / when_any**:
用 latch / event 写的组合写法，不是组件；when_any 的 event 必须共享（未完成的任务还会引用它）。
_Avoid_: join, race

## 可观测性与 IO

**trace**:
记录任务在各线程上起止时刻的事件流。
_Avoid_: log, profile, telemetry

**lane（线程轨道）**:
trace 中一条线程对应的一行时间轴。
_Avoid_: 泳道, swimlane, row

**io adapter**:
把"提交 IO 请求、取回完成事件"抽象出来的接口，engine 只认识它。
_Avoid_: backend, driver, io engine

## 工程

**config.hpp**:
集中声明当前构建所选模块别名与编译期常量的头文件。
_Avoid_: settings, options header