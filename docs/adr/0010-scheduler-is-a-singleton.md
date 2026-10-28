# scheduler 是进程内单例

推翻 [ADR-0002](0002-scheduler-is-an-instance.md)。scheduler 不再由使用者构造，而是进程内唯一的懒构造单例，公开面全是静态函数：`init(n)` / `submit` / `run` / `stop` / `size` / `engine_at`。

理由只有一条：**"提交"需要一个固定的落点，而把它当参数传下去，会把调度拓扑写进每个协程的签名。** M13 之后，`echo` / `accept_loop` / `stop_accept` / `parent` 全都带一个 `scheduler *s`，只为了 `s->submit` 和 `s->engine_at(0)`。

ADR-0002 当初拒绝单例的理由是"测试需要同时存在两个互不干扰的 scheduler"。复核后发现不成立：全仓库没有一个用例要求两套池子**同时**存活，需要的是**先后隔离**——每个用例要一个干净的池子、并且能指定 engine 数。`init(n)` 重建池子正好提供这一点。

## 1. 形态

- `scheduler::instance()` 是私有 Meyers 静态，首次用到时按 `hardware_concurrency()` 建池，所以不写 `init()` 也能直接 `submit`。
- `init(n)` 幂等重建：先 `stop()` 旧池，再建 `n` 个 engine（`n == 0` 夹到 1）。**同一个 n 也重建**——这是测试的归零点，不能省。
- `stop()` 让每个 engine 排空已入队任务后退出，但**保留 engine 对象**：`loop_count()`、`wakeup_count()`、`ring().running_io()` 在 stop 之后仍然可读（demo 打印统计、测试断言关停都依赖它）。之后 `submit` 返回 `false`，要靠 `init(n)` 恢复。
- 析构 = `stop()`，所以 `main` 不需要写任何收尾。

## Considered Options

- **保持普通实例，另加可选访问点**（ADR-0002 留的口子）：类仍可构造，`instance()` 只是便利。放弃是因为收益只有一半——参数还是得传。
- **scoped `run(engines, task)`（RAII，无全局）**：不引入全局状态，但"进程只有一套池子"的意图没被表达；而且 run 之外提交的任务（accept loop 交给连接）仍然拿不到 scheduler。
- **纯析构、不可重建**：省掉 `init`，但 engine 数在进程内固定，现有 7 个要求 1/2/4 个 engine 的用例里 6 个没法共存。
- **可重建单例 + 全静态面**（本 ADR）：代价是多一个进程级状态和一条 `init` 约定，换来调用点不再携带调度器。

## Consequences

- 进程内不能有两套 runtime；需要两个独立 reactor 的场景（多实例嵌入、并行跑两种拓扑）不再支持。
- `init` / `stop` 不能与 `submit` 并发：它们替换的是 engine 表本身，这一条只写进注释，不加锁。
- 测试的固定写法：每个用例第一行 `scheduler::init(N)`；`stop()` 之后 `engine_at(i)` 仍然有效，但任何 `submit` 都会被拒绝。
- `engine` 不再是"给用户手动用的对象"之外的东西：它仍然可构造（manual engine），单例只包 scheduler。
- `llco.hpp` 多了 `runtime/run.hpp`；`scheduler` 的成员 `run` 删除，由 `llco::run` 取代。