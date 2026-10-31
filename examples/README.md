# examples

每个文件是一个可以单独读、单独跑的场景。

| 文件 | 场景 | 关键 API | trace 里看什么（需 trace preset） |
| ---- | ---- | -------- | --------------------------------- |
| [run.cpp](run.cpp) | 提交任务的四种写法 | `run`、`sync_run` | 默认池（核数条）的 `engine-i` 轨道各跑一小段 |
| [chain.cpp](chain.cpp) | 三层嵌套与取值 | `task<T>`、`sync_wait` | 只有 `main` 一条轨道，嵌套切片都在这条线上 |
| [fanout.cpp](fanout.cpp) | 8 个子任务 fan-out + fan-in | `run`、`latch` | `child` 切片分散到 `engine-0..3`，父任务等 latch |
| [resume_on.cpp](resume_on.cpp) | 跨 engine 继续执行 | `resume_on` | 同一段 `resume` 从 `engine-0` 换到 `engine-1` |
| [io_read.cpp](io_read.cpp) | 一次 io_uring read | `io::read`、`current_engine()` | engine 先阻塞，`::write` 之后协程被内核唤醒 |
| [echo_server.cpp](echo_server.cpp) | TCP 回显，accept 后交给 scheduler | `net::listener`、`net::connection` | `accept` 在 `engine-0`，每条连接的 echo 在别的轨道 |
| [trace.cpp](trace.cpp) | 给自己的代码加切片 | `trace::scope` | `pipeline/load/compute` 嵌在 engine 的 `resume` 里 |

## 构建与运行

```bash
cmake --preset default
cmake --build --preset default
./build/bin/fanout
```

默认构建不会产生 `trace.json`。要看上表的 `engine-i` 轨道和 `resume` 切片，必须用 trace preset 构建——`scripts/trace.sh` 会配置好再跑：

```bash
scripts/trace.sh fanout   # 产物在 build/_trace/traces/fanout/trace.json
```

把 `trace.json` 拖进 <https://ui.perfetto.dev> 就能看到每条线程的时间轴（文件已在 `.gitignore` 里）。

> 注意 `echo_server` 是交互式示例，可以启动后通过 `nc 127.0.0.1 7000` 连接并测试。需要手动 `Ctrl` + `C` 退出！`scripts/trace.sh echo_server` 同样会等你连完、`Ctrl` + `C` 收尾后才校验并生成 `trace.json`。
