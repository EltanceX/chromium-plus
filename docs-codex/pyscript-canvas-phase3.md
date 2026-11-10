# 原生 Python Canvas 2D：阶段三实现记录

本阶段已完成，接续 [Mario 总方案](pyscript-mario-implementation.md)，为页面 Python 添加 Canvas 2D 子集、有类型批量提交、受限缓存表面与绘图预算。页面代码直接经过 CPython → Chrome 适配 → Blink 原生 Canvas，不生成或执行 JavaScript。

## 使用方式

```html
<canvas id="game" width="640" height="360"></canvas>
<pyscript>
import canvas, dom

screen = canvas.get_context_2d(dom.get_element_by_id('game'))
sprite = canvas.create_surface(16, 16)
b = canvas.batch()
b.fill_style('#e96041')
b.fill_rect(2, 2, 12, 12)
canvas.submit(sprite, b)

b = canvas.batch()
sky = b.linear_gradient(0, 0, 0, 360)
sky.add_color_stop(0, '#92d0df')
sky.add_color_stop(1, '#e8edc8')
b.fill_style(sky)
b.fill_rect(0, 0, 640, 360)
b.smoothing(False)
b.draw_surface(sprite, 80, 268, 48, 48)
stats = canvas.submit(screen, b)
print(stats)
</pyscript>
```

解释器开关、来源授权、CSP、内联标签规则、Renderer 沙箱与已有生命周期门禁继续生效。完整能力样例见 [canvas-scene.html](../chrome/test/data/pyscript/canvas-scene.html)：三个缓存表面绘制渐变背景、云层和 16 × 16 像素角色；原生动画帧驱动移动方块，左右键移动角色，HUD 低频更新。

从仓库根目录启动本地样例服务，并在另一终端启动本次构建的浏览器：

```powershell
python -m http.server 8765 --bind 127.0.0.1 --directory chrome/test/data/pyscript
```

```powershell
out/Debug/chrome.exe --enable-blink-features=PythonScript --native-python-allowed-origin=http://127.0.0.1:8765 http://127.0.0.1:8765/canvas-scene.html
```

### 接口与提交语义

模块函数为 `get_context_2d(element)`、`create_surface(width, height)`、`resize(context, width, height)`、`release(surface)`、`batch()`、`submit(context, batch)`。

**每次成功提交替换完整画布，并从默认绘图状态执行该批次。** 原生实现先校验全部命令、引用资源与额度，再调用 `reset()` 和绘图操作。应每帧描述完整画面；静态背景、角色姿态等使用缓存表面，不能依赖上一批遗留的像素或绘图状态。

- 非法批次、失效目标 / 来源或窗口额度不足，在绘制前拒绝，保留旧画面。
- 原生执行过程中持续检查文档活动状态、context loss 与异常；此类中途失败不承诺回滚。
- 首次调用 `submit` 即封存批次，成功或失败后都不能追加命令；同一已封存批次可反复提交。
- `resize` 清空位图与状态，返回新版本句柄。必须保存返回值；旧句柄失效。
- 外部尺寸变化也使旧句柄失效。可重新 `get_context_2d` 取得新版本；外部同尺寸重置没有可继续依赖的绘图状态。
- `release` 仅允许释放 `create_surface` 创建的缓存表面；普通 DOM 画布由页面持有。
- 丢弃 Python Context2D 值不会自动释放缓存资源槽位；不再使用时应显式 `release`。文档停止 / 导航也会撤销资源表。
- 首次获取普通画布时要求尚无渲染上下文，避免接管 JavaScript、WebGL 等已使用的画布。Canvas 不因此与 JS 隔离：页面脚本仍可操作其 DOM / 原生上下文。

`submit` 返回 `{commands, bytes, pixels, native_ms}`：真实命令条数、逻辑提交字节数、保守像素成本、Blink 校验及提交耗时。`native_ms` 不包含 Chrome 命令转换、Python 构建批次及最终异步 GPU 栅格化，也不等于整帧耗时。

### Batch 子集

| 类别 | 方法 |
| --- | --- |
| 矩形 | `clear_rect(x,y,w,h)`、`fill_rect(...)`、`stroke_rect(...)` |
| 路径 | `begin_path()`、`close_path()`、`move_to(x,y)`、`line_to(x,y)`、`arc(x,y,r,start,end[,anticlockwise])`、`ellipse(x,y,rx,ry,rotation,start,end[,anticlockwise])`、`fill()`、`stroke()` |
| 状态与变换 | `save()`、`restore()`、`translate(x,y)`、`scale(x,y)`、`rotate(radians)` |
| 样式 | `fill_style(color_or_gradient)`、`stroke_style(...)`、`line_width(n)`、`line_dash(list_or_tuple)`、`alpha(n)` |
| 渐变 | `linear_gradient(x0,y0,x1,y1)` 返回批次局部 Gradient；`gradient.add_color_stop(offset,color)` |
| 文字 | `font(size_px[,generic_family[,bold]])`、`text_align(value)`、`text_baseline(value)`、`fill_text(text,x,y)` |
| 缓存复制 | `smoothing(bool)`、`draw_surface(source,x,y,width,height)` |

颜色接受 `#RGB`、`#RGBA`、`#RRGGBB`、`#RRGGBBAA`。字体仅接受 `monospace`、`sans-serif`、`serif`；加粗参数为 bool。对齐和基线沿用相应原生 Canvas 枚举字符串。首版不开放外部图片、任意字体下载、阴影、滤镜、复合模式、任意成员反射或像素读回给 Python。

所有数字必须为精确 int / float 类型，拒绝 bool、子类、NaN、Infinity 与绝对值超过 1,000,000 的参数；布尔值必须为 bool，字符串必须为精确 str。避免 `__float__`、`__str__` 等参数转换触发重入 Python。数组仅接受有界 list / tuple。

## 实现与资源生命周期

- `components/python_runtime/python_canvas_module.cc`：多阶段内建模块；每解释器独立的 Context2D / Batch / Gradient 类型与 GC 遍历。命令只存普通 C++ 数据；不把 `PyObject*` 交给 Blink。
- `python_canvas_types.h` 与 `web_python_canvas_types.h`：明确操作码、固定数值参数、UTF-8 字符串、数值数组和不透明句柄。
- `chrome/renderer/python_document_context.cc`：逐项验证枚举对应关系，转换命令、句柄、统计和错误。
- `core/html/python_canvas_bridge.*`：modules 工厂接口，保持 core 不依赖 modules。
- `modules/canvas/canvas2d/python_canvas_{batch,bridge_impl}.*`：纯命令预检 / 成本统计与原生 Canvas 调用。
- `python_script_controller.*`：文档授权、DOM 计费、交互执行入口重置单次绘图额度、停止及导航清理。

句柄包含 Document 身份、控制器代数、资源槽位与版本。普通画布使用弱引用；缓存表面由桥接强持有，仍归所属 Document。节点脱离、跨文档收养、进入 Shadow DOM、定制元素状态或 `is` 属性不符合条件、来源不干净及尺寸变化时，旧句柄不可用于提交。表面引用禁止 self-copy，并在目标像素改变前逐一验证。

导航、文档停止与 Dispose 撤销资源表，释放桥接对缓存的引用；最终物理内存释放由 Blink / Skia / GPU 生命周期处理。Python finalizer 不能重新取得活动 native host。

## 固定额度

| 范围 | 上限 |
| --- | --- |
| 单批 | 4,096 命令、512 KiB 逻辑数据 |
| 单解释器 | 64 个存活 Batch |
| 单批渐变 | 32 个，每个最多 16 个停靠点，偏移 [0,1] |
| save 栈 | 32 层；批次结束必须配平 |
| 路径 | 单路径 2,048 点、单批 8,192 点；arc / ellipse 各按 64 点计 |
| 字符串 / 文字 | 每字符串最多 1,024 UTF-8 字节；单批文字合计 16 KiB |
| 虚线 / 线宽 / 字号 | 最多 16 个非负 dash，合计 ≤ 4,096；线宽 (0,128]；字号 [1,128] px |
| 表面 | 最多 8 个，单边 [1,2048]，总计 8 Mi 像素 |
| 单次执行累计 | 8,192 命令、2 MiB、32 Mi 像素估算、32,768 work |
| 最近 1 秒累计 | 131,072 命令、16 MiB、512 Mi 像素估算、1,048,576 work |

每命令基础计 128 字节，加字符串 UTF-8 字节数及 double 数组长度 × 8。该值是协议逻辑数据量，不是 Python / C++ 分配器内存测量。

像素成本包括整画布 reset、变换后与画布裁剪的矩形 / 路径包围盒、描边扩展、按整画布保守计费的文字，以及缓存复制的目标面积和完整来源面积。work 除命令条数外，还计入路径点、重复 fill / stroke 的路径复杂度与文字字节数，防止以一条命令隐藏大量工作。

总表面 8 Mi 像素对应逻辑 RGBA8 约 32 MiB；不代表整个 Renderer、Python 堆、Skia 缓存或 GPU 实际内存硬上限。

单次绘图超限映射为 `kDrawingLimit` 并锁存 `StopReason::kDrawing`，Python 捕获异常也不能恢复。窗口不足返回可恢复 RuntimeError，该批不绘制，可在后续入口重试。窗口账本最多 1,024 条，满时合并末条并保守延后到期。初始标签共用初始化累计额度；每次输入回调 / 动画帧重置单次额度。

既有初始化约 100 ms、输入约 50 ms、帧约 8 ms 及整文档交互窗口预算继续生效，原生提交前后检查时间与 DOM 额度。单次 `submit` 计一次 DOM 调用，内部所有命令仍完整计入绘图预算。时间保护是协作式，不能承诺中断任意正在执行的 C / Skia 操作。

## 验证记录

2026-11-10 在 Windows x64 Debug / component build、CPython 3.13.18 下完成标准五目标构建与统一验收，未关闭 Renderer 沙箱或 Debug / PDB 检查。统一入口：

```powershell
python chrome/test/run_pyscript_event_validation.py --output-dir out/Debug/pyscript-canvas-phase3/final
```

| 验证组 | 通过数 |
| --- | ---: |
| Python Runtime | 67 |
| Chrome 适配 | 15 |
| Canvas 预检 / 预算 / 资源桥接 | 19 |
| 既有原生 Canvas 状态回归 | 2 |
| Blink 标签与执行控制 | 74 |
| CSP | 3 |
| 设置单测 | 8 |
| click / 事件浏览器组 | 34 |
| 输入浏览器组 | 25 |
| Canvas 浏览器组 | 23 |
| 动画浏览器组 | 19 |
| DOM 浏览器组 | 17 |
| 解析浏览器组 | 27 |
| 设置浏览器组 | 12 |

合计 **188 项单测、157 项浏览器测试，共 345 项全部通过**。验证前后功能源码 / 测试输入哈希一致；源码覆盖范围已纳入新 Canvas 代码、相关构建文件与工厂初始化，二进制哈希另包含 Modules、Platform 与 Python Runtime。全部浏览器组记录 `sandboxDisabled=false`；设置组继续检查 Renderer 沙箱令牌。C++ / GN 格式、9 组 checkdeps、JavaScript / Python 语法检查均通过。

新增验证覆盖运行时类型 / GC / finalizer、Chrome 转换、整批预检、可控时钟下的各项窗口边界及 1,024 条账本合并、真实原生 Canvas 像素结果、resize / 失效 / 资源数量 / 像素边界、动画输入及导航。浏览器还验证了外部 JavaScript 污染 canvas 后 Python 句柄失效。精确绘图窗口到期通过单测注入时钟验证，不将受 CPU / 调度影响的浏览器耗时当作该边界的确定性证据。

浏览器脚本中的 CDP JavaScript 只作为外部测试驱动，能力样例不包含可执行 JavaScript。冻结测试同时恢复真实可见性：CDP 的 `active` 只解冻，不撤销 `frozen` 引入的隐藏状态。

最终运行的抽样数据如下；它们是本机 Debug 的单次观测，不是性能保证：

| 场景 | 命令 | 逻辑字节 | 像素成本 | Blink `native_ms` |
| --- | ---: | ---: | ---: | ---: |
| 基本图形、渐变与文字 | 50 | 6,501 | 62,314 | 23.0 |
| 缓存放大复制 | 2 | 256 | 32,772 | 4.0 |
| 分层背景、云层、角色与移动方块 | 7 | 903 | 679,124 | 0.0 |

动画抽样时已执行 33 帧，之后键盘移动、冻结恢复、导航刷新及关闭继续通过。0.0 ms 仅是时钟粒度下的原生提交读数，且绘图可能延后栅格化，不能解释为绘图无成本。保留了[最终截图](pyscript-canvas-validation/final/canvas.json.sample.png)。

最初的探测触发过单块 100 ms 和首帧 8 ms 保护，失败记录保留。测试页将公共初始化与绘图放在独立标签；样例在初始化期间先完成缓存来源的首次栅格化 / 复制，再进入帧循环。没有提高生产时间额度或忽略预算错误。

证据索引：

- [统一汇总与源码 / 二进制哈希](pyscript-canvas-validation/final/summary.json)、[Canvas 浏览器结果](pyscript-canvas-validation/final/canvas.json)、[静态检查](pyscript-canvas-validation/diagnostics/static-checks.json)。
- [构建诊断说明](pyscript-canvas-validation/README.md)、[对象恢复清单](pyscript-canvas-validation/diagnostics/object-recovery.json)、[原始 / 归档 SHA-256](pyscript-canvas-validation/archive-manifest.json)。

本次定位并重建 24 份旧异常对象，涉及 COFF 节名 / 字符串和 CodeView 数据；24 份原件均先保存，重建后 SHA-256 均变化。另有 `animation_translation_util.obj` 被错误信息指向，但重复编译不改变其内容，结构检查未发现损坏，记录中明确标为诊断对象。后续扫描在 metrics 中定位到了真正的损坏输入。

两个测试可执行文件还曾报告无法复现的 PDB 类型错误：未改变输入的诊断链接通过，随后删除这两个生成的 exe，以原有 Debug / GHASH / PDB 参数标准重链通过；最终统一标准构建再次通过。根因尚未确定，未据此修改无关功能源码或放宽检查。原件、完整响应文件与原始日志保留在本机忽略目录 `out/Debug/pyscript-canvas-phase3/`。

## 后续边界

本阶段提供绘图能力与分层样例，不是完整 Mario 迁移或 Release 性能结论。下一阶段补齐所需数学依赖、迁移固定步长物理和核心玩法，再按真实场景测量命令峰值、缓存策略、完整画面一致性与长期帧性能。
