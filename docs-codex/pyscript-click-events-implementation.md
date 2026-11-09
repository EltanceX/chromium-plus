# 原生 Python：元素点击事件首版实现方案与步骤

- 整理日期：2026-11-09（Asia/Shanghai）。
- 实现基线：`dabd65e514`，已完成 Python 解释器设置与页面权限模式。
- 状态：阶段一运行时回调注册表、同步调用与只读事件快照已完成；网页监听 API、Blink click 派发与交互预算仍未接入。详见[阶段一实施记录](pyscript-event-phase1-runtime-callbacks.md)，本文页面示例尚不能作为现有功能使用。
- 目标平台：沿用 Windows x64 Debug，Renderer 主线程内嵌 CPython。
- 前置记录：[Python MVP](pyscript-native-python-mvp-implementation.md)、[文档上下文与 DOM 桥接](pyscript-phase3-document-context-dom.md)、[预算与清理](pyscript-phase4-budgets-navigation-cleanup.md)、[内核调试设置与页面权限](pyscript-settings-and-page-permissions.md)。

## 1. 目标与首版范围

允许页面在获准执行的 `<pyscript>` 中定义 Python 函数，为当前 Document 的元素注册点击监听；点击时在该 Document 已有的子解释器中调用函数。Python 全局变量和闭包在多次点击间保留，不为每次事件重新创建解释器。

采用 Blink 原生事件监听器，经 Chrome 适配层调用 CPython callable。初始化代码和事件回调均在 Renderer 主线程同步执行；Renderer 沙箱和现有受限 DOM 写入规则继续生效。

| 项目 | 首版约定 |
| --- | --- |
| 事件 | 仅 `click`，允许真实输入、键盘激活产生的 click，以及页面脚本生成的 click |
| 注册目标 | 当前 Document 中、已连接的普通 HTML Element；排除 Shadow DOM、SVG、定制内建元素及自定义元素 |
| 注册方式 | Python 函数调用 `dom.add_event_listener()`；支持显式移除和 `once=True` |
| 派发阶段 | 冒泡阶段监听，即 `capture=False`；目标阶段按 Blink 正常规则运行 |
| 调用 | 事件派发过程中同步调用；每次传入一个只读事件快照，返回值忽略 |
| Python 状态 | 复用当前 Document 的子解释器、globals、模块和闭包 |
| 初始化来源 | 保留完整 HTML 首次解析、内联、至多一次等 `<pyscript>` 执行条件 |
| 权限模式 | 沿用解释器总开关、受限来源与放开页面限制模式 |
| 页面能力 | 沿用现有 `get_element_by_id` 与受限 `set_text`；注册监听不扩大文本写入范围 |
| 后续扩展 | 捕获阶段、其他事件类型、`prevent_default()`、停止传播、完整 Event 对象、异步 callable、定时器和 asyncio 单独设计 |

首版不引入 HTML Python 事件属性，例如 `py-click="..."`，也不改变 HTML `onclick` 的现有含义。动态插入 `<pyscript>` 仍不执行；已经授权的 Python 回调可以为当前文档中符合条件的元素新增监听。

## 2. 建议 API 与页面示例

```html
<!doctype html>
<meta charset="utf-8">
<title>Native Python click</title>

<button id="increment">增加</button>
<p id="counter">0</p>

<pyscript>
import dom

count = 0
counter = dom.get_element_by_id("counter")

def handle_click(event):
    global count
    count += 1
    dom.set_text(counter, str(count))

listener = dom.add_event_listener(
    dom.get_element_by_id("increment"),
    "click",
    handle_click,
)
</pyscript>
```

建议签名：

```python
listener = dom.add_event_listener(element, "click", callback, *, once=False)
dom.remove_event_listener(listener)
```

语义约定：

1. `element` 必须是本子解释器生成的有效 `ElementHandle`；不存在的元素、跨文档句柄和失效代数均不能注册。
2. `callback` 必须可调用，接受一个事件参数；仅支持同步执行。不依赖 `inspect` 等未随当前运行时提供的标准库，使用 CPython C API 校验可识别的异步类型，并拒绝协程、生成器等返回结果，不自动 await 或迭代。
3. 返回本子解释器专有、不可由页面自行构造的 `ListenerHandle`，其中只保存文档身份、代数和订阅 ID。丢弃该 Python 句柄不会自动取消监听，监听持续至显式移除、一次性消费或文档停止。
4. 每次注册创建独立订阅；同一函数重复注册会得到不同句柄并分别调用。未在 API 中提供 DOM 原生按函数身份去重的承诺。
5. 移除同一文档内已经撤销的句柄为幂等操作，返回 `None`；错误类型、跨解释器和失效文档句柄返回明确异常。
6. `once=True` 在调用前标记已消费并从元素移除；临时保留 callable 到本次调用返回。回调抛出异常也不再次执行该订阅。
7. 普通 Python 异常写入控制台，保留监听以允许下一次点击。超时、单次资源超限和文档失活按不可逆停止处理。

事件快照先提供三个只读字段：

| 字段 | 含义 |
| --- | --- |
| `event.type` | 固定为 `"click"` |
| `event.current_target` | 注册监听的元素句柄，后续使用仍重新验证所属文档和代数 |
| `event.is_trusted` | 复制 Blink 的 `isTrusted`，供页面观察，不作为授予额外能力的依据 |

快照可以在 Python 中保存，但不持有 Blink `Event*`、`Node*` 或 Chrome host 指针。它不提供取消默认行为或改变传播的方法。Python 返回 `False` 不等同于取消默认行为。链接导航和按钮的原有默认行为仍由 Blink 正常处理。

## 3. 当前基础与需要补齐的入口

现有实现已经具备每 Document 子解释器、跨代码块 globals、有限 DOM 句柄、执行预算、禁止重入、导航失活和安全延迟清理。

阶段一已为 `PythonContext` 增加独立的 callable 注册、调用与移除入口，并与源码执行共用执行边界；`WebPythonScriptContext::Run()` 仍只面向首次解析后的代码块批次，Chrome / Blink 的 callable 适配将在后续接入。事件调用不通过拼接或重新执行 Python 源码实现。

当前 `DomHost` 与 `WebPythonDomHost` 都是执行期间借用的桥接对象。回调等待期间不能保存它们，必须在每一次调用时重新建立桥接，并在异常格式化、引用释放和清理前按既有规则撤销 DOM 能力。

当前 `PythonScriptController::IsActive()` 依赖 `running_`，日志接口依赖 `HTMLPyScriptElement`。事件入口需要配套调整运行状态和日志来源；回调资格检查不能要求已经结束的 HTML parser 继续存在，也不能重新运行初始化代码块。

## 4. 分层实现与所有权

事件路径：

```text
普通 DOM click
  → Blink NativeEventListener
  → PythonScriptController：资格、订阅、重入和预算准入
  → WebPythonScriptContext::InvokeCallback（建议接口）
  → ChromePythonDocumentContext / 临时 ChromeDomHost
  → PythonContext::InvokeCallback（建议接口）
  → 原有子解释器中的 Python callable
  → 现有 DOM host 与结果报告
```

注册方向由 `dom` 模块向外发起：先在解释器内保留 callable、生成回调 ID，再通过临时 host 在 Blink 安装监听。注册失败必须回滚两侧状态和 Python 引用，不能遗留半个订阅。

### 4.1 CPython 运行时

主要位置：

- `components/python_runtime/python_runtime.{h,cc}`
- `components/python_runtime/python_dom_module.{h,cc}`
- `components/python_runtime/python_execution_budget.{h,cc}`
- `components/python_runtime/python_runtime_unittest.cc`

实现要点：

- 回调注册表属于当前子解释器，由该解释器的 `dom` 模块状态持有强引用，并正确参与 CPython `traverse` / `clear`。跨层仅传递数值 ID、句柄、事件快照和执行结果，不向 Blink 暴露 `PyObject*`。
- 新增 callable 调用入口，复用 `Execute()` 的线程、解释器切换、tracing、stdout/stderr、异常格式化与停止逻辑。抽取共用执行边界，避免两条入口在 DOM 撤销或预算计算上出现差异。
- 调用前获取临时强引用。回调可以移除自身或其他订阅；在注册表状态变更后再释放引用，当前 callable 仍存活到本次调用结束。
- callable 参数构造、调用、返回值检查、异常格式化和可能执行 Python 的引用释放均纳入正确的解释器与预算边界。使用既有撤销 DOM 后诊断的策略。
- 订阅 ID 在解释器生命周期内不复用，所有删除和调用均校验上下文；同一个 ID 不能跨文档调用。

### 4.2 Chrome 与 Blink 公共接口

主要位置：

- `third_party/blink/public/web/web_python_script_runner.h`
- `chrome/renderer/python_script_runner.cc`

为 DOM host 增加注册、移除监听所需接口，为 `WebPythonScriptContext` 增加调用已注册回调的入口；具体命名在实施阶段确定。返回结果沿用结构化错误和 `WebPythonScriptResult` 的普通失败 / 致命停止区分。

`ChromePythonDocumentContext` 每次调用创建临时 `ChromeDomHost`，转发参数并同步报告结果。`BudgetedDomHost` 必须覆盖新增接口；不能让注册和移除绕过 DOM 调用预算。

### 4.3 Blink 监听与日志

主要位置：

- `third_party/blink/renderer/core/html/python_script_controller.{h,cc}`
- 视实现复杂度拆分 `python_event_listener.{h,cc}` 等文件，并登记到相应 `BUILD.gn`。
- `third_party/blink/renderer/core/html/html_py_script_element_test.cc`

使用 `NativeEventListener` 接收原生事件，保存订阅 ID 和控制器弱引用。订阅表中的节点采用弱引用；Python 函数、事件快照与句柄不强持有 DOM 节点，避免形成跨 Oilpan / CPython 的引用环。

每次派发前检查节点仍连接、仍属于订阅所属 Document、未进入不支持的 Shadow DOM，且订阅代数仍有效。元素移除后不执行；同一元素重新连接到原 Document 后可以继续生效。迁往其他 Document 时不能调用旧文档的 Python。新建同 ID 元素不会继承原元素的监听。

节点被 GC 后不能在 Oilpan finalizer 内 `Py_DECREF`。首版允许无节点的订阅在受数量上限约束的注册表中保留到显式移除或文档安全清理；后续若做自动回收，必须投递到安全任务，而不是从 GC 栈调用 Python。

日志使用初始化代码块的复制元数据、回调 ID 和原有 Python filename / traceback，不为打印错误而强保留 `<pyscript>` 元素。Blink 与运行时的事件输出计量必须一致。

## 5. 执行条件、CSP 与重入

### 5.1 初始化授权与回调复核

受限模式中，注册操作必须发生在经过现有来源、页面脚本许可和 CSP 检查的 Python 执行链。记录注册入口的授权源元数据，包括原始源码 / nonce、位置和文档身份；多个订阅共享有界的授权记录，避免每个订阅重复保存整块源码。

回调派发时复核：

1. Python 功能仍适用，控制器、执行上下文和 Document 有效、活动、未停止，frame 仍附着且所属 Document 一致。
2. 受限模式下，仍是允许的来源和顶层页面，页面脚本许可有效，并根据授权源元数据与当前 CSP 复核 Python 内联执行许可。后加 CSP 或禁用脚本后不能继续利用旧监听执行。
3. 放开页面限制模式下，沿用已实现的普通页面资格和 Python 脚本 / CSP 覆盖；WebUI、扩展和继承特权 origin 等排除规则仍适用。
4. 子框架仅按当前模式获得资格，各 Document 的解释器和订阅表独立。

回调中新增订阅继承当前调用入口的授权记录。授权复核不能依赖原标签继续存活，也不能把回调名称拼成源码重新 eval。CSP 报告和拒绝诊断应限频。

### 5.2 同步调用与禁止重入

- 默认在原生 click 派发栈内同步调用，使用现有全局 Python 运行门禁及运行时线程检查。
- `ScriptForbiddenScope`、GC 原子暂停、当前线程 sweeping 或已有 Python 执行时不进入解释器。首版跳过这次调用并限频诊断，不建立无界等待队列，也不在之后伪装成原事件同步执行。
- 不向 Python 开放重派发事件、执行 JavaScript、修改 HTML 等新能力。`is_trusted` 只复制事件事实；脚本生成的点击不会被改标为可信输入。
- 由于首版只有只读事件快照，尚不承诺 `preventDefault`、传播控制或完整用户激活 API。将来增加这些接口时必须单独定义其同步性、有效期和授权要求。

## 6. 初始化预算与交互预算

原来的 250 ms / 文档时间、1024 次 / 文档 DOM 调用、16 KiB / 文档输出适用于一次性初始化。若直接累计所有点击，页面最终会耗尽额度；若每次点击无条件重置全部额度，则高频事件可持续占满主线程。

建议增加明确的执行类别，分别管理初始化、事件回调和清理。以下为首版建议初值，需要用 Debug 实测后定稿并更新 UI 说明：

| 项目 | 初始化 | 点击事件 |
| --- | --- | --- |
| 单次 CPU 时间 | 每块 100 ms，协作式 | 每回调 50 ms，协作式 |
| 累计 CPU 时间 | 初始化累计 250 ms | 每 Document 最近 1 秒累计 250 ms |
| DOM 调用 | 初始化累计 1024 次 | 每回调 128 次，最近 1 秒累计 1024 次 |
| stdout / stderr / 异常 | 初始化累计 16 KiB UTF-8 | 每回调 4 KiB，最近 1 秒累计 16 KiB UTF-8 |
| 有效订阅 | 最多 128 个 / Document | 与初始化共享同一订阅数量上限 |
| 清理 | 既有独立 50 ms 尽力预算 | 相同 |

窗口使用单调时钟；仅对实际 Python 执行及相关诊断计费，排除两次点击之间的空闲。准入时为一次回调预留足额的单次时间、DOM 和输出预算，返回后按实际消耗结算，避免开始执行后才发现窗口只剩零碎额度。

停止语义：

- 窗口余量不足：跳过新事件、限频提示；窗口恢复后可以处理后续新事件，不补发跳过的事件。
- 单次回调超时、DOM / 输出超限或文档失活：锁存不可逆停止，撤销全部订阅，清空 DOM 能力，待本次调用返回后安全释放解释器。
- 普通 Python 异常：计入本次消耗并报告 traceback，不自动撤销普通订阅；一次性订阅仍保持已消费状态。
- 初始化已有的致命停止不能通过切换到事件入口或新窗口恢复；不可为刷新事件预算清除上下文的停止原因。

需要同时改造原生 `ExecutionBudget`、Blink `dom_calls_` / 控制台计量和设置页预算说明，避免某一层仍把所有事件算进旧的文档终身额度。不能只修改一处计数器或简单复位 `StopReason`。

这些预算仍是协作式保护，不能硬中断任意 C 操作、保证内存硬上限，或保证整个 Renderer 的 CPU 总量；多个 Document 的额度也不能被描述为进程级强隔离。

## 7. 导航与清理顺序

正常文档停止时按以下顺序处理：

1. 标记停止、递增代数，禁止新的注册与调用。
2. 从元素移除所有原生监听，使绑定失效；清空尚未执行的原生状态。
3. 撤销当前 DOM host 和可用节点句柄；正在执行的回调只返回已发生的结果，之后不再调用任何其他订阅。
4. 当前 Python 调用完全返回后，按既有安全任务策略释放 callback 表、模块、globals 与子解释器。释放引用可能运行 Python finalizer，因此不能在 GC、禁止脚本区域或嵌套 Python 栈执行。
5. 维持既有 BFCache 排除、prerender 拒绝和 Renderer 关闭时任务丢弃的处理语义。

适用入口包括导航、刷新、`Document::Shutdown()`、`document.open()`、执行上下文销毁和安全延迟释放。fragment 导航仍使用原 Document，保留监听但不重复初始化。销毁中先撤销 DOM、关闭页面输出，再执行受预算约束的 Python 清理。

## 8. 分阶段实施步骤

### 阶段一：运行时回调注册表与调用入口

实施状态：运行时实现、35 / 35 单测、Chrome 集成构建、既有 DOM 浏览器回归 17 / 17 及 GN / checkdeps 已通过；网页点击尚未接入。实现边界与验证记录见[阶段一实施记录](pyscript-event-phase1-runtime-callbacks.md)。

1. 为解释器内的回调注册表设计所有权、非复用 ID、注册失败回滚和 GC traverse / clear。
2. 抽取源码执行与 callable 调用共用的进入、退出、DOM 撤销、输出捕获和异常边界。
3. 增加带事件快照的 callable 调用接口，不依赖重新编译源码。
4. 增加独立运行时测试：函数与闭包状态、多次调用、上下文隔离、非法 callable、同步返回约束、自移除期间引用存活、异常与停止、线程 / 重入以及安全销毁。

完成标准：运行时目标构建通过，新增与既有单测全部通过；此阶段尚不向网页宣称完整点击支持。

### 阶段二：Python DOM 注册 / 移除与 Chrome 适配

1. 增加 Python `ListenerHandle` 类型和两项 `dom` API，明确参数、幂等移除、错误类型和 `once` 语义。
2. 扩展原生 / Blink DOM host 与预算包装层，所有跨层对象保持数值句柄或普通数据。
3. 扩展 `WebPythonScriptContext`，在 Chrome 适配层桥接注册、移除、事件快照与调用结果。
4. 测试跨解释器句柄拒绝、注册失败回滚、重复注册、监听器上限及能力借用只存在于执行期间。

完成标准：定向编译、GN / checkdeps 和接口测试通过，接口没有跨层 `PyObject*` / DOM 裸指针泄漏。

### 阶段三：Blink 原生 click 派发

1. 增加原生 listener 和 Document 订阅表，接入普通元素的 click。
2. 实现同步回调、只读快照、`once`、显式移除、监听顺序以及回调期间增删监听的规则。
3. 接入授权元数据、当前页面策略复核、执行门禁和独立的错误来源记录。
4. 增加 Blink 测试：多元素、子元素冒泡、键盘 click、脚本 click、自移除、一次性监听、普通异常、节点断开 / 重连 / 迁移以及跨文档隔离。

完成标准：Blink 定向测试通过，回调运行在原文档解释器中，DOM 文本写入可通过现有桥接完成。

### 阶段四：交互预算、导航与安全清理

1. 分离初始化、事件和清理的预算类别，实现单次限额、窗口准入与实际结算；统一 Blink 与原生输出 / DOM 计量。
2. 实现致命停止时整体撤销订阅，以及窗口不足时跳过、恢复后仅接受新事件。
3. 完成导航、`document.open()`、frame 销毁、GC 和回调中失活的撤销顺序；避免引用释放引发重入或 UAF。
4. 更新设置页预算说明和相关文档，保留现有默认关闭及页面权限模式。
5. 使用可注入时钟测试窗口边界和空闲排除，使用真实循环验证协作式中断；验证异常格式化和 finalizer 仍撤销 DOM 能力。

完成标准：多次正常点击不受初始化终身额度误伤；连续触发受限；停止后不可恢复执行；生命周期和预算回归通过。阶段三的初步事件功能完成本阶段后才能作为首版交付。

### 阶段五：构建、真实浏览器验证与记录

1. 构建 Chrome、原生运行时单测、设置模块单测与 Blink 单测。
2. 添加独立浏览器测试脚本，建议 `chrome/test/pyscript_event_smoke_test.mjs`，并加入本地示例页面。
3. 使用 CDP `Input.dispatchMouseEvent` 驱动真实输入路径，另用页面 `element.click()` 验证非可信事件；不能只直接调用 Python 函数代替事件端到端测试。
4. 覆盖设置与两种页面模式、导航 / 刷新、跨框架、回调异常与超时、移除 / once、连续点击状态及 Renderer 沙箱。
5. 运行既有解析权限、DOM / 生命周期和设置页回归，记录实际配置、退出状态及失败；不把本计划或既有通过数量当作本次验证结果。
6. 将实际结果、必要日志和页面截图保存到建议目录 `docs-codex/pyscript-event-validation/`，更新本文实施状态。关键阶段可单独创建提交，提交时仅选择本次 Python 改动。

建议基础命令：

```powershell
autoninja -C out/Debug chrome python_runtime_unittests native_python_settings_unittests blink_unittests
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1
out/Debug/native_python_settings_unittests.exe --test-launcher-jobs=1
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.* --test-launcher-jobs=1

# 新增脚本完成后执行；当前尚不存在。
node chrome/test/pyscript_event_smoke_test.mjs --output=out/Debug/pyscript-event-smoke.json

node chrome/test/pyscript_smoke_test.mjs --output=out/Debug/pyscript-event-parser-regression.json
node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-event-dom-regression.json
node chrome/test/pyscript_settings_smoke_test.mjs --output=out/Debug/pyscript-event-settings-regression.json
```

若新测试拆分为独立 suite，应同步调整过滤器。对同一个 `out/Debug` 顺序运行构建。遵循实施时相应目录的 `AGENTS.md`；若完整链接再次遇到生成对象问题，保留输入诊断和原件，不能跳过 PDB 或削弱检查来宣称构建通过。

## 9. 首版验收清单

- 示例按钮每点击一次，页面计数加一；多次事件共享 Python 全局变量和闭包，其他文档看不到这些状态。
- 正常点击可持续使用，不在累计 250 ms 或 1024 次 DOM 操作后误用初始化额度停止；时间窗口限额有确定的跳过与恢复行为。
- 显式移除、重复移除、重复注册、once、自移除以及回调中移除后续订阅具有明确、可验证的语义。
- 原生 / 脚本生成的 click 均走真实事件路径，`is_trusted` 正确；返回值不取消默认行为，不将普通元素句柄变为任意 DOM 写入能力。
- 受限来源、CSP、脚本设置及放开模式按约定生效，后加限制能阻止旧订阅继续执行，内部页面排除和 Renderer 沙箱保持有效。
- 元素移除、重连、替换、迁移和 GC 不造成跨文档执行、节点强保留或在 GC 栈运行 Python。
- 普通异常可恢复；协作式超时与单次资源超限撤销整个文档的 Python 监听，后续事件不能复活上下文。
- 导航、刷新、`document.open()`、frame 关闭、BFCache / prerender 和 Renderer 正常关闭没有失效回调、跨解释器引用或非法清理。
- 新增与受影响回归实际运行通过，文档记录真实结果与已知限制。
