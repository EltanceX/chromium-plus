# 原生 Python 点击事件：阶段二 DOM 注册 / 移除与 Chrome 适配

日期：2026-11-09（Asia/Shanghai）。接续[阶段一运行时基础](pyscript-event-phase1-runtime-callbacks.md)，代码基线为 `7580758e69`。整体步骤见[首版方案](pyscript-click-events-implementation.md)。

## 1. 本阶段范围

本阶段接通 Python `dom` API、回调注册表、预算包装层、Blink 公共接口和 Chrome 文档适配。新增 Python API：

```python
listener = dom.add_event_listener(element, "click", callback, once=False)
dom.remove_event_listener(listener)
```

`once` 只能通过关键字传入；上面的调用示例符合这一限制。完整签名为 `add_event_listener(element, event_type, callback, *, once=False)`。仅接受值为 `"click"` 的字符串和真正的 `bool`，不对任意对象执行隐式真值转换。其他参数校验和同步 callable 规则沿用阶段一。

当前实际浏览器尚未安装原生 listener。Blink controller 对新增注册 / 移除入口先执行现有文档与 DOM 次数检查，再返回明确的 `RuntimeError: Native Python click dispatch is not implemented yet`。注册失败会撤回运行时回调表项，不返回可用句柄。阶段三替换这个临时拒绝入口，实现目标节点检查、权限复核和真实 click 派发。

因此，本阶段的成功注册、调用、移除与自移除验证使用独立原生 / 公共接口测试 host；它们验证接口链路，不代表网页按钮已经能够触发 Python。

## 2. 句柄与注册事务

`dom.ListenerHandle` 为当前模块实例专有的不可构造、不可赋值、无动态属性的 Python 类型。内部只保存目标的 document / generation / element ID 与 callback 的 context / ID，不保存 callable、DOM 节点或 host。类型及其模块引用参与 CPython GC。

解释器内的 `dom` 模块仍强持有 callable。丢弃 `ListenerHandle` 不取消监听；同一元素和函数重复注册会产生独立 callback ID。当前上限为每解释器合计 128 个活动回调，原生注册和 Python listener 共用该表。

注册顺序为：

1. 校验参数、解释器句柄类型和当前 DOM 能力。
2. 暂时撤销模块中的 DOM host，预分配结果句柄，再把 callable 放入当前解释器的注册表。
3. 通过预算 host 和 Chrome 桥接，把纯数值目标 / callback 句柄、事件类型及 `once` 传给 Blink host。
4. host 返回成功后才将 Python 句柄交给调用方。失败时先撤销可能安装的原生监听，再移除 Python 注册表项，并保留原始错误。

`once` 在本阶段负责严格解析和跨层传递，调用前消费与单次派发语义由阶段三的真实 listener 实现。

## 3. 移除与回滚边界

`remove_event_listener()` 先验证当前模块的 `ListenerHandle` 与上下文身份，再经过正常文档、权限和 DOM 次数检查，移除 host 订阅；host 成功后从 Python 回调表释放强引用，返回 `None`。同一有效文档中已移除的订阅按 host 幂等语义成功，错误文档或失效代数由 host 返回 `ReferenceError`。普通 host 移除失败时保留 Python 表项，允许随后重试。

引用释放期间撤销 DOM；当前 callback 自移除时由调用入口的临时强引用保持存活，返回后才最终释放。注册 / 移除没有保存临时 host 供下次使用。

新增内部 `RevokeEventListener()` 专用于注册失败回滚：host 可能已经安装监听，但预算包装层的调用后检查发现超时或文档失活，此时普通移除 API 已无法获得准入。内部撤销必须幂等、无脚本 / Python 执行，不再进行权限准入，也不额外消耗页面 DOM 次数。它没有暴露给 Python，不能用于绕过页面注册 / 移除预算。

本阶段没有改变累计预算，也没有提前实现窗口限额。若执行出口在注册操作返回成功后才触发致命停止，仍按整体文档停止与统一清理处理；阶段四将补齐真实订阅的全量撤销。

## 4. Chrome 与 Blink 接口

新增 `WebPythonCallbackHandle` 和 `WebPythonCallbackEvent`，只含数值、布尔值及 `WebString`。`WebPythonDomHost` 增加注册、移除与内部撤销接口；`WebPythonScriptContext` 增加同步 `InvokeCallback()` 和纯注册表 `RemoveCallback()`。

Chrome 文档执行适配从 `python_script_runner.cc` 拆到 `python_document_context.{h,cc}`。生产 runner 和独立接口测试共用 `CreatePythonDocumentContext()`；来源与页面模式设置继续由原 runner 负责。

每次源码批次或 callback 调用重新创建栈上的 `ChromeDomHost`。回调结果沿用 `WebPythonScriptResult`，保留 stdout / stderr、traceback、截断和致命 `stopped` 区分，由未来 Blink 派发入口同步报告。原有批次逐块报告与停止后不执行后续源码的行为保持一致。`RemoveCallback()` 只处理解释器注册表，不代替 Blink 订阅移除。

`BudgetedDomHost` 覆盖两项页面 API 的调用前 / 后检查，host 的 `kStopped` 仍锁存为 DOM 次数停止。Blink 临时入口也通过 `CheckDomCall()`，没有漏掉注册 / 移除计量。所有跨层接口均未传递 `PyObject*` 或 DOM 裸指针。

## 5. 验证

本次实际结果保存在 [pyscript-event-phase2-validation/](pyscript-event-phase2-validation/validation-summary.json)。

| 检查 | 结果 |
| --- | --- |
| Chrome、Blink、运行时及独立适配测试目标标准构建 | 通过，保留 PDB 与正常链接检查 |
| 运行时单测 | 43 / 43，含本阶段新增 8 项 |
| Chrome 公共接口适配单测 | 6 / 6，全部新增 |
| Blink `HTMLPyScriptElementTest.*` | 33 / 33，含 1 项新增临时入口 / 计量测试 |
| 既有 DOM、预算与导航浏览器回归 | 17 / 17，保留 Renderer 沙箱，默认网络服务配置 |
| GN | runtime、Chrome renderer 全部 target、Blink core 及 unit_tests 均通过 |
| checkdeps | runtime、Chrome renderer、Blink controller / 测试均通过 |
| clang-format、GN format、diff 空白检查 | 通过 |

运行时新增覆盖句柄类型与不可构造性、严格参数、模块类型隔离、重复订阅、丢弃句柄后的回调存活、128 上限、150 次安装失败后的容量恢复、调用后超时 / 失活回滚、普通移除失败后重试、自移除与回调中注册、释放与诊断期间撤销 DOM。Chrome 测试通过真实 CPython 验证目标句柄、回调身份、事件字段与 `once` 的双向转换、跨解释器隔离、所有 host 错误类型、停止标志、回滚、重入拒绝、普通异常恢复及逐块报告。

浏览器回归验证原有解析后执行、有限 DOM、异常、预算及生命周期，没有使用网络服务诊断参数。本阶段没有把独立 host 调用计为真实 click 端到端通过，也没有重复整个 MVP / 设置页验收矩阵。

新增独立目标 `python_document_context_unittests`，使用真实 CPython 和 Blink 公共字符串 / 数据接口，host 为可控测试替身。测试入口采用 Blink 官方测试环境初始化；生产代码没有增加测试依赖。依赖通过 Blink 公共 target 获取，未放宽平台库可见性。

首轮 GN 拒绝直接依赖 Blink 私有 platform target，已改为公共 target。随后前端检查要求临时能力范围的引用成员使用 `raw_ref`，已修复。独立测试辅助函数最初与 gtest 的私有 `Run()` 重名，且泛型 lambda 不符合 `base::FunctionRef` 的签名推导要求；现已分别命名为 `RunSource()` 并显式指定参数类型。初始失败日志随最终结果保留。

集成链接还发现旧 Blink Core 对象的 COFF / CodeView 结构错误。对本次实际链接输入的 5,040 个直接对象、59 个库及 560 个库成员扫描，定位到 `css_scale.obj`、`text_iterator_text_state.obj`、`text_fragment_handler.obj` 和 `performance_timing_confidence.obj`。四份原件在普通读取和 mmap 中均稳定复现错误；保存原件与 SHA-256 后，仅使这四份生成对象失效并重建。重建后两种读取方式的检查均无问题，Blink Core 标准调试链接通过。没有改动这些功能的源码，也没有跳过 PDB 或降低链接检查；根因仍未知。

最终完整构建 5 分 1 秒通过。扫描、原件 / 重建哈希和必要诊断见验证目录；原始对象备份保留在本机忽略目录 `out/Debug/pyscript-event-phase2-object-backup/`，未把编译产物提交到仓库。Blink 测试结果只过滤原始 summary 中不相关的全量用例清单与位置索引，本次 33 项运行状态和输出保留。

主要命令：

```powershell
autoninja -C out/Debug python_runtime_unittests python_document_context_unittests chrome blink_unittests
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000
out/Debug/python_document_context_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.* --test-launcher-jobs=1 --test-launcher-timeout=60000
node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-event-phase2-dom-regression.json
```

## 6. 后续接入

阶段三实现 Blink listener 与订阅表、普通元素检查、授权元数据、CSP / 页面资格复核、`once` 消费和真实 click 的同步调用。阶段四再完成交互预算、致命停止时全量撤销、导航与安全清理。当前网页示例仍不能作为完整点击功能使用。
