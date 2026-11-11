# Python Mario 阶段六验收证据

实现说明：[阶段六记录](../pyscript-mario-phase6.md)。实现提交为 `2f5ffd4dc9`。当前标准五目标构建仍在进行，阶段六尚未标记验收完成。

本阶段实际通过过 Runtime 80 项、Chrome 适配 18 项和原生 browser bridge 10 项；最新导出回归新增一项，原生组最终应为 11 项，仍待重跑。浏览器专项已有前 10 项通过，导出在第 11 项失败，其后 4 项未执行。该失败已定位为原生 range 控件 UA shadow root 被误拒绝，修复代码已经提交，尚待新浏览器复测。

完整统一验收预期为 18 组、455 项：219 项原生单测、29 项宿主逻辑、207 项浏览器检查。预期数量不等于通过数量；最终应以新运行目录的 `summary.json` 为准。

当前构建日志在本机 `out/Debug/pyscript-mario-phase6/build-async-23.log`。旧依赖日志存在 2,399 项 missing-deps 与 4,500 项 stale-deps；正常重编译正在修复这些记录。只读 dry run 的 6,954 个计划步骤不是实际执行结果，不作为构建成功证据。

构建期间还定位了旧生成对象的 COFF/CodeView/GHASH 与重定位损坏。原件、哈希、失败日志及只读扫描器保存在本阶段 out 目录；没有关闭 Debug、PDB 或 GHASH。最新恢复对象的汇总须在重建完成后重新生成，不能把所有备份都计为损坏对象。

首次 symbol scanner 的 signed/unsigned 段号误判、合法 `.bss` 误判、旧 exe/新 DLL 混用，以及首次全屏探测的 GBK 解码失败均保留独立记录。它们不作为产品失败或成功证据。

音频目前验证图结构和原生状态，没有扬声器听感结论。2200 分存储测试使用明确声明的终局计分夹具；默认起点整关仍由既有原版数值轨迹对照。headless 软件绘制不能替代阶段七的 Release/GPU 和长期真实游玩。

外部 Mario 原件保持只读，既有 MCJE 修改保留在工作树中。

## 当前归档

`progress/` 是本次续作的初步证据，保持 `stageComplete=false`、`standardBuildComplete=false`，不作为最终验收：

- [归档清单](progress/archive-manifest.json)：采集时间、原始与归档 SHA-256；活动构建日志明确标记为部分快照。文本规范化为 UTF-8/LF，PNG 保持原样。
- [Runtime 80 项](progress/runtime-strict.json)、[Chrome 适配 18 项](progress/adapter-built.json)、[先前原生 10 项](progress/blink-browser-async.log)。
- [浏览器前 10 项通过、第 11 项失败](progress/browser-async-8.json)与[滑块最小复现](progress/export-before.json)。它们均是导出修复前的结果。
- [最新完整输入扫描](progress/blink_unittests.exe.rsp-all-debug-scan.json)、[新增坏对象备份](progress/export-object-recovery.json)、[依赖日志诊断](progress/build-dependency-explanation.json)。
- [静态检查](progress/static-checks.json)：11 组格式、依赖、语法、生成页及差异检查通过，记录外部原件 SHA-256。
- [快照首次启动错误](progress/browser-snapshot-9.json)、[第二次启动超时](progress/browser-snapshot-10.json)及 [SxS 清单修正](progress/browser-snapshot-manifest-2.json)，均不含已执行功能用例。

已查看[先前页面截图](progress/browser-async-8.json.platform.png)：准备状态与夹具保存的 2200 最高分正常显示；它不是实际通关或听感证明。完整验收仍需在构建完成后执行导出/重载、正常重启持久化、致命停止资源清理、禁止存储下可玩及正常关闭，并运行 18 组统一验收。
