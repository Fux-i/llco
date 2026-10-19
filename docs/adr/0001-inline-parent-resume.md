# 控制权默认 inline 交回父协程，只有显式 hop 才入队

`co_await` 一个子任务时，子协程完成的那一刻，控制权在同一个线程上直接交回父协程（symmetric transfer），不经过任何队列；只有代码显式写 `co_await resume_on(engine)` 时，协程才会把自己的句柄投进目标 engine 的队列、等它稍后取出来继续跑。这条路径就是我们现在已经实现的行为，也是 folly::coro 的 `Task`（final awaiter 直接返回 `continuationRef().getHandle()`，即父协程的句柄）与 cppcoro 的默认行为；把每个 await 都改成入队，会为每次挂起增加一次队列往返和重新调度，却换不到正确性。

## Considered Options

- 每次 await 都入队（重新调度）：换来公平、栈深度有界、天然的跨线程安全，代价是每次 await 一次队列往返，并把 engine 的语义从"跑到挂起为止"改成"每任务轮转一次"。
- folly 的 `co_withExecutor` / `viaIfAsync` 式包装器（永远 post）：语义上等价于我们的 `resume_on`，因此采纳这个形状，但作为 opt-in，而不是默认。
- 在 promise 里存 home executor、运行时比较"我是否已经在目标 executor 上"：folly 与 cppcoro 的这两条路径里都不存在这种比较（grep 均为零命中），没有先例可循，放弃。

## Consequences

- 一条协程链默认不换线程：同一个 engine 上的任务会一路跑到挂起。
- 跨 engine 的亲和性必须显式写出；不提供隐形修复。
- 因为默认 inline，深链会吃栈深；将来若需要防线长栈或公平性，补 `yield_on(engine)`，而不是改默认行为。
