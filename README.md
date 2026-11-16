# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium Plus

基于 Chromium 的浏览器定制项目，增加内核调试设置、Minecraft 服务器状态协议，以及在 Renderer 中原生执行 Python 的实验能力。

当前源码版本为 **Chromium 158.0.8126.0**；已完成 Windows x64 Debug component 和非组件开发 Release 构建。Release 的 Python、Mario、MCJE 及公网 MCBE 检查通过；默认独立网络服务下的本机 MCBE UDP 查询仍超时，详见 [Release 检查记录](docs-codex/release-validation-2026-11-16.md)。其他平台的定制功能尚未验证。

## 定制功能

| 功能 | 当前能力 | 详细文档 |
| --- | --- | --- |
| 内核调试 | `chrome://settings/kernelDebug` 展示版本、进程、内存、运行时间、窗口和标签页信息，支持查看标签页详情及关闭标签页 | [定制改动总览](docs-codex/custom-changes.md#内核调试与解释器设置) |
| 浏览器标识 | 页面、iframe 主世界及 Dedicated / Shared / Service Worker 默认提供 `globalThis.chromiumPlus = "0.0.1"` | [字段实现](docs-codex/globalthis-chromium-plus-implementation.md) |
| Bedrock 状态 | `mcbe://host[:port]` 通过原生 UDP 查询 Minecraft Bedrock MOTD，并生成本地结果页 | [MCBE](docs-codex/mcbe-native-protocol-implementation.md) |
| Java 状态 | `mcje://host[:port]` 通过 DNS SRV / TCP SLP 查询 Java 服务器，显示 MOTD、版本、人数、延迟和 PNG 图标 | [MCJE 与 Windows SRV 修复](docs-codex/mcje-native-protocol-implementation.md) |
| 原生 Python | `<pyscript>` 内联代码、文档独立上下文、受限 DOM、点击/键盘/指针事件、动画帧、Canvas 2D 子集及受控浏览器服务 | [Python 能力与边界](docs-codex/custom-changes.md#原生-python-与页面交互) |
| Python Mario | HTML/CSS 页面，由 Python 驱动物理、绘图、输入、教学控件、合成音效、最高分、全屏和源码导出 | [游戏源码与使用说明](chrome/test/data/pyscript/mario/README.md) |

[完整改动说明、37 次定制提交索引与新设备恢复步骤](docs-codex/custom-changes.md)以 `95bfed38d8` 为功能整理截止点；后续文档提交不计入该历史快照。

## 体验示例

完成构建后，在本项目浏览器地址栏打开：

```text
chrome://settings/kernelDebug
mcbe://geo.hivebedrock.network
mcje://mc.hypixel.net
```

MCBE / MCJE 是浏览器内部导航协议，用于查询服务器状态；公网服务器的在线情况和响应数据会变化。它们不提供游戏登录或加入服务器功能。

网页 Python 默认关闭。在“内核调试”中启用解释器，将受限模式来源设为 `http://127.0.0.1:8000`，保存并重启浏览器。从仓库根目录运行：

```powershell
python -m http.server 8000 --bind 127.0.0.1
```

然后访问 `http://127.0.0.1:8000/chrome/test/data/pyscript/mario.html`。最小 Python 页面示例：

```html
<!doctype html>
<meta charset="utf-8">
<p id="result"></p>
<pyscript>
from dom import get_element_by_id, set_text
set_text(get_element_by_id("result"), f"Python 计算结果：{6 * 7}")
</pyscript>
```

Python 在 Renderer 主线程同步执行，沿用 Renderer 沙箱，并受执行时间、输出、DOM 和绘图额度限制。“放开站点与页面执行限制”会覆盖 Python 的部分页面执行策略，保留 Renderer 沙箱，详见[设置与页面权限](docs-codex/pyscript-settings-and-page-permissions.md)。当前提供有限内建/冻结模块和原生桥接；不支持完整标准库、pip、任意本机模块或浏览器主进程的系统权限。

## 获取源码、构建与恢复开发

本项目仓库为 [EltanceX/chromium-plus](https://github.com/EltanceX/chromium-plus)。获取定制分支后，还需使用 **depot_tools / gclient** 按 DEPS 下载 Chromium 依赖，生成 GN 配置并编译。仅克隆主仓库不足以直接运行浏览器。

- [新设备恢复步骤与备份范围](docs-codex/custom-changes.md#新设备恢复开发)
- [Windows 工具链及系统要求](docs/windows_build_instructions.md)
- [Chromium 源码与依赖说明](docs/get_the_code.md)

## 验证记录

最近一轮 Python / Mario 标准五目标 Debug 构建和统一验收 **460 项通过**；独立硬件 GPU 验证 **36 项通过**，包含约 5 分钟交互。MCJE Windows SRV 修复后 **103 项单测通过**，另有服务器查询及既有功能回归。不同阶段和重复覆盖不累加为新的独立用例数。

- [Debug 稳定性、验证条件与限制](docs-codex/pyscript-mario-debug-validation.md)
- [Python / Mario 最终结果和归档证据](docs-codex/pyscript-mario-debug-validation/README.md)
- [MCJE SRV 修复结果](docs-codex/mcje-validation/srv-fix-results-2026-11-07.json)
- [首次 Release 构建、启动修复与产物检查](docs-codex/release-validation-2026-11-16.md)：284 项标准检查、公网 MCBE 查询及隔离硬件图形 36 项通过；本机 MCBE UDP 默认模式仍待修复。

Debug 数据为已归档的历史实测；本次 Release 未重跑原生单测，也未构建安装包。不同配置和重复覆盖不合并计数。

## Chromium 上游与许可证

Chromium 是旨在提供更安全、更快速、更稳定浏览体验的开源浏览器项目。本仓库基于 Chromium 源码，定制内容可通过上述文档和 Git 历史追踪。

- [Chromium 项目网站](https://www.chromium.org/)
- [上游源码文档](docs/README.md)
- [源码目录结构](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code)
- [Chromium 上游问题跟踪](https://crbug.com/new)
- [项目许可证](LICENSE)与 [CPython 来源、版本及许可说明](third_party/cpython/README.chromium)
