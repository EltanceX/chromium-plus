# 原生 Python 阶段四：预算、导航与安全清理

记录日期：2026-11-09。对应[最小实现方案](pyscript-native-python-mvp-implementation.md)的阶段四。

## 1. 当前状态

阶段四实现与验证已完成。完整 `chrome python_runtime_unittests blink_unittests blink_tests` 标准构建通过；运行时 22 / 22、Blink 28 / 28、真实 Chrome DOM 浏览器测试 17 / 17 通过，启用 / 关闭两组 WPT 均为 PASS。全项目验收与既有功能回归在阶段五另行记录。

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

Chrome 每完成一块就在同步回调中交给 Blink 记录结果，只有回调允许继续才执行下一块。控制台前缀和解析诊断同样可以耗尽 Blink 的消息额度，因此不能把整批执行和日志检查分为前后两个阶段。回调返回停止时，控制器仍保有解释器直到 Run 返回，再投递销毁。

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

运行时证据见[实际测试摘要](pyscript-phase4-validation/runtime-unittests.json)与[日志](pyscript-phase4-validation/runtime-unittests.log)。最终集成证据见[验收摘要](pyscript-phase4-validation/final-results-2026-11-09.json)，历史阶段三结果不计入本阶段通过数量。

## 6. 完整构建中的生成对象恢复

第一次完整构建完成 9 项后，`blink_core.dll` 标准链接报告既有 `css_numeric_value_type.obj` 引用不存在的 COFF section 33399。检查该链接的全部 5,040 个直接对象和 59 个静态库的 560 个成员，确认 9 份直接对象存在 COFF 重定位、字符串表或 CodeView 异常；静态库成员未检出结构错误。名单为 `v8_initializer`、`css_numeric_value_type`、`matched_properties_cache`、`style_engine`、`scroll_marker_group_pseudo_element`、`slot_assignment`、`visible_selection`、`fetch_response_data`、`performance_container_timing`。

已保存全部原件、SHA-256、长度、时间与结构诊断后，仅使确认异常的这 9 份生成对象失效并重新构建。备份在忽略目录 `out/Debug/pyscript-phase4-corrupt-objects/`。未修改它们对应的产品源码、调试信息配置或链接诊断。异常产生原因仍未确定；结构扫描用于定位，不能替代完整链接与运行验证。

这 9 份重建对象的哈希全部改变，COFF、字符串表和 CodeView 复查通过，Blink Core 标准链接通过。随后单测链接报告另一处 section 名称异常；检查单测目标的 4,665 个直接对象、194 个静态库的 2,819 个成员，初次扫描指向 `mock_clipboard_host.obj`。保留原件后重建，但重建哈希与原件相同，后续复查和链接通过；不能据此声称该对象已保存字节的损坏被稳定复现。

后续回调修复重建再次遇到旧输入异常。第二次 Core 扫描定位到 `paint_worklet_deferred_image`、`font_builder`、`font_style_resolver`、`scroll_target_group_scope`、`text`，普通读取与内存映射读取均复现相同错误；`gmock-matchers` 的 COFF / CodeView 异常也以两种读取方式确认。content_shell 的 3,817 个直接对象、336 个静态库的 4,267 个成员扫描初次指向 `udp_socket_test_util`，其重复读取却未复现错误；仍明确记录这一差异并保留原件后仅重建该输入。

最后单测链接报告 `v8_external_memory_accounter_test` 的函数 ID 错误。该文件本地 ID 检查正常，针对其函数哈希的 4,665 个直接对象比较未发现种类冲突，却定位到 `media_query_evaluator_test` 的无效类型记录。完整输入扫描和普通 / 内存映射重复读取均复现该记录异常；保留原件后重建，标准单测链接随即通过。

本阶段共保留并重建 18 份旧对象，全部重建对象的 COFF、字符串表与 CodeView 结构复查通过。最终标准构建耗时 1m36.77s。扫描与重复读取结果存在差异的两例单独记录，原因仍未确定，不推断为产品源码或硬件问题。实际哈希变化、原件位置和诊断见[恢复清单](pyscript-phase4-validation/object-recovery.json)；[最终对象复查](pyscript-phase4-validation/all-rebuilt-object-check.json)不能替代已完成的标准链接和运行验证。

## 7. 控制台停止边界的回归修复

首轮真实浏览器测试中，原来在单块发起 1,023 次 DOM 调用的用例先命中了 100 ms 时间限制。改为八块各执行 128 次调用，再验证第 1,025 次失败；时间预算和 1,024 次的断言均未放宽，随后该用例通过。

新增用例先输出 16,330 个 ASCII 字节，低于原生输出硬上限，但高于 Blink 为截断通知预留空间后的消息额度，再在下一块修改 DOM。修复前浏览器确实观察到 `forbidden`，预期的 `before` 断言失败，见[复现结果](pyscript-phase4-validation/console-stop-reproduction.json)与[日志](pyscript-phase4-validation/console-stop-reproduction.log)。修复把结果上报移入逐块同步回调，确保控制台停止发生在下一块之前。

修复前的整体验证已有运行时 22 / 22、Blink 25 / 25、启用 / 关闭 WPT 两文件和阶段二 Chrome 回归 26 / 26 通过。它们不能替代修复后重新构建和验证的最终结果。

## 8. 最终验证范围

最终 Blink 28 项包含四轮 GC / 导航的创建销毁计数相等、frame 关闭的延迟销毁，以及清理任务在禁止脚本时重新投递。测试执行器析构同时断言不处于 ScriptForbiddenScope、sweeping 或 GC 原子暂停。新增 URL 用例保留原本合格的 HTTPS origin / parser，仅修改 Document URL，验证 WebUI、扩展、MCBE、MCJE 和 file scheme 均不能创建解释器。

真实 Chrome 的 17 项测试包含 Python 独立样例和九项 DOM testharness 断言、错误恢复、跨块 1024 次 DOM 上限、纯循环超时、累计 UTF-8 输出、异常格式化预算、巨大异常、解析诊断上限、逐块控制台停止、三轮循环 finalizer 超时导航、`document.open()`、前进 / 后退、刷新、20 轮普通循环 finalizer 导航及 `Browser.close` 正常退出。BFCache 测试同时检查旧 JS 标记消失、Python 结果重新生成和 CDP 的未恢复原因。

所有真实 Chrome 测试保留 Renderer 沙箱，未附加网络服务诊断参数。WPT 使用 content_shell，没有 CPython 嵌入执行器；它验证解析和关闭行为，Python 实际执行由 Chrome 测试证明。WPT 保留本机既有 `--disable-system-font-check` 与 `--enable-features=NetworkServiceInProcess2` 诊断配置，未把结果描述为默认配置通过，也未修改 expectations。
