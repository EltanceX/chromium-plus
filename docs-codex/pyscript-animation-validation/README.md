# 动画帧阶段一验证证据

完整说明见[阶段一记录](../pyscript-animation-phase1.md)。本轮标准构建成功，138 项单测和 109 项真实 headless 浏览器测试通过；Renderer 沙箱保持启用。

- `final/summary.json`：2026-11-10 18:38:03–18:42:11（Asia/Shanghai）的统一验收；包含命令、实际项数、退出码、耗时、源码 / 二进制哈希及验证前后源码一致性。
- `final/`：十组实际测试结果、日志、设置与样例截图。单测 JSON 移除无关的全部测试名称列表，只保留实际运行位置与结果；原始及精简后哈希见统一汇总。
- `diagnostics/attempt3-build.log`：功能代码、设置与对象恢复后的完整标准构建；`final/build.log` 为最后线程回归测试补入后的标准增量构建。
- `diagnostics/attempt1-*`、`object-recovery-verified.json`：Core 对象异常、输入扫描、原始对象哈希及定向重建后的结构检查。对象二进制原件只保留在本机忽略目录，不写入仓库。
- `diagnostics/attempt2-build.log`：新增测试夹具 API 编译错误；已修正，后续标准构建及 62 项 Blink 测试通过。
- `diagnostics/runtime-probe1.json`：早期回滚测试把返回调用方后执行的临时对象析构误当原生引用释放。最终分别验证安装回滚与原生取消释放最后引用时的 DOM 撤权。
- `diagnostics/animation-probe1` 至 `probe3`：帧内密集桥接调用先触发 8 ms，初版测试只等待 128 次调用诊断；后续记录实际先触发的保护，没有改变生产限制。
- `diagnostics/animation-probe4`：同样的 8 ms 保护先于帧内批量 DOM 窗口用例完成；最终由 click 累计共享 DOM 用量，独立验证待执行帧在窗口恢复后的自动派发。
- `diagnostics/animation-probe5`：仅发送 CDP `active` 后页面仍隐藏，恢复等待超时；最终正确切回标签页恢复可见性。
- `diagnostics/animation-probe6`：独立动画帧 19 项首次完整通过；`final/animation.json` 是随后统一验收中的新一轮结果。
- `static-checks.json`：GN / checkdeps、C++ / Python 格式和脚本语法检查索引。
- `archive-manifest.json`：原始文件与归档文件的 SHA-256；文本归档统一 UTF-8、LF、无行末空白、单 EOF 换行，非法 UTF-8 字节转义。原始日志保留在 `out/Debug/pyscript-animation-phase1/`。

失败探测不计入最终通过数量，也未被后续成功结果覆盖。测试中的 CDP JavaScript 仅用于外部驱动和检查，示例页面本身没有 JavaScript 运行依赖。当前证据不替代 Release、其他平台、Canvas 或完整游戏性能验证。
