# 阶段六：合成音效、最高分、全屏与导出

日期：2026-11-11 开始，2026-11-14 更新最终 Debug 验收。原项目 `C:/VD/Projects/mario/index.html` 保持只读。

后续 Debug 稳定性修复与验收已完成：460 项统一检查及硬件 GPU 完整 Mario/5 分钟交互共 36 项全部通过。逐行追踪、GPU 冷启动、冻结恢复驱动修正和新证据见[Debug 最终记录](pyscript-mario-debug-validation.md)。以下 2026-11-12 的失败汇总作为历史保留，不改写为成功；Release 按用户要求暂不构建。

## 实现

新增内建 `browser` 模块，沿用 `DomHost → BudgetedDomHost → ChromeDomHost → WebPythonDomHost → PythonScriptController`；core 通过 `PythonBrowserBridge` 工厂调用 modules 的原生实现，无 core 到 modules 依赖、JavaScript 求值或 Python 音频线程回调。

| Python API | 语义 |
| --- | --- |
| `audio_resume()` / `audio_suspend()` | 返回异步操作编号；首次开启要求真实 transient user activation，继续经过 WebAudio 自动播放策略 |
| `audio_state()` / `audio_stop()` | 查询原生状态；停止并断开文档的全部已调度音符 |
| `play_tones(sequence)` | 每个音符为 `(wave, frequency, end_frequency, duration, delay, volume)`，直接连接原生 OscillatorNode/GainNode |
| `storage_get(key)` / `storage_set(key, value)` | 同源 localStorage；缺失返回 None，访问/配额拒绝抛 PermissionError |
| `request_fullscreen(element)` / `exit_fullscreen()` | 返回操作编号；走正常 unprefixed Fullscreen API 的激活、权限和异步完成路径 |
| `is_fullscreen()` | 当前所属文档的原生全屏状态 |
| `export_document(filename)` | 将有界文档快照提交正常下载策略；返回不代表文件已保存 |
| `poll()` | 返回 None 或一次性 `(operation_id, success, detail)`，调用时重新检查当前文档、来源、策略、执行预算及运行代数 |

同步宿主调用仍计入既有 DOM/时间额度。异步 Promise 的原生反应仅存放普通数据；Python 在已获准的原生帧/事件中领取结果。每个 bridge 只属于一个 controller 生命周期，Dispose 清空队列并使旧完成失效。未完成 Promise 最多 8 个，待领取结果与待通知请求合计最多 8 个。超过 5 秒后领取失败结果；底层仍未结束的 Promise 继续占用原生槽位，不能用超时循环创建无限 Promise。late completion 不重复通知。

真实浏览器探测曾复现首次创建 AudioContext 超过 50 ms 的 Python click 预算。音频 resume/suspend 与全屏 request/exit 因此采用排队的原生任务，Python 调用只登记有界请求并返回编号；任务执行前通过弱引用重新核对 controller、文档、来源、解释器设置和页面策略，并检查超时、元素连接及首次音频真实激活。任务或 Promise 反应都不进入 Python；销毁后任务失效。suspend 在排队前先断开旧音符，避免排队延迟继续发声。冷启动的平台初始化不占用 Python 回调，但仍在 Renderer 主线程，不能据此承诺无卡顿。

第二次探测发现首次播放还会在 Python 帧中惰性生成 PeriodicWave 表，触发原有 16 ms 停止。原生初始化任务现在提前准备固定四种波形的缓存，成功后才通知 Python 已就绪；实际音符参数转换、节点创建及调度仍经过原有同步预算。音调使用六项普通 tuple，数值只接受精确 int/float，拒绝自定义转换钩子，避免解析过程中修改外层列表并释放借用对象。

导航探测还复现 AudioContext 的重复反初始化：bridge 提前关闭后，原生文档 observer 又调用一次非幂等的 Uninitialize，导致 Debug 检查失败。提前清理现仅在 AudioContext 尚有 ExecutionContext 时执行，并注销其生命周期 observer；若原生文档已清理则直接释放 bridge 引用。既不删除原生检查，也不靠进程退出掩盖问题。

音频每文档一个 AudioContext，最多 16 个存活/未来音符，每次 1–16 个，每个一秒计数窗口最多调度 64 个。波形限 sine/square/triangle/sawtooth，频率 20–20000 Hz，时长 0.01–2 秒，延迟 0–4 秒，音量 0–0.25，拒绝 NaN/Inf。统一乘 0.25 输出系数，使 16 个最大音量音符之和不超过 1；短起音和指数衰减避免硬切换。音符在原生音频时钟调度，音频线程不进入 CPython。停止/重开断开旧节点；隐藏、失焦等原生输入重置即静音，即使 Python 事件被窗口额度跳过；导航、关闭、撤权后的致命停止和 GC 清理释放上下文。

存储 key 最多 256 UTF-8 字节、value 最多 16 KiB，保留 StorageArea 的来源、用户设置、配额与隐私语义。Mario 沿用 `marioLabBest` 键，初始化读取、通关提交最高分，缺失/无效数据为零，负值截为零，上限 999999999；保存失败保留本局成绩与玩法。

配额回归发现启用 QuotaExceededErrorUpdate 时，原生异常辅助函数在缺少 V8 isolate 时直接返回，不会置 ExceptionState。桥接写入后因此还读取同一同步缓存校验目标值，将被拒绝的写入映射为 PermissionError；不为存储创建 V8 realm，也不把缓存可见等同于磁盘落盘。测试先明确验证 JavaScript 填充触发 QuotaExceededError，再验证 Python 拒绝与后续可用性。

导出只接受简单 ASCII `.html` 文件名，不接收路径，要求真实用户激活，每文档最多 8 次、间隔至少 2 秒。序列化前限制 4096 节点、128 层深度、128 Ki UTF-16 源单位，包含属性与内部 nonce；序列化结果最多 1 MiB UTF-8。不支持 template 与页面自建 shadow 内容；原生控件的 UA shadow tree 不参与序列化，允许导出其 light DOM 与属性。复用 MarkupAccumulator，恢复被解析器隐藏的 nonce，去掉 dialog 的 open 状态；复用 Blob/PublicURLManager 和 AnchorElementUtils 的正常 sandbox/navigation/download 策略，每文档最多持有一个导出 URL。Mario 启动时重置 tab、音效状态和覆盖层；导出不嵌套存入整页字符串。

真实浏览器曾发现 range 控件的 UA shadow root 被预检误拒绝，下载未产生。含单个滑块的最小页面已复现相同 PermissionError；修复只排除 UA shadow root，仍拒绝页面自建的 open/closed shadow 与 template，并补充原生回归。

后续复查发现 `nodeValue()` 不包含 DOCTYPE 的名称/publicId/systemId，以及处理指令的 target。现将这些字段计入序列化前的源单位上限。新增回归用四种大字段复现：修复前均在分配 markup 后才返回 1 MiB 输出错误，修复后均在预检阶段拒绝；源与结果上限保持不变。

这是对自包含 Mario 页面的源码导出，不打包外链资源，也不保存 Python 运行栈、Canvas 像素或游戏进度。下载文件仍依赖本项目浏览器、启用的解释器与允许的 HTTP 来源；建议将文件放回本地 HTTP 服务后打开，不能绕过来源配置。

## 页面迁移

新增 `mario/platform.py`，页面由 12 个有序内联 `<pyscript>` 组成。八类音效 jump/coin/bump/stomp/hurt/checkpoint/start/win 保留原版波形和音符参数，默认静音，音效按钮与 M 快捷键开启。声音事件队列最多 8 条，提交按 16 个音符截分；输出繁忙时丢弃效果，不影响物理或计分。最高分读写、全屏按钮、导出按钮、课程、提示及 README 同步更新。

平台服务每帧最多处理一个分支，在已有 3 ms 软检查之后执行；UI 仍使用原有合并队列。暂停/页面恢复对目标音频状态进行协调，旧 resume 完成后不会永久覆盖后来的暂停请求。失败可重试，音频和存储失败不停止游玩。普通事件 50 ms、帧 16 ms、初始化 100/250 ms 和全部既有窗口/绘图/沙箱限制保持不变。

本次统一验收与独立复查分别在宽屏、窄屏打开学习地图时记录过 50 ms 停止。当前默认旧版 dialog 聚焦路径会扫描子元素，逐个检查 focusability 以寻找 autofocus。现为两个 dialog 的关闭按钮声明 autofocus，保持原有首个按钮焦点，并让扫描提前结束；浏览器回归补充初始焦点断言，修正后的独立 Mario 35 项已通过。没有提高预算或把 showModal 移出计时；Debug 调度等待等间歇超限风险仍需阶段七长时验证。

## 验证与记录

阶段六 Debug 功能和标准五目标构建已完成。2026-11-14 的最终 460 项统一验收全部通过，验证期间 875 个功能输入保持不变，详见[最终结果](pyscript-mario-debug-validation/final/summary.json)。标准目标为 chrome、blink_unittests、python_runtime_unittests、python_document_context_unittests、native_python_settings_unittests。下面保留此前仍有一项 Debug 帧超限的历史验收。

主体实现提交为 `2f5ffd4dc9`，前次阶段性记录为 `32e60f9e94`。本轮补齐导出元数据预检、对话框 autofocus 与构建后的实际验证。当前结果、源码/二进制哈希和限制见[验证记录](pyscript-mario-platform-validation/README.md)及[统一汇总](pyscript-mario-platform-validation/final/summary.json)。

最终统一运行共计划 18 组、456 项，实际执行 454 项：453 项通过、1 项失败、2 项因 Mario 组提前停止未运行。220 项原生单测和 29 项宿主逻辑全部通过；浏览器通过 204 项。875 个功能输入在运行期间不变，`sourceUnchanged=true`，但 `passed=false`。其中 `unitTestsPassed=249` 包含宿主逻辑，不应再加一次 29。

Mario 的前 32 项通过，包含宽窄屏对话框和 14 条隔离原版轨迹；第 33 项整关轨迹在 frame 2340 因 16 ms 预算停止，其后的导航/刷新及正常关闭未执行。此前相同功能输入的独立 `mario-autofocus-3` 已通过全部 35 项，包含整关 2773 步、25 金币、5170 分、零死亡及末尾两项。该成功不能改写统一验收的失败。回放每帧只推进一个物理步，常规持续绘图未运行，音频默认关闭；当前日志不能区分那次物理/桥接开销与线程调度等待，根因尚未确定。保留原始失败和预算锁存，不通过重复执行挑选全绿结果。Debug 间歇停止、Release/GPU、长时真实游玩及扬声器听感仍需后续验证。

新增运行时 typed payload/参数/预算/撤权/拒绝测试，Chrome 适配测试，Blink 原生拒绝/资源/销毁测试，宿主侧音效、最高分和异步状态协调测试，以及 `pyscript_browser_smoke_test.mjs` 的真实浏览器测试。浏览器脚本仅为外部 CDP 驱动，不构成页面 JavaScript 依赖；音频图和运行状态验证不能替代扬声器听感验收。持久化用明确声明的终局计分夹具，核心通关仍由既有原版轨迹对照覆盖。


## 探测中修正的问题

历史失败保留在本阶段 `out/Debug/pyscript-mario-phase6/`；可审阅副本见[证据目录](pyscript-mario-platform-validation/README.md)，不会用成功结果覆盖：

| 探测 | 发现与处理 |
| --- | --- |
| `browser-first` | 同步创建 AudioContext 超过 50 ms 事件预算，改为经授权复查的原生任务。 |
| `browser-async-1` | 首次 PeriodicWave 惰性创建超过 16 ms 帧预算，提前在原生初始化任务准备四种波形。 |
| `browser-async-2` | 外部驱动误用 OscillatorNode/GainNode 名称，按 DevTools 实际 Oscillator/Gain 观测。 |
| `browser-async-3` | 驱动一次 click 创建/停止 64 音符超过事件预算；改用四次固定间隔的真实 click，Python 明确断言总间隔小于一秒，同时验证 16 并发和 64/s 上限。 |
| `browser-async-4` | 导航重复反初始化 AudioContext 导致 Debug 检查失败，修正提前清理与 observer 注销。 |
| `browser-async-5` | headless 窗口无焦点时原生全屏策略拒绝；标准 JavaScript 对照同样失败。驱动先 bringToFront 并断言焦点，再发送可信输入。 |
| `browser-async-6` | 无 isolate 时 QuotaExceededError 未置 ExceptionState，增加同步缓存写后校验；缺失值与空串分别处理。 |
| `browser-async-7` | 音效按钮需要的 aria-pressed 不在属性白名单，新增该单一属性并补原生断言。 |
| `browser-async-8` | 原生滑块的 UA shadow root 被导出预检拒绝；最小页面复现后调整 UA 判断，保留 author shadow/template 拒绝。 |
| `metadata-before` / `metadata-after` | DOCTYPE 三字段和处理指令 target 的大字段先复现分配后拒绝，再验证预检拒绝。 |
| `final-1` / `mario-after-build-2` | 宽/窄屏打开学习地图超过事件预算；两个关闭按钮加 autofocus，独立 35 项与最终统一运行的对话框检查通过。 |
| `final-2` | 整关数值回放 frame 2340 超过 16 ms；其余 17 组通过，保留未全绿结果与待查原因。 |

首次适配测试在完整重编译前误将旧 17 项 exe 与新 DLL 混用，ABI 不一致导致崩溃；该次属于无效验证环境，保留 `adapter-initial`，完整构建后的 18 项重新验证。首次全屏对照脚本还遇到默认 GBK 解码失败；以显式 UTF-8 的第二次对照证据为准。这些记录不冒充产品测试结果。

## 本次续作与构建恢复

`build-async-18` 已编译并链接导出修复的 modules DLL，但 blink_unittests 链接发现旧 gaia_cookie_manager_service 对象的 CodeView 与重定位损坏。保存原件并定向重建后，`build-async-19` 又报告 layer_context.mojom 对象损坏；完整只读扫描覆盖 7,355 个测试输入，仅检出该对象的异常调试签名。其当前字节与早期 symbol scanner 备份不同，两份均保留，不能据此推断损坏来源。

恢复脚本首次核验备份哈希不匹配时，后续 shell 仍误启动 `build-async-20`，发现后已终止；该次不作为有效验证。之后显式核对退出码，另存当前坏对象，`build-object-22` 禁用该次编译的缓存复用并成功重建。没有用早期备份覆盖当前坏对象，也没有移除正常 Debug/PDB/GHASH 检查。

正常构建随后发现旧依赖日志中 2,399 项 missing-deps 与 4,500 项 stale-deps，故启动较长的标准五目标重编译 `build-async-23`，最终 7,013 步成功，耗时 4 小时 47 分 28.70 秒。`build-explain-21` 是只读 dry run，所报 6,954 个计划步骤不能算作实际构建通过。后续增量构建已恢复正常。

补充元数据回归后，modules 链接的 PDB 错误虽指向 initializer/bridge，4,650 个直接对象的只读扫描实际定位到另外五个旧生成对象：worklet_animation、before_install_prompt_event、periodic_sync_manager、bluetooth 和 canvas_pattern。其中三个同时有 GHASH/type 数量异常。保存原件与哈希后，仅对这五项设置本次缓存重建；`build-metadata-fix-27` 的标准五目标构建成功。合并恢复清单共 56 个已确认重建对象，均保留 before/after 哈希；不把合法 `.bss` 或初版 signed/unsigned 误判算入损坏数量，也不声称已找到损坏来源。详见[恢复清单](pyscript-mario-platform-validation/diagnostics/object-recovery.json)。

为提前复测，另复制并校验了独立浏览器快照。第一次缺少 Windows 版本程序集 manifest，SideBySide 日志确认后补齐；第二次在 DevTools 端口出现前超时。两次均执行零项功能检查，不能算作导出成功或产品回归失败，也不能替代完整构建后的验证。
