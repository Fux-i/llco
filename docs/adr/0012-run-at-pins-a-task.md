# run_at：把任务钉在一个 engine 上

`run` / `sync_run` 只回答"提交"，不回答"放到哪"——它们走 scheduler 的轮转。需要"就在 engine i 上"的场合（唯一的 acceptor、关停时那一发 cancel、bench 里钉住一个 engine），之前只能写 `scheduler::engine_at(i).spawn(task)`。它有两个毛病：`spawn` 满载时销毁帧返回 `false`，示例里的 `(void)` 等于"没放上去，也不吭声"；以及 `engine_at` 把 engine 对象塞进了用户代码。

新增 `llco::run_at(index, ...)` / `sync_run_at(index, ...)`，各三个重载与 `run` / `sync_run` 一一对应；底层是新的 `scheduler::submit_at(index, task<T>&&) -> bool`——与 `submit` 的差别只有一处：不做沿环重试，只试指定的那个 engine。

## 三点约定

1. **pin 不重试**。`submit` 满了一整圈换 engine（ADR-0004），`submit_at` 不换：换了就不是 pin。代价是 engine i 的队列满时 `run_at(i, ...)` 抛，而同样的负载 `run` 会把同一个帧挪到别的 engine 成功。这是 pin 的应有代价，不是缺陷。
2. **失败一律抛**。`submit_at` 返回 bool，`run_at` / `sync_run_at` 按 `run` 家族的契约转成 `std::runtime_error`。`submit_at` 保留 `submit` 里的 `m_running` 检查——少了它就是"`stop()` 之后把帧塞进一个永远不再排空的队列"：`run` 会抛、`run_at` 会静默吞、`sync_run_at` 直接挂死。这正是不能直接用 `engine::spawn` 的原因。
3. **只收 index，不收到 `engine&`**。`resume_on` 收 `Executor&` 是因为它是协程里的挂起点，调用点通常已握着引用，而且它的概念约束不该绑到 scheduler；`run_at` 是提交入口，调用点在 bootstrap 里，手里只有一个数字。不为了对称给 `resume_on` 加 index 重载。

## Considered Options

- **`run_on(engine&, ...)`**：调用点仍要先 `scheduler::engine_at(i)` 拿引用，engine 对象留在示例里；listener 的归属还是靠人肉对齐，与 `run_at` 相比没有多出来的安全性。
- **让 listener 记住自己的 index 并暴露 getter**（`run_at(listener.index(), ...)`）：index 目前只有 0 这一种取值（ADR-0008 钉死"唯一的 listener 在 engine 0"），恒等于 0 的 getter 就是多余的；真要支持非 0 归属，`listener{ring, port}` 这条逃生口一直在，而 `index()` 对 manual engine 无定义（不在池里），只会变成一个撒谎的接口。
- **失败返回 bool 而不抛**：与 `run` 家族不一致，调用点必须自己 `if`，示例里的 `(void)` 会回来。
- **直接用 `engine::spawn`**：没有 `m_running` 检查，`stop()` 之后静默入队。

## Consequences

- 示例的 `main` 里不再出现 `engine_at`：`net::listener listener{PORT}`（构造内部取 engine 0 的 ring）+ `run_at(0, accept_loop(&listener))` + `sync_run_at(0, cancel_accept(&listener))`；`echo` 里的 `net::connection{fd}` 取的是当前 engine 的 ring。
- `test_tcp_placement` 的 `stop_accept` 不再需要 `co_await resume_on(scheduler::engine_at(0))`：`sync_run_at(0, ...)` 同时表达了放置与等待。
- `scheduler` 的公开面多一个 `submit_at`；`engine_at` 的定位收窄为"取 ring / 看计数器"，仍是测试与 demo 的入口。
- `sync_run_at(i, ...)` 在 engine i 上调用必然自锁（任务只能回到那条线程），只写进注释，不加运行期检查。
- index 越界是文档化的前置条件（`index < size()`），与 `engine_at` 一致，不加检查：它在 bootstrap 里是字面量。
- `net::listener{port}` 构造时把 engine 0 的 ring 指进去，所以要在 `init(n)` 之后构造——`init` 会换掉整套 engine，旧 ring 随之失效。与手写 `engine_at(i).ring()` 是同一类前置条件，不新增检查。
