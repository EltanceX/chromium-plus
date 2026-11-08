# 原生 Python 阶段四：预算、导航与安全清理

记录日期：2026-11-09。对应[最小实现方案](pyscript-native-python-mvp-implementation.md)的阶段四。

## 1. 当前状态

运行时预算实现完成，`python_runtime_unittests` 22 / 22 实际执行通过，包括新增的七项预算与清理用例。Blink / Chrome 适配及浏览器回归用例已写入，完整 `chrome python_runtime_unittests blink_unittests blink_tests` 构建正在进行，尚不能据此宣布集成验收完成。

## 2. 预算与停止语义

| 项目 | 限制与行为 |
| --- | --- |
| 源码 | 沿用每块 64 KiB、每文档 16 块；拒绝 NUL 和非法 UTF-8 |
| 单块执行 | 100 ms，协作式检查 |
| 文档执行 | 250 ms，累积编译、执行与诊断时间，排除块间空闲和解释器初始化 |
| 输出 | 文档 stdout、stderr 与异常诊断共享 16 KiB UTF-8 预算 |
| 控制台 | Blink 的 Python 消息另按文档累计计数，含解析拒绝诊断与前缀；在 16 KiB 内预留一次截断通知 |
| DOM | 文档累计 1024 次调用；第 1025 次返回不可逆停止，立即清除句柄 |
| DOM 字符串 | 单次 16 KiB UTF-8；类型、长度、目标错误仍是可恢复异常 |
| 清理 | 独立的 50 ms 协作式 tracing 预算，用于受控 Python finalizer 示例 |

`ExecutionBudget` 属于每个解释器的原生上下文。时间、累计输出、DOM 次数和文档失活会锁存停止原因；Python 捕获异常或清除 tracing 后，原生停止状态仍不能复位。停止后的 `Execute()` 拒绝执行，Chrome 返回截至停止块的结果前缀，不调用后续代码块。普通 SyntaxError / ValueError 等不锁存停止，后续合格块仍可运行。

时间检查通过 C `PyEval_SetTrace` 接入，在函数入口设置 opcode tracing，每 64 个 trace 事件以及函数调用 / 返回检查；编译边界、输出和 DOM 调用前后也复核状态。DOM 调用返回停止结果时，原生预算优先锁存 DOM 调用超限原因。

异常的 `__str__` 在预算内执行，撤销 DOM 能力后才格式化。超长诊断在截断前检测原始长度，避免截断掩盖超限。stdout / stderr 在块间和销毁期间均不向页面输出。

## 3. 导航、生命周期与销毁边界

沿用阶段三的 `Document::Shutdown()`、`document.open()`、执行上下文销毁和 Oilpan prefinalizer 入口：立即标记停止、增加文档代数、清空队列和弱节点句柄；正在执行的解释器先返回，随后释放。

解释器销毁投递到文档主线程任务队列。任务真正运行时再次检查 Python 全局重入状态、ScriptForbiddenScope、Blink 生命周期禁止脚本状态、GC 原子暂停和当前线程 sweeping；不满足条件则延后 1 ms 重试。执行销毁时也设置全局重入保护，避免嵌套消息循环进入其他 Python 上下文。

任务只携带原生上下文裸指针和任务 runner，不捕获 Document / Node，不让任务闭包析构直接运行 Python。若 Renderer 关闭丢弃任务队列，资源保留到进程退出；这不是正常导航的释放策略，也不能宣称任意退出路径均调用了 finalizer。

运行时自身对创建、执行和销毁统一检查线程及重入。销毁先撤销 `dom` 模块借用的 host、关闭输出，再设置清理 tracing，最后释放 globals、模块和子解释器。测试统计仅供 C++ 验证创建、销毁和存活上下文计数，不新增网页 API。

同文档 fragment 导航保留原 Document，不重复执行。跨文档导航和刷新创建新上下文。继续使用原生 Python 的 BFCache sticky feature 排除已执行文档；prerender 继续拒绝执行。

## 4. 重要边界

这些预算是主线程实验的协作式保护，不能保证任意不可信代码在 100 ms 内被强制终止。长时间原生 C 操作、编译本身、巨量内存分配、主动关闭 / 替换 tracing、捕获 tracing 抛出的异常后继续运行等情况，仍可能阻塞 Renderer。原生停止状态可阻止后续 DOM / 输出 / 下一块，不能抢占正在执行的任意机器码。

CPython tracing 出错后可能被关闭，finalizer 也能修改 tracing 或捕获异常；50 ms 清理预算只对受控示例提供尽力中断，不能视为整个 `Py_EndInterpreter()` 的硬超时。多对象恶意 finalizer 不具备统一终止保证。

本阶段没有实现进程级抢占、内存硬限制或语言安全沙箱。默认关闭特性、精确 loopback origin、顶层文档、CSP 和 Renderer 沙箱边界继续保留。若未来面向任意网页，应先改为可终止的独立进程 / worker 设计并重新审视 OS 能力。

## 5. 验证与证据

运行时命令：

```powershell
autoninja -C out/Debug python_runtime_unittests
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000 --test-launcher-summary-output=out/Debug/pyscript-phase4-runtime-final.json
```

运行时 22 / 22 通过，涵盖纯 Python 无限循环、累计文档时间与空闲排除、UTF-8 输出 / 异常累计、停止后的原生 DOM 撤销、文档失活、自定义异常格式化循环、循环引用 finalizer、反复释放、重入和既有隔离回归。精确累计时间使用注入的 TickClock 测试，实际 tracing 中断使用真实时钟。

首轮测试有一项旧回归失败：测试重建上下文后没有重新 `import dom`。补齐初始化后重跑全部通过。随后补充单次超长异常的原始长度检查，再次重跑全部通过。没有放宽测试断言或超限策略。

集成定向编译中 Chrome 适配和 Blink 控制器通过，新增 Blink 测试最初误用了 `String::Contains`；改为该类型实际提供的 `contains`，再进入完整构建。运行时、Chrome 适配、Blink HTML 的 checkdeps 均通过；Blink 检查有既有 DEPS 转义 SyntaxWarning。

运行时证据见[实际测试摘要](pyscript-phase4-validation/runtime-unittests.json)与[日志](pyscript-phase4-validation/runtime-unittests.log)。集成验证结果将在实际执行后追加，历史阶段三结果不计入本阶段通过数量。
