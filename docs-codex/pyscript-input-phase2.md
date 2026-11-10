# Python 阶段二：键盘、指针与页面状态事件

实现基线：`6f53a6b1c04d659959ea6c323c12416a9c2f0495`，Windows x64 Debug、component build、CPython 3.13.18。阶段一的帧接口、初始化限制、来源策略与 Renderer 沙箱继续生效。

本阶段延续 [马里奥迁移方案](pyscript-mario-implementation.md) 和 [原生动画帧](pyscript-animation-phase1.md)，让页面 Python 维护连续输入状态。游戏绘图、Canvas 批次与外部马里奥源码迁移属于后续阶段。

## 接口与目标

```python
import dom

document = dom.get_document()       # dom.DocumentHandle
window = dom.get_window()           # dom.WindowHandle
button = dom.get_element_by_id('pad')

def key(event):
    if (event.code == 'Space' and not event.target_is_editable
            and not event.target_in_dialog and not event.is_composing):
        event.prevent_default()

listener = dom.add_event_listener(document, 'keydown', key,
                                  once=False, passive=False, capture=False)
# dom.remove_event_listener(listener)
# dom.set_pointer_capture(button, event.pointer_id)
# dom.release_pointer_capture(button, event.pointer_id)
page = dom.get_page_state()
```

| 目标类型 | 支持事件 |
| --- | --- |
| DocumentHandle | `keydown`、`keyup`、`visibilitychange` |
| WindowHandle | `blur`、`focus`、`resize` |
| 普通 HTML 元素 | 既有 `click`，新增 `input`、`change`、`pointerdown`、`pointerup`、`pointermove`、`pointercancel`、`gotpointercapture`、`lostpointercapture` |

三个目标类型均为所属解释器的不可构造句柄，带文档身份、代数和目标类别。Document / Window 不接受 `set_text` 或指针捕获。监听及捕获仍排除脱离文档、跨文档、Shadow 子树、自定义元素及 `is` 元素。监听移除、安装失败回滚和一次性消费继续使用原有不透明回调表。

`once`、`passive`、`capture` 必须是 bool，只接受关键字参数。原生监听显式设置 capture / passive，沿用正常事件路径、顺序和派发中增删语义；once 在 Python 获准进入之后消费。返回 `False` 继续不隐式阻止默认行为。

## 事件快照

`dom.CallbackEvent` 保持只读，可保存字段，但不保存原生 Event 或 Node 指针。各类别无关字段取空字符串、0 或 False。

| 类别 | 字段 |
| --- | --- |
| 公共 | `type`、`current_target`、`is_trusted`、`cancelable`、`bubbles`、`default_prevented`、`passive`、`input_epoch` |
| 目标元数据 | `target_is_editable`、`target_in_dialog` |
| 键盘 | `key`、`code`、`repeat`、`is_composing`、`alt_key`、`ctrl_key`、`meta_key`、`shift_key` |
| 指针 | `pointer_id`、`pointer_type`、`button`、`buttons`、`client_x`、`client_y`、`is_primary`及修饰键 |
| 表单 | `value`、`checked`、`value_truncated`，仅在 input / change 时读取 |

`current_target` 是注册监听的目标，可能是 Document / Window，并不承诺是实际输入元素。目标元数据与表单状态从 Blink 正常重定向后的 `event.target()` 读取，不向 Python 暴露内部目标节点。

`target_is_editable` 是保守的键盘让行提示：包含可编辑区域、input / textarea / select、按钮、有链接目标的 anchor 和 Shadow host。该提示不用于权限判断，避免游戏截获原生控件空格激活、链接或 Shadow 控件键盘行为。`target_in_dialog` 表示重定向目标位于打开的 dialog 中。示例还让行输入法组合、Alt / Ctrl / Meta 快捷键；Tab 不属于游戏控制键。

事件字符串上限 16 KiB UTF-8，原生快照先限制转换长度，再按 UTF-8 边界截断。表单值截断由 `value_truncated=True` 明示。运行时额外拒绝无效 UTF-8、超限字符串及非有限坐标。`value` 不通过键盘 / click 快照泄露。快照中的 `default_prevented` 表示该监听进入时的值，调用方法后不会把原快照改为可变对象。

## 同步事件控制与捕获

`event.prevent_default()`、`stop_propagation()`、`stop_immediate_propagation()` 通过当前同步派发的控制令牌调用原生 Event。Python 不可读取令牌，也不能构造事件。回调返回后立即撤销原生令牌，早于日志、输出和控制器清理；保存的旧 event 在后续事件、动画帧或文档初始化调用中不能重新控制原生事件。

非 cancelable 的 prevent_default 是无操作；passive 监听调用 prevent_default 抛出 PermissionError。其余两个方法遵守原生传播语义。已失效令牌抛出 ReferenceError；原生 Event 的临时传播标志在派发结束时按 Blink 正常逻辑清除，取消默认行为及后续监听器是否执行才是跨派发的可观测结果。

捕获 API 要求普通元素和严格的 int32 `pointer_id`，不接受 bool / float；调用原生 `setPointerCapture` / `releasePointerCapture` 检查活动指针、文档连接和指针锁等条件。脚本构造 PointerEvent 不能创造原生活动指针。最多记录 32 项 Python 捕获，元素弱引用；每次申请清理失效记录。导航、文档停止、致命超限、真实窗口失焦或页面可见性变化会释放仍由记录元素持有的捕获，已经转移给其他元素的捕获不会被误释放。释放只改变原生捕获状态，不同步进入 Python；后续捕获事件继续受全局重入门禁约束。

## 输入复位、页面状态与额度

`dom.get_page_state()` 返回不可构造、只读的 `PageState`：`hidden`、`has_focus`、CSS 像素 `width` / `height`、`reduced_motion`、`input_epoch`。尺寸来自 FrameView 已发布的视口并按布局缩放换算，不强制同步布局；首次布局前取当前可用尺寸，后续 resize / 帧查询反映更新。减少动态效果状态使用 Blink 动态 MediaValues，保留 DevTools 媒体偏好覆盖。

窗口压力、禁止脚本、重入、策略等原因可能跳过 keyup / pointercancel，不能依赖收到每个事件维护集合。本阶段增加文档级单调 `input_epoch`：输入派发被跳过时推进，原生窗口 blur / Document visibilitychange 观察器也推进；这两个观察器不依赖 Python 回调获准进入。原生窗口观察器过滤子元素的 blur，避免把普通控件焦点变化误当窗口失焦。后续事件及页面状态可读到新值，Python 应清空先前 held keys / pointers。没有排队、补发历史输入或绕过预算。

示例每帧读取页面状态；代数变化、隐藏或失焦即清空输入，约每 100 ms 更新一次文字 HUD。在 keyup、pointerup、pointercancel、lostpointercapture 和页面状态事件中也清理集合。该策略可能在过载时丢弃一次输入，但不会把遗漏的释放事件变成持续移动。

所有新增操作，包括读取目标 / 页面状态、控制事件和指针捕获，都计入现有 DOM 调用额度并检查调用前后活动状态。输入与 click 共用 50 ms 单次、128 次 DOM、4 KiB 输出；与动画帧共用最近一秒 250 ms、1,024 次 DOM、16 KiB 输出。帧继续使用 8 ms 单次、200 ms 帧子窗口。设置页预算说明同步改为包含键盘、指针等输入事件。协作式时间保护、现有站点 / CSP 门禁及 Renderer 沙箱保持生效。

## 示例、验证与实施记录

无 JavaScript 示例：[input-state.html](../chrome/test/data/pyscript/input-state.html)。需要按现有设置允许页面来源并启用 Python。页面可记录左右 / 跳跃、repeat、鼠标 / 触摸及表单状态；触控区使用 `touch-action: none`，允许显式捕获并拖出元素。

独立真实浏览器验证：

```powershell
node chrome/test/pyscript_input_smoke_test.mjs
```

统一构建与回归入口：

```powershell
python chrome/test/run_pyscript_event_validation.py
```

本阶段已完成。统一验收于 **2026-11-10 21:50:49–21:56:27（Asia/Shanghai）** 执行，标准五目标增量构建退出码 0，耗时 69.86 秒；保留正常 Debug、PDB 及链接检查，并非从空输出目录重新构建。

| 组 | 通过 / 实际 | 耗时（秒） |
| --- | --- | --- |
| 运行时 | 61 / 61 | 4.94 |
| Chrome 文档适配 | 12 / 12 | 2.84 |
| Blink Python 元素 / 输入 / 生命周期 | 74 / 74 | 28.88 |
| CSP 定向回归 | 3 / 3 | 3.72 |
| Python 设置单测 | 8 / 8 | 0.14 |
| click 浏览器回归 | 34 / 34 | 33.70 |
| 输入浏览器验证 | 25 / 25 | 22.23 |
| 动画帧浏览器回归 | 19 / 19 | 35.92 |
| DOM 浏览器回归 | 17 / 17 | 30.09 |
| 解析 / 权限浏览器回归 | 27 / 27 | 42.27 |
| 设置 / 重启 / 页面模式 / 沙箱浏览器回归 | 12 / 12 | 57.75 |

合计 **158 项单测、134 项浏览器测试，共 292 项全部通过**。验证前后功能源码与测试输入哈希一致，包含尚未提交的输入测试与示例。全部浏览器组记录 `sandboxDisabled=false`；设置验证还检查放开页面限制后的 Renderer 沙箱令牌。C++ 格式、六组 checkdeps、JavaScript / Python 语法检查均通过。

主要证据：

- [统一结果、执行命令与源码 / 二进制哈希](pyscript-input-validation/final/summary.json)。
- [输入 25 项结果](pyscript-input-validation/final/input.json)、[示例截图](pyscript-input-validation/final/input.json.sample.png)及 [Blink 74 项结果](pyscript-input-validation/final/blink.json)。
- [静态检查](pyscript-input-validation/diagnostics/final-static-checks.json)、[对象恢复清单](pyscript-input-validation/diagnostics/final-object-recovery.json)、[诊断索引](pyscript-input-validation/README.md)及[原始 / 归档哈希](pyscript-input-validation/archive-manifest.json)。

输入浏览器组使用 CDP 实际键盘、鼠标和触摸输入，覆盖长按、repeat、keyup、阻止默认滚动、文本框与复选框原生行为、按钮空格激活、组合键 / 输入法字段、Shadow 控件让行、指针拖出捕获区后的释放、显式释放与触摸取消。页面状态组覆盖 resize、减少动态效果偏好、真实失焦、隐藏 / 冻结和恢复；过载组验证遗漏 keyup 后 `input_epoch` 清理 held state。另验证保存事件在后续事件 / 帧中失效、passive 限制、传播顺序、致命停止、捕获所有权转移、导航和正常关闭。脚本构造的事件另行验证 `is_trusted=False`，不能获得原生活动指针。测试驱动使用的 JavaScript 不属于示例页面运行依赖。

首次统一回归暴露旧 click 的错误类型兼容性问题：新增目标解析把 SVG / 自定义元素注册提前映射为 ReferenceError，而旧接口承诺 PermissionError。已在注册入口先区分失效句柄与不允许的元素，派发时继续严格重新检查目标；新增 Blink 回归覆盖该区别，最终 click 34 项通过。没有修改旧测试来掩盖该行为变化。

早期独立输入探测两次触发初始化 100 ms 协作式时间限制；其失败结果与后续独立 25 项通过结果均保留。生产额度未放宽，这些结果也不代表所有机器、冷启动负载或 Debug 构建下都能避免时间超限。初次编译中的原生监听选项、清理回调与 Blink 字符串 / 表单 API 兼容问题已修正，失败日志仍归档。

构建过程中共定位并定向重建 **18 份异常对象**，涉及 Core、Bindings、测试支持及存储对象；表现包括乱码符号、COFF 字符串 / 节名和 CodeView 数据异常。每份原件均先保留并记录重复读取 / mmap 一致的哈希，再删除对应生成对象并重建。最后一份为 `obj/base/i18n/test_support/locales_for_test.obj`，节 1453 的异常节名与链接错误一致；恢复后 `blink_unittests.exe` 和标准五目标构建通过。最终清单记录全部原件 / 重建 SHA-256，18 份均已变化；对象原件及完整响应文件保留于本机忽略目录 `out/Debug/pyscript-input-phase2/`。异常根因尚未确定，没有修改这些对象对应的功能源码，也未关闭 Debug / PDB 检查。

本轮采用 Windows x64 Debug 的真实 headless 浏览器，验证功能、权限与生命周期；不构成 Release、长期运行或完整游戏性能结论。外部马里奥源码 SHA-256 仍为 `53b556316bbc25a5c2c80b046be836dbaaea8aa5c25e85a2cf5f29a4b4bc9ef1`。下一阶段为 Canvas 2D 子集、批量提交与绘图预算。
