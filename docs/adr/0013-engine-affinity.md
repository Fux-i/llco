# 0013 · engine 可以绑定到一个核

**状态**：已接受

## 背景

单 engine 的 echo 吞吐能跑满一个核；多 engine 在基准机上却完全不涨。实测（edi，Ryzen 5 PRO 6650U，`isolcpus=2-11 nohz_full=2-11`）：4 个 engine 线程的 `last_cpu` 全部是 2，`cpu2 busy=98%`，而 CPU 4/6/8 空闲。同一份二进制在非隔离核上就会分散。

原因是隔离核没有周期性负载均衡：线程被唤醒时落在唤醒者所在的核上，之后不再迁移。同一台机器上，4 个独立进程各绑一核聚合 282 k msg/s，1 个进程 4 个 engine 只有 145 k——差别只在"线程能不能落到别的核上"。

llco 之前不管亲和性，所以它无法在隔离核机器上体现多 engine 的价值。

## 决策

**亲和性归 `engine`，不归 `scheduler`。**

```cpp
void engine::start(std::optional<unsigned> cpu = std::nullopt);
```

`loop()` 开头对 `m_cpu` 做一次 `pthread_setaffinity_np`。`scheduler::init(count, cpus)` 把 `cpus[i % size]` 传给 engine i；`cpus` 为空则不绑。

四点具体约定：

1. **默认不绑。** 只有显式传核表才绑。自动从 `sched_getaffinity` 分配会在隔离核机器上悄悄改变线程行为（edi 默认掩码只有 `0-1`），而这种改变应当是一次有意识的选择。
2. **绑核失败不处理。** 单 CPU 集合只有两种结果：该核在 cgroup cpuset 内 → 成功且掩码就是它；不在 → `EINVAL`，掩码不变。不存在部分生效，所以返回值不用检查也不用回读校验，退化成不绑即可。绑核是调优手段，绑不上没有正确性问题。
3. **一个 engine 一个核，不共享。** `cpus` 短于 `count` 时按取模回绕，两个 engine 共用一个核是合法的，只是没有扩展性。
4. **`submit_at(i, t)` 的语义叠加为"钉在 engine i 所在的核上"。** 这两层正交，不需要额外代码。

## 后果

- `init(count)` 的所有既有调用点零改动。
- 隔离核机器上要做扩展性测量，必须传核表；不传时数字只反映"线程抢同一个核"。
- 需要 `<pthread.h>` 与 `<sched.h>`，`engine.impl.hpp` 因此只支持 POSIX——与既有的 io_uring 依赖一致。
- 亲和性是启动期一次性设置，热路径一行都没多。
