# Python Canvas 阶段三验收证据

实现说明：[pyscript-canvas-phase3.md](../pyscript-canvas-phase3.md)。

`final/` 是同一次统一验证的完整结果：标准五目标构建成功，188 项单测、157 项真实浏览器测试全部通过；`summary.json` 记录 `passed=true`、`sourceUnchanged=true`、全部组的数量 / 状态以及源码和二进制 SHA-256。`sourceCommit` 是验证开始时的仓库基线，实际待提交功能源码由 `sourceSha256` 记录。

- [统一汇总](final/summary.json)、[Canvas 浏览器结果](final/canvas.json)、[Canvas 预检 / 预算 / 桥接单测](final/canvas-unit.json)。
- [最终样例截图](final/canvas.json.sample.png)：原生 Python 绘制分层背景、云层、像素角色和移动方块；页面不含可执行 JavaScript。
- [静态检查](diagnostics/static-checks.json)：C++ / GN 格式、9 组 checkdeps、JavaScript / Python 语法。
- [归档清单](archive-manifest.json)：每个文件的本机源路径、原始 SHA-256 与归档 SHA-256。文本统一为 UTF-8 / LF，去掉行尾空白和多余 EOF 换行；空日志仍为空，PNG 原样复制。测试汇总中的原始结果哈希与归档文件哈希不同时，以此清单区分。

`probes/` 保留开发中的失败和成功探测，不能替代最终结果：

- `canvas-probe.json`：首次公共初始化及绘图合在一个标签时触发 100 ms 单块保护。
- `canvas-probe2.json`：首次缓存复制放在动画回调中，触发 8 ms 帧保护。样例随后将缓存来源首次栅格化 / 复制移到初始化阶段。
- `canvas-probe3.json`：CDP 解冻后页面仍隐藏，恢复测试超时；驱动随后通过真实标签激活恢复可见性。
- `canvas-probe4.json`：Canvas 专项 23 项通过；最终统一组再次通过相同 23 项。
- `runtime-probe*`、`adapter-probe*`：中间单测探测。其中第一次适配探测仍使用旧的 12 项二进制，第二次及最终验收为新增测试后的 15 项。

`diagnostics/` 保留构建失败、修复与标准重链证据：

- `build-probe1.log` 至 `build-probe3.log`：新增代码接入时的编译诊断，均已修复。
- `build-probe4.log` 至 `build-probe11.log`、`*-scan.json`：旧对象的 COFF / CodeView 异常及后续链接诊断。
- [对象恢复记录](diagnostics/object-recovery.json)：24 份确认损坏对象的原始 / 重建 SHA-256，24 份均变化；另列一份未发现结构损坏、重编译内容相同的诊断对象，避免把错误信息指向的文件一概当作损坏文件。
- `scan_sections.py`、`scan_debug.py`、`scan_hashes.py`：当时使用的只读诊断辅助脚本，输入为本机输出目录的链接响应文件。实际损坏包括节名 / 字符串及 CodeView 数据。
- `animation-compile-command.txt`、`animation-direct-compile.log`、`recache-animation.log`：同一诊断对象经普通重编译、强制缓存刷新尝试和直接编译后保持相同哈希；后续在 metrics 输入中定位了真正损坏的数据。
- `adapter-unlimited-link*`、`blink-unlimited-link*`：未改变输入，仅将诊断错误条数设为无限后，链接反而通过，未复现原有测试可执行文件 PDB 类型错误。没有把这些诊断产物用作最终标准构建结果。
- [标准重链记录](diagnostics/standard-relink.json)、[标准重链日志](diagnostics/build-probe12.log)：删除这两个生成的 exe 后，使用原有 Debug / GHASH / PDB 参数重新执行标准五目标构建并通过。最终 `final/build.log` 再次确认标准构建成功，随后全部测试通过。

异常根因尚未确定。没有修改相关无关功能源码，没有关闭 Debug / PDB 检查，也没有提高 Python 执行时间额度。完整对象原件、响应文件与未归档的本机输出仍保留在 `out/Debug/pyscript-canvas-phase3/`，不将这些大型二进制提交到仓库。
