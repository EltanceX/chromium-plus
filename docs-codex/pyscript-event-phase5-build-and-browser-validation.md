# 原生 Python 点击事件：阶段五构建、真实浏览器验证与记录

日期：2026-11-10（Asia/Shanghai）。本阶段完成首版验收，接续[阶段四](pyscript-event-phase4-budgets-and-cleanup.md)，总体接口和语义见[实现方案](pyscript-click-events-implementation.md)。

本轮标准五目标构建成功，**117 项单测、90 项真实浏览器测试全部通过**。结果来自当前工作区的新一轮运行，没有沿用阶段四的通过计数。阶段五增加验收脚本与覆盖，不修改原生运行时、Blink 或设置页的功能实现。

## 环境与构建

- 源码基线：`b3a05f25f236ea0231df7c886b8dffef5aee07a0`。验收时包含本阶段尚未提交的测试改动，最终汇总保存工作区状态、功能相关源码和测试文件的 SHA-256；验证前后这些输入一致。
- Windows 11，x64，`out/Debug`，`is_debug=true`、`is_component_build=true`、`cc_wrapper="sccache"`。本轮未改变 Debug、PDB 或链接检查。
- 编排工具 Python 3.12.10、Node.js v24.22.1；Renderer 内的 CPython 为 3.13.18，见设置测试的运行时诊断。
- 完整验收于北京时间 **01:00:32–01:04:14** 执行，使用临时独立 profile、本地 loopback HTTP 服务与实际 `chrome.exe`，浏览器采用 headless 模式。

```powershell
autoninja -C out/Debug chrome blink_unittests python_runtime_unittests python_document_context_unittests native_python_settings_unittests
```

标准构建退出码 **0**，编排器测得 40.95 秒，构建工具报告 37.89 秒、0 步、`ninja: no work to do`。这表明现有原生输出与当前依赖图一致；本阶段只变更测试和文档，不将此结果描述为从空目录重新编译。上阶段的生成对象异常与恢复记录继续保存在[阶段四证据](pyscript-event-phase4-validation/validation-summary.json)，本轮没有新增对象修复。

## 可重复运行的入口

新增 [`chrome/test/run_pyscript_event_validation.py`](../chrome/test/run_pyscript_event_validation.py)，从任意工作目录定位仓库，顺序构建和运行全部九组验收：

```powershell
python chrome/test/run_pyscript_event_validation.py

# 指定输出位置时必须使用尚不存在的目录，以保留既有结果。
python chrome/test/run_pyscript_event_validation.py --build-dir=out/Debug --output-dir=out/Debug/pyscript-event-validation-new
```

默认输出在 `out/Debug/pyscript-event-validation/<UTC时间>/`。入口不会并行构建同一输出目录；构建失败时停止测试，保留日志。每组记录实际命令、退出码、耗时、预期和实际项数，并检查所有项目成功；浏览器组还要求整组通过且 `sandboxDisabled=false`。缺结果、少跑、失败或源文件在验收期间变化，都不能得到最终通过结论。

汇总记录标准构建参数、源码及关键二进制 SHA-256。单测结果删除启动器附带的全部测试名称、禁用名称清单，仅保留实际运行结果和相应 `test_locations`；同时记录精简前后结果的哈希，避免把大型无关枚举写入仓库。新增 Python 编排器语法、格式、Node 脚本语法、文档格式与 diff 空白检查另见[静态检查记录](pyscript-event-validation/static-checks.json)。本阶段没有 C++ / GN 变更，相关依赖和格式验证保留阶段四记录。

## 本轮实际结果

| 组 | 通过 / 实际 | 退出码 | 耗时（秒） |
| --- | --- | --- | --- |
| 原生运行时 | 49 / 49 | 0 | 4.78 |
| Chrome 文档适配 | 8 / 8 | 0 | 0.81 |
| Blink `HTMLPyScriptElementTest.*` | 49 / 49 | 0 | 16.11 |
| CSP nonce 三项定向回归 | 3 / 3 | 0 | 3.31 |
| Python 设置单测 | 8 / 8 | 0 | 0.23 |
| 点击与事件生命周期浏览器测试 | 34 / 34 | 0 | 30.81 |
| 既有 DOM / 生命周期浏览器回归 | 17 / 17 | 0 | 28.78 |
| 既有解析与权限浏览器回归 | 27 / 27 | 0 | 34.70 |
| 设置页、重启、页面模式与沙箱浏览器回归 | 12 / 12 | 0 | 54.06 |

合计 **207 项**。原始结构化结果和日志见[最终汇总](pyscript-event-validation/final/summary.json)，便于阅读的计数见[验收汇总](pyscript-event-validation/validation-summary.json)。

## 首版验收对应关系

| 验收要求 | 本轮实际覆盖 |
| --- | --- |
| 状态共享与文档隔离 | 增加跨两个 `<pyscript>` 的全局变量和闭包用例；重复点击保留闭包状态，重新加载清零；同时存在的两个子框架各自保持独立闭包计数 |
| 预算与恢复 | 单测用可注入时钟验证时间窗口与初始化额度分离；浏览器验证窗口不足保留 once、不补发、空闲恢复；24 次持续点击累计 1,560 次 DOM 调用，分窗口累计输出 30,720 字节 |
| 注册、移除和派发顺序 | 运行时覆盖重复移除、类型与跨上下文句柄、上限和失败回滚；浏览器覆盖重复注册、丢弃句柄、once 抛异常、自移除、移除后续监听以及新增监听下次生效 |
| 原生输入和默认行为 | CDP `Input.dispatchMouseEvent` 及键盘输入产生 `is_trusted=True`，`element.click()` 为 False；新增复选框用例证明 Python 返回 `False` 不取消默认动作，后续 DOM listener 观察到 `defaultPrevented=false` |
| 授权与页面模式 | 受限子框架拒绝初始化；CSP nonce、源节点移除后的授权、后加 CSP 与禁用脚本均按受限模式复核；放开模式允许既定页面策略覆盖；Blink 特权页面排除及既有解析、设置回归通过 |
| DOM 与 GC | 浏览器验证断开跳过、重连恢复、同 ID 替换不转移、迁往其他文档不调用旧 Python、弱引用节点可回收，以及 Shadow DOM / 自定义元素拒绝；单测覆盖禁止脚本及延迟安全清理 |
| 异常和致命停止 | 普通异常保留后续调用及来源信息；50 ms 协作超时、128 次 DOM、4 KiB 输出或诊断超限撤销所有监听，窗口过期后仍不能复活；返回值析构没有 DOM 能力 |
| 导航与生命周期 | 刷新、普通导航、fragment、`document.open()`、子框架移除和替换、子文档 open、活动监听器 BFCache 排除均通过；真实 prerender 页面不初始化 Python，激活后也不补执行；存在监听器时浏览器正常关闭退出码 0 |
| 构建与记录 | 本轮五目标标准构建、117 项单测、90 项浏览器验收完成；结果、日志、截图和测试脚本调试记录分别保存 |

设置测试读取 Renderer 自检诊断，10 条结果均为 `passed=true`、`sandboxed=true`。事件测试分别启动受限与放开页面限制模式，未使用 `--no-sandbox` 或 `--disable-web-security`。

## prerender 测试接入与失败记录

点击脚本从直接连接 `/devtools/page` 改为连接 browser endpoint，再通过 tab target 自动附加主页面和 prerender 会话。此版本 `HasSessionsWithoutTabTargetSupport()` 会让旧式 page 连接阻止 prerender；仅调用 `Page.setPrerenderingAllowed` 无法绕过这一判定。脚本观察 tab 自动附加及 target 更新事件，以获取 prerender 会话，而不是只依赖 `Target.getTargets` 的枚举。

前五次探测都保存了当时的失败结果，没有改写为最终成功：

1. 初版只枚举目标，未取得 prerender target，等待超时。
2. 增加 tab 附加与 Preload 诊断，明确收到 `PrerenderingDisabledByDevTools`。
3. 显式设置 `Page.setPrerenderingAllowed` 后，旧式 page 会话仍触发同一阻止条件。
4. 全面切换 tab-aware 连接后，事件已显示 prerender `Ready`，但依赖目标枚举的等待仍超时；据此改用 target 更新事件。
5. prerender 前后文本断言均通过，但发起页面会话被替换，未收到预期的 Preload Success 通知；最终使用激活后文档的 Navigation Timing `activationStart > 0` 证明实际激活。

最终完整验收中，观察到实际 prerender target，预渲染完成时 `document.prerendering=true`、结果仍为 `0`；激活后为 false，点击仍不触发 Python，`activationStart=206 ms`。测试未跳过该用例，也未修改生产代码来适配测试。以上探测见[诊断索引](pyscript-event-validation/README.md)。

## 证据与边界

- [点击示例截图](pyscript-event-validation/final/events.json.sample.png)：不含 JavaScript 的示例页面，计数为 2，源标签移除后监听仍有效。
- [设置页截图](pyscript-event-validation/final/settings-browser.json.settings.png)：解释器、页面限制模式和生效状态；[DOM 示例截图](pyscript-event-validation/final/dom.json.sample.png)记录既有桥接回归。
- [归档清单](pyscript-event-validation/archive-manifest.json)保存原文件与归档文件哈希。日志统一为 UTF-8、LF、单 EOF 换行；非法 UTF-8 字节转义，不改变结构化测试结果或截图。未规范化的原始日志保留在本机 `out/Debug/pyscript-event-phase5-final/`。

浏览器日志仍有 `Network service crashed or was terminated, restarting service` 诊断，以及 Debug / headless 的 GPU、OAuth 等消息，已一并保留。页面断言与正常关闭检查通过；本轮没有定位这些子进程诊断的原因，也不将验收描述为长时间无子进程重启的稳定性测试。

首版仍限于普通 HTML / light DOM 的同步 click 回调；不提供其他事件、传播控制、`preventDefault`、asyncio 或任意 DOM 写入。初始化、单次事件、窗口和清理预算沿用阶段四，时间限制仍为协作式，不能硬中断任意 C 操作或提供内存硬上限。放开页面限制模式仍保留 Renderer 沙箱。当前证据覆盖 Windows x64 Debug 和真实 headless 浏览器，不替代 Release、其他平台或长期压力验证。
