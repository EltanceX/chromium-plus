# Chromium Plus 定制改动总览与提交索引

整理日期：2026-11-15（Asia/Shanghai）。本文汇总已经提交的定制实现、验证资料及开发恢复方式，作为根目录 [README](../README.md) 的详细入口。

## 整理范围

| 项目 | 范围 |
| --- | --- |
| Chromium 版本 | 158.0.8126.0，见 [chrome/VERSION](../chrome/VERSION) |
| 官方源码基线 | `41d3eb785bd9aec40ca01fb345c9b0c32428f116` |
| 定制功能截止提交 | `95bfed38d861aec7467febe5c458aa25bce8c9a4` |
| 定制历史 | 基线之后的 37 次提交，提交日期为 2026-11-07 至 2026-11-14 |
| 文件范围 | 截止提交相对基线的净差异涉及 2663 个路径，包含源码、测试、第三方导入源码和文档证据 |
| 已验证平台 | Windows x64，Debug component build |
| 远程仓库 | [EltanceX/chromium-plus](https://github.com/EltanceX/chromium-plus)；默认分支 `main` |

此处的计数是固定历史范围，本文及 README 的文档整理提交不计入上述 37 次。部分早期方案保留了当时的待办和接口设计，应结合后续阶段记录及代码判断当前状态，不能把方案中的计划当成已经验证的能力。

可在具有上述历史的仓库中复核：

```powershell
git rev-list --count 41d3eb785bd9aec40ca01fb345c9b0c32428f116..95bfed38d861aec7467febe5c458aa25bce8c9a4
git log --reverse --oneline 41d3eb785bd9aec40ca01fb345c9b0c32428f116..95bfed38d861aec7467febe5c458aa25bce8c9a4
git diff --name-only 41d3eb785bd9aec40ca01fb345c9b0c32428f116 95bfed38d861aec7467febe5c458aa25bce8c9a4
```

## 已实现功能

### 内核调试与解释器设置

在设置侧栏增加“内核调试”，入口为 `chrome://settings/kernelDebug`。通过 Settings WebUI 与 C++ Handler 通信，每约两秒展示浏览器版本、当前进程 ID、运行时间、驻留内存、窗口数和标签页列表；支持读取标签页详情与关闭当前 Profile 内的标签页，使用稳定标签句柄并校验归属。

同一页面增加 Python 启用开关、受限来源、“放开 Python 的站点与页面执行限制”、生效状态及保存/重启操作。配置保存至当前 user-data-dir 的 Local State，由启动快照统一应用。

受限模式采用精确 loopback HTTP(S) origin、顶层文档及页面脚本/CSP 检查；放开模式允许既定普通页面与子框架，覆盖 Python 的来源、页面脚本开关、CSP 和 sandbox 脚本执行检查。两种模式均保留 Renderer 操作系统沙箱、受限接口、解析规则和执行预算；WebUI、扩展及 MCBE/MCJE 页面仍不执行 Python。详见[设置与页面权限](pyscript-settings-and-page-permissions.md)。

### globalThis 默认字段

在页面及 iframe 主世界、Dedicated Worker、Shared Worker、Service Worker 上下文中，业务脚本执行前定义字符串属性 `globalThis.chromiumPlus`，初始值为 `"0.0.1"`。属性可写、可枚举、可配置，页面可修改或删除；新上下文重新获得默认值。

字段用于版本/功能识别。扩展隔离世界、全部 Worklet 和独立 V8 环境不属于当前覆盖承诺。源码集中在 [chromium_plus_bindings.cc](../chrome/renderer/chromium_plus_bindings.cc)，实现与 15 项检查的条件见[字段记录](globalthis-chromium-plus-implementation.md)。

### MCBE：Bedrock UDP 状态页

访问 `mcbe://geo.hivebedrock.network` 或 `mcbe://host:port`，浏览器通过 Network Service 异步 DNS/UDP 查询 RakNet Unconnected Ping/Pong，解析 MOTD、版本、人数等状态，并返回固定模板的本地结果页。默认 UDP 端口为 19132，支持显式端口、IPv4 和方括号 IPv6。

原生 Loader 处理顶层导航、错误结果、超时和取消；刷新发起新查询，服务器返回文本经过 HTML 转义。该协议不提供账号登录、游戏连接、自动轮询、系统级协议关联或网页通用 UDP API。详见 [MCBE 实现与验证](mcbe-native-protocol-implementation.md)。

### MCJE：Java SLP、SRV 与 PNG 图标

访问 `mcje://mc.hypixel.net`，浏览器执行现代 Java Server List Ping：URL 解析、可选 DNS SRV 发现、TCP 握手与状态请求、JSON/文本组件解析，以及 Ping/Pong 延迟测量。结果页展示 MOTD、版本、协议号、在线人数、逻辑地址、实际端点和诊断信息。

隐式默认端口为 25565；域名未显式指定端口时尝试 `_minecraft._tcp` SRV，显式端口或 IP 字面量跳过 SRV。可选 `favicon` 附件经过有界 Base64/PNG 校验、隔离解码和重新编码，显示标准 64×64 图标；附件缺失、损坏或超时可降级展示状态。

后续增加 Windows 系统 DNS SRV 回退：仅在 NetworkContext 报告查询能力不可用的指定错误时调用异步 `DnsQueryEx`；尊重附加查询类型开关与严格 Secure DNS 模式，沿用原 SRV/总超时及取消流程。修复了有 SRV 重定向的地址被错误连接至默认端口的问题。

入口、协议边界、PNG 处理和失败记录见 [MCJE 实现文档](mcje-native-protocol-implementation.md)，最新修复摘要见 [SRV 验证结果](mcje-validation/srv-fix-results-2026-11-07.json)。公网地址、在线人数、DNS 记录和延迟均只是各次测试的观察值。

### 原生 Python 与页面交互

将 CPython 3.13.18 的选定源码子集导入 [third_party/cpython](../third_party/cpython/README.chromium)，由 GN 构建运行时及冻结模块。当前构建明确限定 Windows x64，不依赖开发者机器的 Python DLL、site-packages 或页面目录中的模块。

Blink 原生识别完整文档解析中的内联 `<pyscript>`，在 Renderer 主线程同步执行。每个合格 Document 拥有独立上下文/子解释器，同一文档的代码块共享 globals；Python 全局变量与 JavaScript 全局对象独立。输出及异常以有界诊断进入当前页面控制台。

后续逐步加入以下能力：

| 能力 | 当前实现与入口 |
| --- | --- |
| DOM | 当前文档的不透明句柄、文本、类型化控件、class/安全属性、焦点、滚动、原生 dialog；[文档上下文与桥接](pyscript-phase3-document-context-dom.md)、[控件与教学页面](pyscript-mario-phase5.md) |
| 回调和点击 | Python callable 注册表、监听注册/移除、Blink 原生 click 派发、once/capture/passive 与生命周期管理；[点击事件方案](pyscript-click-events-implementation.md)、[首版验收](pyscript-event-phase5-build-and-browser-validation.md) |
| 输入和页面状态 | 键盘、指针、input/change、焦点、可见性、resize、只读事件快照及受控同步派发操作；[输入阶段记录](pyscript-input-phase2.md) |
| 动画帧 | `animation.request_frame` / `cancel_frame`、原生帧时间戳、有界待执行请求；[帧调度与预算](pyscript-animation-phase1.md) |
| Canvas | Canvas 2D 子集、批量命令、缓存表面、整幅替换、资源/命令/像素计量；[绘图阶段记录](pyscript-canvas-phase3.md) |
| 数学 | 静态内建 `math`，游戏使用明确的固定种子 xorshift32；[核心玩法迁移](pyscript-mario-phase4.md) |
| 浏览器服务 | 有界合成音效、同源最高分存储、正常用户激活下的全屏和页面源码导出；[阶段六记录](pyscript-mario-phase6.md) |

初始化、事件和动画帧采用分开的单次额度；事件与动画帧共享交互时间窗口。当前帧保护为 16 ms；初始化、交互、输出、DOM、Canvas、音频和原生资源各有上限。文档失效、导航、撤权或致命停止会撤销后续执行，并通过安全任务清理 Python 引用和原生资源。

时间限制采用协作式检查，不能硬中断尚未返回的任意 C 调用，也不提供 Python 堆的硬内存上限。当前不提供完整标准库、pip、任意原生扩展、通用文件/进程/网络 API、外链 Python、动态插入代码执行、asyncio 或完整 DOM 映射。完整方案见 [Python MVP](pyscript-native-python-mvp-implementation.md)；后续能力以表内阶段记录为准。

### Python Mario 与 Debug 稳定性

在 [chrome/test/data/pyscript/mario](../chrome/test/data/pyscript/mario/README.md) 中维护 HTML/CSS shell 与 Python 源文件，由生成脚本组成 [mario.html](../chrome/test/data/pyscript/mario.html)。游戏运行不依赖页面 JavaScript，物理、关卡、碰撞、计分、输入、绘图指令和教学交互由 Python 驱动，栅格和平台服务由原生引擎执行。

迁移包括开始/暂停/恢复/重开、生命与检查点、通关、键盘/触屏输入、参数滑块、慢动作、碰撞框、课程及 dialog、合成音效、最高分、全屏和源码下载。Canvas 使用批量提交、缓存及裁剪；部分视觉细节有简化，确定性测试对照原版物理与计分。

最终 Debug 修复降低逐字节码追踪开销，改用行/调用/返回/异常检查并保留超时保护；在已授权的含 Canvas 页面进入解释器前准备原生共享 GPU 上下文，返回后复核授权；修正导出预检、dialog 焦点和真实浏览器冻结/恢复测试时序。期间遇到的旧 COFF/CodeView 对象损坏在保存诊断后定向重编恢复。

阶段总览见 [Mario 实现方案](pyscript-mario-implementation.md)，最终结果、性能观察及墙钟保护边界见 [Debug 稳定性与验收](pyscript-mario-debug-validation.md)。源码导出仍需在本项目浏览器中、获准来源和解释器设置下运行，不是通用 Python 网页的跨浏览器发布能力。

## 源码与文档导航

| 目录或入口 | 职责 |
| --- | --- |
| [settings/kernel_debug_page](../chrome/browser/resources/settings/kernel_debug_page) 与 [KernelDebugHandler](../chrome/browser/ui/webui/settings/kernel_debug_handler.cc) | 内核调试 WebUI、原生状态通信和设置表单 |
| [chrome/browser/native_python](../chrome/browser/native_python) | Python 设置校验、持久化和 Renderer 启动参数 |
| [chrome/renderer/chromium_plus_bindings.cc](../chrome/renderer/chromium_plus_bindings.cc) | JavaScript 全局默认字段 |
| [chrome/browser/mcbe](../chrome/browser/mcbe)、[mcje](../chrome/browser/mcje)、[minecraft](../chrome/browser/minecraft) | 原生协议、查询、结果页及公共响应辅助 |
| [components/python_runtime](../components/python_runtime) | CPython、原生模块、回调表、普通数据及预算 |
| [chrome/renderer/python_document_context.cc](../chrome/renderer/python_document_context.cc) | Chrome 文档适配与运行时桥接 |
| [PythonScriptController](../third_party/blink/renderer/core/html/python_script_controller.cc) | Blink 执行策略、文档生命周期、原生调度与资源管理 |
| [third_party/cpython](../third_party/cpython/README.chromium) | 锁定的 CPython 源码、许可证、清单和 GN 构建 |
| [chrome/test/data/pyscript](../chrome/test/data/pyscript) | 内联 Python、DOM、事件、动画、Canvas 和 Mario 示例 |
| [chrome/test](../chrome/test) | `pyscript_*`、`mcje_smoke_test.mjs`、`mcbe_smoke_test.mjs` 等宿主/浏览器验证入口 |
| [docs-codex](../docs-codex) | 实现方案、阶段记录、结构化测试结果、截图及诊断归档 |

## 已归档验证与适用范围

以下为对应实现阶段的历史实测，不是本次文档整理重新执行的构建或测试，也不能跨阶段累计成一个独立覆盖总数。

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| Python / Mario 最终统一验收 | 五个标准 Debug 目标构建成功；18 组、460 项通过：224 原生、29 宿主逻辑、207 浏览器 | [最终汇总](pyscript-mario-debug-validation/final/summary.json)、[验收说明](pyscript-mario-debug-validation.md) |
| 硬件 GPU Mario | 36 项通过；含 300345 ms 延长交互、259 轮输入、25 次冻结恢复 | [硬件结果](pyscript-mario-debug-validation/diagnostics/soak-gpu-final.json)、[最终审计](pyscript-mario-debug-validation/final-audit.json) |
| MCJE Windows SRV 修复 | 103 项单测；默认网络服务和进程内诊断模式各 34 项浏览器检查 | [结构化结果](mcje-validation/srv-fix-results-2026-11-07.json)、[详细条件](mcje-native-protocol-implementation.md#16-windows-系统-srv-回退修复) |
| 同轮既有功能回归 | MCBE 17 项、chromiumPlus 15 项通过 | [SRV 修复回归记录](mcje-validation/srv-fix-results-2026-11-07.json) |

最终 Python 统一入口默认采用软件绘制；硬件 GPU 结果单独记录，重复覆盖不重复计数。预算、Renderer 沙箱与 Debug 符号保持生效。5 分钟测试不代表数小时游玩、跨设备帧率、整个 Python 堆无泄漏或声音主观听感；Release 及其他平台验证暂缓。

早期网络服务崩溃、图标超时、构建对象异常以及失败探测按原结果保留。证据中的绝对路径和原始提交状态反映实际执行时环境，不能将它们改写为提交后的干净状态；部分原始日志/坏对象二进制仅保存在本机 `out/Debug`，Git 保存的是选定的归档资料与哈希。目录入口见[最终证据说明](pyscript-mario-debug-validation/README.md)。

## 37 次定制提交索引

按 Git 提交顺序排列，日期取提交作者日期；条目说明概括提交内容，不表示每一条都新增产品功能。链接指向 GitHub 中的完整提交对象。

| 序号 | 提交 | 日期 | 内容 |
| --- | --- | --- | --- |
| 1 | [54c778f5c2](https://github.com/EltanceX/chromium-plus/commit/54c778f5c2e19a041117344301abe2457583f718) | 2026-11-07 | 内核调试页、实时状态、标签页详情与关闭 |
| 2 | [7947dadae1](https://github.com/EltanceX/chromium-plus/commit/7947dadae12d8e01c9baa5b08824bd0a0cd80e23) | 2026-11-07 | chromiumPlus 默认字段与 MCBE 实现方案 |
| 3 | [23aca714af](https://github.com/EltanceX/chromium-plus/commit/23aca714af1008ecd161d30f3d87336a08f6b559) | 2026-11-07 | 原生 MCBE 导航、UDP 查询与 Bedrock 状态页 |
| 4 | [25281292f3](https://github.com/EltanceX/chromium-plus/commit/25281292f38b38af99963e1c8d3f6bdf0097bede) | 2026-11-07 | MCJE 状态页与服务器图标实现方案 |
| 5 | [9257464d67](https://github.com/EltanceX/chromium-plus/commit/9257464d67a3a1cfbf24e08b112e446e570bb0af) | 2026-11-07 | 原生 MCJE、SLP/SRV/TCP 与 PNG 图标 |
| 6 | [c53586b2f7](https://github.com/EltanceX/chromium-plus/commit/c53586b2f7ddf8393d6f250839b10581370eccbb) | 2026-11-08 | CPython 依赖、独立运行时与沙箱内诊断 |
| 7 | [002df59590](https://github.com/EltanceX/chromium-plus/commit/002df595906e2a6a3bf3b42894b07437829368d7) | 2026-11-08 | `<pyscript>`、解析登记与执行准入 |
| 8 | [b02f92b92b](https://github.com/EltanceX/chromium-plus/commit/b02f92b92b1346b9888cd31db5bea3a7b3371173) | 2026-11-08 | Chrome 构建恢复与 Python 浏览器检查记录 |
| 9 | [d63e4e2d64](https://github.com/EltanceX/chromium-plus/commit/d63e4e2d64bdbebbcad3b1dfc007ef71fcd9e6fb) | 2026-11-08 | 标签与解析阶段完整验证 |
| 10 | [5dcb4e0cde](https://github.com/EltanceX/chromium-plus/commit/5dcb4e0cde7dad36011f618827e70ba219f5b038) | 2026-11-08 | 子解释器独立的原生 dom 模块 |
| 11 | [1ffbda7979](https://github.com/EltanceX/chromium-plus/commit/1ffbda7979cfe17fc917ba11d13755db0c32db8a) | 2026-11-08 | 文档独立 Python 上下文与最小 DOM 桥接 |
| 12 | [cfbc3e0972](https://github.com/EltanceX/chromium-plus/commit/cfbc3e097259b6d98544d96eae8487bc70d5d260) | 2026-11-08 | 文档/DOM 阶段构建恢复与续验记录 |
| 13 | [09644b7baa](https://github.com/EltanceX/chromium-plus/commit/09644b7baad6a9a61a16c7d7a341be36a5c4b695) | 2026-11-08 | DOM 浏览器验证和 Chrome 构建恢复记录 |
| 14 | [f5cfae5971](https://github.com/EltanceX/chromium-plus/commit/f5cfae59711b0c712e7997ea7135995fad3a8723) | 2026-11-09 | 文档失效接口导出与第三阶段验证 |
| 15 | [624d248976](https://github.com/EltanceX/chromium-plus/commit/624d24897602fffb38189850c6090f258083fa91) | 2026-11-09 | 协作式执行预算与不可恢复的运行时停止 |
| 16 | [f8ce4124a4](https://github.com/EltanceX/chromium-plus/commit/f8ce4124a4c4c188094be188e51317bb86b7d6b0) | 2026-11-09 | 文档预算、生命周期与清理集成 |
| 17 | [ecf6cd884d](https://github.com/EltanceX/chromium-plus/commit/ecf6cd884d556315ff5dc0431906190fe8b983c6) | 2026-11-09 | 原生 Python MVP 完整构建与验收 |
| 18 | [dabd65e514](https://github.com/EltanceX/chromium-plus/commit/dabd65e514103711e3dda8bc6457edcc736130e6) | 2026-11-09 | Python 设置、启动配置及页面权限模式 |
| 19 | [83433306b8](https://github.com/EltanceX/chromium-plus/commit/83433306b8d93bd71d05c64582aeba8901465467) | 2026-11-09 | 原生 Python 点击事件方案 |
| 20 | [7580758e69](https://github.com/EltanceX/chromium-plus/commit/7580758e69c4822242d8d093b2dc651e43860d49) | 2026-11-09 | 运行时回调注册表与调用入口 |
| 21 | [b7c2b53701](https://github.com/EltanceX/chromium-plus/commit/b7c2b537011ca0af7c613d5ff3720428f67a95ce) | 2026-11-09 | DOM listener 注册/移除与 Chrome 回调适配 |
| 22 | [8f4c92eaa9](https://github.com/EltanceX/chromium-plus/commit/8f4c92eaa964585fdd7dac3e96c55e235b63a9a3) | 2026-11-09 | Blink 原生 click 监听与 Python 派发 |
| 23 | [9ae16a492d](https://github.com/EltanceX/chromium-plus/commit/9ae16a492d6cc6a65cfeae43468927391c1fe2ab) | 2026-11-09 | 事件阶段构建恢复和组件回归记录 |
| 24 | [186af85632](https://github.com/EltanceX/chromium-plus/commit/186af85632e3280b4f42747773f952d3984c4199) | 2026-11-09 | 原生 click 派发阶段构建验证 |
| 25 | [dd47ddebfc](https://github.com/EltanceX/chromium-plus/commit/dd47ddebfc8d5e9a075aebe62682defc8054bf54) | 2026-11-09 | 初始化与回调资源额度分离 |
| 26 | [b3a05f25f2](https://github.com/EltanceX/chromium-plus/commit/b3a05f25f236ea0231df7c886b8dffef5aee07a0) | 2026-11-09 | 交互窗口准入、导航与生命周期清理 |
| 27 | [ed9a245bf1](https://github.com/EltanceX/chromium-plus/commit/ed9a245bf1e2d6913029eaf733865776ecde6e3a) | 2026-11-10 | 点击事件完整真实浏览器验收 |
| 28 | [6f53a6b1c0](https://github.com/EltanceX/chromium-plus/commit/6f53a6b1c04d659959ea6c323c12416a9c2f0495) | 2026-11-10 | 原生动画帧调度与帧预算 |
| 29 | [024e92383e](https://github.com/EltanceX/chromium-plus/commit/024e92383ef2dba185f3bb0c4b54db21f0b0560c) | 2026-11-10 | 键盘、指针与页面状态事件 |
| 30 | [73bc3ca13c](https://github.com/EltanceX/chromium-plus/commit/73bc3ca13c35595c0edd0e5744d220722c8cd380) | 2026-11-10 | Canvas 2D 子集、批量提交与绘图预算 |
| 31 | [914522822b](https://github.com/EltanceX/chromium-plus/commit/914522822b25af5d60c21bdd60174aa1be9ca600) | 2026-11-11 | 数学依赖及 Mario 核心物理与玩法迁移 |
| 32 | [cd413a9694](https://github.com/EltanceX/chromium-plus/commit/cd413a9694c450e036f7f59bb37a89208c2e2be3) | 2026-11-11 | 类型化控件、教学面板和完整页面交互 |
| 33 | [2f5ffd4dc9](https://github.com/EltanceX/chromium-plus/commit/2f5ffd4dc938d9b83130c1e3c6be37eb15c098c1) | 2026-11-11 | 受控浏览器服务、音效、最高分、全屏与导出 |
| 34 | [32e60f9e94](https://github.com/EltanceX/chromium-plus/commit/32e60f9e94224c12a46f2b926aef008e2d6b1f5b) | 2026-11-11 | Mario 平台功能验证进度与构建恢复记录 |
| 35 | [77aaf6d27b](https://github.com/EltanceX/chromium-plus/commit/77aaf6d27b656bd930f37b4660d121f6c07b788d) | 2026-11-12 | Python 导出预检及 Mario dialog 焦点修复 |
| 36 | [1117ad7a9a](https://github.com/EltanceX/chromium-plus/commit/1117ad7a9a1b5c276315050c2cbb7b7cd8f29a1c) | 2026-11-14 | Debug 追踪与 GPU 冷启动优化、最终验收及硬件 GPU 实测 |
| 37 | [95bfed38d8](https://github.com/EltanceX/chromium-plus/commit/95bfed38d861aec7467febe5c458aa25bce8c9a4) | 2026-11-14 | Windows 系统 DNS SRV 回退、测试及验证记录 |

## 新设备恢复开发

### 能恢复的内容

GitHub `main` 已包含上述 37 次定制历史、源码、页面、工具及归档文档。2026-11-15 文档整理前，本地 HEAD 与实时远端 `main` 同为 `95bfed38d861aec7467febe5c458aa25bce8c9a4`；后续文档提交将在其上继续推进。源码可恢复到当前功能开发阶段，但 Git 不保存完整机器环境或增量构建进度。

| 内容 | 恢复方式 |
| --- | --- |
| 定制代码、提交历史、测试及归档证据 | 从 GitHub 克隆 |
| CPython 选定源码子集 | 已随主仓库提交；776 份导入文件由 source_manifest.json 校验 |
| V8、Skia、LLVM、CIPD 等依赖 | 按已提交 DEPS，通过 gclient、hooks 和依赖服务重新获取 |
| depot_tools、Visual Studio、SDK、Node、宿主 Python | 新机安装或配置；宿主 Python 与内嵌 CPython 分别管理 |
| `.gclient`、`args.gn`、环境变量、sccache | 重新创建或另外迁移 |
| Debug EXE/DLL、PDB、对象、编译缓存 | 重新构建；不随 Git 恢复 |
| 浏览器用户数据、Python 开关/来源、最高分等 | 另外迁移或重新设置 |
| 外部 Mario 原项目 | 普通构建和回归不需要；仅重新生成原版对照快照时需要原件 |

### 推荐恢复顺序

1. 准备 Windows x64、充足的 NTFS 磁盘空间和内存，按 [Windows 构建说明](../docs/windows_build_instructions.md) 安装 VS 2026 C++、ATL/MFC、Windows SDK 及调试工具。将 depot_tools 放在 PATH 前部，并设置 `DEPOT_TOOLS_WIN_TOOLCHAIN=0`。浏览器验收另需可用的 Node 与宿主 Python。
2. 在新的工作父目录中克隆本项目，配置 fork solution，固定首次同步的源码修订。下面以新机 `C:\chromium-plus` 为例，目录中不应已有其他 checkout：

   ```powershell
   New-Item -ItemType Directory -Path C:\chromium-plus
   Set-Location C:\chromium-plus
   $env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
   git clone --depth=100 --single-branch --branch main https://github.com/EltanceX/chromium-plus.git src
   $chromiumPlusRevision = git -C src rev-parse HEAD
   gclient config --name=src --unmanaged https://github.com/EltanceX/chromium-plus.git
   gclient sync --no-history --revision "src@$chromiumPlusRevision"
   Set-Location src
   git switch -c codex/development
   ```

   深度 100 的克隆可包含本次整理的 37 次定制提交及当前文档更新；以后提交增加时应调整深度或补拉历史。首次同步可能耗时很长，并需要访问 Chromium、CIPD 和相关下载服务。使用 gclient 处理平台条件与依赖，不以直接递归初始化全部 Git submodule 替代。

3. 新建 `out/Debug/args.gn`，写入以下配置后执行 `gn gen out/Debug`：

   ```gn
   is_debug = true
   is_component_build = true
   target_cpu = "x64"
   ```

   原机器还使用 `cc_wrapper = "sccache"`；新机安装并配置 sccache 后可加入。GN 配置与 `out/` 输出被忽略，需要另行保存；原有 Debug 对象和缓存不是恢复源码的前提。

4. 构建 Debug 并复验，使用新的输出目录保存本机证据：

   ```powershell
   autoninja -C out/Debug chrome blink_unittests python_runtime_unittests python_document_context_unittests native_python_settings_unittests mcje_unittests mcbe_unittests
   python chrome/test/run_pyscript_event_validation.py --output-dir out/Debug/pyscript-validation-new-device
   ```

   统一 Python 入口会再次检查标准五目标构建及全部配置中的测试。MCJE/MCBE 目标可单独执行；真实浏览器、公网查询和硬件 GPU 需要在新机分别验证。上述为恢复步骤说明，本次文档更新没有在新设备执行完整克隆和重建。

5. 启动新构建，在 `chrome://settings/kernelDebug` 配置 Python 解释器及实际测试来源，保存重启后再访问示例。普通 Mario 回归读取已提交的 `reference.json` 与 `route.json`；重新生成参考数据时才需提供原项目，且要通过记录的 SHA-256 校验。

原机器的远程名 `origin` 指向 Chromium 官方、`github` 指向定制仓库；按上面的新克隆流程，`origin` 将指向 GitHub。操作前以 `git remote -v` 确认目标，后续同步官方更新时单独配置上游。当前本地官方历史是浅克隆，这不影响上述 37 次定制提交的恢复，但不代表已备份 Chromium 的全部历史。

## 后续维护

功能变更应同步更新根 README 的能力状态，并在 `docs-codex/` 保存适用版本、实现步骤和验证证据。本文的 37 次提交索引保留为固定历史快照，后续提交通过 Git 历史继续追踪；若扩大整理范围，应同时更新基线、截止点和计数。

当前保持 Debug 开发。Release、其他平台、完整 Python 模块生态、更多 DOM/Canvas 能力及长期稳定性需独立实施和验收，不能从已归档的首版结果推定完成。
