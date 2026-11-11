# Python Mario 阶段五验收证据

实现说明：[阶段五记录](../pyscript-mario-phase5.md)。本次完成控件、教学面板和页面交互；声音、持久化、全屏、导出及长期性能仍按后续阶段推进。

`final/` 来自 `out/Debug/pyscript-mario-phase5/final-2/` 同一次统一验收。标准五目标构建成功，202 项原生单测、21 项宿主逻辑和 192 项浏览器测试全部通过，共 **415 项**，16 组均为 `verified=true`。验证期间 866 个功能输入文件保持不变。

- [统一汇总](final/summary.json)：`passed=true`、`sourceUnchanged=true`、完整命令、配置、源码及二进制 SHA-256。`sourceCommit` 是运行开始时的提交基线，本次未提交实现通过工作树 `sourceSha256` 约束。
- [构建日志](final/build.log)：常规五目标构建成功，保留 Debug、PDB、GHASH 和正常链接诊断。
- [Runtime](final/runtime.json)：75 项；包含控件快照、精确类型/转换钩子拒绝、共享 DOM 预算、调用后的时间/撤权锁存及旧能力。
- [Chrome 适配](final/adapter.json)：17 项；新增 Unicode/NUL、布尔/数值负载及原生拒绝映射。
- [Blink](final/blink.json)：78 项；覆盖控件原生归一化、非法类型/属性/过期句柄、dialog/disabled，以及同步 UI 通知被重入门禁跳过时不错误增加输入代数。真正丢失输入时的旧复位保护仍有回归。
- [游戏逻辑](final/mario-logic.json)：21 项；15 条原版轨迹、时钟/随机/软额度，新增 0.45× 慢动作计时及八节课程语法检查。
- [Mario 浏览器](final/mario.json)：35 项；类型化控件、原生像素、实际可信键鼠/指针、按钮键盘长按/松开/焦点清理、参数/恢复默认、Tab、八课程/四流程、dialog/Esc/焦点、HUD/日志、390px 窄屏/790 逻辑宽度及 reduced motion、15 条轨迹、导航/刷新和退出码为零的正常关闭。
- 其他浏览器回归：事件 34、输入 25、Canvas 23、动画 19、DOM 17、解析 27、设置 12，合计 157；其他原生回归包括 Canvas 19+2、CSP 3、设置 8。
- [静态检查](diagnostics/static-checks.json)：C++/GN 格式、依赖、Node/Python 语法、生成页一致性、差异空白及 776 份 CPython 上游文件的 SHA-256。
- [归档清单](archive-manifest.json)：153 个输出的来源、原始与归档 SHA-256。文本转为 UTF-8/LF、去除行尾空白和多余 EOF 换行；不可解码的损坏链接符号按替换字符归档，原始日志仍保留在 out 目录，原始哈希可核对。PNG 原样复制。

已检查[准备画面](final/mario.json.ready.png)、[实际游玩](final/mario.json.playing.png)、[状态面板](final/mario.json.state.png)、[原生学习地图](final/mario.json.dialog.png)、[窄屏](final/mario.json.narrow.png)和[数值回放结束画面](final/mario.json.won.png)。截图由 Windows x64 Debug headless 浏览器生成，保留 Renderer 沙箱，使用软件绘制。CDP 只作外部输入、观测和截屏驱动，交付页面本身没有可执行 JavaScript。

整关回放仍使用默认起点与原版参数，只输入向右和跳跃，2773 个物理步、25 金币、5170 分、3 生命、零死亡；六个检查点按 `1e-7` 容差对照。每原生回调一步，观测、构建与提交分开，它验证嵌入式 Python 数值行为，不代表人工通关或全关实时绘图性能。

## 保留的探测与修正

`probes/` 保留历史失败，失败没有改写为成功：

| 记录 | 结果与处理 |
| --- | --- |
| `browser-probe1` | 原生 focus 同步产生的 focusin 被重入门禁跳过后错误增加输入代数，开始后自动暂停。修正 UI 通知分类，保留真实键盘/指针丢失时的复位。 |
| `browser-probe2` | 驱动误以为指针按钮 preventDefault 后会获得键盘焦点；改为实际 Tab 导航后检查键盘长按，没有改动原生指针语义。 |
| `browser-probe3` | 课程点击过快填满共享事件窗口，后续点击被按策略跳过。驱动加入固定 120 ms 点击间隔，不自动重试被丢弃的事件，不提高窗口额度。 |
| `browser-probe4` | 首次 showModal 的回调超过 50 ms。保留真实致命停止。 |
| `browser-probe5` | 在初始化 show/close 预热两个 dialog 超过文档累计 250 ms；该尝试已撤销，未增大额度。 |
| `browser-probe6` | 更新课程/Tab 后立即关闭、滚动、聚焦，引发 50 ms 事件停止。改为不可见的闭合 dialog 布局准备、按帧合并页面写入，并在后续独立帧滚动和聚焦。 |
| `browser-probe7` | 上述实现的独立 35 项专项全部通过。最终统一验收又通过同样用例并补存 dialog 截图。 |
| `runtime-probe1` | 旧断言把元素 keydown 当作非法类型。本阶段有意支持它，非法样本改为 wheel，并新增 keydown 成功断言。 |
| `blink-probe2-executed` | 新夹具给 close/cancel 复用了同一回调 ID，原生层正确拒绝；修正为每监听独立 ID，最终 78 项通过。保留执行结果与原始 JSON 哈希。 |
| `first-unified/` | 标准构建被旧测试对象 CodeView/GHASH 损坏阻断，未开始测试。恢复后在新目录运行最终统一验收。 |

## 构建产物恢复

本阶段多次链接遇到缓存对象的 COFF 节名/关联、重定位、字符串表、CodeView/GHASH 损坏，以及 thin archive 符号索引指向错误成员。扫描结果及全部 `build-probe*.log` 见 `diagnostics/`；[恢复清单](diagnostics/object-recovery.json)记录 **21 份对象和 1 份库**的原件位置、前后哈希，22 份生成产物重建后哈希均变化。

恢复前复制原件并核验哈希，只移除确认有问题的 `out/Debug/obj/` 生成产物，让原构建规则重建；没有修改无关 C++ 源码、关闭符号/GHASH 或忽略链接错误。原始对象/库保存在本机 `out/Debug/pyscript-mario-phase5/object-backups/`，不提交大二进制。归档的扫描器只读；错误索引和错误指向成员有独立证据。首次统一链接报错提到的 V8 测试本身正常，完整扫描定位到另外三份损坏类型流，未盲目重编报错中的正常文件。

这不是缓存损坏来源的根因分析。最终构建和测试已通过，但 Debug 墙钟额度依然会包含调度等待；单次 16 ms/50 ms 的间歇停止风险、Release/GPU 和长期游玩继续属于阶段七。Chrome 日志中的既有 GPU/服务及 `WebFrame LEAKED` 等诊断原样保留；正常关闭的退出码零不能证明整个 Chromium 无任何泄漏。

没有修改外部 Mario 原件；其 SHA-256 仍为 `53b556316bbc25a5c2c80b046be836dbaaea8aa5c25e85a2cf5f29a4b4bc9ef1`。已有 MCJE 工作树修改保留，不纳入本阶段提交。
