# 放置先硬编码 round-robin，不做策略抽象

`scheduler` 用一个 `std::atomic<unsigned>` 游标决定放置：`m_next.fetch_add(1) % n`。没有 `placement_policy` 接口，没有虚函数，也没有 `least_loaded`。M8 原计划两个策略，实现前砍掉：只有一种放置规则时，虚接口换来的只是"将来好扩展"的期权，代价却是一个堆对象、一个构造参数和一个必须先解释才能读懂的抽象。真出现第二种放置需求时再抽，那时才知道接口该长什么样。

## Considered Options

- runtime 多态：`placement_policy` 基类 + `pick(std::span<engine*>)`（roadmap 初稿）。代价是多一层间接、一个 `unique_ptr` 所有权和一处生命周期约定；只有一个实现时无人受益。
- `std::function<unsigned(std::span<engine*>)>`：更轻，但把策略降级成可传空的回调，构造签名同样要解释。
- 编译期多态（`scheduler` 变模板类）：与"engine 是具体类"的既定风格冲突，而放置本来就是在构造时决定一次的事。
- `least_loaded`（挑队列最短的 engine）：需要"负载"的定义，而队列长度在快消费时几乎恒为 0，"最闲"退化成"总挑 0 号"；要么接受这个噪声，要么为它去动 promise 的完成钩子——收益抵不上代价。

## Consequences

- `scheduler` 的构造签名只有 `(unsigned count)`；加第二种放置要动成员与构造签名，这是已知成本，不是疏忽。
- `engine::load()` 保留，但它不再是放置的输入，只给测试与 demo 观测队列积压。
- 满载重试与放置分离：放置只给出起点，能不能落地由 `try_adopt` 说了算（ADR-0004）。
- roadmap 的 M8 目标、实现顺序与"陷阱"已按此重写；"放置策略"进延伸清单。