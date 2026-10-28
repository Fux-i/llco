# io 的提交、完成与关停各只有一条路径

M11 把读写做成 awaiter：`co_await io::read(ring, fd, buffer)` 只负责填 SQE，**提交**由 engine 循环统一做；CQE 在 `ring::reap()` 里**内联**恢复等待者；`engine::stop()` 要等到内核不再持有本 engine 的 op 才返回。四件事各自只有一条路，没有第二条。

## 1. `user_data` 指回 awaiter，`nullptr` 表示"未登记"

CQE 到手时用 `io_uring_cqe_get_data` 找回 awaiter，`reap()` 只对非空指针分发。`io_uring_get_sqe()` **不会**清 `user_data`——只清 `flags/ioprio/rw_flags/buf_index/personality/file_index/addr3/__pad2[0]` 八个字段，槽位重用时带着上一轮的旧值（实测读到 `0xaaa0`）。所以每个填 SQE 的地方都必须显式 `set_data`：awaiter 填 `this`，测试与 demo 里无 awaiter 的 op 填 `nullptr`。

替代方案是给指针打标签（低位 1 = awaiter），但无 awaiter 的 op 用整数 `user_data` 时一样会撞上标签位，两类 op 仍然分不开；约定"无 awaiter 的 op 显式清空"只多一行，且不变量写在注释里。

## 2. 完成在 `reap()` 里内联恢复，`cqe_seen` 先于 `resume`

顺序是 `peek → 拷贝 res 与 user_data → cqe_seen → complete(res)`。`cqe_seen` 必须在 `resume` 之前：被恢复的协程可能立刻填下一个 SQE，正在使用的 cqe 不能还挂在 CQ 上。

不投回 engine 队列（对照 M10 的 `event::set()`，它选择投递）：`set()` 可能在任何线程被调用，而 CQE 本来就在 ring 属主线程上；投递还要处理"队列满时丢一个完成"——那是正确性问题，得再加一层后备表。同向 ADR-0001：控制权默认 inline 交回。

## 3. 提交延迟到循环；SQ 满是常态，就冲刷

awaiter 只填 SQE。循环里 `drain() → submit() → reap()`：一次 `io_uring_enter` 交一批。空提交不进内核（实测 200 次空 `submit()` 触发 0 次 `io_uring_enter`，2.62 ns/次），所以每轮都调用是免费的。阻塞条件因此是"任务队列空、CQ 空、**SQ 无未提交项**"——`io_uring_sq_ready()` 实测就是 `sqe_tail - *khead`，正好是"此时阻塞就会让 op 滞留未提交"的那批。

一次 `drain()` 里 arm 的 op 可以超过 `RING_ENTRIES`，所以 `get_sqe()` 返回 `nullptr` 是常态：awaiter 先 `submit()` 把已有批次交给内核（实测提交即还槽，不必等 reap），再取槽。这一步不只是"溢出阀"：没有它时，调用方无条件解引用返回值属于 UB，优化器会把 liburing 的 `return NULL` 分支删掉——实测 2 槽的 ring 在 `-O2` 下安静地发出第 3 个 SQE，`-O0` 才崩。

## 4. `running_io` 与关停：在提交线程上等回来

`ring` 记 `running_io`（arm 时 +1，分发时 -1），`stop()` 的语义是"等到归零"。**排空必须发生在提交 op 的那个线程上**：实测同一个 ring，op 由线程 A 提交、A 退出后由主线程 reap，拿到的是 `-ECANCELED`（`res=-125`）；提交与 reap 在同一线程则是 `res=1`。所以循环线程收到 `stop_requested()` 后不能立刻返回，要满足"任务队列空 且 `running_io()==0`"才 `return`；`stop()` 只做 `request_stop + join`，之后再用一次尾部 `drain()` 收掉"循环最后一次查看之后才 push 进来的任务"——这些任务 arm 的 op 由调用线程提交，调用线程活到最后，不会被内核取消。

## 实测（2026-10-24，本机 WSL2 内核 6.18.40；liburing 头 2.10 / 运行库 2.5）

| 场景 | 结果 |
|---|---|
| 空 `submit()`（200 次） | 0 次 `io_uring_enter`，2.62 ns/次 |
| `sq_ready()`：填 3 个 / 提交后 | 3 / 0（`sqe_tail - *khead`） |
| `queue_init(2)` / `(3)` 的可用 SQE | 2 / 4（向上取整 2 的幂，无额外余量） |
| 提交即还槽 | `submit()` 后 `get_sqe` 立刻发满，不必 reap |
| op 由已退出的线程提交 | `res=-125`（`-ECANCELED`） |
| op 由当前线程提交 | `res=1`（管道读 1 字节） |
| `io_uring_queue_exit` 时仍有未完成 op | 37 µs 返回，不等待不取消 |
| 管道写端 / 已关闭 fd 读 | `-EBADF`；写端已关的空管道是 `0`（EOF，不是错误） |

## Considered Options

- **每个 op 自己 `submit()`**：一次系统调用一个 op，M12 的 echo 会把它放大；攒批把 N 个 op 合成一次 enter，且空提交免费。
- **完成投回任务队列**：与 M10 一致，但要为"队列满丢一个完成"再设计后备队列，而丢完成是不可接受的。
- **关停时逐 op `IORING_OP_ASYNC_CANCEL`、以 `-ECANCELED` 恢复协程**：等于半个协作式取消机制，等 `stop_token` 传播一起做。
- **在调用线程排空（join 之后再 reap）**：实测 op 已随提交线程退出被内核取消，不可行。
- **给 awaiter 一个虚接口 / 抽 IO 后端抽象**：只有一个后端，等第二个出现再抽（architecture "模块替换"）。

## Consequences

- 无 awaiter 的 op（测试里的 timeout、M6 的 demo）必须显式 `set_data(nullptr)`，否则 `reap()` 会把旧值当指针解引用。
- `engine::stop()` 可能阻塞：有未完成 op 时等到完成，没有时与 M5 一样立即返回。优雅停机对自我续期的任务无界（echo 的 read 循环会让它一直等），强制切断要等取消机制。
- `RING_ENTRIES` 不再是"未完成 op 上限"，只是批大小（`config.hpp` 注释已改）。
- awaiter 必须活在协程帧里；`complete()` 之后不得再碰 awaiter，帧可能已经销毁。
- `ring` 仍未泄漏到 task 层：`io::read` 的第一个参数就是 ring 本身，调用点因而必须显式持有它；sync_wait 的 root 线程（没有 engine）用不了 IO。