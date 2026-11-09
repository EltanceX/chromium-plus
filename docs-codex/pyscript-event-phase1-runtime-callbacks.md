# 原生 Python 点击事件：阶段一运行时回调注册表与调用入口

日期：2026-11-09（Asia/Shanghai）。接续[首版实现方案](pyscript-click-events-implementation.md)，代码基线为 `83433306b8`。

## 1. 实施范围

本阶段已实现独立运行时的回调注册、同步调用、移除和只读事件快照。它是后续原生点击监听的基础：当前网页尚不能调用 `dom.add_event_listener()`，也没有接入 Blink click 派发、`once` 或交互滑动窗口预算。

改动仅涉及 `components/python_runtime/` 的五个源文件，没有新增运行时依赖或改变 Python 启用设置、页面授权规则和 Renderer 沙箱。

## 2. 原生接口与所有权

`PythonContext` 新增三个原生入口：

```cpp
CallbackRegistrationResult RegisterCallback(std::string_view global_name);
ExecutionResult InvokeCallback(CallbackHandle,
                               const CallbackEvent&,
                               DomHost* dom_host = nullptr);
ExecutionResult RemoveCallback(CallbackHandle);
```

`RegisterCallback()` 从当前解释器的 globals 中取得指定名称当前对应的 callable，不 eval 名称、不编译调用源码。名称必须为 1 至 2048 字节、无 NUL 的 UTF-8。注册后重新赋值或删除该全局名称不会改变已捕获的 callable，闭包与全局状态在调用间保留。后续 Python DOM API 将直接使用模块内部的 callable 注册函数，不需要把闭包写入全局变量再按名称注册。

`CallbackHandle` 包含进程内单调生成的 context ID 和解释器内单调生成的 callback ID。两个子解释器即使恰好生成同号 callback ID，也不能互相调用或移除；销毁旧上下文后，新上下文不会继承其句柄。每个解释器最多同时保留 128 个回调，移除后可重新注册，但不复用 ID。

回调表由 `dom` 模块的每解释器状态强持有，纳入 `traverse` / `clear`。注册失败不返回可用句柄；表插入失败不遗留新 callable 引用。若注册已完成、随后执行出口才触发致命预算停止，对外清空句柄，表内有界引用随已停止上下文统一销毁，不再允许调用。

同一上下文中重复移除已经删除的非零句柄成功；错误上下文和零句柄返回 `ReferenceError`。原生删除接口对本上下文内不存在的非零 ID 采用幂等语义。调用已经删除的句柄失败。

调用前另取 callable 临时强引用。允许当前上下文的活动 host 调用执行纯注册表删除，以支持后续自移除和一次性消费；删除不重新获取 GIL、不切换解释器、不重置预算，也不递归调用 callback。删除状态先完成，再释放表内引用，正在执行的 callable 保留至调用结束。其他上下文的删除，以及嵌套注册、调用和创建上下文仍被拒绝。

## 3. 共用执行边界与事件快照

源码执行与回调入口共用 `Impl::Run()`，统一所属线程检查、禁止解释器重入、不可逆停止状态、GIL 与子解释器切换、tracing、输出捕获、异常格式化和预算结算。

DOM host 只在 `PyEval_EvalCode()` 或 `PyObject_CallOneArg()` 的同步执行范围借用。编译、构造事件、检查返回值、异常格式化、清理返回值及释放 callable 引用期间均撤销 DOM 能力。清理和诊断仍受执行预算约束。

`CallbackEvent` 原生数据通过只读 `dom.CallbackEvent` 传入 Python：

| 字段 | 当前实现 |
| --- | --- |
| `type` | 仅接受 `"click"` |
| `current_target` | 数值文档身份、代数和元素 ID 转为当前解释器的 `ElementHandle` |
| `is_trusted` | 复制原生布尔值，不作为额外授权依据 |

快照不可由 Python 自行构造、不可赋值或添加属性，可以保存在 Python 中；只保存 Python 字符串、元素数值句柄和布尔值，不保存 Blink 对象或 host。原生入口要求目标句柄的三个字段非零，具体文档身份与节点有效性继续由 DOM host 校验；真实事件来源和可信标志由后续 Blink 适配层负责。

事件和模块参与 CPython GC；现有 `ElementHandle` 也补齐 heap type 的遍历，避免新增的 callable、event、type 和 module 引用链形成不可遍历环。CPython 分配的结构字段使用原始 Python 引用并明确标注 `RAW_PTR_EXCLUSION`。

普通回调返回值被忽略，并在撤销 DOM 后释放。注册时拒绝非 callable 和可以识别的协程、生成器、异步生成器函数及其绑定方法。对工厂函数或 callable 对象，调用后再次拒绝 coroutine / generator / async-generator 结果，不 await、不迭代；普通 coroutine / generator 在无 DOM 能力的预算边界内关闭，避免未 await 诊断或遗留暂停执行。

普通 Python 异常保留源码 filename 与 traceback，调用入口返回失败但不删除订阅或停止上下文，后续调用仍可成功。致命预算停止则锁存，源码、注册、调用和移除不能重新激活上下文。销毁时先关闭输出、撤销 DOM、进入既有清理预算，再清空回调表并释放 globals、模块和子解释器。

## 4. 预算边界

本阶段保留原有 100 ms / 次、250 ms / 文档、16 KiB / 文档输出和既有 host DOM 调用限制。源码、注册、调用、移除共享当前执行与输出计量；两次入口之间的空闲时间不计入执行时间。

因此当前接口适合运行时验证，尚不能据此承诺页面无限次正常点击。初始化与交互预算分离、窗口准入及监听整体撤销属于阶段四。协作式 tracing 仍不构成任意原生执行或内存分配的硬限制。

## 5. 验证

本次实际验证及原始结果保存在 [pyscript-event-phase1-validation/](pyscript-event-phase1-validation/validation-summary.json)。

| 检查 | 结果 |
| --- | --- |
| 修改前运行时基线 | 22 / 22 |
| `python_runtime_unittests` 定向构建 | 通过 |
| 修改后运行时单测 | 35 / 35，包含 13 项新增测试 |
| GN runtime header dependency check | 通过 |
| `checkdeps components/python_runtime` | 通过 |
| Chrome 集成构建 | 通过，标准调试链接，2 分 29 秒 |
| 既有 DOM、预算与导航浏览器回归 | 17 / 17，保留 Renderer 沙箱 |

新增覆盖包括闭包持久化与全局名称重新绑定、只读快照、跨解释器和销毁后的句柄隔离、128 上限和 ID 不复用、参数与同步返回约束、异常恢复、自移除、finalizer / 诊断 / generator 关闭时撤销 DOM、禁止重入、非活动 host、超时停止、累计时间与输出预算、空闲时间排除、GC / teardown 和错误线程 CHECK。

Chrome 集成构建重编译两个现有 Renderer 调用方并重新链接 `chrome.dll`。17 项浏览器回归使用默认网络服务配置，未增加 `--no-sandbox`、`--disable-web-security` 或网络服务诊断参数；覆盖原有 DOM 桥接、CSP、文档隔离、累计 DOM / 输出预算、超时、异常格式化、导航、`document.open()`、BFCache 排除、刷新、循环引用清理和正常关闭。它们验证执行入口重构没有破坏现有页面功能，不代表真实 click 已接入；本阶段未新增页面事件端到端测试，也未重复整个 MVP 验收矩阵。

首轮编译发现新增 CPython 事件对象的两个 Python 指针字段缺少 Chromium 要求的标注，已补齐 `RAW_PTR_EXCLUSION`；随后标准编译通过。失败诊断与修复后日志均保留。

运行命令：

```powershell
autoninja -C out/Debug python_runtime_unittests
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000 --test-launcher-summary-output=out/Debug/pyscript-event-phase1-unittests.json
gn check out/Debug //components/python_runtime:python_runtime
python buildtools/checkdeps/checkdeps.py components/python_runtime

autoninja -C out/Debug chrome python_runtime_unittests
node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-event-phase1-dom-regression.json
```

## 6. 下一阶段

阶段二将增加 Python `ListenerHandle`、`dom.add_event_listener()` / `dom.remove_event_listener()`，扩展原生与 Blink host、预算包装层及 Chrome 适配，并验证注册失败的跨层回滚。阶段三再接入真实原生 click，阶段四补齐交互预算与订阅生命周期。
