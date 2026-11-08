# 原生 Python 阶段五：构建、端到端验证与验收

记录日期：2026-11-09（Asia/Shanghai）。对应[第一版最小实现方案](pyscript-native-python-mvp-implementation.md)阶段五。阶段一至三的历史记录保留各自当时的统计；本记录与[阶段四最终结果](pyscript-phase4-validation/final-results-2026-11-09.json)记录当前实现的验证。

## 1. 验收状态

阶段一至五的第一版必要实现、测试和记录已完成，验收范围为本文列出的 Windows Debug 实验配置。Python MVP 的当前测试全部通过；WPT 和 chromiumPlus 使用的诊断配置及 chromiumPlus 默认模式失败分别记录，不宣称所有默认配置回归均通过。

| 检查 | 最终实际结果 |
| --- | --- |
| 完整 Chrome / Blink / 运行时测试目标构建 | 标准构建成功 |
| CPython 运行时单测 | 22 / 22 |
| Blink `HTMLPyScriptElementTest.*` | 28 / 28 |
| WPT 启用 / 关闭 | 两文件、七个子测试通过；使用已记录诊断参数 |
| 真实 Chrome DOM 测试 | 17 / 17，包含九个页面 testharness 断言 |
| 真实 Chrome 解析 / 权限回归 | 27 / 27，新增顶层 opaque origin 拒绝 |
| Renderer 固定诊断与环境隔离回归 | 2 / 2，六份实际沙箱 Renderer 报告通过 |
| chromiumPlus | 默认模式超时，日志含网络服务崩溃；进程内网络服务诊断配置 15 / 15 |
| MCBE 本地 UDP 回归 | 17 / 17，默认网络配置 |
| MCJE 本地 TCP / PNG 回归 | 33 / 33，默认网络配置 |
| GN / include / 格式 | Core 与运行时 GN check、运行时 / Chrome 适配 / 控制器 / Blink 测试 checkdeps、修改 C++ clang-format 检查均通过 |

结构化证据：[阶段四结果](pyscript-phase4-validation/final-results-2026-11-09.json)、[阶段五结果](pyscript-phase5-validation/final-results-2026-11-09.json)、[原始顺序队列状态](pyscript-phase5-validation/validation-queue-state.json)。队列保留 `completed-with-failures`，其中唯一失败项是 chromiumPlus 默认模式；后续诊断复测独立记录，没有改写失败历史。

浏览器证据：[DOM](pyscript-phase4-validation/dom-browser.json)、[权限 / 解析](pyscript-phase4-validation/phase2-browser.json)、[Renderer](pyscript-phase5-validation/runtime-renderer.json)、[MCBE](pyscript-phase5-validation/mcbe-browser.json)、[MCJE](pyscript-phase5-validation/mcje-browser.json)。[最终截图](pyscript-phase4-validation/dom-browser-sample.png)已目视核对，显示计算结果、共享变量和 HTML 字符纯文本，Python 源码隐藏。

chromiumPlus 默认模式与阶段一已记录的独立网络服务问题症状一致，本次仍无法报告成功，原因未确定。保留[失败日志](pyscript-phase5-validation/chromium-plus-default-failure.log)，之后仅为测试追加 `--enable-features=NetworkServiceInProcess2`，保留 Renderer 沙箱，得到 [15 / 15 结果](pyscript-phase5-validation/chromium-plus-diagnostic.json)；[完整命令及退出码](pyscript-phase5-validation/chromium-plus-diagnostic-command.json)已保存。没有更改产品网络配置，也没有将这一诊断通过替代默认模式的失败结果。

## 2. 构建与依赖

平台为 Windows x64 Debug component build，Chromium 158.0.8126.0。最终标准构建：

```powershell
autoninja -C out/Debug chrome python_runtime_unittests blink_unittests blink_tests
```

构建成功，耗时 1m36.77s。之前的链接失败、仅针对生成对象的恢复与复查见[阶段四记录](pyscript-phase4-budgets-navigation-cleanup.md)。没有调整产品调试信息、禁用链接诊断或放宽 Renderer 沙箱。

嵌入版本为 CPython **3.13.18**，来源为 `https://www.python.org/ftp/python/3.13.18/Python-3.13.18.tar.xz`，压缩包 SHA-256：

```text
abb0102e6d4ae027509bcb82327d9dd85515b44a3ba8ad538cfc5e9847029f68
```

`python third_party/cpython/verify_source.py` 已复核 775 份未修改的上游源文件。内建模块和冻结模块清单、唯一 `python_runtime.dll` 的链接及资源方案见[阶段一记录](pyscript-phase1-cpython-runtime.md)与 `third_party/cpython/frozen_modules.json`；阶段三增加每解释器 `dom` 模块。无外部 Python 安装、磁盘标准库、pip 或动态原生扩展依赖。安装器已登记 DLL staging，完整安装包制作 / 安装、其他平台及 Release 性能均不在本次验收范围。

[源码校验日志](pyscript-phase5-validation/cpython-source.log)和[最终二进制长度 / SHA-256](pyscript-phase5-validation/final-results-2026-11-09.json)已保存。阶段一的冷初始化 52–74 ms 样本是当时的本机 Debug 观测；阶段四的执行预算排除解释器初始化，不把冷启动时间计入 100 ms 单块限制。

## 3. 测试矩阵逐项核对

| 原方案领域 | 实际覆盖与证据入口 |
| --- | --- |
| 原始文本解析 | 阶段二浏览器 `/basic`、`/chunks`、`/unclosed`、`/literal-end`；Blink RAWTEXT、大小写结束标签、源码快照测试；启用 WPT 验证缩进、中文、`<`、`&`、CRLF 和分块 |
| 扫描与片段 | 浏览器请求记录断言不存在伪 img / script / link / 外链 Python 请求；WPT 通用片段 / 快路径回退、DOMParser、template；动态插入与克隆不执行 |
| 时序与共享 | 浏览器断言 deferred JS → Python 队列 → DCL；Python 模块 / globals 同文档共享、跨文档分离；DOM testharness 断言 MutationObserver 在队列返回后运行 |
| 一次执行 | 不可变源码、修改 / 移除重插 / 跨文档移动 / 克隆拒绝；fragment 不重跑；刷新重建；缺失结束标签和 document.write 不执行 |
| 页面权限 | 默认关闭、缺失 / 错误 / 畸形 / 非 loopback origin、iframe / srcdoc、DOMParser 无 frame、prerender 拒绝；真实 HTTP 顶层 sandbox `allow-scripts` 但 opaque origin 仍拒绝；Blink 单独验证 chrome / extension / mcbe / mcje / file URL 拒绝 |
| CSP | `script-src 'none'`、`script-src-elem`、default-src 回退、正确 / 错误 nonce、hash、Report-Only、安全日志、dangling-markup / 重复属性、禁用脚本、sandbox |
| 解释器 | 运行时 globals / modules / 输出隔离、共享 GIL / 线程状态、重复及乱序销毁、禁止创建线程；实际沙箱 Renderer 在污染 PYTHONHOME / PYTHONPATH / stdio 环境时仍独立初始化 |
| DOM | 严格字符串、中文 / HTML 纯文本、不存在 ID、受限元素、shadow / template 不可见、伪造句柄、移除 / 跨文档 / 过期句柄；C++ 检查弱节点与文档代数 |
| 错误 | 语法与运行异常、虚拟文件名 / 原始位置 / traceback、普通错误后续块继续、NUL / 非法 UTF-8 / surrogate 拒绝、有界 UTF-8 控制台 |
| 预算 | 64 KiB 源码、16 块、16 KiB 文本、1024 DOM 调用、文档累计输出 / 诊断、100 ms 纯循环、250 ms 累计时间且排除空闲、超限后原生停止锁存；逐块控制台回调阻止下一块副作用 |
| 生命周期 | 执行中失效、document.open、导航、frame 关闭、GC 的创建销毁计数；受控异常 / finalizer 循环；20 轮导航、刷新和前进 / 后退；BFCache 原生 sticky feature 排除；Browser.close 正常退出 |
| 既有产品 | chromiumPlus、MCBE 本地 UDP、MCJE 本地 TCP / PNG 回归，具体统计与配置见最终结果；MCJE 既有工作区改动保留且不纳入本次提交 |

测试文件：

- `components/python_runtime/python_runtime_unittest.cc`
- `third_party/blink/renderer/core/html/html_py_script_element_test.cc`
- `third_party/blink/web_tests/wpt_internal/pyscript/{parsing-enabled,feature-disabled}.html`
- `chrome/test/{pyscript_smoke_test,pyscript_dom_smoke_test,python_runtime_smoke_test}.mjs`
- `chrome/test/data/pyscript/{phase3,dom-bridge-test}.html`

Blink 测试启动器的完整 JSON 列出所有注册用例，不能视作全部执行；保存的摘要仅包含本次实际运行的 28 项及其位置、状态、耗时，并记录原始文件 SHA-256。DOM 浏览器 17 项中有一项包含额外的 9 个 testharness 断言，不重复累加为端到端用例数。

## 4. 验证配置与复现

测试脚本自行建立本地服务器与独立临时 Profile，精确传入 origin；Python 计算和 DOM 写入由 CPython / C++ 完成。CDP 仅用于驱动导航、读取 DOM、控制台、CSP 和请求记录。`phase3.html` 样例不含页面 JavaScript，在 nonce CSP 下显示 `Python 计算结果：42`、共享变量 `43` 与 HTML 字符纯文本。

```powershell
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.* --test-launcher-jobs=1 --test-launcher-timeout=60000
node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-phase4-final-dom.json
node chrome/test/pyscript_smoke_test.mjs --output=out/Debug/pyscript-phase4-final-phase2.json
node chrome/test/python_runtime_smoke_test.mjs --output=out/Debug/pyscript-phase5-runtime-renderer.json
python chrome/test/chromium_plus_smoke_test.py --chrome-binary=out/Debug/chrome.exe --output=out/Debug/pyscript-phase5-chromium-plus.json
node chrome/test/mcbe_smoke_test.mjs --output=out/Debug/pyscript-phase5-mcbe.json
node chrome/test/mcje_smoke_test.mjs --output=out/Debug/pyscript-phase5-mcje.json
```

测试宿主有外部超时，失败时只清理其启动的浏览器进程；这是测试保护，不是产品硬中断。最后一项使用本地模拟 TCP 服务器，无公网 MOTD / DNS 可用性依赖。

两个 WPT 文件使用以下公共前缀，分别补上启用 / 关闭特性与测试路径：

```text
vpython3 -vpython-root C:/src/src/out/pyscript-vpython third_party/blink/tools/run_web_tests.py -t Debug --child-processes=1 --no-show-results --additional-driver-flag=--disable-system-font-check --additional-driver-flag=--enable-features=NetworkServiceInProcess2
  --additional-driver-flag=--enable-blink-features=PythonScript wpt_internal/pyscript/parsing-enabled.html
  --additional-driver-flag=--disable-blink-features=PythonScript wpt_internal/pyscript/feature-disabled.html
```

后两行表示两次独立调用，不作为同一命令拼接。本机 WPT 沿用字体检查与进程内网络服务诊断配置；未修改 expectations，两个文件的 actual / expected 均为 PASS，无 flaky / skip。content_shell 没有 CPython 执行器，仅验证标签与解析；实际执行由保留沙箱的 Chrome 测试证明。

## 5. 第一版边界

本版保持默认关闭、精确 loopback origin、活动顶层 HTML、首次完整解析内联代码、既有脚本权限与 CSP。只支持 `dom.get_element_by_id` 和受限普通文本节点的 `dom.set_text`。导航先撤销 DOM 能力，再在主线程安全任务边界释放解释器。

100 / 250 ms 执行和 50 ms 清理限制均为协作式预算。它们不能硬性中断任意 C 操作、关闭 tracing / 捕获异常后的恶意代码或复杂 finalizer，也不硬限内存。子解释器、精简模块、origin 限制和 Renderer 沙箱不能提供 Python 语言安全隔离。Renderer 关闭丢弃清理任务时，原生上下文可能保留至进程退出；正常导航释放已有计数验证。

外链、异步执行、动态标签执行、iframe / Worker、事件回调、完整 DOM、任意第三方包与公网默认开启均未实现。后续开放给不可信代码需要独立的隔离与资源约束设计。
