# task 是 move-only

`task` 之前用 `CLASS_NO_COPY_MOVE`，既不能拷贝也不能移动，所以任何容器都装不进去（`std::vector<task<>>` 的 `push_back` 直接编译失败）。要支持"一次提交一批协程"，批量入口必须能收容器，因此放开移动：**拷贝仍删除，移动转移帧的所有权**，moved-from 是空壳（`handle() == nullptr`）。

## 1. 为什么不能交给 `CLASS_MOVE_ONLY`

宏展开成 `= default` 的移动，而成员是裸 `std::coroutine_handle<>`（平凡可拷贝）——默认移动等同于复制，moved-from 仍然持有句柄，两个 `task` 析构时同一帧被 destroy 两次：

```
task a = make();
task b = std::move(a);
→ ASan: heap-use-after-free ... task::~task() → coroutine_handle::destroy()
```

所以移动是手写的：构造用 `std::exchange(other.m_handle, nullptr)`，赋值先 destroy 自己的帧再接管（自赋值直接返回）。`CLASS_MOVE_ONLY` 留给成员自带移动语义的类。

## Consequences

- `run` / `sync_run` 的容器重载只收右值 range（`run(std::move(batch))`）：消费这件事在调用点必须看得见。
- moved-from 的 `resume()` / `handle()` 是空壳，误用是 UB；`release()` 早就有同样的后置状态，没有新增契约。
- `task` 仍然不可拷贝：一个帧只有一个所有者。
- 默认构造仍然删除，容器只能 `push_back` / `emplace_back`，不能 `resize`。
