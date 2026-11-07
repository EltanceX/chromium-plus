# globalThis.chromiumPlus 默认字段实现方案

- 整理日期：2026-11-07
- 项目：Chromium 158.0.8126.0，Windows x64 Debug 组件构建
- 当前分支：`codex/kernel-debug-settings`
- 当前功能基线：`54c778f5c2`（内核调试设置页、实时状态和标签页交互）
- 状态：已实现页面与 iframe 主世界，以及 Dedicated Worker、Shared Worker、Service Worker；Debug 构建成功，15 项浏览器检查通过。验证条件见第 9 节。

## 1. 目标与结论

在目标 JavaScript 上下文创建后、业务脚本执行前，向全局对象添加字符串属性，使脚本默认可以读取：

```js
globalThis.chromiumPlus === '0.0.1'; // true
```

可以实现。推荐在 Chrome Renderer 层使用 V8 C++ API 定义属性，复用现有上下文生命周期回调。固定版本字符串直接由 Renderer 提供，无需经过 Settings WebUI Handler 或跨进程通信。

每个页面、iframe、Worker 和隔离世界都有自己的全局环境。因此，“添加到 globalThis”需要明确覆盖哪些上下文，不能只在一个设置页里赋值。

## 2. 历史背景

已读取 Codex 会话“chromium1”的全部 6 轮记录。已有工作依次完成：

1. 分析 Chromium 的目录、进程结构和 Debug 构建。
2. 在设置页侧栏增加“内核调试”，页面地址为 `chrome://settings/kernelDebug`，按当时要求显示 `Hello chromium` 标题。
3. 通过 BrowserProxy、`chrome.send()` 和 C++ Settings Handler 展示浏览器内部状态，每 2 秒更新一次。
4. 增加标签页详情与关闭交互，使用稳定的标签句柄，并校验当前 Profile。

历史会话包含成功执行 `autoninja -C out/Debug chrome` 的记录；新增标签页交互当时尚未完成界面实测。上述现有改动已保存到提交 `54c778f5c2`，共 14 个文件、增加 756 行。

新字段属于 Renderer 中 JavaScript 全局环境的初始化，与现有设置页的浏览器进程状态通信属于不同接入路径。

## 3. 推荐覆盖范围

| 环境 | 接入方式 | 说明 |
| --- | --- | --- |
| 普通页面与 iframe 的主世界 | `ChromeRenderFrameObserver::DidCreateScriptContext()` | 第一阶段实现；每个 frame 创建上下文时分别初始化 |
| Dedicated Worker、Shared Worker、Service Worker | `ChromeContentRendererClient::DidInitializeWorkerContextOnWorkerThread()` | 第二阶段复用同一初始化函数，并分别验证经典脚本和模块脚本路径 |
| 扩展内容脚本的隔离世界 | 根据产品需要调整 world 过滤条件 | 默认方案只提供给页面主世界 |
| Worklet、其他特殊 realm | 核对各自上下文创建路径后补充 | 不将 Worker 接入等同于全部 Worklet 已覆盖，主线程 Worklet 尤其需要单独确认 |
| 独立 V8 环境，例如 d8 | V8 初始化层 | 若产品明确要求再考虑，涉及更广的初始化、快照和测试范围 |

推荐先实现页面及 iframe 主世界，再加入 Worker。使用 Chrome Renderer 层的公共初始化函数，把版本字符串集中维护在一处。

当前版本的相关源码：

- [页面上下文回调](../chrome/renderer/chrome_render_frame_observer.cc)：`ChromeRenderFrameObserver::DidCreateScriptContext()`。
- [每个 RenderFrame 的观察者创建](../chrome/renderer/chrome_content_renderer_client.cc)：`ChromeContentRendererClient::RenderFrameCreated()`。
- [Blink 窗口上下文初始化](../third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc)：`LocalWindowProxy::Initialize()`。
- [Worker 回调接口](../content/public/renderer/content_renderer_client.h)：`DidInitializeWorkerContextOnWorkerThread()`。
- [Worker 回调转发](../content/renderer/renderer_blink_platform_impl.cc)：`RendererBlinkPlatformImpl::WorkerContextCreated()`。
- [Worker 线程初始化](../third_party/blink/renderer/core/workers/worker_thread.cc)：创建脚本上下文后调用 `Platform::Current()->WorkerContextCreated()`。

## 4. 属性语义

最小方案采用普通数据属性：

```js
Object.getOwnPropertyDescriptor(globalThis, 'chromiumPlus');
// {
//   value: '0.0.1',
//   writable: true,
//   enumerable: true,
//   configurable: true
// }
```

页面可以修改或删除该字段；新建上下文时重新获得默认值。该行为接近在全新全局对象上执行 `globalThis.chromiumPlus = '0.0.1'`。

如果后续要求固定版本标识，可改用 `DefineOwnProperty()`：

| V8 属性标志 | 效果 |
| --- | --- |
| `v8::None` | 可写、可枚举、可配置 |
| `v8::ReadOnly` | 不可写 |
| `v8::DontEnum` | 不可枚举 |
| `v8::DontDelete` | 不可配置 |

标志可以组合。是否只读、是否可枚举、是否可删除，应在实现前选定，并与测试预期保持一致。即使只读，也应将其用于功能或版本识别，不作为权限判断依据。

## 5. 页面主世界的最小实现

修改 `chrome/renderer/chrome_render_frame_observer.cc`，保留现有两个 bindings 安装调用，在其后追加字段初始化。

以下使用 `v8.h` 展示完整示例；正式实现时可按仓库风格使用所需的细分 V8 头文件。

```cpp
#include "content/public/common/isolated_world_ids.h"
#include "v8/include/v8.h"

void ChromeRenderFrameObserver::DidCreateScriptContext(
    v8::Local<v8::Context> context,
    int32_t world_id) {
  BenchmarkingBindings::InstallConditionally(context);
  LoadTimesBindings::Install(context);

  // 仅向页面主世界提供，保留原有 bindings 的安装行为。
  if (world_id != content::ISOLATED_WORLD_ID_GLOBAL) {
    return;
  }

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);

  context->Global()
      ->CreateDataProperty(
          context,
          v8::String::NewFromUtf8Literal(isolate, "chromiumPlus"),
          v8::String::NewFromUtf8Literal(isolate, "0.0.1"))
      .Check();
}
```

实现要点：

- `context->Global()` 获取当前上下文的全局对象；在页面中可通过 `globalThis.chromiumPlus` 或 `window.chromiumPlus` 读取。
- world 过滤只作用于新字段，不能提前跳过原有 bindings 的安装。
- 不限制为顶层 frame，以便 iframe 也获得该字段；跨站 iframe 在所属 Renderer 中分别初始化。
- 在上下文初始化期间定义原生属性，避免等待页面加载完成后再注入脚本造成时序差异。
- 当前示例假定新上下文内该属性尚未被其他初始化逻辑定义；正式实现需要确认冲突策略和失败处理。
- 当前版本的 V8 不提供 `Context::GetIsolate()`，实现按仓库现有 bindings 使用 `v8::Isolate::GetCurrent()`，并在上下文所属线程调用。
- 只修改现有 `.cc` 文件时，通常无需新增 GN source 条目。

## 6. 公共初始化函数与 Worker 扩展

需要页面与 Worker 一致时，建议提取公共函数，例如新增：

```text
chrome/renderer/chromium_plus_bindings.h
chrome/renderer/chromium_plus_bindings.cc
```

公共函数接收 `v8::Local<v8::Context>`，在当前线程、当前上下文中定义属性，并集中维护版本常量。调用位置为：

1. 页面：现有 `DidCreateScriptContext()` 完成主世界判断后调用。
2. Worker：在 `ChromeContentRendererClient` 的头文件声明并实现以下 override，再调用同一公共函数：

   ```cpp
   void DidInitializeWorkerContextOnWorkerThread(
       v8::Local<v8::Context> context) override;
   ```

3. 将新增文件加入 `chrome/renderer/BUILD.gn` 的适当目标，核对 V8 相关依赖。

公共函数在传入上下文所属线程获取当前 isolate，不在不同线程之间共享 V8 Local 句柄。固定字符串初始化不需要访问浏览器进程对象。

已有 Service Worker 专用生命周期回调也可用于更细的控制；采用通用 Worker 回调时，应验证实际覆盖范围，避免在多个回调里重复安装。Worklet 覆盖另行核实。

## 7. 构建与验证

### 7.1 增量构建

当前 `out/Debug/args.gn` 使用 Windows x64、`is_debug = true`、`is_component_build = true`。源码修改后运行：

```powershell
cd C:\src\src
autoninja -C out/Debug chrome
```

构建系统按依赖关系重编受影响的源文件和目标；修改公共头文件可能扩大重编范围。构建完成后重启对应 Debug Chromium，验证新建上下文中的行为。

### 7.2 基本检查

在网页控制台执行：

```js
globalThis.chromiumPlus;
// '0.0.1'

typeof globalThis.chromiumPlus;
// 'string'

Object.hasOwn(globalThis, 'chromiumPlus');
// true

Object.getOwnPropertyDescriptor(globalThis, 'chromiumPlus');
// 与选定的属性权限一致
```

还应将断言放在测试页面的首个脚本中，确认字段在业务脚本启动前已经存在，而非只在加载完成后的控制台中可见。

### 7.3 覆盖矩阵

| 场景 | 验证目标 |
| --- | --- |
| 普通 HTTP/HTTPS 页面的首个脚本 | 值为字符串 `0.0.1` |
| 刷新、完整导航、跨站导航 | 新上下文初始化正确 |
| 同源 iframe、跨站 iframe | 各自脚本中可读取；跨站结果由 iframe 自身回传，遵守同源策略 |
| `about:blank`、`srcdoc`、弹窗初始文档 | 确认特殊和初始空文档的上下文创建时序 |
| 同文档导航与 BFCache 恢复 | 普通可写方案应遵循原上下文的属性状态，不假定每次恢复都会重置 |
| 字段覆盖与删除 | 符合第 4 节选定的权限；新上下文恢复默认值 |
| 扩展隔离世界 | 符合所选暴露范围，不依赖主世界属性自动同步 |
| Dedicated Worker、Shared Worker、Service Worker | 实现扩展后，由 Worker 脚本自身读取并回传结果 |
| 经典 Worker 与模块 Worker | 分别确认顶层脚本执行前字段已安装 |

建议加入有针对性的浏览器或 Renderer 测试，覆盖首脚本可见性、frame 隔离和所支持的 Worker 类型。现有设置页的历史构建成功不代表新字段方案已经验证通过。

## 8. 建议实施顺序

1. 确定默认数据属性权限和目标上下文范围。
2. 实现页面与 iframe 主世界初始化，验证首脚本和导航行为。
3. 提取公共安装函数，加入 Worker 初始化回调，集中维护版本值。
4. 完成所选环境的运行验证与针对性测试。
5. 将新字段实现作为独立提交保存，便于与设置页功能分别追踪和回退。

## 9. 实施与验证记录（2026-11-07）

### 9.1 已实现内容

- 公共函数：`chromium_plus::InstallBindings()`，位于 `chrome/renderer/chromium_plus_bindings.h/.cc`。
- 版本值集中维护为 `chromium_plus_bindings.cc` 中的 `kVersion = "0.0.1"`。
- 页面入口：`ChromeRenderFrameObserver::DidCreateScriptContext()`，仅在 `ISOLATED_WORLD_ID_GLOBAL` 安装新字段，保留原有 bindings 行为。
- Worker 入口：新增 `ChromeContentRendererClient::DidInitializeWorkerContextOnWorkerThread()` override，调用同一公共函数。
- 新文件已加入 `chrome/renderer/BUILD.gn`。
- 使用 `CreateDataProperty()` 创建可写、可枚举、可配置的数据属性。初始化函数只在新上下文创建回调调用一次；安装失败按 V8 `Maybe<bool>::Check()` 处理。

### 9.2 构建和静态检查

- `autoninja -C out/Debug chrome`：成功完成 C++ 编译及 `chrome.dll` 链接。
- `python buildtools/checkdeps/checkdeps.py chrome/renderer`：通过。
- `git diff --check`：通过。

### 9.3 浏览器运行检查

新增可独立运行的 [烟雾测试脚本](../chrome/test/chromium_plus_smoke_test.py)，仅依赖 Python 标准库。它启动本地 HTTP 测试服务与独立临时 Profile 的无头 Chromium，收集页面和 Worker 自身脚本的检查结果。测试页面及脚本位于 `chrome/test/data/chromium_plus/`。

常规命令：

```powershell
python chrome/test/chromium_plus_smoke_test.py --chrome-binary out/Debug/chrome.exe --output out/Debug/chromium-plus-smoke-results.json
```

本机两次默认模式测试均在初始导航阶段超时，浏览器日志记录了独立网络服务崩溃和重启。使用以下测试参数后，15 项检查全部通过：

```powershell
python chrome/test/chromium_plus_smoke_test.py --chrome-binary out/Debug/chrome.exe --output out/Debug/chromium-plus-smoke-results.json --chrome-arg=--enable-features=NetworkServiceInProcess2
```

`NetworkServiceInProcess2` 仅用于这次无头验证的启动参数，未修改产品源码中的网络服务配置。默认网络服务模式下的运行验证仍未完成。

已通过的 15 项检查：

1. 页面首段脚本的字段值和属性描述符。
2. 改写、同文档导航保持改写值、删除字段。
3. 初始 `about:blank` iframe。
4. 初始 `about:blank` 弹窗。
5. `srcdoc` 首段脚本。
6. 同源 iframe 首段脚本。
7. 跨站 iframe 首段脚本（启用 `--site-per-process`）。
8. 限制脚本来源的 CSP 页面。
9. Dedicated Worker，经典脚本。
10. Shared Worker，经典脚本。
11. Service Worker，经典脚本。
12. Dedicated Worker，模块脚本。
13. Shared Worker，模块脚本。
14. Service Worker，模块脚本。
15. 页面完整刷新后，在首段脚本执行前恢复默认值。

尚未对扩展隔离世界、BFCache 恢复或全部 Worklet 路径进行运行验证。字段实现与测试的具体提交版本可通过 Git 历史追踪。
