# 原生 <pyscript> 与 CPython 第一版最小可行性验证方案

- 整理日期：2026-11-08（Asia/Shanghai）。
- 项目：Chromium 158.0.8126.0，首个验证平台为 Windows x64 Debug component build。
- 当前分支：`codex/kernel-debug-settings`；方案初版整理基点：`9257464d67`，阶段一依赖提交：`c53586b2f7`。
- 已选方向：Renderer 内嵌 CPython，Blink 原生识别 `<pyscript>`，在主线程同步执行，通过 C++ 函数桥接当前 Document 的有限 DOM 操作。
- 状态：2026-11-09 阶段一至五的第一版实现与必要验证完成，适用范围为 Windows x64 Debug 的受控实验配置。最终标准构建、运行时 22 / 22、Blink 28 / 28、DOM 浏览器 17 / 17、解析与权限浏览器 27 / 27、Renderer 沙箱检查 2 / 2、MCBE 17 / 17、MCJE 33 / 33 通过；WPT 两文件与 chromiumPlus 15 / 15 使用已记录诊断配置。chromiumPlus 默认模式仍因网络服务崩溃 / 超时失败，原始记录保留。详见[阶段五验收](pyscript-phase5-build-e2e-acceptance.md)的完整测试矩阵、配置和限制；[阶段一](pyscript-phase1-cpython-runtime.md)、[阶段二](pyscript-phase2-tags-parsing-execution.md)、[阶段三](pyscript-phase3-document-context-dom.md)与[阶段四](pyscript-phase4-budgets-navigation-cleanup.md)保留各阶段实施经过。
- 本次文档与既有 MCJE Windows SRV 回退工作分别管理，实施时不得覆盖工作区已有修改。

后续扩展（2026-11-09）：已增加设置页启用与页面权限模式，详见[内核调试设置与页面权限](pyscript-settings-and-page-permissions.md)。本文下述精确来源、顶层、脚本 / CSP 限制描述第一版受限模式；用户显式选择的放开模式及其验证另行记录，Renderer 沙箱保持生效。

交互扩展（2026-11-09）：[元素点击事件首版方案与步骤](pyscript-click-events-implementation.md)规划原生事件监听、Python callable 调用、交互预算和安全清理；[阶段一运行时基础](pyscript-event-phase1-runtime-callbacks.md)与[阶段二注册 / 移除及 Chrome 适配](pyscript-event-phase2-dom-registration-adapter.md)已完成，Blink 当前明确拒绝安装监听，真实点击派发与交互预算尚未接入。

## 1. 验证目标

验证浏览器能直接执行 HTML 内联 Python 代码，通过 C++ 调用 Blink DOM API，将计算结果显示在同一页面中。示例无需页面 JavaScript、Wasm 运行时或本机 Python 安装：

```html
<!doctype html>
<meta charset="utf-8">
<title>Native Python MVP</title>
<p id="result"></p>

<pyscript>
from dom import get_element_by_id, set_text

answer = 6 * 7
set_text(get_element_by_id("result"), f"Python 计算结果：{answer}")
print("Python block finished")
</pyscript>
```

首版验收：

1. 页面显示 `Python 计算结果：42`，Python 源码本身不显示。
2. 同一 Document 的多个代码块按顺序执行，并共享 Python 全局变量；不同 Document 使用独立解释器状态。
3. Python 异常进入 DevTools 控制台，包含代码块标识、源码位置和有界 traceback，不使 Renderer 崩溃。
4. 实验开关、页面资格、脚本权限和 CSP 能阻止执行。
5. DOM 句柄在文档失效后不能继续操作节点；刷新、导航和关闭不产生回调越界或重复执行。
6. 能对可控的纯 Python 死循环样例触发执行预算检查；不将此结果描述为任意 Python 代码的强制终止保证。

这是原生嵌入路径的可行性验证，不是面向任意互联网页面的 Python Web 平台发布。`<pyscript>` 是本项目的私有实验标签，与现有第三方 PyScript 产品没有运行时或 API 兼容承诺。

## 2. 第一版范围与明确语义

| 项目 | 第一版约定 |
| --- | --- |
| 启用方式 | 新增默认关闭的 Blink 实验特性 `PythonScript`，执行还需通过受控来源检查 |
| 页面范围 | 活动、已提交、未销毁的顶层 HTML Document；使用明确配置的本地测试 origin |
| 测试来源 | `http://127.0.0.1:<测试端口>` 的精确 origin；不把所有 localhost、所有 HTTP 或 `file://` 自动视为可信 |
| 代码来源 | 首次完整 HTML 文档解析过程中，由解析器创建的内联 `<pyscript>` |
| 执行时机 | 文档解析和既有延迟脚本完成后，在 `DOMContentLoaded` 派发前执行 Python 队列 |
| 同步含义 | 每个代码块在 Renderer 主线程同步执行，不切到 Worker，也不实现解析器阻塞脚本 |
| 执行顺序 | 保存解析器登记顺序；代码块在关闭标签时记录源码快照，之后按登记顺序执行 |
| 共享状态 | 同一 Document 共享 Python globals 和其子解释器；不同 Document 分离 |
| DOM 能力 | `get_element_by_id()` 与面向普通文本展示节点的 `set_text()` |
| 日志 | 有界的 `print`、stderr、异常与源码位置输出到当前页面控制台 |
| 一次性执行 | 每个已登记代码块至多准备一次、执行一次；被拒绝的块不会等待修改后重试 |
| 错误处理 | 普通语法或运行异常只使当前块失败，后续合格块继续；预算耗尽或文档失效则停止该文档剩余队列 |

暂不实现：

- `src` 外链代码、模块下载、`async` / `defer` 属性的脚本调度语义、顶层 await。
- 动态插入、`innerHTML`、`DOMParser`、template 内容、`document.write` 或克隆产生的代码执行。
- iframe、Worker、Worklet、扩展隔离世界、WebUI、MCBE / MCJE 结果页及 SVG / XML 中的 Python 执行。
- Python 与 JavaScript 对象互访、`globalThis` 映射、Python 事件回调、定时器、网络接口和完整 DOM 包装层。
- pip、任意第三方包、原生扩展、文件访问、进程启动，以及完整标准库的可用性承诺。
- prerender 执行或激活后补执行、含 Python 状态文档的 BFCache 保存 / 恢复。

遇到 `src` 属性时整块拒绝，不发起请求，也不退而执行内联内容。对不支持的调度属性应给出明确诊断，不能暗示已支持其标准 `<script>` 语义。

Python globals 与 JavaScript 全局对象独立；`globalThis.chromiumPlus` 不会自动出现在 Python 中。

## 3. 总体架构与模块职责

```text
HTML 完整文档解析
  -> HTMLPyScriptElement + RAWTEXT
  -> PythonDocumentContext 登记源码快照与来源位置
  -> DOMContentLoaded 前执行入口
  -> 特性 / origin / 文档状态 / 脚本权限 / CSP 检查
  -> CPython 文档子解释器：compile + exec
  -> 内建 dom 模块
  -> C++ 文档句柄表
  -> Blink Document / Element API
```

建议拆成以下组件，名称和最终文件布局在实施时按依赖关系调整：

| 组件 | 职责 |
| --- | --- |
| `PythonRuntime` | 每个 Renderer 的 CPython 初始化、版本与资源路径配置、子解释器创建 / 销毁、线程状态切换 |
| `PythonDocumentContext` | 文档资格、子解释器、globals、脚本队列、执行状态、预算、DOM 句柄及生命周期 |
| `HTMLPyScriptElement` | 原生元素、解析器创建标志、源码快照、源码位置、至多一次登记与执行状态 |
| `PythonDomModule` | 为当前子解释器注册 `dom` 模块、参数转换、句柄检查、Blink 调用及异常转换 |
| `PythonConsole` | stdout / stderr、traceback、UTF-8 与控制台消息长度限制 |
| Chrome / Content 嵌入层配置 | 将显式启用的实验配置和已验证的测试 origin 传入 Renderer，不让 Blink 依赖 Chrome 头文件 |

CPython 是第三方执行引擎依赖，不应只是在某个 HTML 元素方法中调用一次 `PyRun_SimpleString()`。运行时、文档状态、DOM 绑定和执行权限需要分别管理。既有 V8 `ScriptLoader` 的权限和时序可作为参考，不应直接把 Python 源码交给其 JavaScript 编译路径。

## 4. CPython 依赖、初始化与解释器

### 4.1 构建和资源

阶段一先锁定一个仍受维护、具有所需子解释器 API 的常规 GIL CPython 发布版，记录完整版本、来源、校验值、许可证和补丁。本文不虚构尚未选择的补丁版本或校验值。

建议新增 `third_party/cpython/` 的依赖定义与 GN 构建封装。至少验证：

- 与 Chromium 使用的 Windows x64 编译器、CRT 和构建参数兼容。
- Debug component build 中只有一份 CPython 运行时状态，不因多个组件重复链接而重复初始化。
- CPython 符号可见性、静态 / 动态链接方式和分发位置明确；不得依赖开发者机器 PATH 中的 DLL。
- 编译、导入和异常处理所需的内建 / 冻结模块、编码支持等已打包。不能直接清空模块路径后假定初始化仍能成功。
- 启动必需资源不依赖沙箱建立后的任意磁盘读取。需要 DLL 或资源文件时，应在 Renderer 沙箱约束下验证加载时机和可达性；优先使用可内嵌资源，不通过关闭沙箱解决。

解释器采用隔离配置，关闭环境变量、用户 site 和 `site` 自动导入，不加入当前目录、页面目录或用户安装目录。最小模块清单需要显式记录，支持 `dom` 和基本语言执行所需的运行时模块；标准库与原生扩展按最小需求裁剪，不从页面下载代码。

### 4.2 解释器模型

- Renderer 按需初始化一次 CPython。
- 每个合格 Document 按需创建独立子解释器，同一 Document 的代码块共享 globals。
- 使用所选 CPython 版本支持的子解释器配置 API，例如 `Py_NewInterpreterFromConfig()`；常规 GIL 构建不等于必须为每个子解释器开启独立 GIL。
- 子解释器的 `sys.modules`、内建 `dom` 模块和模块状态分别初始化。不要在静态全局变量中共享跨解释器 `PyObject*` 或 DOM 状态。
- `dom` 模块使用支持多解释器的初始化方式和每解释器模块状态；模块槽位与能力声明要匹配所选 CPython 版本。
- 显式管理当前 `PyThreadState` 与 GIL 的进入 / 退出；不要把通常隐含主解释器假设的 `PyGILState_*` 用法直接套到子解释器上。
- 不在每次页面导航时执行进程级 `Py_FinalizeEx()`。子解释器清理必须在所属线程、无活动执行且线程状态正确时完成；进程级最终清理策略单独验证。

仅使用一个主解释器并给每个 Document 分配不同的 globals，不满足文档模块状态分离要求。子解释器同样不提供安全隔离，也不能把 Renderer 进程隔离等同于 CPython 与同进程网页之间的完整语言沙箱。

## 5. 标签与 HTML 解析

### 5.1 原生元素与源码

在 HTML 标签表注册 `pyscript`，新增 `HTMLPyScriptElement` 及必要的元素类型 / 绑定定义、构建列表项。无需首版就为 JavaScript 暴露可执行 Python 的 IDL 方法。

开启特性后，使用 RAWTEXT 读取标签内容，保留 Python 所需的缩进和 `<`、`&` 字符，不按 HTML 实体转换源码。HTML 输入本身的换行规范化仍按解析器规则执行。

在关闭标签时，记录当前解析器来源、原始代码文本和位置。执行队列持有不可变源码快照：后续修改 `textContent`、移除再插入或移动元素都不生成新的执行任务。执行前仍需确认原始元素保持在同一合格 Document 中。

默认隐藏元素，避免 Python 源码参与页面展示。特性关闭时保持现有未知标签的解析与执行行为；隐藏样式和解析状态变化也必须纳入开关设计。

### 5.2 需要同步核对的解析路径

- HTML tree builder 对起始标签的处理与文本模式切换。
- tokenizer 的 RAWTEXT 状态选择和对应结束标签识别。
- fragment / `innerHTML` 的上下文初始状态。
- HTML 快速片段解析路径：若不支持该元素，应正确退回通用路径。
- 预加载扫描器及其 tokenizer 状态，避免 Python 字符串中的 `<img src=...>` 等内容触发推测请求。
- 分块输入、大小写、CRLF、缺失结束标签、template 和异常 HTML 恢复行为。

第一版建议仅对收到显式结束标签且通过完整文档来源校验的块登记执行；未关闭块不执行并给出可控诊断。RAW text 内出现字面量 `</pyscript>` 会结束标签，应在说明和测试中明确，不额外设计 Python 专属转义规则。

片段可以按正确的文本规则构造 DOM，但不具备执行资格。仅检查“由解析器创建”不足以区分完整文档与片段解析。

## 6. 调度、页面资格和执行权限

### 6.1 调度

在既有延迟脚本流程完成、`DOMContentLoaded` 尚未派发的安全入口执行文档 Python 队列。可从 `Document::FinishedParsing()` 前后相关调用链核对接入点，不能只按函数名判断该入口总能运行任意脚本。

要求：

1. 文档只运行一次已登记队列，执行前固定任务列表，DOM 变化不追加任务。
2. 先执行既有 HTML / JavaScript 调度流程，再执行 Python 队列，随后派发 `DOMContentLoaded`；不承诺与 async JavaScript 的额外相对顺序。
3. 不从构造函数、`InsertedInto()` 或 `ChildrenChanged()` 任意执行 Python。
4. 不在 `ScriptForbiddenScope`、解析器不可重入区段或 GC 回收回调中进入 Python。
5. 不嵌套运行消息循环，不主动执行 JavaScript；如 DOM 操作引发上下文变化或其他语言重入，阻止 Python 嵌套进入并重新核对文档状态。
6. 成功与失败状态分别记录。普通异常继续后续块；预算耗尽或导航失效后不执行剩余块。

### 6.2 双重启用条件

新增 `PythonScript` 实验特性，默认关闭，同时限制到显式配置的测试 origin。实现时在 Chrome / Content 层校验并传递配置，Blink 只接收已定义的数据与策略，不直接读取 Chrome 私有状态。

页面资格至少要求：顶层 HTML、真实活动文档、非片段、非 prerender、非销毁状态、当前 origin 精确匹配测试允许项。拒绝 opaque origin、WebUI、扩展和自定义协议页面，不因父页合格而允许子框架。

默认关闭与受控 origin 是实验范围约束，不是提供给任意不可信 Python 代码的安全保证。阶段二已实现 `--enable-blink-features=PythonScript` 与 `--native-python-allowed-origin=<精确本地HTTP(S) origin>`；校验规则、执行器接口及验证状态见[阶段二实施记录](pyscript-phase2-tags-parsing-execution.md)。

### 6.3 CSP 与脚本权限

沿用现有页面“是否允许执行脚本”的判定，以及 CSP 的内联脚本检查。原生 Python 必须遵守：

- 用户或设置禁用脚本、sandbox 无 `allow-scripts` 时拒绝执行。
- `script-src` / `script-src-elem` 及其 `default-src` 回退，按现有 Blink 内联脚本有效指令逻辑处理。
- nonce、源码 hash、Report-Only 和正式拒绝所对应的现有 CSP 行为与报告。
- 使用实际准备执行的源码快照与原始位置检查，不能改写源码后再用不一致的内容计算 CSP。

第一版没有动态 Python 执行方法，也不增加可执行的动态文本赋值入口。后续若开放动态标签或求值 API，必须重新设计 Trusted Types 等代码入口限制，不能直接沿用本版的惰性文本属性。

MCBE / MCJE 结果页当前具有禁止脚本的 CSP，本方案不放宽这些响应头。

## 7. 最小 DOM 桥接

内建模块的第一版接口为：

```python
node = dom.get_element_by_id("result")  # 找不到时返回 None
dom.set_text(node, "Python result")
```

行为约定：

- 查询只针对当前 Document 的普通 light DOM，不暴露其他 frame、window 或封闭 shadow root。
- `get_element_by_id()` 返回专用 `ElementHandle`，其内部绑定文档身份、代数和句柄编号；页面不能从一个整数构造任意句柄。
- `set_text()` 直接调用 Blink 文本 DOM API，按纯文本写入，不解析 HTML，也不执行 JavaScript。
- 首版只允许 HTML 命名空间的普通 `p`、`div`、`span` 文本展示节点，排除自定义元素及其定制内建形式；为降低首版重入范围，可拒绝含元素子节点的替换操作。
- 对 `script`、`pyscript`、`style` 等节点拒绝写入；不提供属性、URL、`innerHTML` 或脚本执行桥接。
- 文本严格转换为有效字符串并检查字节长度，不静默接受任意对象后触发不受控的隐式转换。
- 参数类型错误抛出 Python `TypeError`；过期或失效句柄抛出 `ReferenceError`；受限目标给出明确异常。Blink 异常需要转换，不能跨 C API 边界泄漏 C++ 异常。

DOM 句柄表由 Blink 可追踪对象管理，显式 Trace 对应的弱节点引用。Python 对象不直接拥有裸 `Node*`，也不为每个句柄无限增加强 Persistent 根。调用时同时验证文档代数、当前节点所属文档、连接状态和允许的节点种类。

普通 DOM 操作也可能间接引发自定义元素反应、页面回调或导航；桥接实现不能假定它绝无重入。每次调用前后检查运行状态，并避免在检查通过后跨可重入操作长期保存裸指针。

创建元素、追加节点、属性操作和完整 DOM 对象封装留到后续版本，在这两个函数验证通过后再扩展。

## 8. 日志、执行预算与生命周期

### 8.1 日志与源码位置

stdout / stderr 使用解释器内受控对象转发到当前文档控制台。采用有界缓冲，正确处理 UTF-8 与分段写入。异常提供有界 traceback、块编号、文档 URL 和原始行号信息，不注入页面 HTML。

编译使用可识别的虚拟文件名，例如文档 URL 加 `#pyscript-1`，同时记录标签正文起始行用于位置映射。DevTools Python 断点、变量检查和单步执行不在首版范围内。

### 8.2 初始预算

以下为实施起点，最终阈值在实测后记录；不能为通过测试而悄悄取消预算：

| 资源 | 建议起始上限 |
| --- | --- |
| 源码 | 每块 64 KiB，文档至多 16 块 |
| 控制台输出 | 每文档累计 16 KiB，超出后明确截断 |
| 单次 DOM 文本 | 16 KiB |
| DOM 调用 | 每文档至多 1024 次 |
| 同步执行时间 | 单块约 100 ms，文档累计约 250 ms，属于协作式预算 |

在编译 / 执行边界、CPython monitoring 或 opcode trace 回调和 DOM 桥接入口检查预算；解释器初始化成本另行测量和记录。计时不得只在 `exec` 返回后检查。

这些限制不能硬性约束所有 CPython 内存分配，也不能打断所有耗时内建操作、原生调用或恶意捕获异常的代码。`while True` 可控样例被终止不等于支持安全运行任意恶意 Python。V8 终止 API不能代替 CPython 中断，也不能强杀 Renderer 主线程后继续使用其进程。

不把 import 黑名单、禁用 `os` 或子解释器当作安全沙箱。面向任意公网代码开放前，必须另行设计执行隔离、硬资源约束和 DOM 权限模型，可能需要改变“主线程同步直连 DOM”的前提。

### 8.3 文档生命周期

- 页面导航、Document 被替换、frame detach、上下文销毁时，先把上下文标记为不可调用，清空待执行队列并使全部句柄代数失效。
- 同时观察 Document 身份与相关执行上下文生命周期，不能假定所有文档替换都等于窗口对象销毁。
- 已在执行中的 Python 通过预算 / 桥接检查感知失效；不在 Python 仍位于栈上时销毁对应子解释器。
- 子解释器清理安排在受控的主线程执行边界，先解除 DOM 能力再释放 Python 状态，防止 Python finalizer 重入有效 DOM。
- 不在 Oilpan `Trace()`、GC 析构或弱引用清理回调内执行任意 Python finalizer。
- 初版使用已有生命周期 / BFCache 机制使创建 Python 状态的文档不进入 BFCache，具体标记和原因在实现时核对；不通过添加假的 unload 监听器实现。
- 同文档 fragment 导航不重复执行；普通刷新创建新上下文。初版跨文档前进 / 后退按未使用 BFCache 的重新加载路径验证。

## 9. 源码接入清单

已核对存在的接入参考：

| 文件 | 用途 |
| --- | --- |
| [html_tag_names.json5](../third_party/blink/renderer/core/html/html_tag_names.json5) | 新增标签及元素映射 |
| [html/build.gni](../third_party/blink/renderer/core/html/build.gni) | HTML 元素源码构建列表 |
| [html_script_element.h](../third_party/blink/renderer/core/html/html_script_element.h) 与 [html_script_element.cc](../third_party/blink/renderer/core/html/html_script_element.cc) | 解析器创建标志、一次执行、源码与 CSP 行为参考 |
| [html_tree_builder.cc](../third_party/blink/renderer/core/html/parser/html_tree_builder.cc) | 完整文档 RAWTEXT 接入 |
| [html_tokenizer.cc](../third_party/blink/renderer/core/html/parser/html_tokenizer.cc) | RAWTEXT 状态、`UpdateStateFor()` 和结束标签识别 |
| [html_document_parser.cc](../third_party/blink/renderer/core/html/parser/html_document_parser.cc) | 片段上下文与解析完成时序 |
| [html_preload_scanner.cc](../third_party/blink/renderer/core/html/parser/html_preload_scanner.cc) | 推测扫描与代码文本中的伪资源 |
| [html_document_parser_fastpath.cc](../third_party/blink/renderer/core/html/parser/html_document_parser_fastpath.cc) | 片段快解析的支持或正确回退 |
| [document.cc](../third_party/blink/renderer/core/dom/document.cc) | `FinishedParsing()`、文档状态和 DOMContentLoaded 顺序 |
| [script_loader.cc](../third_party/blink/renderer/core/script/script_loader.cc) | 脚本权限、CSP 和执行条件参考 |
| [content_security_policy.h](../third_party/blink/renderer/core/frame/csp/content_security_policy.h) | 现有内联脚本策略入口 |
| [runtime_enabled_features.json5](../third_party/blink/renderer/platform/runtime_enabled_features.json5) | 新增实验特性 |
| [html.css](../third_party/blink/renderer/core/html/resources/html.css) | 特性开启时的默认隐藏方式参考 |
| [chrome_content_renderer_client.cc](../chrome/renderer/chrome_content_renderer_client.cc) | Chrome 嵌入配置与 Renderer 初始化接入参考 |

阶段一已新增 `third_party/cpython/`、`components/python_runtime/`、Renderer 固定诊断接入及基础测试。阶段二已新增 `HTMLPyScriptElement`、`PythonScriptController`、公开嵌入执行器接口、Chrome 来源配置及对应构建项与测试。阶段三已扩展为文档持久上下文、每解释器 `dom` 模块、弱节点句柄和最小生命周期接入；完整构建、运行时与 Blink 定向单测、浏览器测试及已记录诊断配置下的 WPT 均通过，阶段三计划内验收完成。

Blink Core 修改遵循 [core/AGENTS.md](../third_party/blink/renderer/core/AGENTS.md)，测试优先采用其要求的 WPT 框架与仓库适用的实验特性套件配置。

## 10. 分阶段实现步骤

### 阶段一：CPython 依赖与独立运行时

实施状态：已完成。版本、源码校验、模块清单、沙箱适配、实测成本与原始结果见[阶段一实施记录](pyscript-phase1-cpython-runtime.md)。此状态只覆盖阶段一。

1. 锁定 CPython 版本、来源、校验值、许可证和模块清单。
2. 完成 Windows x64 GN 构建、资源封装和链接方式，验证 component build 只有一份运行时。
3. 实现隔离初始化、子解释器创建、线程状态切换和受控销毁。
4. 执行字符串计算、中文字符串、普通异常和有界输出，验证两个子解释器状态独立。
5. 在实际 Renderer 沙箱中验证初始化和必要资源，不只运行一个沙箱外测试程序。

阶段产物：可编译的运行时封装、模块与分发说明、基础测试及初始化成本记录。尚不执行网页标签。

### 阶段二：标签、解析与执行条件

实施状态：已完成本阶段实现与计划内验证。Chrome、Blink 单元测试与网页测试目标构建通过；浏览器端到端 26 / 26、阶段一 Renderer 沙箱回归 2 / 2、运行时单测回归 9 / 9、Blink 用例 13 / 13 通过。两个 WPT 文件覆盖 7 个子测试，在本机字体检查与进程内网络服务诊断配置下通过。默认独立网络服务的启动问题仍单独记录，不算作默认模式通过。实际结果与阶段边界见[阶段二实施记录](pyscript-phase2-tags-parsing-execution.md)。

1. 加入默认关闭的 `PythonScript` 特性及精确测试 origin 配置。
2. 注册元素与构建项，实现 RAWTEXT、默认隐藏、源码快照和一次登记状态。
3. 同步处理通用 / 快速 / 片段解析与预加载扫描；片段始终保持不可执行。
4. 加入合格顶层文档判定、现有脚本权限与 CSP 检查。
5. 接入文档解析完成后的队列入口，先以无 DOM 副作用的测试执行器验证时序，再接入阶段一运行时。

阶段产物：内联计算与 print 可运行，DCL 顺序正确，关闭特性和不合格来源均不执行，伪 HTML 不生成节点或推测请求。

### 阶段三：文档上下文与最小 DOM 桥接

实施状态：2026-11-09 完成阶段三计划内验收。完整 Chrome 与 Blink 测试目标构建、运行时 15 / 15 单测、Blink 定向单测 20 / 20、GN / include 检查、DOM 浏览器测试 9 / 9 和阶段二浏览器回归 26 / 26 均通过，两组浏览器测试保留 Renderer 沙箱并使用默认网络服务配置。启用 / 关闭两组 WPT 沿用字体检查与进程内网络服务诊断参数，均为 PASS。持久解释器所必需的撤销、延迟清理与 BFCache 限制已提前接入，完整阶段四仍未完成。详见[阶段三实施记录](pyscript-phase3-document-context-dom.md)。

1. 把子解释器、globals、队列、控制台和状态归属到 Document。
2. 以每解释器模块状态实现 `dom`，加入两个最小接口。
3. 实现可追踪的弱节点句柄、文档代数、类型转换和 Blink 异常转换。
4. 明确允许的普通文本元素及替换条件，拒绝代码 / 样式 / 自定义元素等目标。
5. 跑通 `42` 示例、多块共享变量、不同 Document 独立和 HTML 字符纯文本展示。

阶段产物：不经 JavaScript 的原生 Python -> C++ -> Blink DOM 链路。

### 阶段四：预算、导航与安全清理

实施状态：2026-11-09 已完成。完整标准构建、运行时 22 / 22、Blink 28 / 28、DOM 浏览器 17 / 17、解析与权限回归 27 / 27 均通过；两组 WPT 在已记录诊断配置下通过。包括预算锁存、逐块控制台停止、安全任务清理、GC / 关页计数、导航与前进 / 后退验证。详见[阶段四实施记录](pyscript-phase4-budgets-navigation-cleanup.md)。

1. 实现源码、输出、DOM 操作和协作式执行预算。
2. 加入不可逆的停止状态、Python 重入保护及每次桥接的状态复核。
3. 处理 Document 替换、导航、frame detach、关闭和 Python finalizer。
4. 在安全线程 / 栈边界销毁子解释器，验证循环引用和反复加载不积累文档上下文。
5. 加入 BFCache 限制、prerender 拒绝和同文档导航不重跑的行为。

阶段产物：可控超时样例、失效句柄错误、稳定的文档清理与有界诊断。对无法强制中断或硬限内存的情况如实记录。

### 阶段五：构建、端到端验证与记录

实施状态：已完成第一版范围内的构建、测试矩阵核对、真实沙箱浏览器验证、既有功能回归、失败 / 诊断复测、源码校验及截图记录。WPT 和 chromiumPlus 的诊断配置、chromiumPlus 默认模式失败均明确保留，不算作默认配置全部通过。详见[阶段五验收记录](pyscript-phase5-build-e2e-acceptance.md)。

1. 运行相应单元测试与 WPT；检查 expectations，避免把预期失败视作功能通过。
2. 构建完整浏览器，在实际沙箱、真实 HTTP 页面和独立测试 Profile 下验证。
3. 通过 CDP 记录 DOM、控制台、CSP 违规和请求日志；确认样例无需 JavaScript 执行桥接。
4. 检查特性关闭时的既有页面行为，并运行 chromiumPlus、MCBE、MCJE 的适当回归。
5. 保存真实构建命令、版本 / 校验值、测试统计、失败与复测记录、限制和页面截图。

阶段产物：`docs-codex/` 中的实施记录与验收证据。没有实际完成的步骤继续标记为未完成。

## 11. 测试矩阵

| 领域 | 必须覆盖 |
| --- | --- |
| 解析 | `<`、`&`、缩进、中文、换行、网络分块、大小写结束标签、缺失结束标签、字面量结束标签 |
| 扫描与片段 | Python 字符串内的伪 img / script / link 不发请求；快解析正确退回；innerHTML / DOMParser / template 不执行 |
| 顺序 | 多块依登记顺序、同文档共享 globals、先完成既有延迟脚本、DCL 监听器能看到 Python 结果 |
| 一次执行 | 修改源码、移除重插、移动、克隆、同文档 fragment 不触发额外执行；普通刷新重新执行 |
| 权限 | 默认关闭、非允许 origin、无 frame 文档、iframe、opaque origin、WebUI、自定义 scheme、prerender 拒绝 |
| CSP | `script-src 'none'`、`script-src-elem`、default-src 回退、有效 / 无效 nonce、hash、Report-Only、禁用脚本与 sandbox |
| 解释器 | 文档间 globals / 模块状态隔离、同 Renderer 重复创建与销毁、正确 GIL / 线程状态、不依赖环境与用户 Python |
| DOM | 找不到 ID、中文文本、HTML 字符不解析、错误类型、受限元素、移除或跨文档节点、伪造 / 过期句柄 |
| 错误 | 语法和运行异常、traceback 位置、后续块继续、无效文本编码、输出截断 |
| 预算 | 源码 / 块数 / 文本 / 输出 / DOM 调用边界、可控纯 Python 循环、总预算耗尽后队列停止 |
| 生命周期 | 导航、Document 替换、关闭、GC、finalizer、重入、BFCache 限制和长时间反复加载 |
| 产品 | 沙箱开启、开关关闭回归、chromiumPlus / MCBE / MCJE 既有功能 |

执行预算测试须由测试宿主设置外部超时，必要时只终止测试创建的浏览器进程，避免卡住整个验证流程；这种测试清理不能被描述为产品已有的 CPython 硬中断机制。

## 12. 构建与验证命令约定

当前目录使用 `out/Debug`。以下构建和验证入口均已实际执行，完整命令、配置及结果见各阶段记录：

```powershell
autoninja -C out/Debug chrome
autoninja -C out/Debug blink_unittests blink_tests
```

运行新增 Blink 单元测试时，使用实际注册的测试名：

```text
out/Debug/blink_unittests.exe --gtest_filter=<实际新增测试类或测试名>
```

Web 行为测试优先使用 WPT 框架，按本项目实验特性建立明确启用的测试套件；私有 `<pyscript>` 不因此成为通用 Web 标准。当前两个文件注册在 `wpt_internal/pyscript/`，本仓库由 `run_web_tests.py` 与 `content_shell` 运行；`run_wpt_tests.py` 会跳过 `wpt_internal`。基本入口如下，本机复现所需的缓存、字体与网络服务诊断参数见阶段二实施记录：

```text
vpython3 third_party/blink/tools/run_web_tests.py -t Debug --additional-driver-flag=--enable-blink-features=PythonScript wpt_internal/pyscript/parsing-enabled.html
vpython3 third_party/blink/tools/run_web_tests.py -t Debug --additional-driver-flag=--disable-blink-features=PythonScript wpt_internal/pyscript/feature-disabled.html
```

浏览器端到端脚本为 `chrome/test/pyscript_smoke_test.mjs` 与 `chrome/test/pyscript_dom_smoke_test.mjs`，自行启动本地 HTTP 服务，并以 `--enable-blink-features=PythonScript --native-python-allowed-origin=http://127.0.0.1:<实际端口>` 配置实验范围及独立 Profile。具体命令和最终结果见阶段五记录。

测试不使用 `--no-sandbox`、全局关闭 CSP 或页面 JavaScript 求值代替原生 Python 执行。验证脚本可以用 CDP 读取结果，但样例的计算和 DOM 写入必须由 Python / C++ 完成。

## 13. 第一版完成标准与后续边界

仅在阶段一至五的必要工作完成、有真实构建与测试记录后，才能把本文状态改为“验证完成”。至少保存：

- CPython 精确版本、校验值、链接 / 资源方案和最小模块清单。
- 顶层示例结果、两个代码块共享状态、不同文档独立状态的证据。
- 权限 / CSP 拒绝、原始文本解析、无伪资源请求的证据。
- 超时样例、导航后句柄失效、反复加载清理和异常控制台定位结果。
- 适用范围、已知失败、复测及无法提供硬中断 / 硬内存限额的说明。

后续可分别研究外链加载、事件回调、异步执行、DOM 对象封装、iframe 和第三方包。任意公网代码的执行能力需要独立的隔离评审与架构设计，不把最小验证自然扩展为默认开启的浏览器能力。
