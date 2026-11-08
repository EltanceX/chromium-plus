# 阶段三：文档上下文与最小 DOM 桥接

- 日期：2026-11-08（Asia/Shanghai）。
- 分支：`codex/kernel-debug-settings`；阶段二基点：`d63e4e2d64`。
- 依据：[第一版实现方案](pyscript-native-python-mvp-implementation.md)第 7 节及阶段三。
- 当前状态：运行时 DOM 模块已构建并通过 15 / 15 单测；Blink 控制器、Chrome 适配器和 20 个 Blink 用例源码已定向编译通过。恢复并重建一份结构异常的生成对象后，新的 `blink_core.dll` 已链接通过。完整 Chrome 正在继续构建，浏览器端到端及 Blink 用例执行尚未完成，不能视为阶段三验收通过。
- 关键提交：`5dcb4e0cde`（每解释器 DOM 模块与 15 项运行时测试），`1ffbda7979`（文档上下文、DOM 接入及示例）。

## 1. 接口与使用

```html
<p id="result">Waiting</p>
<pyscript>
import dom
answer = 6 * 7
handle = dom.get_element_by_id("result")
dom.set_text(handle, f"Python 计算结果：{answer}")
</pyscript>
<pyscript>
dom.set_text(handle, f"共享句柄：{answer + 1}")
</pyscript>
```

`dom.get_element_by_id(str)` 只查询当前 Document 的 light DOM，找不到时返回 `None`。`dom.set_text(ElementHandle, str)` 直接通过 C++ 写入 Blink 文本 DOM，不调用页面 JavaScript，也不解析 HTML。完整示例：[phase3.html](../chrome/test/data/pyscript/phase3.html)。

仍需显式启用 `PythonScript`，并配置精确的本地 HTTP(S) origin。阶段二的顶层文档、原始解析来源、脚本权限和 CSP 检查继续适用。示例中的 nonce 可以配合 `Content-Security-Policy: script-src 'nonce-python-dom'` 使用。

## 2. 运行时 DOM 模块

新增 `components/python_runtime/python_dom_module.{h,cc}`，在 CPython 初始化前注册多阶段初始化内建模块 `dom`。每个子解释器拥有独立模块状态和 `ElementHandle` 堆类型。

- 句柄只保存文档编号、代数和条目编号，不保存 `Node*` 或可调用的 Host。
- 句柄类型不可直接构造、不可派生且不可修改类型；整数、`None` 及其他对象不能伪造句柄。
- 参数必须是 Python Unicode 字符串，不对任意对象调用 `str()`；单个参数最多 16 KiB UTF-8。孤立 surrogate 产生 `UnicodeEncodeError`，超长产生 `ValueError`。
- 参数类型错误为 `TypeError`，失效文档 / 句柄为 `ReferenceError`，受限目标为 `PermissionError`，Host 状态错误为 `RuntimeError`。
- `PythonContext::Execute(..., DomHost*)` 仅在实际求值期间借用 Host。编译、异常字符串格式化、结果释放及解释器清理时均没有 DOM 能力。
- 运行时拒绝嵌套 `Execute` / `Create`，避免从 DOM Host 重入共享 GIL 后死锁。
- `PythonContext` 持有原始 `dom` 模块引用，不依赖页面是否修改 `sys.modules` 或模块公开属性。

新增六个运行时用例验证类型与伪造、跨块句柄与模块隔离、异常映射、UTF-8 边界、诊断 / finalizer 能力撤销和嵌套进入；连同原九项共 15 / 15 通过。

## 3. 文档所有权与 Blink 句柄

`PythonScriptController` 作为 Document supplement 持有一个 `WebPythonScriptContext`；Chrome 的实现持有 `PythonContext` 子解释器。Python globals、导入模块与句柄在文档批次执行后继续保留，同文档的多个块共享状态，不同 Document 使用独立子解释器。

句柄表使用可追踪条目中的 `WeakMember<Element>`，不把节点永久强引用。查询去重且不复用编号；写入复核文档编号、代数、编号范围、节点所属文档、连接状态和 shadow 状态。

只允许 HTML 命名空间、未定制且不带 `is` 属性的普通 `p/div/span`；含元素子节点的目标拒绝。`script/pyscript/style`、SVG、自定义元素及其定制内建形式均拒绝。写入仅替换文本 / 注释子节点，使用脚本与事件禁入范围；MutationObserver 仍在 Python 栈返回后的微任务检查点运行。

每文档最多 1024 次到达 Blink 的 DOM 调用，字符串在运行时和 Blink 边界分别检查。当前调用预算用尽后报告 `ValueError`；不可逆停止和协作式执行时间预算属于阶段四。

## 4. 失效、清理与 BFCache

持久解释器必须同时具备最小生命周期处理，因此本阶段提前接入导航 / Shutdown、`document.open()`、ExecutionContext 销毁及 GC prefinalizer。它们先停止上下文、清空队列与弱句柄、增加代数；执行中的上下文在 `Run` 返回后才能释放。

解释器清理会运行 Python finalizer，不能在 Oilpan 回调或生命周期中的 script-forbidden 范围直接析构。控制器将无 GC 引用的解释器对象交给所属主线程任务释放，并提前撤销 DOM 能力。任务闭包不拥有析构型智能指针，防止队列丢弃任务时在错误时机进入 Python。如果线程任务队列已经关闭，资源故意保留到 Renderer 进程退出；当前没有对这种退出边界提供精确清理保证。

创建上下文的文档注册独立的 sticky scheduler feature `kNativePython = 73`，显示名 `native-python`，由 BFCache 明确拒绝，并同步导航 metrics 枚举。没有伪造 unload 监听器。私有实验没有新增公开 CDP enum；CDP 原因暂映射为 `Unknown` / `SupportPending`，scheduler 原因仍是 `native-python`。

这些措施只覆盖持久解释器必需的能力撤销与生命周期边界。完整执行时间 / 输出累计预算、长时间循环引用压力和阶段四验收仍未完成。

## 5. 验证与复现

```powershell
autoninja -C out/Debug python_runtime_unittests
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000 --test-launcher-summary-output=out/Debug/pyscript-phase3-runtime-unittests.json

autoninja -C out/Debug chrome
autoninja -C out/Debug blink_unittests blink_tests
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.*

node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-dom-smoke.json
node chrome/test/pyscript_smoke_test.mjs --output=out/Debug/pyscript-phase3-phase2-regression.json
```

新增浏览器脚本保留 Renderer 沙箱，启动独立 Profile 和精确 origin 的本地服务。网页行为用 WPT `testharness.js` 断言；真实 CPython 用例放在 `chrome/test/data/pyscript/` 并由 Chrome 脚本运行，因为 `content_shell` 不安装 Chrome 的 Python 执行器。现有 `wpt_internal/pyscript/` 仍验证 Blink 的解析与默认关闭行为。

| 检查 | 当前结果 |
| --- | --- |
| 运行时 DLL 与单测构建 | 通过，格式化后的产物已再次执行测试 |
| 运行时单测 | 15 / 15 通过 |
| 控制器 / Chrome 适配器 / 20 用例源码编译 | 通过；首次发现并修正 raw_ref 规则、窄化和测试 API 差异 |
| GN 检查 | python_runtime、Chrome Renderer、Blink Core 通过 |
| checkdeps | 上述三个目录通过；工具提示既有 DEPS 字符串的 SyntaxWarning |
| Node 语法与本阶段 diff 空白检查 | 通过 |
| 示例 / testharness 内嵌代码语法 | 两个示例的 7 块 Python 及测试用 JavaScript 已检查，保留预期语法异常样例 |
| Navigation metrics XML 校验 | `validate_format.py --xml_paths tools/metrics/histograms/metadata/navigation/enums.xml` 通过 |
| Blink Core DLL | 结构异常对象重建后链接通过，耗时 1m25.24s；本次没有修改产品源码 |
| 完整 Chrome | 首轮在链接时失败；已恢复构建，约 3,337 项增量任务，结果待完成 |
| Blink 20 用例 / 浏览器 DOM smoke / 阶段二回归 | 待运行 |

原始日志和结构化结果保存在 [pyscript-phase3-validation/](pyscript-phase3-validation/)。未完成的检查不会记为通过。既有 MCJE SRV 修复不属于本阶段提交。

## 6. 后台构建与待验证队列

2026-11-08 21:25（Asia/Shanghai）启动了独立隐藏的 PowerShell 后续验证进程，等待本轮 Chrome 构建成功后顺序运行：

1. 新 `pyscript_dom_smoke_test.mjs`，包含无 JavaScript 的 nonce CSP 示例、WPT testharness 的 9 项网页断言、文档隔离、DOM 调用预算、`document.open()`、BFCache、刷新和六轮 finalizer 导航。
2. 阶段二 26 项浏览器回归。
3. `blink_unittests blink_tests` 构建，然后运行 `HTMLPyScriptElementTest.*` 的 20 项用例。
4. 已有启用 / 关闭两组 WPT；沿用阶段二的字体检查与进程内网络服务诊断参数，单独标记这种配置。

同一输出目录的构建始终串行。Chrome 构建失败时队列停止，不运行 ABI 尚不一致的旧浏览器。运行后的检查结果会单独记录，失败不会自动改写断言或变成预期失败。

本机任务文件位于忽略目录 `out/Debug/pyscript-phase3-followup.ps1`，实时状态为 `out/Debug/pyscript-phase3-validation-state.json`。构建日志为 `out/Debug/pyscript-phase3-chrome-build.log`，后续各项日志与 JSON 同在 `out/Debug/`。本次交付的 [结构化快照](pyscript-phase3-validation/results-2026-11-08.json)仅记录当时状态；后台实际完成后仍需检查结果、修复失败并补充验收记录。

## 7. 22:04 恢复与生成对象修复

首轮完整 Chrome 构建在 22:01 左右停止：耗时 48m05.80s，完成 947 项、失败 1 项、剩余 3,348 项。`blink_core.dll` 链接器报告 `core_initializer.obj` 的 procedure symbol 引用的 PDB item 不是有效的 function ID。后续队列正确停止，没有运行旧浏览器或未重新链接的测试程序。

先保存并重建了被诊断直接指向的 `core_initializer.obj`，重建后的 SHA-256 与旧件完全一致，单独链接仍出现相同错误。其局部函数 ID 与 18,328 项类型记录检查正常，不能据此认定这份对象损坏。sccache 统计显示当前 `-fmodules-cache-path` 调用均不可缓存；绕过缓存没有改变结果。

随后检查 `blink_core.dll` 的 5,040 个直接对象输入，核对 806 项目标函数哈希与类型记录。没有发现目标函数哈希的跨类型冲突，但发现已有 `initial_letter_utils.obj` 在类型索引 `0x41ae` 处存在无效记录长度。该旧件时间为 14:19，早于本轮阶段三构建。

保留旧件后重建 `initial_letter_utils.obj`，文件长度仍为 7,762,874 字节，SHA-256 已改变；两份重建对象的 COFF 结构和 CodeView 记录检查均通过。再次运行标准 `autoninja -C out/Debug blink_core.dll`，在 1m25.24s 内完成编译和链接，原 PDB 错误消失。异常对象的产生原因仍未确定，没有修改源码、关闭调试信息或放宽链接器诊断来绕过问题。

旧对象与元数据位于忽略目录 `out/Debug/pyscript-phase3-corrupt-objects/`。证据包括[首轮链接失败](pyscript-phase3-validation/chrome-initial-link-failure.log)、[对象哈希与恢复结果](pyscript-phase3-validation/object-recovery.json)、[5,040 项诊断结果](pyscript-phase3-validation/function-hash-check.json)、[重建对象结构检查](pyscript-phase3-validation/coff-rebuilt-check.json)、[CodeView 检查](pyscript-phase3-validation/coff-codeview-rebuilt-check.json)和[成功的 Blink Core 构建](pyscript-phase3-validation/blink-core-repaired-build.log)。

22:15 恢复标准 `autoninja -C out/Debug chrome python_runtime_unittests`，本轮待完成约 3,337 项；22:16 重新启动后续验证队列。实时构建进程为 `6736`，后续进程为 `16144`，仅用于本次本机恢复定位，退出后不代表其他同号进程仍属于此任务。当前恢复状态见 [resume-results-2026-11-08.json](pyscript-phase3-validation/resume-results-2026-11-08.json)。浏览器端到端、20 项 Blink 用例和 WPT 回归仍待构建完成后运行。
