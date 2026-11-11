# Python Mario 阶段六验收证据

更新：2026-11-12。功能实现和标准五目标构建已完成；平台专项通过，统一验收尚未全绿。实现与接口见[阶段六记录](../pyscript-mario-phase6.md)。主体实现提交为 `2f5ffd4dc9`，前次阶段性记录为 `32e60f9e94`；本轮新增导出元数据预检、对话框 autofocus 和实际构建后的验证记录。

[最新统一汇总](final/summary.json)为 `passed=false`、`sourceUnchanged=true`，875 个功能输入在运行期间不变。18 组计划 456 项，实际执行 454 项：**453 项通过、1 项失败、2 项未执行**。220 项原生单测和 29 项宿主逻辑全部通过，浏览器通过 204 项。`unitTestsPassed=249` 已包含宿主逻辑，不能再重复加 29。详见[机器可读状态](validation-status.json)。

| 组 | 通过 / 计划 |
| --- | ---: |
| Runtime | 80 / 80 |
| 宿主 Mario 逻辑 | 29 / 29 |
| Chrome 文档适配 | 18 / 18 |
| 原生 browser bridge | 12 / 12 |
| 浏览器平台专项 | 15 / 15 |
| Canvas 单测 / 原生回归 | 19 / 19、2 / 2 |
| Blink 标签 / CSP / 设置单测 | 78 / 78、3 / 3、8 / 8 |
| 事件 / 输入 / Canvas 浏览器 | 34 / 34、25 / 25、23 / 23 |
| Mario 浏览器 | 32 / 35 |
| 动画 / DOM / 解析 / 设置浏览器 | 19 / 19、17 / 17、27 / 27、12 / 12 |

## 实际完成的验证

- [平台专项 15 项](final/browser-platform.json)：原生音频图及状态、真实激活、音符数量/速率上限、全屏、导航与致命停止清理、禁止存储时可玩、正常关闭。
- 导出保留 12 个 `<pyscript>` 及 nonce，没有可执行 JS；导出 HTML 通过已允许的 HTTP 来源重载后可以开始游戏。2200 最高分在正常关闭、重启后仍可读取。
- [导出预检修复前](diagnostics/metadata-before.log)四种大元数据字段均在 markup 分配后才拒绝；[修复后](diagnostics/metadata-after.log)在预检拒绝。新回归计入最终原生 12 项。
- [独立 Mario 35 项](probes/mario-autofocus-3.json)全部通过，包含宽窄屏、对话框初始焦点、15 条原版数值轨迹、导航/刷新和正常关闭。默认起点整关 2773 步、25 金币、5170 分、零死亡。这是数值回放，不能当作持续绘图全关或人工真实通关的证据。
- [静态检查](diagnostics/static-checks.json)11 组通过：C++/GN 格式、依赖、Node/Python 语法、生成页一致性及差异空白检查。本轮没有另跑 CPython 上游清单检查。

外部 CDP 仅用作测试驱动，页面没有 JavaScript 运行依赖。音频验证图结构和原生状态，没有扬声器听感结论。2200 分持久化用明确声明的终局计分夹具，不能当作真实通关所得分数。

## 尚未通过的统一用例

[最新 Mario 结果](final/mario.json)的前 32 项通过；第 33 项 `full-level-route` 在 frame 2340 触发 16 ms 单帧预算，随后导航/刷新、正常关闭两项未执行。其他 17 组通过。此前相同实现的独立 35 项成功不能替换这次失败，也不能与统一结果合并宣称 456 项通过。

该轨迹每帧只推进一个物理步，检查点观测和最终绘图另占回调，音频默认关闭，常规持续绘图未运行。当前日志不能确定那次物理/桥接成本与线程调度等待各占多少；没有证据证明间歇超限已根治，也没有提高预算或自动复活已停止的解释器。`stageComplete=false` 保持不变，后续需定位 Debug 间歇停止，再做 Release/GPU、长时游玩、声音听感与性能验收。

[第一次统一运行](first-unified/summary.json)曾在宽屏打开学习地图时超过 50 ms；[独立复查](probes/mario-after-build-2.json)曾在窄屏相同路径停止。原生旧版 dialog 聚焦扫描在找到 autofocus 前逐个检查后代；为两个关闭按钮声明 autofocus 后，独立 35 项和最新统一运行的宽窄屏检查通过。保持原有首个关闭按钮焦点，并新增初始焦点断言；showModal 仍同步计入原预算。

## 构建与恢复

[长构建](diagnostics/build-async-23.log)已成功，实际 7,013 步、4 小时 47 分 28.70 秒；旧依赖日志中的 2,399 项 missing-deps 和 4,500 项 stale-deps 已经正常重编译处理。[元数据修复后的标准五目标构建](diagnostics/build-metadata-fix-27.log)成功；最新统一运行的[构建日志](final/build.log)报告无工作可做。`build-explain-21` 的 6,954 个 dry-run 计划步骤不算实际构建结果，其摘要和原始哈希见[依赖诊断](diagnostics/build-dependency-explanation.json)。

保留 Debug、PDB、GHASH 和全部执行预算。旧生成对象的 COFF/CodeView/GHASH 与重定位损坏经扫描定位后定向重建，[恢复清单](diagnostics/object-recovery.json)共 56 个对象，均有 before/after 哈希。最后一次扫描覆盖 4,650 个 modules 直接对象，定位五个实际异常，见[调试段扫描](diagnostics/metadata-module-failure.rsp-debug-scan.json)、[哈希扫描](diagnostics/metadata-module-failure.rsp-hash-scan.json)和[恢复原件信息](diagnostics/metadata-object-recovery.json)。原始对象保存在本机 out 目录，不提交大体积二进制。

初版 symbol scanner 的 signed/unsigned 段号误判、合法 `.bss` 误判、旧 exe/新 DLL 混用、提前浏览器快照缺 SxS manifest/超时，以及首次全屏探测的 GBK 解码失败均独立保留，不作为有效产品用例。layer_context 当前坏对象与早期备份字节不同，来源未确定；不能由此声称定位到缓存损坏根因。

## 归档与边界

[归档清单](archive-manifest.json)记录原始和归档 SHA-256；文本规范化为 UTF-8/LF，PNG 保持原样。[最终完整性核验](final-audit.json)检查当前功能输入、全部本轮归档、文档链接、外部原件及 32 个 MCJE 文件，`integrityChecksPassed=true` 仅代表证据核验通过，统一验收仍为失败。归档和核验脚本一并保存在 diagnostics。

`final/` 是最近一次实际运行，含失败；`first-unified/` 保留首次统一失败；`probes/` 保留专项和独立复查；`diagnostics/` 保存构建、修复前后、扫描与静态检查。`progress/` 保留构建完成前的历史快照，其 `stageComplete=false`、`standardBuildComplete=false` 仅描述采集当时，不能代替本页的新状态。

已查看[平台页面](final/browser-platform.json.platform.png)、[宽屏对话框](final/mario.json.dialog.png)、[窄屏](final/mario.json.narrow.png)及[独立整关回放](probes/mario-autofocus-3.json.won.png)截图。它们用于布局核查，不是听感、实时性能或人工通关证明。

导出依赖本项目浏览器、启用的解释器和允许的 HTTP 来源，不打包外部依赖或保存运行栈、Canvas 像素和游戏进度。Renderer 沙箱保留，未开放进程级文件/网络/执行权限。headless 软件绘制不能代替 Release/GPU 和长期真实游玩。

外部 Mario 原件保持只读，SHA-256 为 `53b556316bbc25a5c2c80b046be836dbaaea8aa5c25e85a2cf5f29a4b4bc9ef1`。既有 MCJE 修改保持原样且不纳入本轮提交。
