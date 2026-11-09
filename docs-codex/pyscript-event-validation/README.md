# Python 点击事件首版验收证据

日期：2026-11-10（Asia/Shanghai）。实施与验收对应关系见[阶段五记录](../pyscript-event-phase5-build-and-browser-validation.md)。

- [validation-summary.json](validation-summary.json)：最终通过计数与探测记录索引。
- [final/summary.json](final/summary.json)：本轮标准构建、九组验收的完整命令、退出码、耗时、源码和二进制哈希。
- `final/`：各组结构化结果、启动器日志、浏览器日志、示例与设置截图；最终为 117 项单测和 90 项浏览器检查通过。
- [archive-manifest.json](archive-manifest.json)：45 份运行证据的来源、原始与归档 SHA-256、大小和日志文本规范化说明。
- [static-checks.json](static-checks.json)：测试脚本、文档格式及归档一致性检查。

早期探测属于测试脚本接入诊断，均保留失败状态，不计入最终通过数：

| 探测 | 当时结果 | 证据 |
| --- | --- | --- |
| 1 | 未发现 prerender target，超时 | [结果](diagnostics/probe-1/events.json) |
| 2 | 旧式 page 会话阻止 prerender | [结果](diagnostics/probe-2/events.json) |
| 3 | 显式允许仍受旧式 page 会话阻止 | [结果](diagnostics/probe-3/events.json) |
| 4 | 已收到 prerender Ready，但目标枚举等待超时 | [结果](diagnostics/probe-4/events.json) |
| 5 | 激活前后文本检查通过，发起会话未交付 Success 通知 | [结果](diagnostics/probe-5/events.json) |

各探测目录同时保存对应 Chrome 日志。最终脚本使用 tab-aware CDP 连接、target 更新事件和 Navigation Timing 验证真实预渲染与激活，完整验收的 34 项事件检查全部通过。
