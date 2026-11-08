# 原生 Python 阶段二：标签、解析与执行条件

- 日期：2026-11-08（Asia/Shanghai）。
- 工作区：`C:\src\src`，分支 `codex/kernel-debug-settings`。
- 实施依据：[第一版方案](pyscript-native-python-mvp-implementation.md)的阶段二。
- 状态：实现与测试已加入，定向编译与运行时回归通过；恢复构建时已修复 StyleAdjuster 对新元素类型的穷举遗漏。当前优先构建 Chrome，新增 Blink 测试、WPT 和网页端验证尚未运行，阶段二运行验收未完成。
- 已按“关键部分完成后可提交”的授权建立阶段一依赖提交 `c53586b2f7`；阶段二实现接入单独提交，验证中的后续修复再分别记录。既有 MCJE 修复保留在工作区。

## 1. 启动与示例

首个平台为 Windows x64。必须同时显式开启 Blink 特性，并配置一个精确的本地测试 origin：

```powershell
python -m http.server 8000 --bind 127.0.0.1 --directory chrome/test/data/pyscript
# 在另一个终端启动：
out/Debug/chrome.exe --enable-blink-features=PythonScript --native-python-allowed-origin=http://127.0.0.1:8000 http://127.0.0.1:8000/phase2.html
```

允许项只接受 HTTP(S) 的 `127.0.0.1`、`[::1]` 或 `localhost` origin；端口属于匹配条件。不接受凭据、非根路径、query、fragment、通配符或列表。Browser 校验并规范化后传给 Renderer，Renderer 再校验一次。`localhost` 与 `127.0.0.1` 是不同 origin。

```html
<!doctype html>
<meta charset="utf-8">
<p>结果请查看 DevTools Console。</p>
<pyscript>
answer = 6 * 7
print("Python 世界", answer)
</pyscript>
<pyscript>
print("同批次变量共享", answer + 1)
</pyscript>
```

控制台应出现 `Python 世界 42` 和 `同批次变量共享 43`。标签默认隐藏，其 JavaScript 接口是 `HTMLElement`，没有执行 Python 的 IDL 方法。

`PythonScript` 默认关闭，也不随通用 experimental 开关开启。关闭时标签保持 `HTMLUnknownElement`、普通 HTML 子节点与 inline 样式行为。阶段一的 `NativePythonRuntimeSelfTest` 只运行固定诊断，不能启用页面 Python。

## 2. 解析与源码

完整文档 tree builder 将标签内容切换到 RAWTEXT：缩进、`<`、`&amp;` 等源码原样保留，网络输入中的 CRLF 按 HTML 规则规范化为 LF。结束标签大小写不敏感；开始标签的自闭合标记不会结束 RAWTEXT。

源码中出现字面量 `</pyscript>` 会立即结束标签，即使它位于 Python 字符串中。没有额外的 Python 字符串转义规则。缺失显式结束标签时不执行该块，并给出诊断。

预加载扫描器通过 tokenizer 的同一状态选择识别 RAWTEXT，避免 Python 字符串内伪造的 img、script、link 触发请求。片段解析、`innerHTML`、Range、DOMParser、template 同样保留文本，但不登记执行；快速片段解析不支持此标签时回退到通用解析器。

收到结束标签后保存不可变源码、nonce 和位置快照。解析期间被 DOM API 改写过正文的块不登记。关闭标签后的 `textContent` 修改不替换已登记的源码；克隆和动态创建不登记；移除重插不会新增执行任务。执行时原元素必须仍连接在原 Document 的 light DOM 中。

首版拒绝 `src`、`async`、`defer`、`type`，有 `src` 时也不请求或改为执行内联正文。每个可登记块最多 64 KiB UTF-8，队列最多 16 块。

为避免原始网络输入与 `document.write()` 输入跨 token 拼接，采用保守规则：**只要解析器插入过 document.write 输入，该文档的整个 Python 队列都不执行**。由脚本创建的文档解析器同样不具备资格。

## 3. 执行与权限

`Document::FinishedParsing()` 中，既有 deferred JavaScript 已执行完毕，在派发 `DOMContentLoaded` 之前消费一次固定队列。

执行条件包括：

- 特性开启、嵌入执行器已安装、精确 origin 匹配。
- HTTP(S) 顶层 HTML 活动文档，具有已连接的实际 frame，且仍是当前 Document。
- 非初始空文档、文本包装文档、DOMParser 文档、prerender、opaque origin、iframe 或片段来源。
- 不处于 `ScriptForbiddenScope`，既有 `CanExecuteScripts(kAboutToExecuteScript)` 允许脚本。
- 当前 CSP 对原始源码快照允许内联脚本。

CSP 使用既有 `AllowInline(kScript, ...)` 路径，复用 `script-src`、`script-src-elem`、`default-src` 回退、nonce、源码 hash 和 Report-Only 行为。nonce 除既有 duplicate attribute / dangling markup 检查外，还检查 `<pyscript` 信号。没有放宽其他协议结果页的 CSP。

Blink 通过公开的 `WebPythonScriptRunner` 调用嵌入层；Core 不引入 Chrome 或 CPython 依赖。Chrome Renderer 执行器在一次同步批次中创建一个阶段一子解释器，各块共享 globals，普通异常继续后续块，返回前销毁子解释器。当前不跨批次或导航保留 Python 状态。

stdout / stderr 与有界异常信息进入文档控制台。控制台消息定位到原始块起始位置，Python traceback 使用 `文档URL#pyscript-N` 虚拟文件名。**traceback 的 Python 行号当前仍相对于块正文，未实现全文档行号映射。**

## 4. 阶段边界

尚未加入 `dom` 模块、文档持久上下文、DOM 句柄、Python 回调或执行超时。阶段三负责文档上下文与最小 DOM 桥接，阶段四负责预算和后续生命周期控制。

沿用阶段一的每次执行 16 KiB 输出上限；文档累计预算尚未实现。当前同步执行没有时间中断机制，只适合显式允许的本地受控短代码。测试宿主的外部超时不属于产品执行预算。

阶段二子解释器在同步批次返回前已销毁，不会把活跃解释器交给 GC 或 BFCache 管理；页面从 BFCache 恢复也不会重新消费已执行的队列。持有文档 Python 状态后的 BFCache 禁用将在后续阶段实现。

## 5. 验证入口

```powershell
autoninja -C out/Debug chrome blink_unittests blink_tests

out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.*

python third_party/blink/tools/run_wpt_tests.py -t Debug --additional-driver-flag=--enable-blink-features=PythonScript wpt_internal/pyscript/parsing-enabled.html
python third_party/blink/tools/run_wpt_tests.py -t Debug --additional-driver-flag=--disable-blink-features=PythonScript wpt_internal/pyscript/feature-disabled.html

node chrome/test/pyscript_smoke_test.mjs --output=out/Debug/pyscript-smoke.json
```

新增 SimTest 使用记录型执行器检查 Blink 行为；WPT 检查 RAWTEXT、序列化、片段、预加载和默认关闭行为；端到端脚本使用真实本地 HTTP 服务、独立临时 Profile、CDP 日志与请求记录，保留 Renderer 沙箱。

端到端覆盖多块共享变量、中文、错误恢复、defer / Python / DCL 顺序、源码快照、动态与子框架拒绝、分块与 CRLF、缺失关闭标签、源码和块数上限、输出截断、CSP、禁用脚本、origin 配置和导航隔离。prerender 通过 Blink SimTest 验证。

## 6. 实际验证结果

| 检查 | 当前结果 |
| --- | --- |
| 新元素、控制器、Chrome 执行器及初版 Blink 测试源码 | 定向编译通过；初版 10 个测试仅编译，未运行 |
| Document、tree builder、document parser、tokenizer、序列化、元素工厂及 Chrome Browser / Renderer 接入点 | 8 个目标定向编译通过 |
| `python_runtime_unittests` 回归 | 9 / 9 通过 |
| GN 头文件依赖检查 | Chrome Renderer、Chrome common_lib、Blink Core 均通过 |
| include 规则检查 | Chrome Renderer 与 Blink core/html 均通过 |
| C++ 格式、新端到端脚本语法、`git diff --check` | 通过；既有 MCJE 文件仅提示 CRLF 将被转换为 LF |
| 原 `chrome blink_unittests blink_tests` 完整构建 | 55 分钟后因 StyleAdjuster 缺少新元素的穷举分支而失败，已修复并通过定向复编 |
| 当前 `chrome` 构建 | 正在后台进行，尚未完成链接；目前未见新的编译错误 |
| `HTMLPyScriptElementTest.*` | 当前 13 个用例的源码已通过定向编译，未运行 |
| 两个新增 WPT 文件 | 共 7 个子测试，未运行 |
| 新浏览器端到端脚本 | 26 项检查已写入，未运行 |
| 阶段一 Renderer smoke、chromiumPlus 等浏览器回归 | 本轮尚未重跑 |

首轮定向编译发现 `HeapVector<WeakMember<...>>` 不受支持，以及 `HTMLDocumentParser` 没有对应的 DowncastTraits。已改为“强队列项 + 弱元素引用”，并让 DocumentParser 默认拒绝、HTMLDocumentParser 显式提供原始输入资格，再次编译通过。首次完整构建为提前验证接口而主动停止，定向编译完成后已恢复完整构建。

2026-11-08 恢复工作时检查到，随后进行的完整构建因 `StyleAdjuster::GetElementTypeCacheKey()` 的穷举遗漏停止。`HTMLPyScriptElement` 没有额外的样式调整行为，已将它归入既有的普通 HTML 元素缓存分组。修复后的 `style_adjuster.obj` 和包含全部 13 个用例的测试源码在同一次定向构建中通过；CPython 运行时再次回归 9 / 9 通过。暂存的 775 个上游文件也逐字节校验通过，包括被全局忽略规则排除、需要显式加入的 `_freeze_module.vcxproj`。

为先取得可运行浏览器产物，当前后台命令调整为：

```powershell
autoninja -C out/Debug chrome *> out/Debug/pyscript-phase2-chrome-build.log
```

本轮观察到的 siso 进程 PID 为 `4084`；上次交付时的 `16328` 已结束。PID 只表示观察值，不应在后续会话中未经确认就用来操作进程。继续工作时先检查进程和实时日志，避免在同一输出目录并发启动构建。Chrome 构建成功后先运行浏览器 smoke，再顺序构建 `blink_unittests blink_tests` 并运行第 5 节测试与既有浏览器回归。后续仍需确认构建过程中修改的源码已被最终增量构建覆盖。

证据：

- [交付状态与待验证清单](pyscript-phase2-validation/results-2026-11-08.json)。
- [本轮恢复记录](pyscript-phase2-validation/resume-results-2026-11-08.json)。
- [StyleAdjuster 构建失败诊断](pyscript-phase2-validation/style-build-failure.log)与[修复及 13 用例源码定向构建](pyscript-phase2-validation/style-and-tests-build.log)。
- [本轮运行时回归](pyscript-phase2-validation/resume-runtime-unittests.json)与[日志](pyscript-phase2-validation/resume-runtime-unittests.log)。
- [定向构建成功日志](pyscript-phase2-validation/targeted-build.log)。
- [既有接入点构建成功日志](pyscript-phase2-validation/integration-build.log)。
- [运行时测试结果](pyscript-phase2-validation/runtime-unittests.json)与[日志](pyscript-phase2-validation/runtime-unittests.log)。
- [首次定向构建失败及诊断](pyscript-phase2-validation/initial-targeted-build-failure.log)。
- [完整构建交付时快照](pyscript-phase2-validation/background-build-snapshot.log)；实时日志保存在 `out/Debug/pyscript-phase2-build-final.log`，快照不是最终构建结果。

未设置任何预期失败来掩盖结果。原始交付快照保留历史观察值；本轮结果以恢复记录为准。实现提交和编译通过均不表示尚未运行的网页检查已经通过。
