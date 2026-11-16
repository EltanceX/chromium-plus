# Windows x64 Release 首次构建与产物检查

日期：2026-11-16（Asia/Shanghai）。源码基线为 `206918ff3c7a6aa0fb1738afa1975d7885d0a832`，另加本次运行时链接及输入测试修复。原始测试记录保留实际执行时的未提交状态；对应补丁和哈希见[源码补丁](release-validation-2026-11-16/source-changes.patch)及[最终评估](release-validation-2026-11-16/final-assessment.json)。

**构建成功，浏览器及主要定制功能能够运行。默认独立网络服务下的本机 MCBE UDP 查询仍超时，因此完整默认模式验收尚未全部通过。** 公网 Hive MCBE 查询、MCJE、Python 与 Mario 的以下检查已通过。

## 构建与产物

构建命令为 `autoninja -C out/Release -j12 chrome`，实际使用 12 个本地 job、单链接并发。首次完整构建于 2026-11-15 23:50:20 开始，2026-11-16 12:01:36 完成，耗时约 12 小时 11 分，58,921 steps，退出码为 0。修复后的增量构建耗时约 2 分 52 秒，13 steps，再次成功。

配置见[归档 args.gn](release-validation-2026-11-16/args.gn)：

```gn
target_cpu = "x64"
is_debug = false
is_component_build = false
is_official_build = false
dcheck_always_on = false
symbol_level = 0
blink_symbol_level = 0
v8_symbol_level = 0
use_thin_lto = false
chrome_pgo_phase = 0
concurrent_links = 1
cc_wrapper = "sccache"
```

这是非组件开发 Release，尚未构建安装包，未启用 PGO 或 ThinLTO。旧 `out/Debug` 已移除；必要 Debug 资料此前备份到 `C:\src\build-backups\chromium-plus-debug-20261115-233231`，本轮未重建 Debug。

运行入口为 `C:\src\src\out\Release\chrome.exe`。应保留正常的输出目录及资源依赖，不能只复制一个 EXE。

| 检查 | 结果 |
| --- | --- |
| EXE / 主 DLL 资源版本 | `158.0.8126.0` |
| `chrome.exe`、`chrome.dll`、`chrome_elf.dll` | PE 签名有效、machine `0x8664`（x64） |
| ICU、资源包、V8 snapshot、英文及简体中文语言包 | 存在，已记录 SHA-256 |
| `chrome.dll` 对 Python 的链接 | CPython 已静态链入；PE 导入表不再依赖 `python_runtime.dll` |
| 主要二进制与 GN 配置 | 最终检查与验证时哈希一致 |
| 输出目录逻辑大小 / C 盘可用空间 | 检查时约 9.0 GiB / 213 GiB；大小包含构建和本轮诊断资料 |

产物大小、哈希和磁盘快照见[artifacts.json](release-validation-2026-11-16/artifacts.json)，PE 依赖见[chrome-dependents.txt](release-validation-2026-11-16/chrome-dependents.txt)。

## Release 启动和切页崩溃修复

最初的完整构建虽然成功，默认 Renderer 启动却反复报 `0x241`（`ERROR_INVALID_IMAGE_HASH`）。Windows CodeIntegrity 3033 事件指向未满足 Microsoft 签名等级的 `python_runtime.dll`，见[原始系统事件](release-validation-2026-11-16/code-integrity-events.json)。

先按现有 `AllowExtraDll` 机制尝试产品 DLL 的精确路径许可后，默认页面能够启动，Python 首个页面也能够执行；但下一次导航在文档上下文销毁时触发 `PartitionRoot::FreeInUnknownRoot` 访问异常，见[失败日志](release-validation-2026-11-16/dll-exception-parser-failure.chrome.log)。

根因是运行时被强制声明为独立 `shared_library`，其 C++ 接口却跨边界传递 `unique_ptr`、字符串等对象。非组件 Windows 构建中，独立 DLL 与调用方的 CRT / Chromium 分配路径不能据此安全混用。仅增加加载许可没有解决对象释放问题。

最终修改：

1. [BUILD.gn](../components/python_runtime/BUILD.gn) 使用 Chromium 的 `component("python_runtime")`，保留组件构建中的 DLL 名称；非组件构建使用静态库，将 CPython 与调用方链入同一主 DLL。
2. [导出声明](../components/python_runtime/python_runtime.h) 跟随 `COMPONENT_BUILD`：组件构建使用导入/导出声明，非组件构建不再声明 DLL 导入。
3. 从非组件安装文件清单中移除独立 Python DLL；本轮没有验证完整安装器。
4. 撤回诊断时添加的 `AllowExtraDll` 例外；最终 BrowserClient 没有这项改动。

重建后检查导入表，并将旧 `python_runtime.dll` 移到本机失败诊断目录保存。全部后续成功检查均在 Release 根目录没有该 DLL 的条件下运行。

没有关闭 CIG、Renderer 沙箱或网页安全检查。运行时自检在 6 个 Renderer 中均报告受限 token、完整性级别不高于 Low，以及上下文隔离、异常、截断和清理通过。另通过 `GetProcessMitigationPolicy(ProcessSignaturePolicy)` 只读抽查 13 个 Renderer，全部返回 flags `5`，`MicrosoftSignedOnly` 保持开启。证据分别为[运行时自检](release-validation-2026-11-16/runtime-self-test.json)和[CIG 抽查](release-validation-2026-11-16/renderer-signature-policy.json)。

## 浏览器验证

所有命令显式指定 `out/Release/chrome.exe`，使用独立临时 Profile。原生单测可执行文件没有在本轮构建或执行，不能将下面结果称为重跑历史 460 项统一验收。

| 组 | 结果 | 说明 |
| --- | --- | --- |
| `chromiumPlus` | 15 / 15 | 页面、iframe 和 Worker |
| Python 解析 / 策略 | 27 / 27 | 含连续导航及上下文清理 |
| Python 设置 | 12 / 12 | 启用、来源、页面限制及重启 |
| DOM | 17 / 17 | 文档句柄与桥接 |
| 点击事件 | 34 / 34 | 注册、派发、预算、导航与清理 |
| 动画帧 | 19 / 19 | 调度、取消与预算 |
| 输入 | 25 / 25 | 键盘、指针捕获、页面状态 |
| Canvas | 23 / 23 | 绘图、资源及批量提交 |
| 浏览器服务 | 15 / 15 | 音频、存储、全屏、导出 |
| Mario 浏览器 | 35 / 35 | 控件、教学、原版对照及生命周期 |
| Mario 宿主逻辑 | 29 / 29 | 物理与玩法逻辑 |
| MCJE 本地模拟服务器 | 33 / 33 | SLP、PNG 附件、失败、取消与历史 |
| MCBE 公网 Hive，默认模式 | 1 / 1 | 实际收到并展示服务器状态 |
| MCBE 本地 UDP，默认模式 | 首项失败，后续未执行 | `127.0.0.1` 假服务器查询超时 |
| MCBE 本地 UDP，进程内网络服务诊断模式 | 17 / 17 | 只作定位，不能代替默认模式验收 |
| Mario 硬件图形，隔离复测 | 36 / 36 | 单列重复覆盖；约 60 秒交互 |

标准通过部分合计 **284 项**，其中 Python 浏览器 207 项、字段/MCJE 浏览器 48 项、宿主逻辑 29 项；再单列 1 项公网 MCBE 检查。诊断模式的 MCBE 17 项、硬件模式 36 项和多个 Renderer 的固定自检不加入该独立覆盖计数。

输入组最初在“显式释放 capture”处超时。事件探测表明 Python 与 JS 在 pointerdown 中都能取得 capture；CDP 后续拖动只传 `buttons: 1`、未指定当前 `button`，会丢失待处理的原生鼠标捕获状态。修正[测试驱动](../chrome/test/pyscript_input_smoke_test.mjs)，使按下掩码与当前按钮一致后，原有 25 项检查全部通过。产品的指针捕获实现没有改动；中途尝试的状态等待方案未解决问题，已撤回，失败记录原样保留。

硬件复测使用 AMD Radeon (TM) Graphics、ANGLE D3D11。60,362 ms 的交互完成 55 个循环及 5 次冻结/恢复，页面继续绘制、输入和正常退出。首轮在与其他浏览器诊断重叠时出现一次 pause 点击未命中；随后隔离重跑 36 项通过，原失败保留在 `mario-hardware.json`。复测通过不能单独证明首轮失败的全部原因，也不能替代数小时稳定性验证。

已查看本轮生成的 Mario 游戏画面和 Python 设置截图，中文、主要控件及 Canvas 画面正常。截图见[游戏](release-validation-2026-11-16/mario-hardware-isolated.json.playing.png)和[设置](release-validation-2026-11-16/settings.json.settings.png)。

## 剩余问题：Windows 本机 MCBE UDP

默认独立 Network Service 在向 `127.0.0.1` 假服务器查询时超时，假服务器没有收到 Ping。宿主 Node 的 IPv4 / IPv6 UDP 回环自检成功；使用 `--enable-features=NetworkServiceInProcess2` 作诊断时，同一浏览器的本地 MCBE 17 项通过。默认模式的 Hive 公网查询也成功，实际响应端点为当次网络环境中的 `198.18.2.72:19132`；不能把它当成固定公网地址。

现有 MCBE 使用 `NetworkContext::CreateUDPSocket`，其实现直接打开 UDP socket。Chromium 的 [BrokerHelperWin](../services/network/broker_helper_win.cc) 明确说明 Windows AppContainer 的回环防火墙限制，并为需要的目的地址提供 broker 判断；TCP 路径已有 broker 支持。这与本次本机 UDP 失败相符。

建议下一项修复为：让 Windows 的 connected UDP 创建接入既有 `SocketBrokerClient::CreateUdpSocket`，在 Network Service 中接管 broker 创建的 socket，再执行连接及收发。需覆盖 broker 创建失败、超时期间的迟到回调、导航取消、IPv4 / IPv6，并保留 MCBE 的 5 秒总预算和最多两次尝试。产品不应通过关闭 Network Service 沙箱、加入系统回环豁免或强制进程内网络服务来绕开问题。本轮尚未实施该网络层修复。

## 复查与证据

```powershell
autoninja -C out/Release -j12 chrome
gn check out/Release //components/python_runtime:python_runtime
node chrome/test/pyscript_input_smoke_test.mjs --chrome-binary=out/Release/chrome.exe --output=out/Release/input-recheck.json
node chrome/test/mcbe_smoke_test.mjs --chrome-binary=out/Release/chrome.exe --output=out/Release/mcbe-default-recheck.json
node chrome/test/pyscript_mario_smoke_test.mjs --chrome-binary=out/Release/chrome.exe --output=out/Release/mario-hardware-recheck.json --graphics=default --soak-seconds=60
```

多个涉及焦点或原生输入的浏览器检查应顺序运行。上面的默认 MCBE 本地检查在修复前预计仍失败。

[最终评估](release-validation-2026-11-16/final-assessment.json)保留 `defaultModeFullAcceptancePassed: false`；[首次完整编排结果](release-validation-2026-11-16/summary.json)同样保持原来的失败状态。逐组通过结果、失败样本、构建日志、脚本与哈希清单均在[归档目录](release-validation-2026-11-16/manifest.json)。本机额外诊断资料在 `out/Release/validation/`，构建二进制、临时 Profile 与旧 DLL 不加入 Git。
