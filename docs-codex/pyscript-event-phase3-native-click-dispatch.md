# 原生 Python 点击事件：阶段三 Blink 原生派发

日期：2026-11-09（Asia/Shanghai）。接续 `b7c2b53701` 的[阶段二注册与适配](pyscript-event-phase2-dom-registration-adapter.md)，整体步骤见[实现方案](pyscript-click-events-implementation.md)。

## 行为与实现

`dom.add_event_listener(element, "click", callback, *, once=False)` 现在安装 Blink `NativeEventListener`。事件在正常冒泡监听阶段同步调用当前文档已有解释器中的 callable，传入只读的 `type`、`current_target`、`is_trusted` 快照。鼠标、键盘激活和脚本生成的 click 使用同一入口；返回值不取消默认行为。

每次注册有独立原生 listener 和 callback ID，遵循 Blink 的监听顺序及派发期间增删规则。丢弃 Python 句柄不取消监听；显式移除允许重复调用，也允许在回调中自移除或移除后续监听。

`once` 通过两端协作实现：Blink 在准入通过后、进入解释器前撤销原生监听；运行时注册表保存 `(callable, once)`，获取 callable 的临时强引用后立即消费 once 表项，再构造参数、授予 DOM 能力和调用用户函数。异常也会消费注册项；参数构造、表项删除及最终引用释放均不继承 DOM 能力。公共 Blink / Chrome 调用接口无需新增消费参数。

普通 Python 异常报告后保留普通监听。致命预算停止或文档失活时撤销整份文档的原生订阅，失效 DOM 代数，并通过既有安全任务释放解释器。回调内失活不会在仍有 Python 栈时销毁上下文。

## 权限与节点生命周期

注册和每次派发均要求目标仍属于原文档、已连接、处于 light DOM，且为普通 HTML 元素；拒绝 SVG、自定义元素及带 `is` 的定制内建元素。监听不扩大 `set_text` 的写入范围。

原生 listener 弱引用目标和 controller，DOM 句柄也保持弱引用。断开节点时跳过；同一节点重连原文档可恢复；同 ID 的新节点不继承监听；迁移到其他文档不会调用旧解释器。被 GC 回收的节点对应订阅可有界保留到显式移除或文档清理，最多 128 项。

每个初始化块复制原始源码、nonce、URL 和源码位置，订阅共享对应记录，回调中新增订阅继承当前回调的授权记录。日志及 CSP 检查不强持有原 `<pyscript>` 元素。批次逐块报告结果时切换当前来源，避免后续块错误继承前一块的 nonce。

派发时重新检查解释器功能、当前页面资格、来源、脚本许可和当前 CSP，不依赖已经结束的 parser。CSP 使用复制的源信息检查 inline script；允许没有原元素的快照检查，继续执行相同 nonce/hash 策略，也不会获得动态脚本的信任传递。新增 CSP 可以阻止已有监听。放开页面限制模式沿用原有覆盖规则，特权页面仍被排除。

已有 Python 执行、`ScriptForbiddenScope`、GC 原子暂停或 owning-thread sweeping 时跳过当前事件，不补发。一般准入失败的提示每文档最多一次；GC 不执行解释器或 Python 引用释放。回调输出和异常附带 callback ID，控制台来源使用复制的 URL 和行列。

## 示例与测试入口

示例：[click-events.html](../chrome/test/data/pyscript/click-events.html)。通过获准的 HTTP 来源加载，并按现有设置启用 Python。示例页面仅使用 Python 注册按钮监听；测试服务器为它设置 `script-src 'nonce-python-click'`。

新增浏览器脚本：[pyscript_event_smoke_test.mjs](../chrome/test/pyscript_event_smoke_test.mjs)。它保留 Renderer 沙箱，使用 CDP 原生鼠标和键盘输入，并验证脚本 click、once、增删、异常、节点生命周期及页面策略。

```powershell
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1
out/Debug/python_document_context_unittests.exe --test-launcher-jobs=1
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.* --test-launcher-jobs=1
node chrome/test/pyscript_event_smoke_test.mjs --output=out/Debug/pyscript-event-phase3-browser.json
```

## 验证记录

实际结果见 [validation-summary.json](pyscript-event-phase3-validation/validation-summary.json)。下表的单测与浏览器结果已在四目标标准构建通过后重新验证，最终证据见 [build-completion/summary.json](pyscript-event-phase3-validation/build-completion/summary.json)。

| 检查 | 实际结果 |
| --- | --- |
| 运行时单测 | 44 / 44 |
| Chrome 公共接口适配单测 | 7 / 7 |
| Blink `HTMLPyScriptElementTest.*` | 45 / 45 |
| CSP nonce 回归 | 3 / 3 |
| 真实浏览器点击测试 | 17 / 17，保留 Renderer 沙箱 |
| 既有 DOM / 预算 / 生命周期浏览器回归 | 17 / 17 |
| 既有解析与来源浏览器回归 | 27 / 27 |
| GN / checkdeps / 改动范围格式 / diff | 通过 |

本次先通过 `autoninja` 定向编译改动对象和运行时。为尽早运行上述测试，从当前 GN 生成的 Ninja 规则提取完整链接参数，用新对象及其余现有对象定向重链 `blink_core.dll`、`python_document_context_unittests.exe`、`blink_unittests.exe`。保留相同的 `/DEBUG`、PDB、输入和链接检查。浏览器验证使用现有 Chrome 程序加载本轮更新的 Blink Core / Python Runtime 组件。这属于定向集成验证；完整依赖图构建另行记录。

首次交付时完整构建约展开 2,900 项 Blink 任务，`chrome blink_unittests python_runtime_unittests python_document_context_unittests` 留在后台运行。实时状态和退出码保存在本机 `out/Debug/pyscript-event-phase3-full-build-status.json`，完整日志路径由其中的 `log` 字段给出；[首次交付快照](pyscript-event-phase3-validation/full-build-snapshot.json)保留当时状态。四目标标准构建现已通过，恢复过程见下文。

定向链接期间，对 Core 的 5,040 个直接对象、59 个库 / 560 个成员，以及 Blink 测试的 4,665 个直接对象、194 个库 / 2,819 个成员进行扫描，发现 27 份 Core 和 5 份测试输入存在 COFF / CodeView 结构异常。原件时间戳早于本轮改动，多次普通读取和 mmap 的 SHA-256 一致。保存原件和哈希后，仅使这 32 份已诊断对象失效并定向重建；重建扫描为 0 问题，随后链接通过。未改动对应功能的源码。原件位于本机忽略目录 `out/Debug/pyscript-event-phase3-object-backup/`；结构扫描与哈希见验证目录。损坏原因尚未确定。

测试准备时曾修复安全缓冲区检查不接受的 CPython tuple 宏访问，改用检查索引的函数接口；新增析构测试改用嵌入式环境可用的析构标记。Blink 测试显式区分原生和绑定层事件入口，并在节点回收断言前完成渲染生命周期。CDP 键盘测试补全 Enter 文本事件，局部变量限定在独立作用域。最终结果均来自这些修复后的实际重跑。

### 2026-11-09：完整构建失败恢复

后续检查发现，首次后台构建完成 191 步后，在 Blink Core 链接处因 `svg_text_element.obj` 的 COFF 符号引用不存在的 section 而失败，退出码为 1。该对象未在这次构建中重新编译，原件时间戳为 2026-11-08 13:53:29（本机时间）。

保存实际响应文件、失败命令和日志后，再次扫描 Core 的 5,040 个直接对象、59 个库及 560 个成员，仅发现这一份异常对象。多次普通读取及 mmap 的 SHA-256 一致；保存原件后仅重建该对象，重建扫描为 0 问题。损坏原因仍未确定。

`autoninja -C out/Debug blink_core.dll` 的标准构建已通过，共 2 步、1 分 13.65 秒，保留正常 Debug / PDB 链接检查。使用这次标准构建的 Core 组件，重新运行 Blink Python 单测 **45 / 45** 和真实浏览器点击测试 **17 / 17**，均通过。

四目标完整构建随后作为 `attempt2` 恢复，完成 382 项任务后，在 `blink_unittests.exe` 的 CodeView / PDB 链接处失败，退出码为 1。这一次没有 Python 源码编译错误。此前 Core 恢复过程见 [build-recovery/summary.json](pyscript-event-phase3-validation/build-recovery/summary.json)，其异常原件保存在本机 `out/Debug/pyscript-event-phase3-completion/object-backup/attempt1/`。

### 2026-11-09：四目标标准构建与最终回归通过

保存 `attempt2` 的实际响应文件和失败命令后，对 4,665 个直接对象、194 个库及 2,819 个成员完整扫描，并独立检查直接对象的本地函数 ID。两项检查均定位到 5 份对象的 CodeView 记录长度 / 类型异常；链接器首先点名的 `v8_external_memory_accounter_test.obj` 未发现这些异常，因此没有直接重建该文件。

5 份异常对象的普通重复读取和 mmap 的 SHA-256 一致。保存原件、时间戳及哈希后，仅定向重建这 5 份生成对象；两项复查均为 0 问题。原件位于本机 `out/Debug/pyscript-event-phase3-completion/object-backup/attempt2/`，损坏原因仍未确定，未修改对应生产源码。

`attempt3` 执行正常命令 `autoninja -C out/Debug chrome blink_unittests python_runtime_unittests python_document_context_unittests`，**退出码 0，共 6 步、1 分 58 秒**。保留正常 Debug / PDB 和链接检查。这次是四目标完整依赖图的增量构建通过；前两次已经完成的任务继续复用。

使用这次标准构建的程序重跑：运行时 **44 / 44**、Chrome 适配 **7 / 7**、Blink Python **45 / 45**、CSP nonce **3 / 3**、真实点击 **17 / 17**、DOM / 生命周期 **17 / 17**、解析与来源 **27 / 27**，均通过，浏览器测试保留 Renderer 沙箱。

首轮浏览器回归也保留了两处失败和对应修正：

- 节点移除后立即强制 GC 的检查，可能遇到仍持有节点的待处理渲染生命周期。点击测试改为等待两帧再收集，仍严格检查按钮和源码节点的弱引用均失效；修正后完整点击套件连续两次 **17 / 17**。该顺序也与 Blink 单测完成生命周期后再收集的处理一致。
- DOM 次数测试曾先触发文档 250 ms 时间限制；原测试独立重跑 **17 / 17**。为减少两类预算的竞争，测试改用 `map` 执行原有 8 块、每块 128 次原生桥接调用，减少 Python 循环指令追踪开销。仍验证 1,024 次成功、第 1,025 次被拒绝以及后续块不能恢复执行，修正后 DOM 套件 **17 / 17**。生产时间、DOM 和输出限额均未调整。

最终状态、完整成功构建日志、失败现场、对象哈希和实际测试结果统一保存至 [build-completion/](pyscript-event-phase3-validation/build-completion/summary.json)。首次交付和此前后台运行快照继续作为历史记录保留。

## 阶段三交付时的限制

阶段三交付时仍共用既有累计预算：单次 100 ms、文档累计 Python 执行 250 ms、累计 DOM 调用 1,024 次及累计输出 16 KiB。该限制的后续处理见[阶段四：交互预算、导航与安全清理](pyscript-event-phase4-budgets-and-cleanup.md)。

这些交互预算及导航、`document.open()`、frame 销毁和清理矩阵已在后续阶段四实现并验证；阶段五负责首版验收汇总。
