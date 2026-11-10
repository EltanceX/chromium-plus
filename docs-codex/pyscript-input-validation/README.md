# 输入事件阶段二验证证据

完整接口与验收说明见[阶段二记录](../pyscript-input-phase2.md)。本轮标准五目标构建、158 项单测、134 项真实 headless 浏览器测试全部通过，Renderer 沙箱保持启用。

- `final/summary.json`：2026-11-10 21:50:49–21:56:27（Asia/Shanghai）的统一验收，包含实际命令、项数、退出码、耗时、源码 / 二进制哈希及源码一致性检查。
- `final/`：十一组结果、日志和截图；单测 JSON 保留实际运行的测试与位置，原始结果和精简结果哈希见汇总。输入示例页面本身不依赖 JavaScript。
- `diagnostics/final-static-checks.json`：最终 C++ 格式、六组 checkdeps 和脚本语法检查的命令与退出码。
- `diagnostics/final/`：首次统一验收，其中输入 25 项通过，但旧 click 对不允许元素的注册错误类型变化导致失败。修正生产注册入口并增加 Blink 回归后，最后统一验收全部通过。
- `diagnostics/input-probe.json`、`input-probe2.json`：早期初始化触发 100 ms 限制；`input-probe3.json` 是后续独立 25 项通过结果，最终证据仍以 `final/input.json` 为准。未放宽生产预算。
- `diagnostics/pyscript-input-build-probe.log`、`build-attempt3.log` 等：早期 API 编译错误与对象异常，保留实际失败过程。
- `diagnostics/final2/` 至 `final6/`：错误类型修正后的构建恢复尝试，因其他已有对象结构异常而中止，不计入通过数量。
- `diagnostics/attempt*-object-recovery.json`、`attempt*-*scan.json`、`final-object-recovery.json`：18 份异常对象的诊断、原始哈希、重复读取 / mmap 检查及最终重建哈希。完整对象原件、响应文件与原始输出仍在本机 `out/Debug/pyscript-input-phase2/`，不把二进制对象提交到仓库。
- `archive-manifest.json`：每份原始文件与归档文件的 SHA-256。归档文本统一 UTF-8、LF、无行末空白和单 EOF 换行，非法 UTF-8 与控制字节转义；诊断文件名保留原始尝试编号。`final/` 对应本机的 `final7/`。

失败探测未被成功结果覆盖。对象损坏的根因尚未确定；本轮成功构建保留 Debug、PDB 与链接检查。本证据不替代 Release、其他平台、长期运行、Canvas 或完整马里奥迁移验证。
