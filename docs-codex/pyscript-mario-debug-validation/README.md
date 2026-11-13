# Python Mario Debug 验收证据

本轮按用户要求只构建 `out/Debug`。排查过程与预算边界见[稳定性记录](../pyscript-mario-debug-validation.md)，原阶段六失败仍保存在[历史目录](../pyscript-mario-platform-validation/README.md)。

## 结果入口

- [最终统一结果](final/summary.json)：标准五目标 Debug 构建及 18 组、460 项验收全部通过，期间 875 个功能输入保持不变。
- [硬件 GPU 完整 Mario 与延长测试](diagnostics/soak-gpu-final.json)：36 项全部通过，其中延长交互 300,345 ms、259 轮输入、25 次冻结恢复、51 次采样；末尾导航/刷新和正常退出通过。
- [GPU 测试输入与二进制哈希](diagnostics/soak-gpu-final-inputs.json)：整个运行期间两者均不变。
- [全新 profile 冷启动复查](diagnostics/soak-gpu-cold-fixed-2.json)：过滤执行一项 60 秒交互，明确列出其余跳过用例，不作为全部 Mario 用例通过的证据。
- [最终审计](final-audit.json)、[状态](validation-status.json)及[归档清单](archive-manifest.json)：核对功能输入、二进制、原件、保留改动、文档链接和归档 SHA-256。

硬件后端通过 CDP `SystemInfo.getInfo` 记录，实际为 AMD Radeon Graphics、ANGLE D3D11，驱动 `31.0.21912.14`，Canvas 2D、合成与栅格加速启用，GPU 与 Renderer 保留沙箱。机器的系统原始报告见[配置](diagnostics/machine.json)；其中核心数量是系统报告值，不据此推断处理器物理拓扑。统一入口默认的软件绘制与这里的硬件 GPU 运行分别保存，重复覆盖不累加为新的独立测试数量。

延长测试使用外部 CDP 可信键鼠输入，无页面 JavaScript 运行依赖。采样保存原生帧推进、游戏/绘图测量及 DOM 计数；它不测量整个 Python 堆硬上限、不保证稳定帧率，也不替代数小时人工游玩或扬声器听感。

## 失败及诊断保留

| 记录 | 结果与用途 |
| --- | --- |
| `diagnostic-mario` | 带墙钟/CPU/检查间隔诊断的 Mario 专项。 |
| `profile-before` / `profile-after` | 同一 2773 步整关轨迹，物理 update 单次性能对照，平均约 2.208 → 1.424 ms；不是完整绘图或 FPS 测试。 |
| `pause-before` / `pause-renderer-before` | 最初两次未命中 Python Renderer 主线程的无效暂停探测，保留为诊断。 |
| `pause-active-before` | 命中实际 Renderer 后注入 20 ms 暂停，复现 16 ms 保护停止；证明墙钟保护仍生效。 |
| `first-unified/` | 加入行追踪后、GPU 入口修复和最终驱动改动前的 458 项成功，保留为中间结果。 |
| `soak-gpu-default` | 最初延长测试在恢复后点击继续存在竞态，不能记为通过。 |
| `soak-settle-probe` | 冷启动 GPU 原生依赖约 138 ms，触发 Python 初始化 100 ms 限制。 |
| `gpu-before` / `gpu-after` | 新增两项 Blink 原生 GPU 准备回归，修复前均失败、修复后均通过。 |
| `soak-gpu-cold-fixed` | GPU 冷启动修复后能进入游戏，但驱动仅解冻未恢复可见状态，等待帧推进超时。 |
| `soak-gpu-cold-fixed-2` | 增加原生标签页可见切换与就绪断言，60 秒冷启动/恢复验证通过。 |
| `build-gpu-*`、`gpu-core-*` | 本轮四份旧 COFF/CodeView 对象的定位、初次漏检、修正扫描及定向重编过程。 |

上述名称对应 `diagnostics/` 下的 JSON、日志和可用截图。初次扫描漏检了字符串表末尾终止符，后续按 LLVM 初始化逻辑修正；未安装的 `llvm-readobj` 尝试不计作通过。四份坏对象的本地原件仍保存在 `out/Debug/pyscript-mario-debug-stability/object-backups/`，二进制原件不纳入 Git；[恢复清单](diagnostics/recovered-objects.json)包含原件、备份及重编后哈希。没有关闭 Debug、PDB、GHASH 或放宽预算。

## 复现

```powershell
python chrome/test/run_pyscript_event_validation.py --output-dir out/Debug/pyscript-validation-new
node chrome/test/pyscript_mario_smoke_test.mjs --graphics=default --soak-seconds=300 --output=out/Debug/pyscript-gpu-new.json
```

使用新的输出目录/文件保留每次运行。`--case-filter=文本` 只用于定向排查，报告记录实际执行和跳过数量；默认完整 Mario 为 35 项，延长测试增加一项。原版项目 `C:/VD/Projects/mario/index.html` 保持只读，原件哈希及既有 MCJE/页面改动都需通过审计。

`diagnostics/` 保留排查脚本、原始日志的可审阅文本副本及最终驱动。文本转为 UTF-8/LF、移除行末空白并转义控制字符，PNG 原样保存；清单分别记录原始与归档哈希。Launcher 的无关全量测试名称清单被精简，执行结果和位置保留，原始 JSON 哈希另行记录。Release 按用户要求暂缓，音频已验证原生图及状态，未声称完成主观听感验证。
