# 原生 Python 动画帧：阶段一实现与验证

日期：2026-11-10（Asia/Shanghai）。实现基线为 `ed9a245bf1e2d6913029eaf733865776ecde6e3a`，对应[马里奥能力扩展方案](pyscript-mario-implementation.md)阶段一。阶段二及后续能力尚未实施。

## 接口与用法

新增解释器内建模块 `animation`，页面可以直接申请 Blink 原生动画帧，无需 JavaScript 转调：

```html
<p id="count">0</p>
<pyscript>
import animation, dom

target = dom.get_element_by_id('count')
count = 0
request = None

def frame(timestamp_ms):
    global count, request
    count += 1
    dom.set_text(target, str(count))
    request = animation.request_frame(frame)

request = animation.request_frame(frame)
</pyscript>
```

`request_frame(callable)` 返回不可构造、不可改写、所属解释器唯一的 `animation.FrameHandle`。回调接收一个浮点毫秒时间戳；时间原点及精度处理沿用 Blink 的文档动画时钟。它不是 Unix 时间，也不应与系统壁钟相减。

`cancel_frame(handle)` 在有效同步 Python 执行期间取消匹配请求并释放回调引用；重复取消或取消已消费的旧句柄均可成功，旧句柄不能取消后续新请求。导航或停止后的旧文档不再提供 DOM / 动画操作能力。丢弃句柄不会自动取消，回调仍由注册表持有。

一个 Document 最多保留一个待执行 Python 帧；重复申请抛出 `RuntimeError`，失败注册完整回滚。实际进入前先消费旧请求，回调可以申请下一帧；新请求只能在后续原生刷新周期执行。普通异常消费当前请求，但不撤销此前已申请的下一帧；没有新请求时不会自动循环。协程、生成器及异步返回值沿用既有同步回调限制。click 与帧共同使用每解释器最多 128 项的回调注册表。

完整的暂停 / 继续示例为 [`chrome/test/data/pyscript/animation-frames.html`](../chrome/test/data/pyscript/animation-frames.html)。示例没有 `<script>`、JavaScript 事件属性或 JavaScript 运行依赖；测试脚本中的 CDP JavaScript 仅用于外部检查及构造生命周期条件。

## 调度与生命周期

运行时新增 `ExecutionKind`，明确区分初始化、普通输入回调及动画帧；帧使用独立 `InvokeFrameCallback(handle, timestamp_ms, host)`，不再借用 click 事件快照。注册表核对用途与解释器身份后才消费一次性回调；无效时间戳、错误用途或其他解释器句柄不能消费有效请求。

Chrome 适配层只转换有类型的句柄、时间戳与错误。Blink 的 `PythonAnimationFrame` 继承 `FrameCallback`，以 `FrameCallbackType::kWebExposed` 进入 Document / ScriptedAnimationController 正常调度，保留页面冻结和生命周期语义。它只持有控制器弱引用、来源快照和回调 ID，不持有 Python callable 或源 DOM 节点。

窗口不足、隐藏页面、短暂禁止脚本或 Python 重入时保留一个逻辑请求，在后续合格刷新周期使用新时间戳重试。不保存历史时间戳，不补发旧帧，也不建立无界任务队列。重试分配新的原生适配器，避免复用尚在 Invoke / DevTools probe 栈上的异步任务上下文。GC 期间不能分配重试对象或进入 Python，安全任务只负责重新挂入原生帧调度。

派发复核当前 feature gate、页面与来源策略，以及从注册来源复制的源码、nonce 和位置。删除源标签不撤销已获准的请求；不可继续的页面策略或 CSP 拒绝则停止文档 Python，并撤销帧及 click 监听。既有 click 的暂时跳过规则保持不变。

导航、`document.open()`、上下文销毁、致命超限与主动文档停止均取消原生待执行请求。运行中的 Python 先失去 DOM 和输出能力，解释器仍在同步调用返回后由安全任务销毁，不在 GC、禁止脚本范围或其他 Python 调用中释放。帧结果复用 click 的输出、异常及停止处理。

游戏层仍需对 dt 设上限，并在长间隔后重建物理时间基准；浏览器不代替游戏补算后台经过的时间。

## 帧预算与共享窗口

| 额度 | 本阶段值 | 行为 |
| --- | --- | --- |
| 单次帧执行 | 8 ms，协作式 | 超限锁存停止整个文档 Python |
| 最近一秒帧时间 | 200 ms | 余量不足保留请求，后续刷新重试 |
| 最近一秒所有交互时间 | 250 ms | click 与帧共享，不能叠加为两份额度 |
| 单次帧 DOM / 输出 | 128 次 / 4 KiB UTF-8 | 超限锁存停止；申请及取消帧也计 DOM 调用 |
| 最近一秒交互 DOM / 输出 | 1,024 次 / 16 KiB | 与 click 共享，余量不足延后帧或跳过 click |

准入按入口分别预留 8 ms 或 50 ms，以及完整的单次 DOM / 输出额度；结算按实际耗时和用量。时间窗口由单调时钟计算，测试可注入时钟，帧间空闲不计入 Python 执行时间。初始化与清理保持原有独立额度。任何入口的致命停止都不能通过窗口过期或切换入口复活。

设置页“内核调试”已同步显示帧额度及共享窗口规则。上述值仍是阶段一实验初值；未获得完整游戏与 Release 性能数据前，不承诺 Debug 达到 60 FPS，也不扩大现有 Renderer 权限。协作式时间检查不能硬中断任意 C 操作，当前仍无 Python 堆内存硬隔离。

## 验证入口与当前记录

```powershell
python chrome/test/run_pyscript_event_validation.py
```

验收入口已纳入动画帧浏览器测试及新增单测数量，同时为尚未提交的新测试输入记录哈希。标准目标仍为 `chrome`、`blink_unittests`、`python_runtime_unittests`、`python_document_context_unittests`、`native_python_settings_unittests`，同一输出目录顺序构建。

本轮统一验收于北京时间 **18:38:03–18:42:11** 执行。标准五目标构建退出码 0，编排器测得 43.62 秒，构建工具报告 42.36 秒、2 步（最后补入的线程检查回归测试重编译及链接）。此前功能源码与四份对象修复后的标准构建已完成 16 步、6 分 14.61 秒。两次均保留正常 Debug / PDB / 链接检查，不是从空输出目录重建。

| 组 | 通过 / 实际 | 耗时（秒） |
| --- | --- | --- |
| 运行时 | 55 / 55 | 4.17 |
| Chrome 文档适配 | 10 / 10 | 0.92 |
| Blink Python 元素 / 调度 / 生命周期 | 62 / 62 | 15.84 |
| CSP 定向回归 | 3 / 3 | 2.84 |
| Python 设置单测 | 8 / 8 | 0.11 |
| click 浏览器回归 | 34 / 34 | 30.19 |
| 动画帧浏览器验证 | 19 / 19 | 27.55 |
| DOM 浏览器回归 | 17 / 17 | 27.73 |
| 解析 / 权限浏览器回归 | 27 / 27 | 35.28 |
| 设置 / 重启 / 页面模式 / 沙箱浏览器回归 | 12 / 12 | 54.88 |

合计 **138 项单测、109 项浏览器测试，247 项全部通过**。验证前后功能源码与测试文件哈希一致；新增、尚未提交的测试和样例也纳入哈希。全部浏览器组记录 `sandboxDisabled=false`，设置自检 10 条均为 `passed=true`、`sandboxed=true`。环境为 Windows x64、`out/Debug`、component build、CPython 3.13.18；没有修改外部马里奥项目，参考源文件 SHA-256 保持本文基线值。

主要证据：

- [统一结果及源码 / 二进制哈希](pyscript-animation-validation/final/summary.json)。
- [动画帧 19 项结果](pyscript-animation-validation/final/animation.json)与[无 JavaScript 示例截图](pyscript-animation-validation/final/animation.json.sample.png)。
- [依赖、格式与语法检查](pyscript-animation-validation/static-checks.json)，[原文件 / 归档文件哈希清单](pyscript-animation-validation/archive-manifest.json)。
- [诊断索引](pyscript-animation-validation/README.md)；原始本机输出保留于 `out/Debug/pyscript-animation-phase1/`，仓库归档日志已规范为 UTF-8、LF、无行末空白和单 EOF 换行。

运行时通过注入时钟验证 8 ms 单次、200 ms 帧子窗口、250 ms 交互总窗口、用途隔离、错误回滚、取消释放及致命停止锁存；Blink 验证原生刷新周期、一次性消费、取消、禁止脚本、隐藏恢复、来源快照、CSP、导航和清理。浏览器结果来自实际 CPython → C++ → Blink 路径，不以模拟运行时替代。

浏览器输出窗口用例的时间戳为 `142, 158, 175, 192, 208, 1158` ms：额度不足后仅保留一个请求，恢复时使用当前时间戳，相邻间隔 950 ms，没有补发旧帧。共享 DOM 用例由 8 次 click 累计 960 次 DOM 调用并挂起一帧，窗口不足时不派发，随后自动恢复。隐藏 / 冻结用例确认 `document.hidden=true`，1.2 秒期间计数不变；恢复可见后从 `6:272` 继续到 `13:1955`，未补算约 1.7 秒的历史帧。

本机 Debug 中，尝试在帧内密集执行 64 / 129 次桥接调用，会先触发 8 ms 时间保护。这是实际限制，不宣称浏览器已在这些用例中到达精确第 129 次调用；精确 128 次边界由注入时钟及 Blink 单测验证。浏览器过载用例检查最先触发的致命保护，DOM 窗口则通过有 50 ms 单次额度的 click 累计，独立验证对待执行帧的延后。生产预算没有因测试被放宽；后续 Canvas 批量提交与低频 HUD 更新仍是必要方向。

页面冻结测试曾只发送 CDP `active` 并等待计数恢复，导致超时。源码确认该命令只解冻，`frozen` 所做的隐藏不会被自动撤销；最终测试通过切换标签页并激活原页恢复真实可见性，再验证原待执行帧继续。早期失败探测与最后通过结果分别保存，没有覆盖历史失败。

真实浏览器采用 headless 模式；本轮验证功能与生命周期，不构成 Release 或长时间游戏性能结论。日志保留 Debug、GPU、OAuth 及 network service 重启诊断，本轮未定位这些后台诊断的根因，不把验收描述为长期无子进程重启的稳定性测试。后续键盘 / 指针、Canvas 和游戏迁移仍未实施。

构建中出现既有对象异常：Core 链接报告 `getSectionName failed: #955: invalid section name`。保留响应文件与失败输出后，对 5,040 份直接对象、59 个库及 560 个库成员进行结构扫描，定位 4 份异常对象：`computed_style_css_value_mapping`、`css_to_length_conversion_data`、`display_lock_context`、`window_event_context`。重复读取及 mmap 哈希一致，原件和哈希保存在本机忽略目录 `out/Debug/pyscript-animation-phase1/object-backup/attempt1/`。只定向重建这 4 份对象，重建后的 COFF、字符串表及 CodeView 检查为 0 问题，Core 已正常链接；异常根因仍未确定。未修改其功能源码，未关闭 Debug / PDB / 链接检查。

此前类内默认虚函数规范错误、新测试的夹具 API 编译错误，以及一项误把调用方临时对象析构当作原生释放的测试预期，均保留在探测日志中。测试现分别检查安装回滚与取消注册时原生释放最后引用的撤权语义，不更改生产限制来迎合测试。
