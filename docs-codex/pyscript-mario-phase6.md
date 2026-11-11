# 阶段六：合成音效、最高分、全屏与导出

日期：2026-11-11。原项目 `C:/VD/Projects/mario/index.html` 保持只读。

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

音频每文档一个 AudioContext，最多 16 个存活/未来音符，每次 1–16 个，每秒最多调度 64 个。波形限 sine/square/triangle/sawtooth，频率 20–20000 Hz，时长 0.01–2 秒，延迟 0–4 秒，音量 0–0.25，拒绝 NaN/Inf。统一乘 0.25 输出系数，使 16 个最大音量音符之和不超过 1；短起音和指数衰减避免硬切换。音符在原生音频时钟调度，音频线程不进入 CPython。停止/重开断开旧节点；隐藏、失焦等原生输入重置即静音，即使 Python 事件被窗口额度跳过；导航、关闭、撤权后的致命停止和 GC 清理释放上下文。

存储 key 最多 256 UTF-8 字节、value 最多 16 KiB，保留 StorageArea 的来源、用户设置、配额与隐私语义。Mario 沿用 `marioLabBest` 键，初始化读取、通关提交最高分，缺失/无效数据为零，负值截为零，上限 999999999；保存失败保留本局成绩与玩法。

配额回归发现启用 QuotaExceededErrorUpdate 时，原生异常辅助函数在缺少 V8 isolate 时直接返回，不会置 ExceptionState。桥接写入后因此还读取同一同步缓存校验目标值，将被拒绝的写入映射为 PermissionError；不为存储创建 V8 realm，也不把缓存可见等同于磁盘落盘。测试先明确验证 JavaScript 填充触发 QuotaExceededError，再验证 Python 拒绝与后续可用性。

导出只接受简单 ASCII `.html` 文件名，不接收路径，要求真实用户激活，每文档最多 8 次、间隔至少 2 秒。序列化前限制 4096 节点、128 层深度、128 Ki UTF-16 源单位，包含属性与内部 nonce；序列化结果最多 1 MiB UTF-8。不支持 template 与页面自建 shadow 内容；原生控件的 UA shadow tree 不参与序列化，允许导出其 light DOM 与属性。复用 MarkupAccumulator，恢复被解析器隐藏的 nonce，去掉 dialog 的 open 状态；复用 Blob/PublicURLManager 和 AnchorElementUtils 的正常 sandbox/navigation/download 策略，每文档最多持有一个导出 URL。Mario 启动时重置 tab、音效状态和覆盖层；导出不嵌套存入整页字符串。

真实浏览器曾发现 range 控件的 UA shadow root 被预检误拒绝，下载未产生。含单个滑块的最小页面已复现相同 PermissionError；修复只排除 UA shadow root，仍拒绝页面自建的 open/closed shadow 与 template，并补充原生回归。

这是对自包含 Mario 页面的源码导出，不打包外链资源，也不保存 Python 运行栈、Canvas 像素或游戏进度。下载文件仍依赖本项目浏览器、启用的解释器与允许的 HTTP 来源；建议将文件放回本地 HTTP 服务后打开，不能绕过来源配置。

## 页面迁移

新增 `mario/platform.py`，页面由 12 个有序内联 `<pyscript>` 组成。八类音效 jump/coin/bump/stomp/hurt/checkpoint/start/win 保留原版波形和音符参数，默认静音，音效按钮与 M 快捷键开启。声音事件队列最多 8 条，提交按 16 个音符截分；输出繁忙时丢弃效果，不影响物理或计分。最高分读写、全屏按钮、导出按钮、课程、提示及 README 同步更新。

平台服务每帧最多处理一个分支，在已有 3 ms 软检查之后执行；UI 仍使用原有合并队列。暂停/页面恢复对目标音频状态进行协调，旧 resume 完成后不会永久覆盖后来的暂停请求。失败可重试，音频和存储失败不停止游玩。普通事件 50 ms、帧 16 ms、初始化 100/250 ms 和全部既有窗口/绘图/沙箱限制保持不变。

## 验证与记录

阶段六验收正在执行；最终结果、构建故障及限制在完成后补齐。标准目标仍为 chrome、blink_unittests、python_runtime_unittests、python_document_context_unittests、native_python_settings_unittests。

新增运行时 typed payload/参数/预算/撤权/拒绝测试，Chrome 适配测试，Blink 原生拒绝/资源/销毁测试，宿主侧音效、最高分和异步状态协调测试，以及 `pyscript_browser_smoke_test.mjs` 的真实浏览器测试。浏览器脚本仅为外部 CDP 驱动，不构成页面 JavaScript 依赖；音频图和运行状态验证不能替代扬声器听感验收。持久化用明确声明的终局计分夹具，核心通关仍由既有原版轨迹对照覆盖。


## 探测中修正的问题

历史失败保留在本阶段 `out/Debug/pyscript-mario-phase6/`，不会用成功结果覆盖：

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

首次适配测试在完整重编译前误将旧 17 项 exe 与新 DLL 混用，ABI 不一致导致崩溃；该次属于无效验证环境，保留 `adapter-initial`，完整构建后的 18 项重新验证。首次全屏对照脚本还遇到默认 GBK 解码失败；以显式 UTF-8 的第二次对照证据为准。这些记录不冒充产品测试结果。
