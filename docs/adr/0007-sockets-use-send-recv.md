# socket 走 send/recv，关停靠 cancel

M12 在 io 层加了 `accept` / `connect` / `send` / `recv` / `cancel` 五个 op，`net/` 只放四个不碰 io_uring 的 socket 助手（`listen_tcp` / `local_port` / `tcp_socket` / `loopback`）。三条边界是实测定的：

## 1. socket 的数据面只能是 send/recv

同一台机器、peer 已被 RST 之后：

| op | res | 进程 |
|---|---|---|
| `IORING_OP_WRITE` | `-32`（EPIPE） | handler 被调用一次；SIG_DFL 子进程被信号 13 杀死 |
| `IORING_OP_SEND` flags=0 | `-32` | 不产生信号，子进程存活 |
| `IORING_OP_SEND` MSG_NOSIGNAL | `-32` | 同上 |

`io_uring_prep_write` 真的能杀进程，`send` 不会——本机内核的 send 路径本身就抑制了 SIGPIPE，`MSG_NOSIGNAL` 是冗余的。`io::send` 仍然显式带上它：一次实测到的"内核恰好帮我挡了"不值得押在版本假设上。`read` / `write` 留给 pipe 与文件，`io::write` 的注释里写明这一点。

## 2. accept / connect 与提交时机

- `accept(NULL, NULL)` 合法（实测 `res=6`），所以 `accept` 只返回新 fd；要 peer 地址就 `getpeername`，不把 `sockaddr_storage` 放进 awaiter。`prep_accept` 的 flags 只作用于新 socket（`SOCK_NONBLOCK | SOCK_CLOEXEC`），不影响 accept 这个调用本身。
- `connect` 的 `sockaddr_in` 在**提交时**被内核拷走（把地址 `mprotect` 掉的探针仍然连上），而提交延迟在循环里，所以地址只需活到协程挂起期间；awaiter 存指针，注释写清契约。成功 `res=0`，失败 `-ECONNREFUSED`（-111）。
- 这几个 op 与读写共用同一条提交路径：awaiter 只填 SQE、`set_data(completion *)`、`armed()`，循环 `drain → submit → reap`；`reap()` 对任何 awaiter 都只做"写 result、`running_io--`、resume"三件事。

## 3. 关停：cancel 是 stop() 能返回的前提

M11 把 `stop()` 定义成"等到 `running_io() == 0`"。监听 fd 上的 pending accept 永远等不到，服务器会卡在 `stop()` 里。`io::cancel(ring, fd)`（`IORING_OP_ASYNC_CANCEL` + `IORING_ASYNC_CANCEL_FD`）解决它：cancel 自己的 CQE 是 `0`，被打断的 accept 是 `-ECANCELED`，两个 CQE 都要 reap（cancel 先到）；没有可取消的 op 时 cancel 返回 `-ENOENT`，映射成 `await_resume() == false` 而不是异常——"取消时连接刚好进来"是正常竞态。

demo 因此可以跑到 SIGINT：主线程 `sync_wait` 一个开头 `co_await resume_on(engine)` 的任务（cancel 必须在 ring 属主线程提交），再 `stop()`、`close(listener)`。accept 循环收到 `ECANCELED` 后 `co_return`，并且在每轮末尾看一眼中断标志——连接恰好落在信号与 cancel 之间时它自己退出，不会留下"再 arm 一个没人能打断的 accept"。

## Considered Options

- socket 上继续用 `read` / `write`、靠进程级忽略 SIGPIPE 作为后备：库不该替进程装信号处置，而且这是"忘一次就死"的约定。
- accept 带出 peer 地址：echo 不需要，需要时 `getpeername` 一行。
- `connect` 收 `const sockaddr *`：每个调用点都要 `reinterpret_cast`（clang-tidy 还会报错）；收 `sockaddr_in` 把唯一一次 cast 关进 awaiter（带 NOLINT 与理由），代价是 IPv4-only。
- 用"有界 accept"绕开 pending accept：问题只是被藏起来，demo 也不再是服务器。

## Consequences

- socket 数据一律 `send` / `recv`；`write` 是 SIGPIPE 隐患，测试与 demo 都不碰。
- accepted fd 一定是非阻塞 + close-on-exec；关掉之前该 fd 上不能有未完成 op。
- `stop()` 对未完成 op 有了通用解法：先 cancel，再等归零。
- IPv4 only；没有超时、`TCP_NODELAY`、连接数上限（`spawn` 满载丢连接，ADR-0004 的既有语义）。
- 一条连接 = 一个 detached 协程：`accept` → `recv`/`send` 循环 → EOF 或 error → `shutdown(SHUT_WR)` → `close`。