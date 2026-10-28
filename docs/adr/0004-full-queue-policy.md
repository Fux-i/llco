# 队列满时对外返回失败，resume_on 回退为内联继续

队列有界。外部提交路径（`engine.add` / `scheduler.submit`）在 `try_push` 返回 false 时把失败交回调用者：engine 自己既不重试也不终止进程，换一个 engine 重试是 scheduler 的职责。协程内部的注入路径（`resume_on`）无法返回失败——协程已经挂起——因此约定：`try_push` 失败就由当前线程直接继续执行它（对称传递返回自己的句柄）。永不阻塞、永不丢失，也不引入无界后备队列。

## Considered Options

- engine 满载即 `terminate`（只有一个 engine 时尤其如此）：把容量问题升级成进程死亡，而且让单线程调试时唯一的失败路径直接杀进程。
- `resume_on` 路径用阻塞 push：atomic_queue 的阻塞 `push` 是纯自旋，没有超时、没有取消、没有 closed 状态，在关停路径上不可取消，且可能死锁（目标 engine 的消费者正在等源 engine）。
- `resume_on` 路径失败后丢进无界 fallback 队列：有界就名存实亡。
- 引入 `queue_full_policy = reject | block | terminate` 配置位：在真的需要 block 之前不做这层抽象。

## Consequences

- `add` / `submit` 的返回值必须被检查（atomic_queue 的 `try_push` / `try_pop` 是 `[[nodiscard]]`）。
- 满载路径按 `QUEUE_CAPACITY` 填满写成确定性测试：容量是构建级常量，engine 不按容量参数化，测试里的断言必须容量相对。
- `spawn` 在转移所有权之后没有退路：队列满时销毁已移交的帧并返回 false。这个失败是**终局**的，所以需要重试的放置层（M8）不用 `spawn`，改用 `engine::try_adopt`：它接受一个已经 `release()` 的帧，失败时**不**销毁、所有权仍在调用方，于是同一个句柄可以换 engine 再试，整圈都满才销毁并返回 false。
- `engine` 必须比它 spawn 出去的任务活得久（异常处理函数指针指向 engine 的成员），关停时由 M5 的循环负责排空。