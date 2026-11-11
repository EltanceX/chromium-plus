# Python Mario 阶段五：控件、教学面板与完整页面交互

日期：2026-11-11。范围对应[总方案阶段五](pyscript-mario-implementation.md)。阶段五已完成，阶段六至七继续按总方案推进。

## 交付内容

[游戏页面](../chrome/test/data/pyscript/mario.html)保留 HTML/CSS 原生结构，游戏和教学交互全部由 Python 驱动。新增 `ui.py`，由生成器组合成 11 个有序内联块，每块仍不超过 64 KiB。页面没有可执行 JavaScript、事件属性、外部脚本或 HTML 字符串注入。

- 三项 range 参数：速度 160–420、跳跃 480–860、重力 1100–2300；原生 input/change 快照立即更新 Python 参数及 output 文本。
- 碰撞框、0.45× 慢动作与恢复默认。慢动作同时缩放物理累计时间及关卡计时；默认参数下原版轨迹不变。
- 三个 ARIA tab，支持鼠标、左右方向键、Home/End、roving tabindex、焦点与 hidden 面板状态。
- 八节 Python 课程、前后循环、四个主循环入口和学习地图。示例作为字面文本显示，不执行课程字符串；宿主测试逐段检查 Python 语法。
- 运行状态、关卡进度、HUD、最多三条事件日志、限时提示和 aria-live 通知。回调频率明确标记为原生回调次数，不能当作显示帧率。
- 原生学习地图及快捷键 dialog，遵循模态焦点、Esc/cancel/close 和关闭后焦点恢复；打开时暂停并清输入，关闭后由用户主动继续。
- DOM 状态遮罩、开始/暂停/继续、键盘与触屏控制。方向及跳跃按钮也支持 Space/Enter 长按和松开；焦点转向控件、隐藏或失焦时清理输入。
- 响应式双栏、窄屏单列及 790/1120 逻辑画布切换；重建画布句柄并丢弃旧绘制批次。滚动遵循实时 reduced-motion 偏好。

## 原生接口与限制

`components/python_runtime` 使用 `ElementAction`、`ElementUpdate` 和 `ControlStateResult` 普通数据；Chrome 转换字符串和类型，Blink 执行原生操作。没有任意对象反射或 CPython/Blink 指针互传。

```python
state = dom.get_control_state(element)
# 普通 dict：value / checked / hidden / disabled / open
dom.set_value(element, '280')
dom.set_checked(element, True)
dom.set_hidden(element, False)
dom.set_disabled(element, False)
dom.toggle_class(element, 'selected', True)
dom.set_attribute(element, 'aria-selected', 'true')
dom.set_tab_index(element, 0)  # 只接受 -1 或 0
dom.set_progress(element, 37.5)  # div 的 width 百分比，0–100
dom.focus(element)
dom.show_modal(dialog)
dom.close_dialog(dialog)
dom.scroll_into_view(element)
```

所有入口复用当前文档授权、所属解释器/代数、节点连接性、Shadow/custom element 限制和 DOM/时间预算。UI tag 有明确白名单。值读写只开放 input range/text/number、textarea、select；checkbox 使用 checked。file/password 等 input 类型不可读写值。字符串最多 16 KiB UTF-8，超出拒绝，不静默截断；快照保留 Unicode 和 NUL。

布尔必须为 bool，数值必须为精确 int/float 且有限，不调用任意转换钩子。class token 仅 ASCII 字母数字、下划线及连字符，最多 64 字节、每元素最多 64 个 class。属性仅 `aria-label`、`aria-selected`、`aria-expanded`、`aria-valuenow`、`title`；拒绝事件、URL 和任意 CSS 属性。进度入口仅设置有范围的数值百分比，不接受样式源码。

`set_text` 新增叶子 code/pre/output/button/li/h1/h2/h3；仍拒绝包含元素子节点、脚本、样式或定制元素。`focus` 使用 preventScroll，滚动使用 nearest，并动态选择 reduced-motion 下 instant、其他情况 smooth。dialog 使用原生 showModal/close，异常映射后重新检查文档有效性。原生操作引发的同步事件仍受全局禁止 Python 重入门禁保护。

新增元素 keydown/keyup、Document focusin，以及仅原生 dialog 可注册的 close/cancel。原生 focus/modal 操作同步产生的这些通知可能受禁止重入门禁跳过；它们不代表丢失按键/指针转换，不能增加输入复位代数。首次浏览器探测发现开始后自动暂停，已据此修正分类，并加入原生回归；真正跳过的输入转换仍会使输入代数失效。输入字段、默认操作和派发控制令牌仍遵循已有事件策略，不通过 Python 模拟浏览器默认行为。

## 调度与预算

浏览器探测记录过首次 showModal，以及课程变更后立即 close/滚动/聚焦触发 50 ms 事件停止。尝试在初始化 show/close 预热时又触发累计 250 ms 停止，该尝试已撤销。当前闭合 dialog 通过不可见且不参与命中的 CSS 保留布局准备，仍保持原生 open/modal 状态；课程、class、tab/hidden 变更按固定键合并进帧队列，队列清空后的独立帧分别滚动和聚焦，避免同一次回调连续修改结构并强制布局。

没有修改任何原生预算。单帧 16 ms、帧子窗口 200 ms/秒、交互总窗口 250 ms/秒，以及初始化/DOM/输出/绘图/资源额度保持阶段四值。控件读取和修改都计入 DOM 次数，原生调用前后检查时间与撤权，致命停止仍锁存。

离散输入仍使用原有 50 ms 事件入口，复杂页面变更异步按帧合并，因此可在后续数帧中完成。动态 HUD 每 250 ms 合并，待写项按固定 `(元素 id, 操作)` 键保存最新值，已应用值去重，每帧最多执行两项且 4 ms 软检查后停止；原有测量文本仍每帧最多一项。没有后台 Python 忙循环或无界积压。物理仍有 2 ms 软工作额度，绘制构建与提交分帧进行。

Debug 墙钟保护包含线程调度等待，阶段四已有 16 ms 间歇停止记录；本阶段不能据一次验收通过宣称该问题根治或稳定 30/60 FPS。Release/GPU 和长期真实游玩继续属于阶段七。

## 验证与当前边界

标准五目标构建与 16 组统一验收全部通过：202 项原生单测、21 项宿主逻辑、192 项浏览器测试，共 **415 项**；`passed=true`、`sourceUnchanged=true`，验证期间 866 个功能输入文件保持不变。包含 75 项 Runtime、17 项 Chrome 适配、78 项 Blink、21 项游戏逻辑和 35 项 Mario 浏览器专项。

[统一汇总](pyscript-mario-ui-validation/final/summary.json)、[完整证据与失败记录](pyscript-mario-ui-validation/README.md)、[原生对话框](pyscript-mario-ui-validation/final/mario.json.dialog.png)及[窄屏截图](pyscript-mario-ui-validation/final/mario.json.narrow.png)均已归档。浏览器使用真实可信键鼠/指针输入、headless 软件绘制及原有 Renderer 沙箱；它不替代阶段七的 Release/GPU 和长期人工游玩。

构建期间恢复了 21 份缓存对象和 1 份库，均先保存原件/哈希再定向重建，保留全部失败日志和正常链接诊断。静态格式、语法、依赖、生成页一致性、差异及 CPython 上游清单检查通过。

阶段六的音效、跨刷新最高分、全屏与下载导出仍未实现，页面明确说明这些边界。最高分仅在当前文档保存。本阶段没有修改外部 `C:/VD/Projects/mario/index.html`，也不代表原项目已经完整迁移。
