# 阶段三：文档上下文与最小 DOM 桥接

- 实施日期：2026-11-08；最终验收：2026-11-09（Asia/Shanghai）。
- 分支：`codex/kernel-debug-settings`；阶段二基点：`d63e4e2d64`。
- 依据：[第一版实现方案](pyscript-native-python-mvp-implementation.md)第 7 节及阶段三。
- 当前状态：阶段三计划内验收完成。完整 Chrome 与 Blink 测试目标构建通过，运行时 15 / 15、Blink 定向单测 20 / 20、DOM 浏览器测试 9 / 9 和阶段二浏览器回归 26 / 26 通过。启用 / 关闭两组 WPT 均通过，沿用字体检查和进程内网络服务诊断参数；两组浏览器测试使用默认网络服务配置并保留 Renderer 沙箱。最终结果见第 10 节。阶段四的执行时间与累计输出预算仍未实现。
- 关键提交：`5dcb4e0cde`（每解释器 DOM 模块与 15 项运行时测试），`1ffbda7979`（文档上下文、DOM 接入及示例）。

## 1. 接口与使用

```html
<p id="result">Waiting</p>
<pyscript>
import dom
answer = 6 * 7
handle = dom.get_element_by_id("result")
dom.set_text(handle, f"Python 计算结果：{answer}")
</pyscript>
<pyscript>
dom.set_text(handle, f"共享句柄：{answer + 1}")
</pyscript>
```

`dom.get_element_by_id(str)` 只查询当前 Document 的 light DOM，找不到时返回 `None`。`dom.set_text(ElementHandle, str)` 直接通过 C++ 写入 Blink 文本 DOM，不调用页面 JavaScript，也不解析 HTML。完整示例：[phase3.html](../chrome/test/data/pyscript/phase3.html)。

仍需显式启用 `PythonScript`，并配置精确的本地 HTTP(S) origin。阶段二的顶层文档、原始解析来源、脚本权限和 CSP 检查继续适用。示例中的 nonce 可以配合 `Content-Security-Policy: script-src 'nonce-python-dom'` 使用。

## 2. 运行时 DOM 模块

新增 `components/python_runtime/python_dom_module.{h,cc}`，在 CPython 初始化前注册多阶段初始化内建模块 `dom`。每个子解释器拥有独立模块状态和 `ElementHandle` 堆类型。

- 句柄只保存文档编号、代数和条目编号，不保存 `Node*` 或可调用的 Host。
- 句柄类型不可直接构造、不可派生且不可修改类型；整数、`None` 及其他对象不能伪造句柄。
- 参数必须是 Python Unicode 字符串，不对任意对象调用 `str()`；单个参数最多 16 KiB UTF-8。孤立 surrogate 产生 `UnicodeEncodeError`，超长产生 `ValueError`。
- 参数类型错误为 `TypeError`，失效文档 / 句柄为 `ReferenceError`，受限目标为 `PermissionError`，Host 状态错误为 `RuntimeError`。
- `PythonContext::Execute(..., DomHost*)` 仅在实际求值期间借用 Host。编译、异常字符串格式化、结果释放及解释器清理时均没有 DOM 能力。
- 运行时拒绝嵌套 `Execute` / `Create`，避免从 DOM Host 重入共享 GIL 后死锁。
- `PythonContext` 持有原始 `dom` 模块引用，不依赖页面是否修改 `sys.modules` 或模块公开属性。

新增六个运行时用例验证类型与伪造、跨块句柄与模块隔离、异常映射、UTF-8 边界、诊断 / finalizer 能力撤销和嵌套进入；连同原九项共 15 / 15 通过。

## 3. 文档所有权与 Blink 句柄

`PythonScriptController` 作为 Document supplement 持有一个 `WebPythonScriptContext`；Chrome 的实现持有 `PythonContext` 子解释器。Python globals、导入模块与句柄在文档批次执行后继续保留，同文档的多个块共享状态，不同 Document 使用独立子解释器。

句柄表使用可追踪条目中的 `WeakMember<Element>`，不把节点永久强引用。查询去重且不复用编号；写入复核文档编号、代数、编号范围、节点所属文档、连接状态和 shadow 状态。

只允许 HTML 命名空间、未定制且不带 `is` 属性的普通 `p/div/span`；含元素子节点的目标拒绝。`script/pyscript/style`、SVG、自定义元素及其定制内建形式均拒绝。写入仅替换文本 / 注释子节点，使用脚本与事件禁入范围；MutationObserver 仍在 Python 栈返回后的微任务检查点运行。

每文档最多 1024 次到达 Blink 的 DOM 调用，字符串在运行时和 Blink 边界分别检查。当前调用预算用尽后报告 `ValueError`；不可逆停止和协作式执行时间预算属于阶段四。

## 4. 失效、清理与 BFCache

持久解释器必须同时具备最小生命周期处理，因此本阶段提前接入导航 / Shutdown、`document.open()`、ExecutionContext 销毁及 GC prefinalizer。它们先停止上下文、清空队列与弱句柄、增加代数；执行中的上下文在 `Run` 返回后才能释放。

解释器清理会运行 Python finalizer，不能在 Oilpan 回调或生命周期中的 script-forbidden 范围直接析构。控制器将无 GC 引用的解释器对象交给所属主线程任务释放，并提前撤销 DOM 能力。任务闭包不拥有析构型智能指针，防止队列丢弃任务时在错误时机进入 Python。如果线程任务队列已经关闭，资源故意保留到 Renderer 进程退出；当前没有对这种退出边界提供精确清理保证。

创建上下文的文档注册独立的 sticky scheduler feature `kNativePython = 73`，显示名 `native-python`，由 BFCache 明确拒绝，并同步导航 metrics 枚举。没有伪造 unload 监听器。私有实验没有新增公开 CDP enum；CDP 原因暂映射为 `Unknown` / `SupportPending`，scheduler 原因仍是 `native-python`。

这些措施只覆盖持久解释器必需的能力撤销与生命周期边界。完整执行时间 / 输出累计预算、长时间循环引用压力和阶段四验收仍未完成。

## 5. 验证与复现

```powershell
autoninja -C out/Debug python_runtime_unittests
out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000 --test-launcher-summary-output=out/Debug/pyscript-phase3-runtime-unittests.json

autoninja -C out/Debug chrome
autoninja -C out/Debug blink_unittests blink_tests
out/Debug/blink_unittests.exe --gtest_filter=HTMLPyScriptElementTest.*

node chrome/test/pyscript_dom_smoke_test.mjs --output=out/Debug/pyscript-dom-smoke.json
node chrome/test/pyscript_smoke_test.mjs --output=out/Debug/pyscript-phase3-phase2-regression.json
```

新增浏览器脚本保留 Renderer 沙箱，启动独立 Profile 和精确 origin 的本地服务。网页行为用 WPT `testharness.js` 断言；真实 CPython 用例放在 `chrome/test/data/pyscript/` 并由 Chrome 脚本运行，因为 `content_shell` 不安装 Chrome 的 Python 执行器。现有 `wpt_internal/pyscript/` 仍验证 Blink 的解析与默认关闭行为。

| 检查 | 当前结果 |
| --- | --- |
| 运行时 DLL 与单测构建 | 通过，格式化后的产物已再次执行测试 |
| 运行时单测 | 15 / 15 通过，2026-11-09 再次执行通过 |
| 控制器 / Chrome 适配器 / 20 用例源码编译 | 通过；首次发现并修正 raw_ref 规则、窄化和测试 API 差异 |
| GN 检查 | python_runtime、Chrome Renderer、Blink Core 通过 |
| checkdeps | 上述三个目录通过；工具提示既有 DEPS 字符串的 SyntaxWarning |
| Node 语法与本阶段 diff 空白检查 | 通过 |
| 示例 / testharness 内嵌代码语法 | 两个示例的 7 块 Python 及测试用 JavaScript 已检查，保留预期语法异常样例 |
| Navigation metrics XML 校验 | `validate_format.py --xml_paths tools/metrics/histograms/metadata/navigation/enums.xml` 通过 |
| Blink Core / Modules DLL | 异常生成对象重建后均通过标准链接，详见第 7、8、10 节 |
| 完整 Chrome 与 Blink 测试目标 | 最终标准构建通过：4m21.45s，21 项任务 |
| 浏览器 DOM smoke | 默认配置 9 / 9 通过，包含页面 testharness 的 9 项断言；保留 Renderer 沙箱 |
| 阶段二浏览器回归 | 默认配置 26 / 26 通过 |
| Blink 定向单测 | 实际执行 20 / 20 通过，包含执行中失效和延迟释放用例 |
| WPT 回归 | 启用 / 关闭两个文件均为 PASS，无跳过、flaky 或回归；使用已记录的诊断参数 |

原始日志和结构化结果保存在 [pyscript-phase3-validation/](pyscript-phase3-validation/)。未完成的检查不会记为通过。既有 MCJE SRV 修复不属于本阶段提交。

## 6. 后台构建与待验证队列

2026-11-08 21:25（Asia/Shanghai）启动了独立隐藏的 PowerShell 后续验证进程，等待本轮 Chrome 构建成功后顺序运行：

1. 新 `pyscript_dom_smoke_test.mjs`，包含无 JavaScript 的 nonce CSP 示例、WPT testharness 的 9 项网页断言、文档隔离、DOM 调用预算、`document.open()`、BFCache、刷新和六轮 finalizer 导航。
2. 阶段二 26 项浏览器回归。
3. `blink_unittests blink_tests` 构建，然后运行 `HTMLPyScriptElementTest.*` 的 20 项用例。
4. 已有启用 / 关闭两组 WPT；沿用阶段二的字体检查与进程内网络服务诊断参数，单独标记这种配置。

同一输出目录的构建始终串行。Chrome 构建失败时队列停止，不运行 ABI 尚不一致的旧浏览器。运行后的检查结果会单独记录，失败不会自动改写断言或变成预期失败。

本机任务文件位于忽略目录 `out/Debug/pyscript-phase3-followup.ps1`，实时状态为 `out/Debug/pyscript-phase3-validation-state.json`。构建日志为 `out/Debug/pyscript-phase3-chrome-build.log`，后续各项日志与 JSON 同在 `out/Debug/`。本次交付的 [结构化快照](pyscript-phase3-validation/results-2026-11-08.json)仅记录当时状态；后台实际完成后仍需检查结果、修复失败并补充验收记录。

## 7. 22:04 恢复与生成对象修复

首轮完整 Chrome 构建在 22:01 左右停止：耗时 48m05.80s，完成 947 项、失败 1 项、剩余 3,348 项。`blink_core.dll` 链接器报告 `core_initializer.obj` 的 procedure symbol 引用的 PDB item 不是有效的 function ID。后续队列正确停止，没有运行旧浏览器或未重新链接的测试程序。

先保存并重建了被诊断直接指向的 `core_initializer.obj`，重建后的 SHA-256 与旧件完全一致，单独链接仍出现相同错误。其局部函数 ID 与 18,328 项类型记录检查正常，不能据此认定这份对象损坏。sccache 统计显示当前 `-fmodules-cache-path` 调用均不可缓存；绕过缓存没有改变结果。

随后检查 `blink_core.dll` 的 5,040 个直接对象输入，核对 806 项目标函数哈希与类型记录。没有发现目标函数哈希的跨类型冲突，但发现已有 `initial_letter_utils.obj` 在类型索引 `0x41ae` 处存在无效记录长度。该旧件时间为 14:19，早于本轮阶段三构建。

保留旧件后重建 `initial_letter_utils.obj`，文件长度仍为 7,762,874 字节，SHA-256 已改变；两份重建对象的 COFF 结构和 CodeView 记录检查均通过。再次运行标准 `autoninja -C out/Debug blink_core.dll`，在 1m25.24s 内完成编译和链接，原 PDB 错误消失。异常对象的产生原因仍未确定，没有修改源码、关闭调试信息或放宽链接器诊断来绕过问题。

旧对象与元数据位于忽略目录 `out/Debug/pyscript-phase3-corrupt-objects/`。证据包括[首轮链接失败](pyscript-phase3-validation/chrome-initial-link-failure.log)、[对象哈希与恢复结果](pyscript-phase3-validation/object-recovery.json)、[5,040 项诊断结果](pyscript-phase3-validation/function-hash-check.json)、[重建对象结构检查](pyscript-phase3-validation/coff-rebuilt-check.json)、[CodeView 检查](pyscript-phase3-validation/coff-codeview-rebuilt-check.json)和[成功的 Blink Core 构建](pyscript-phase3-validation/blink-core-repaired-build.log)。

22:15 恢复标准 `autoninja -C out/Debug chrome python_runtime_unittests`，本轮待完成约 3,337 项；22:16 重新启动后续验证队列。实时构建进程为 `6736`，后续进程为 `16144`，仅用于本次本机恢复定位，退出后不代表其他同号进程仍属于此任务。当前恢复状态见 [resume-results-2026-11-08.json](pyscript-phase3-validation/resume-results-2026-11-08.json)。浏览器端到端、20 项 Blink 用例和 WPT 回归仍待构建完成后运行。

## 8. 23:37 第二次生成对象修复

22:15 恢复的完整构建在 23:17 停止：耗时 1h2m15.05s，完成 1,120 项、失败 1 项、剩余 1,074 项。此次失败目标为 `components_webapps_isolated_web_apps.dll`，同样报告 PDB function ID 错误，首先指向 `client.obj` 和 `ecdsa_p256_public_key.obj`。单独运行标准链接仍在 39.09s 后失败。后续验证队列再次停止，没有运行旧浏览器。

检查该目标的 2,911 个直接对象后，COFF 结构、CodeView 记录边界和局部函数 ID 引用均没有发现异常。针对 `client.obj` 的 63 项函数哈希也没有发现跨类型冲突，因此没有重建或认定这两个报错对象损坏。

进一步扫描响应文件中可直接定位的 233 个静态库，包括 thin archive 引用的外部对象，共检查 2,831 个成员，其中 2,671 个带有类型记录。发现 `obj/third_party/tflite/tflite_framework_core/flatbuffer_to_proto.obj` 的类型索引 `0x1b62` 使用无效的记录类型 `0xce2d`。目标对象的 167 项函数哈希与静态库成员对照未发现跨类型冲突。这里的自编结构检查只用于定位，不能代替编译器或完整链接验证。

异常对象的原时间为 2026-10-27 02:49:46，长度 3,209,935 字节。保留原文件与元数据后，仅重建该生成对象；新件长度相同、SHA-256 已改变，COFF 和 CodeView 检查均通过。再次执行 `autoninja -C out/Debug components_webapps_isolated_web_apps.dll`，在 1m24.24s 内完成目标与依赖的 10 项重建，原链接错误消失。本次未修改产品源码、构建配置或链接诊断；旧对象异常的产生原因仍未确定。

证据包括[第二次完整构建失败](pyscript-phase3-validation/chrome-iwa-link-failure.log)、[独立链接复现](pyscript-phase3-validation/iwa-link-retry.log)、[直接对象 COFF 检查](pyscript-phase3-validation/iwa-coff-full-check.json)、[CodeView 检查](pyscript-phase3-validation/iwa-codeview-full-check.json)、[局部函数 ID 检查](pyscript-phase3-validation/iwa-local-function-ids.json)、[函数哈希检查](pyscript-phase3-validation/iwa-function-hash-check.json)、[静态库成员检查](pyscript-phase3-validation/iwa-archive-check.json)、[对象恢复元数据](pyscript-phase3-validation/iwa-object-recovery.json)、[重建对象结构检查](pyscript-phase3-validation/iwa-coff-rebuilt-check.json)、[重建对象 CodeView 检查](pyscript-phase3-validation/iwa-codeview-rebuilt-check.json)和[成功的标准链接](pyscript-phase3-validation/iwa-repaired-build.log)。原文件保存在忽略目录 `out/Debug/pyscript-phase3-corrupt-objects/flatbuffer_to_proto-before-rebuild.obj`。

23:37 再次恢复完整 Chrome 与运行时测试目标构建，23:38 重新启动后续队列。此次构建进程为 `5096`，队列进程为 `12280`。默认浏览器测试失败时，队列会额外运行 `NetworkServiceInProcess2` 诊断配置，并分别保存结果；诊断通过不会覆盖默认配置失败。最终验收仍取决于真实运行结果。

完整 Chrome 构建随后在 1m49.36s 内通过，共完成 8 项重建，包含新的 `chrome.dll` 链接。[构建日志](pyscript-phase3-validation/chrome-repaired-build.log)与[恢复结果快照](pyscript-phase3-validation/iwa-recovery-results-2026-11-08.json)已经保存。后续浏览器与 Blink 检查单独记录，不能由构建成功推定通过。

## 9. 2026-11-08 浏览器端到端结果

重新链接后的 Chrome 在默认网络服务配置下通过全部 9 项 DOM smoke，其中真实页面 testharness 的 9 项断言也全部通过。测试使用独立 Profile、精确的本地 HTTP origin，保留 Renderer 沙箱与 CSP；没有使用 `--no-sandbox`、`--disable-web-security` 或进程内网络服务诊断参数。

实测覆盖无 JavaScript 的 nonce CSP 示例、同文档共享 globals / 句柄、DOM 参数和目标限制、16 KiB UTF-8 边界、普通异常后继续执行、MutationObserver 在所有 Python 块返回后运行、文档隔离、1024 次共享调用预算、`document.open()` 失效、BFCache 排除、刷新以及六轮含循环引用 finalizer 的导航。

随后阶段二浏览器回归 26 / 26 通过，覆盖解析边界、来源限制、默认关闭、页面脚本权限、CSP、输出限制与导航。两个测试均无需启动网络服务诊断重试。

证据：[DOM smoke 结构化结果](pyscript-phase3-validation/dom-browser-smoke.json)、[DOM smoke 日志](pyscript-phase3-validation/dom-browser-smoke.log)、[阶段二回归结果](pyscript-phase3-validation/phase2-browser-regression.json)和[阶段二日志](pyscript-phase3-validation/phase2-browser-regression.log)。[无 JavaScript 示例截图](pyscript-phase3-validation/dom-browser-sample.png)显示 Python 计算结果 42、共享变量与句柄结果 43，以及按纯文本显示的 HTML 字符串。

当晚记录时，Blink 单元测试和 WPT 仍需构建后实际运行。[验收进度快照](pyscript-phase3-validation/acceptance-progress-2026-11-08.json)与[Blink 构建日志快照](pyscript-phase3-validation/blink-tests-build-progress.log)记录了约 1,234 项剩余任务，构建进程为 `2656`，后续队列为 `12280`。这些历史快照不会自动更新；最终验收结果见第 10 节。

## 10. 2026-11-09 Blink 验证恢复

后台 `blink_unittests blink_tests` 构建在 01:09 停止：耗时 1h28m37.88s，完成 1,175 项、失败 1 项、剩余 4 项。失败为 `blink_unittests.exe` 无法解析 `PythonScriptController::StopDocument(Document&)`；新增的执行中失效用例直接调用该静态方法，而控制器的声明没有导出它。[完整日志](pyscript-phase3-validation/blink-tests-initial-link-failure.log)和[原队列状态](pyscript-phase3-validation/blink-tests-initial-failed-state.json)已保留；20 项单测和 WPT 当时没有运行。

修复在控制器头文件显式包含 `core_export.h`，仅为 `StopDocument()` 添加 `CORE_EXPORT`。保留原测试调用及断言，继续测试执行中撤销 DOM 能力和延迟释放上下文。涉及的四份源文件重新编译通过，GN header dependency、checkdeps 和 clang-format 检查通过。

首次重新链接 `blink_core.dll` 又遇到已有 `css_parser.obj` 的 COFF 符号表异常：一个符号引用 section 9340，但该对象只有 5644 个 section。保存原件后连续三次读取的 SHA-256 一致。随后同时检查 Blink Core 的全部 5,040 个直接对象输入，COFF 扫描发现 5 份异常，CodeView 扫描发现 5 份异常，去重后共 8 份：`local_font_face_source`、`css_parser`、`whitespace_attacher`、`character_iterator`、`local_frame_view`、`html_details_element`、`html_microphone_element` 和 `frame_set_layout_algorithm`。异常涉及重定位、符号表、字符串表或调试记录。这些自编检查用于定位结构问题，不能保证所有机器码与调试数据语义正确。

八份原对象及 SHA-256、长度、时间和诊断已保存在忽略目录 `out/Debug/pyscript-phase3-corrupt-objects/final-validation/`，然后只使这些已确认异常的生成对象失效并重建；没有修改对应产品源码。异常产生原因仍未确定。证据：[链接失败](pyscript-phase3-validation/blink-core-css-parser-link-failure.log)、[全部直接对象 COFF 检查](pyscript-phase3-validation/blink-core-final-coff-scan.json)和[全部直接对象 CodeView 检查](pyscript-phase3-validation/blink-core-final-codeview-scan.json)。

重建这八份对象后，链接器进一步报告 `radio_button_group_scope.obj` 与 `v8_abort_signal.obj` 的符号名包含异常数据。补充检查全部直接输入的 COFF 字符串表，确认这两份对象含非法 UTF-8 和控制字节；同时检查 59 个可直接定位的静态库、共 560 个成员，未发现结构异常。两份对象也已保留原件后重建，因此本轮累计重建 10 份异常生成对象。[补充扫描](pyscript-phase3-validation/blink-core-string-and-archive-scan.json)与[第二次链接失败](pyscript-phase3-validation/blink-core-stringtable-link-failure.log)保留诊断；后者将控制字符转为可见转义形式。结构检查能够通过而字符串内容仍然异常，说明不能把前述扫描等同于完整产物校验。

这 10 份新对象的 SHA-256 均与原件不同，COFF 和 CodeView 检查均通过，随后 Blink Core 标准链接通过。继续构建时，`blink_modules.dll` 又报告 CodeView 损坏。检查该目标的 4,647 个直接输入和 60 个静态库的 1,309 个成员，发现 9 份直接对象的 CodeView 记录异常，包括 WebGPU、语言模型、WebAuthn、Service Worker、WebXR、CSS、图像来源 union 的生成绑定及两份 modules 对象。逐一保留原件后重建；没有根据链接器首先提到的 `modules_initializer.obj` 名称直接认定它损坏。证据：[Modules 链接失败](pyscript-phase3-validation/blink-modules-initial-link-failure.log)和[完整输入扫描](pyscript-phase3-validation/blink-modules-input-scan.json)。

19 份对象重建后的 SHA-256 均与原件不同，COFF 和 CodeView 检查均通过。它们的[恢复元数据](pyscript-phase3-validation/final-object-recovery.json)、[COFF 检查](pyscript-phase3-validation/final-rebuilt-coff-check.json)和[CodeView 检查](pyscript-phase3-validation/final-rebuilt-codeview-check.json)已保存；原二进制保留在忽略目录，不纳入提交。

最终标准命令 `autoninja -C out/Debug chrome blink_unittests blink_tests` 在 4m21.45s 内通过，共完成 21 项任务，包含新的 Chrome DLL、Blink 单测和 content_shell 链接。没有关闭调试信息或放宽链接器诊断。[最终构建日志](pyscript-phase3-validation/final-build.log)、[GN 检查](pyscript-phase3-validation/final-gn-check.log)和[include 检查](pyscript-phase3-validation/final-checkdeps.log)已保存。

随后七个顺序验证步骤均以 exit code 0 结束，阶段三计划内验收完成：

| 检查 | 最终结果与证据 |
| --- | --- |
| CPython 运行时单测 | 15 / 15，通过[日志](pyscript-phase3-validation/final-runtime-unittests.log)与[结构化结果](pyscript-phase3-validation/final-runtime-unittests.json)记录 |
| `HTMLPyScriptElementTest.*` | 20 / 20 实际执行通过，包含此前无法链接的执行中失效用例；[日志](pyscript-phase3-validation/final-blink-unittests.log)、[执行项摘要](pyscript-phase3-validation/final-blink-unittests.json) |
| PythonScript 启用 WPT | `parsing-enabled.html` 为 PASS；[日志](pyscript-phase3-validation/final-wpt-enabled.log)、[结果](pyscript-phase3-validation/final-wpt-enabled-results.json) |
| PythonScript 关闭 WPT | `feature-disabled.html` 为 PASS；[日志](pyscript-phase3-validation/final-wpt-disabled.log)、[结果](pyscript-phase3-validation/final-wpt-disabled-results.json) |
| DOM 浏览器测试 | 9 / 9 通过，含页面 testharness 的 9 项断言；[日志](pyscript-phase3-validation/final-dom-browser-smoke.log)、[结果](pyscript-phase3-validation/final-dom-browser-smoke.json)、[示例截图](pyscript-phase3-validation/final-dom-browser-sample.png) |
| 阶段二浏览器回归 | 26 / 26 通过；[日志](pyscript-phase3-validation/final-phase2-browser-regression.log)、[结果](pyscript-phase3-validation/final-phase2-browser-regression.json) |

两组 WPT 使用 `--disable-system-font-check` 与 `--enable-features=NetworkServiceInProcess2`，结果没有跳过、flaky 或回归。content_shell 不安装 CPython 执行器，因此这两项只覆盖 Blink 的标签与解析行为。真实 CPython 与 DOM 集成由 Chrome 浏览器测试覆盖；两组 Chrome 测试的额外参数列表均为空，保留默认网络服务配置、Renderer 沙箱、来源限制与 CSP。

Blink 结构化结果仅保存这次实际执行的 20 项及其位置、状态和耗时。完整测试启动器 JSON 中还列出了 33,390 项注册用例，该清单不表示执行了整个 Blink 测试套件；原始文件保留在 `out/Debug/pyscript-phase3-final-blink.json`，摘要记录了它的 SHA-256。

[最终验收结果](pyscript-phase3-validation/final-results-2026-11-09.json)和[验证队列状态](pyscript-phase3-validation/final-validation-state.json)取代此前待完成的历史快照。阶段四尚未开始；协作式执行时间预算、累计输出预算和不可逆停止策略继续保持未完成状态。
