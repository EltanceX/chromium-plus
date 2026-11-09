# 原生 Python：内核调试设置与页面权限模式

日期：2026-11-09（Asia/Shanghai）。接续[Python MVP 验收](pyscript-phase5-build-e2e-acceptance.md)。

## 1. 本次需求与权限含义

用户明确选择“只放开 Python 的站点与页面执行限制，保留 Renderer 沙箱”。因此本次不把“所有权限”解释为浏览器主进程的系统权限。设置页使用“放开 Python 的站点与页面执行限制”这一准确名称，并说明 Python 继续拥有 Renderer 的系统权限。

入口：`chrome://settings/kernelDebug`，新增“Python 解释器”卡片，保留既有内核和标签页信息。

| 设置 | 行为 |
| --- | --- |
| 启用 Python 解释器 | 默认关闭；允许符合当前模式的 HTML 完整解析执行内联 `<pyscript>` |
| 放开站点与页面执行限制 | 默认关闭；仅在解释器启用时生效；切换为下表的放开模式 |
| 受限模式允许的来源 | 一个精确的 loopback HTTP(S) origin，包含实际端口；后端验证、规范化；禁用通配符、路径、列表及任意公网来源 |
| 当前生效状态 | 显示启动时的启用状态、权限模式与来源，与表单待保存 / 待重启值区分 |
| 保存 / 重新启动 | 配置持久保存到 Local State；浏览器重启统一应用；失败时显示校验原因 |

设置是当前 user-data-dir 的浏览器级配置，适用于其各 Profile，不同步到其他设备。Windows x64 以外平台显示不支持状态。

## 2. 两种执行模式

| 条件 | 受限模式 | 放开页面限制模式 |
| --- | --- | --- |
| 来源 | 精确配置的 loopback HTTP(S) origin | 普通 HTTP(S)、file、data、blob、about:blank / srcdoc 文档 |
| 顶层 / 子框架 | 仅顶层 | 允许普通子框架，每个 Document 仍有独立解释器 |
| opaque origin | 拒绝 | 允许普通页面的 opaque origin |
| 用户页面脚本开关 / CSP / sandbox 脚本权限 | 沿用现有检查 | 用户显式覆盖这些 Python 执行检查；不改变 JavaScript 的行为 |
| 内部页面 | 拒绝 | chrome、chrome-untrusted、扩展、MCBE / MCJE 及继承特权 origin 的 about / blob 文档仍拒绝 |
| 原生沙箱 | Renderer 沙箱 | Renderer 沙箱 |
| 解析与生命周期 | 首次完整解析、内联、至多一次；有效活动文档 | 相同；动态插入、document.write、片段、prerender 仍不执行 |
| DOM / 预算 | 当前 Document 的受限 DOM 桥接和既有预算 | 相同，不增加文件、网络、进程或完整 DOM API |

放开模式可执行页面 CSP 原本会阻止的 Python，仅应对可信内容开启。它没有加载完整标准库、原生扩展或第三方包，也没有取消 100 / 250 ms 协作式执行、输出和 DOM 次数预算。协作式预算依旧不提供硬中断或内存硬限制。

## 3. 实现链路

`chrome/browser/native_python/` 提供独立设置模块、Local State 注册、输入校验、启动配置快照和 Renderer 参数生成。三个持久键为：

```text
native_python.enabled
native_python.unrestricted_pages
native_python.allowed_origin
```

`ChromeBrowserMainParts::PreCreateThreadsImpl()` 在 Local State 可用后读取配置。之后每个新 Renderer 只接收启动时快照，保存设置不会使部分新进程提前启用。未改写浏览器自身命令行，避免重新启动时把上次设置生成的开关带回，导致无法关闭。

子进程按配置接收 `--enable-blink-features=PythonScript`、规范化的 `--native-python-allowed-origin` 和可选 `--native-python-unrestricted-pages`。追加特性时保留已有 Blink feature 列表；没有增加 `--no-sandbox`、`--disable-web-security` 或修改 token / broker 策略。

已有 CLI 启用方式继续可用。显式 `--disable-blink-features=PythonScript` 优先于设置总开关；CLI 指定来源覆盖保存来源；CLI 放开参数与保存的放开选项均要求解释器启用才生效。设置页提示 CLI 可能覆盖表单配置。

Blink 通过嵌入接口 `AllowsUnrestrictedPages()` 接收模式，不直接读取 Chrome 设置。普通模式保持原资格、脚本与 CSP 检查；放开模式调整页面资格和 Python 的脚本 / CSP 判断。两者均先验证活动文档、frame、原解析器输入、生命周期和当前句柄；普通 about / blob 还核对继承的实际安全来源，拒绝特权来源。

`KernelDebugHandler` 提供 `getNativePythonSettings` / `setNativePythonSettings`。后端完整验证三个字段后才写入，错误输入不产生部分保存。前端使用真实保存结果、重启提示和既有 RelaunchMixin，不只改变显示状态。

## 4. 验证

本次代码在 Windows x64 Debug 完成验证，共 96 项通过。以下结果均来自本次构建和执行，证据保存在 [pyscript-settings-validation/](pyscript-settings-validation/validation-summary.json)。

| 验证 | 本次结果 |
| --- | --- |
| 标准构建 `chrome native_python_settings_unittests blink_unittests` | 通过，保留调试信息和标准链接诊断 |
| 设置模块单元测试 | 8 / 8 |
| Blink `HTMLPyScriptElementTest.*` | 32 / 32，包含 4 项新增页面权限测试 |
| 真实设置页与多次浏览器重启 | 12 / 12 |
| 既有受限解析与权限回归 | 27 / 27 |
| 既有 DOM、预算与生命周期回归 | 17 / 17 |
| GN 依赖检查 | settings、Blink core、Blink unit_tests、Chrome renderer 均通过 |
| checkdeps | 设置模块、Blink 控制器、Blink 测试、设置 Handler 均通过 |
| TypeScript / WebUI ESLint | 实际编译与 lint 通过 |

执行命令：

```powershell
autoninja -C out/Debug chrome native_python_settings_unittests blink_unittests
out/Debug/native_python_settings_unittests.exe --test-launcher-jobs=1
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.* --test-launcher-jobs=1
node chrome/test/pyscript_settings_smoke_test.mjs --output=out/Debug/pyscript-settings-smoke.json
node chrome/test/pyscript_smoke_test.mjs --output=out/Debug/pyscript-settings-restricted-regression.json
node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-settings-dom-regression.json
```

设置专用单测覆盖默认关闭、来源规范化、非法输入原子拒绝、CLI 优先级、启动快照以及特性参数合并和沙箱开关缺席。Blink 增加放开模式的 CSP / opaque 页面、特权 URL、prerender 和继承特权来源拒绝测试。

新端到端脚本驱动真实设置页表单，检查错误提示、保存后未生效、复用 Profile 重启后生效、受限来源 / CSP、放开后的普通页面 / 本地 HTML / opaque data / iframe / srcdoc、禁用 JS 时 Python 行为、预算保留和再次关闭。测试有外部超时与仅针对本次临时 Profile 的清理，保留实际 Renderer 沙箱。

设置页测试没有使用 `--enable-blink-features=PythonScript` 启用页面功能，启用来自真实表单及持久配置；只增加原生自检诊断 `--enable-features=NativePythonRuntimeSelfTest`。自检实际报告 `passed: true`、`sandboxed: true`，并由脚本断言。iframe 与 srcdoc 的源码各自检查不存在先前的 `page_marker`，确认 Document 间没有共享 Python 全局变量。

本次 27 / 17 回归保持默认网络服务配置与 Renderer 沙箱，没有增加网络服务诊断参数。界面顶部截图已目视检查，见 [settings-page.png](pyscript-settings-validation/settings-page.png)。重启测试采用正常关闭浏览器后复用同一个临时 Profile 启动；设置页重启按钮复用现有 RelaunchMixin 与确认对话框。

### 4.1 构建与测试期间的问题及恢复

- 首轮 WebUI ESLint 拒绝四个事件处理器名称；已按规则改为 `on...Change_` / `on...Click_`，后续编译与 lint 通过。
- 旧生成对象曾导致 Blink Core、单测和最后一次 Chrome 增量链接的 COFF / CodeView 错误。扫描实际链接输入后，保存原件、SHA-256 和诊断，仅使 9 个生成 `.obj` 失效并重建；没有跳过 PDB 或降低链接检查。Core 的 5 份原件和 Chrome 输入中的 `iwa_dev_page_handler.obj` 在普通读取与 mmap 中稳定复现结构错误；单测输入的另外 3 份只在初次扫描报告异常，保存原件前复读未复现，因此不将这 3 份记为稳定损坏。Chrome 链接器首先报告的是 `chrome_main.obj`，实际扫描全部 12,081 个直接对象与 788 个库后才定位到 IWA 页面处理器对象，未盲目删除首个被报告的对象。根因尚未确定。[恢复清单](pyscript-settings-validation/object-recovery.json)保留原始与重建哈希；[重建后扫描](pyscript-settings-validation/rebuilt-inputs-scan.json)检查 9 份对象，问题数为 0。
- 首轮新增 Blink 测试触发 SimTest 导航断言：通用加载辅助方法没有提供 CSP sandbox 对应的浏览器计算 opaque origin。改为显式提交带 opaque origin 的模拟导航，继续验证完整 `sandbox` 和 `script-src 'none'`，没有放宽断言。
- 继承特权来源测试最初未注册扩展 scheme，得到的是 opaque origin；补上标准 scheme 注册及非 opaque 断言后，又发现测试体结束时 scheme 注册先于 Document 恢复，导致 teardown 崩溃。现保留并恢复原来源后再退出测试体。两轮失败日志和结果均归档，最终完整 32 项通过。

最初的构建诊断、对象扫描以及测试失败记录保留在验证目录中；没有将失败改写成通过，也没有依赖重试来接受失败断言。
