# 阶段一：CPython 依赖与独立运行时

- 日期：2026-11-08（Asia/Shanghai）。
- 对应方案：[原生 Python MVP](pyscript-native-python-mvp-implementation.md) 的阶段一。
- 平台：Windows x64，Chromium 158.0.8126.0，`out/Debug` component build。
- 状态：阶段一已完成，阶段二至五尚未实施。
- 本阶段实现依赖、解释器和固定诊断入口；网页标签与 DOM 桥接属于后续阶段。

## 1. 依赖与构建

锁定普通 GIL 版本 **CPython 3.13.18**：

```text
https://www.python.org/ftp/python/3.13.18/Python-3.13.18.tar.xz
SHA-256: abb0102e6d4ae027509bcb82327d9dd85515b44a3ba8ad538cfc5e9847029f68
```

`third_party/cpython/` 包含源码、GN 构建、导入及校验脚本、模块清单和许可证信息。现有 `third_party/cpython3/` 是构建宿主工具，不能作为浏览器内嵌解释器。

`import_source.py` 先校验压缩包哈希，再按上游 `PCbuild/_freeze_module.vcxproj` 选取核心源码，并补充编码、输出、线程和内存分配器所需输入。本次导入 775 个上游文件，共 20,896,105 字节。`src/` 保持上游字节不变；`source_manifest.json` 记录全部文件哈希，`.gitattributes` 禁止对这些文件转换换行。许可证及上游附带软件的许可说明保存在 `src/LICENSE` 和 `src/Doc/license.rst`。

构建关系：

```text
Chromium clang-cl / Windows SDK / CRT
  ├─ CPython core → freeze_module.exe（只在构建时运行）
  │                  └─ 冻结同一版本的 Python 源码 → frozen.c
  └─ CPython core + frozen.c + 显式路径实现
                      └─ python_runtime.dll
                           ├─ Chrome Renderer
                           └─ python_runtime_unittests.exe
```

始终使用一个 `shared_library` 承载进程内 CPython。调用方只使用运行时的 C++ 接口，不链接另一份核心，不依赖系统 `python313.dll`。导出宏独立于 `COMPONENT_BUILD`；当前实测范围仍只有 Debug component build，没有宣称 Release 或其他架构已构建。

`chrome/installer/mini_installer/chrome.release` 已登记 `python_runtime.dll` 到版本目录。分发构建产物时必须同时携带此 DLL 和正常的 Chromium 依赖。没有需要额外拷贝的 Python 标准库目录、ZIP、`.pyc` 或 Python 安装。本阶段只检查安装器的 DLL staging 行为，未构建或运行完整安装包。

## 2. 模块与资源

内建模块：

```text
_ast _abc _codecs _weakref _thread _io _imp _string _tokenize
_warnings _signal _tracemalloc atexit faulthandler gc marshal
nt winreg time builtins sys
```

`_chromium_python` 由封装层注册，是每解释器独立状态的多阶段初始化模块，当前仅提供输出捕获。

冻结模块（完整清单见 `third_party/cpython/frozen_modules.json`）：

```text
_frozen_importlib _frozen_importlib_external zipimport
abc codecs io _collections_abc
encodings encodings.aliases encodings.utf_8 encodings.ascii
encodings.latin_1 encodings.utf_8_sig encodings.cp1252 encodings.mbcs
```

冻结工具与运行时由同一套 CPython 源码构建。宿主 Python 只负责构建编排，不生成目标版本字节码。

`nt`、`winreg`、`_io` 等启动模块包含 OS 能力；精简模块和子解释器不构成语言安全沙箱。当前没有把任意页面源码交给运行时的入口。

## 3. 初始化、执行和清理

代码位于 `components/python_runtime/`：

- `PythonContext::Create()`：延迟初始化进程运行时，创建子解释器及其 globals。
- `Execute(source, filename)`：同步编译、执行，返回成功状态、stdout、stderr、截断标记和异常文本。
- `~PythonContext()`：先禁用宿主输出回调，再释放 globals，并在所属线程结束子解释器。
- `RunSelfTest()`：执行固定的原生诊断代码，返回可序列化的结果。

初始化使用隔离的 `PyPreConfig` / `PyConfig`，启用 UTF-8，忽略 `PYTHONHOME` / `PYTHONPATH` 等环境设置；关闭 site、用户 site、信号处理器安装、字节码写盘和 C stdio 重配置。所有解释器使用空 `sys.path` 和显式占位路径；替代路径初始化实现不搜索磁盘、注册表或可执行文件目录。生成的配置禁用动态扩展加载，构建选择 `dynload_stub.c`。

每个上下文使用 `Py_NewInterpreterFromConfig()` 创建独立解释器，分离对象分配器与模块状态，禁止创建线程、fork、exec，启用多解释器扩展检查，使用共享 GIL。所有上下文在同一所属线程上操作；通过 `PyEval_RestoreThread()` / `PyEval_SaveThread()` 管理线程状态，不使用 `PyGILState_*` 切换子解释器。

同一上下文保持 globals，不同上下文分离 globals、`sys` 模块状态与输出捕获。输出对象的 C 模块状态只在所属上下文存活且允许回调时访问宿主；析构期间的 Python finalizer / atexit 输出不会回调有效宿主。

边界：源码每次最多 64 KiB，文件名最多 2048 字节，均须为无 NUL 的有效 UTF-8；每次执行 stdout 与 stderr 合计最多 16 KiB，截断保持有效 UTF-8；异常文本最多 16 KiB，traceback 最多遍历 32 帧。普通异常和语法错误保留类型、虚拟文件名与行号，后续执行仍可使用该上下文。

进程主解释器保留至进程退出，不反复 `Py_Finalize()`。阶段四尚未实施，因此当前没有执行超时、文档累计输出预算、DOM 调用预算或硬内存限制。

## 4. Renderer 沙箱接入与实际修正

Windows x64 的 `ChromeContentRendererClient::PostSandboxInitialized()` 在默认关闭的 `NativePythonRuntimeSelfTest` 特性开启时运行固定诊断：

```text
--enable-features=NativePythonRuntimeSelfTest
```

日志以 `CPYTHON_RUNTIME_SELF_TEST` 开头，携带 JSON。检查实际进程 token 的 restricted 状态及完整性级别，要求不高于 Low；不依赖可能把 `--no-sandbox` 视作成功的宽泛沙箱判定。

首次实际沙箱验证失败，错误为 `failed to get random numbers to initialize Python`，而沙箱外 9 项单元测试已通过。CPython 默认调用 `BCryptGenRandom`；Chromium 自身的随机数实现使用可在 Renderer 中运行的 `ProcessPrng` 路径。

修正位于自有包装文件 `embedded_bootstrap_hash.c` / `embedded_entropy.cc`：将 CPython 的 Windows 熵调用接到 `base::RandBytes`。这覆盖哈希随机秘密和 `_PyOS_URandom` 路径，保持随机种子，没有设置固定哈希种子，也没有调整沙箱策略。上游源码和源码哈希保持不变。

其他实际构建修正包括：为 CPython 版本信息提供固定的构建日期 / 时间标识、提供空 `VPATH`、补齐被直接包含的 mimalloc C 源码、在关闭动态加载时排除 Windows 动态加载实现，以及补齐 Windows import bootstrap 依赖的 `winreg`。

## 5. 验证命令

从源码根目录运行：

```powershell
python third_party/cpython/verify_source.py
autoninja -C out/Debug chrome python_runtime_unittests
gn check out/Debug //components/python_runtime:*
gn check out/Debug //third_party/cpython:*
gn check out/Debug //chrome/renderer:renderer

out/Debug/python_runtime_unittests.exe --test-launcher-jobs=1 --test-launcher-timeout=60000 --test-launcher-summary-output=out/Debug/python-runtime-unittests.json

node chrome/test/python_runtime_smoke_test.mjs --output=out/Debug/python-runtime-smoke.json
```

`python_runtime_smoke_test.mjs` 启动独立临时 Profile 和本地 HTTP 服务，保持 Renderer 沙箱开启。分别检查默认关闭和诊断开启两种情况，并在子进程环境中设置无效 `PYTHONHOME`、污染 `PYTHONPATH`、可能写出标记文件的 `sitecustomize.py` 及非 UTF-8 stdio 设置。诊断应仍输出中文，隔离环境不执行标记文件。

脚本还检查页面正常加载、`globalThis.chromiumPlus` 保持 `0.0.1`、`<pyscript>` 仍是 `HTMLUnknownElement`。宿主设有外部 60 秒超时，超时时仅终止自己创建的浏览器进程树；这不是产品内的 Python 执行预算。

## 6. 结果与后续

| 检查 | 结果 |
| --- | --- |
| 官方压缩包校验、上游文件哈希 | 通过，775 个文件字节一致 |
| 从原压缩包重新导入 | 通过，manifest、GN 源码列表及冻结模块清单一致 |
| `chrome` 与 `python_runtime_unittests` 构建 | 通过 |
| 运行时、CPython、Renderer 的 GN 头文件依赖检查 | 通过；上游 C 集成按构建文件关闭 include checking |
| 运行时单元测试 | 9 / 9 通过，最终一轮共 1694 ms |
| 默认关闭 / 开启固定诊断的浏览器检查 | 2 / 2 通过，5 个实际沙箱 Renderer 报告全部通过 |
| stdout、traceback、隔离、截断、销毁后重建 | 全部通过 |
| 污染 Python 环境、未知标签与 chromiumPlus | 全部通过 |
| DLL 依赖与导出检查 | Chrome 导入 `python_runtime.dll`；该 DLL 无系统 Python DLL 依赖，仅导出 8 个封装符号 |
| 安装器 DLL staging | 通过，拷贝后 SHA-256 一致；完整安装器未构建 |
| chromiumPlus 完整回归，进程内网络服务诊断模式 | 15 / 15 通过 |
| chromiumPlus 完整回归，默认网络服务模式 | 启动超时，未通过，见下文 |

最终 `python_runtime.dll` 为 **7,561,216 字节**（约 7.21 MiB，不包含 PDB），SHA-256 为 `ac1326038c6f8190d06bdb2f7aca27c3a640fb4f41d452bbf7a1a51e06de3bcd`。5 个沙箱 Renderer 的冷初始化观测为 **52、54、73、74、74 ms**；包含初始化、3 个上下文的创建 / 销毁、计算、异常和截断检查的固定诊断分别耗时 **244–338 ms**。这些是本机 Debug 样本，不是 Release 性能或后续每个代码块的执行预算。

`chromiumPlus` 默认模式回归在上报前超时，浏览器日志出现独立网络服务崩溃 / 重启，与[此前记录](globalthis-chromium-plus-implementation.md)的环境问题一致；不能据此宣称该完整模式回归通过。使用既有诊断参数 `--enable-features=NetworkServiceInProcess2` 后 15 项通过，未改产品网络配置。原生 Python 的上述 2 项端到端检查使用默认网络服务配置，且始终保持 Renderer 沙箱开启。

原始及精简证据：

- [结果摘要](python-runtime-validation/results-2026-11-08.json)。
- [最终沙箱结果](python-runtime-validation/renderer-smoke.json)与[首次随机数初始化失败](python-runtime-validation/renderer-initial-failure.json)。
- [单元测试结果](python-runtime-validation/unittests.json)及[日志](python-runtime-validation/unittests.log)。
- [最终构建日志](python-runtime-validation/build.log)。
- [重新导入校验](python-runtime-validation/reimport-check.json)、[安装器 staging 检查](python-runtime-validation/installer-check.json)、[DLL 依赖](python-runtime-validation/dependencies.txt)。
- [chromiumPlus 诊断模式回归](python-runtime-validation/chromium-plus-in-process.json)与[默认模式失败日志](python-runtime-validation/chromium-plus-default-failure.log)。

后续进入阶段二时才注册标签、处理 RAWTEXT / 片段 / 预加载扫描、增加页面来源配置、检查脚本权限与 CSP，并接入安全的解析完成执行时机。阶段一的固定诊断开关不能作为页面执行开关。
