# i3 Fork 改动说明与性能分析

本文档记录本 fork（基于上游 i3 `8a97b0fe`）相对上游的全部改动、性能优化及其依据。
当前改动规模：**30 个文件，+3204 / -53 行**，自定义提交共 4 个：

```
219f3f16 try bugfixing        （浮动窗口 titlebar 色块修复）
95e094fb enhance UIUX         （渐变 titlebar、圆角、间距等视觉特性）
055ec312 merge mangowc and hyprland features
82b6c7eb port hyprland stuff to i3
```

## 一、功能改动总览

### 1. 动画引擎（`src/animation.c`、`include/animation.h`，全新文件）

Hyprland 风格动画，几何插值完全独立于布局代码：

- **贝塞尔缓动曲线**：8 条内建曲线（default/linear/easeOutCubic/…）+ 最多 16 条自定义曲线
  （配置指令 `bezier <name> <x1> <y1> <x2> <y2>`），牛顿迭代求值。
- **几何动画**：窗口打开滑入（`animation_open_*`）、布局变化过渡（`animation_*`）、
  工作区横滑（`animation_workspace_*`）。
- **透明度**：`active_opacity` / `inactive_opacity` / `dim_inactive` + `dim_strength`，
  通过 `_NET_WM_WINDOW_OPACITY` 实现（惰性 intern atom，仅首次一次往返）。
- 帧定时器 60 Hz，仅在动画进行时运行；无动画时自动停表（零 CPU 开销）。

### 2. 新布局（`src/render.c`、`src/con.c`、`src/tree.c`、`src/workspace.c`）

| 布局 | 说明 | 关键配置 |
|------|------|----------|
| `layout dwindle` | Hyprland 默认布局，阶梯式分割 | `dwindle_split_ratio`、`layoutmsg togglesplit` |
| `layout master`  | Hyprland master 布局，主区四向可配 | `master_ratio`、`master_orientation` |
| `layout scroller` | mangowc/niri 风格滚动列布局 | `scroller_proportion`、`scroller_structs`、`scroller_focus_center`、`scroller_prefer_center`、`scroller_prefer_overspread`、`scroller_proportion_single`、`scroller_default_proportion_single` |

这三种布局下 `split` 不再创建嵌套容器（布局算法自行决定位置），焦点移动改用
`get_tree_next_geometric()`（按渲染矩形几何距离选最近叶子，见 `src/tree.c`）。

### 3. 新命令（`src/commands.c`、`parser-specs/commands.spec`）

- `centerwindow`：把当前**浮动**窗口在工作区内居中（对平铺窗口报错，见 userguide）。
- `snap left|right|up|down|top-left|top-right|bottom-left|bottom-right`：KDE 式快速贴边。
  浮动窗口按方向贴半屏/四分屏；同一方向连按会从“四分 → 半屏 → 整个工作区”逐级放大。
  **只对浮动窗口生效**：平铺窗口由树布局管理，`snap` 对它们不做任何事，
  方向键请继续绑定上游的 `focus left|right|up|down`，避免 fork 扩展覆盖原生语义。
- `switcher next|prev|commit|close`：niri/mangowc 风格 Alt+Tab 缩略图切换器
  （Composite 抓图预览 + MRU 顺序 + 松开修饰键确认），见 `src/switcher.c`。
  每个 tile 只画预览，选中项用当前主题的边框渐变高亮；窗口标题统一显示在
  面板底部的共享标题栏里（对应 mangowc 的 `title_message_bar`）。
  `switcher next|prev sticky` 为冻结模式：松开修饰键不会确认，面板保持显示，
  便于截图/排查，需用 Esc、Enter 或 `switcher commit|close` 结束。
- `titlebar normal|none|toggle`：运行时显示/隐藏匹配窗口的标题栏（可按窗口绑定按键）。
  浮动窗口默认保留标题栏：应用的 `_MOTIF_WM_HINTS`（Chromium/Electron/GTK 等 CSD 客户端
  会用它请求“无边框”）在浮动状态下被忽略，见 `src/floating.c`、`src/handlers.c`。
- `hot_reload on|off`（默认 on）：自动监视主配置及 `include` 的文件，改动后
  0.2s 防抖、先子进程校验再应用（见 `src/config_watcher.c`）。默认开启时可以
  不再绑定 `$mod+Shift+c reload`；`restart` 仍用于换二进制等场景。
- `cyclenext` / `cycleprev`：循环聚焦。
- `zoom`、`swapdir`：主区交换/方向交换。
- `layoutmsg <msg>`：`togglesplit`、`proportion ±delta`、`setproportion <x>`
  （后者仅 scroller 布局）。
- `focusmaster`、`swapwithmaster`、`pseudo`、`togglesplit`（独立命令形式）。

### 4. 视觉增强（`src/x.c`、`libi3/draw_util.c`、`src/config_directives.c`）

- **渐变 titlebar / 边框**：`client.focused_gradient "#a #b [angle]"` 等，
  cairo 线性渐变（`draw_util_gradient_rounded_rectangle`）。
- **圆角**：`rounding <px>`，通过 X Shape 扩展对 frame 和 client 同时裁剪；
  行级 shape 矩形缓存（静态缓冲，避免每帧重算三角函数与分配）；
  对自带 shape 的窗口（如 xeyes）不干预，自触发 ShapeNotify 计数防误判。
- **titlebar 内边距**：`titlebar_padding <0-100>`（图标内边距为内部默认值 2px）。
- **独立标题栏字体**：`titlebar_font <font>`（缺省回退全局 `font`）。
  所有权与全局字体分离：`load_font()` 的隐式 `free_font()` 只释放
  `savedFont`，`CFGFUN(titlebar_font)` 会先把释放目标重定向到被替换的
  旧标题栏字体再恢复，避免把全局 `font` 的 pango 资源提前释放
  （UAF/double-free）；回退路径不做浅拷贝，由使用点动态选择字体
  （新增 `libi3` 的 `free_font_struct()`，reload 时释放独立资源）。
- **Hyprland 风格标题栏**：标题文字默认居中（`title_align` 默认 `center`）；
  标题栏底部绘制 `indicator` 颜色的 accent 细线（聚焦窗口金色高亮、
  非聚焦暗灰）；圆角开启时标题栏背景顶部跟随圆角
  （`draw_util_rounded_top_rectangle`）；默认配色改为深色面板
  （背景 `#11111b` / `#181825`，文字 `#cdd6f4`，accent `#f9e2af`）。

### 5. 配置（`src/config.c`、`src/config_directives.c`、`parser-specs/config.spec`）

新增约 30 个指令（动画时长/曲线、透明度、圆角、渐变、三种布局参数），
默认值集中在 `config.c` 的 `load_configuration`（如 `scroller_proportion = 0.9`、
`scroller_structs = 20`、`scroller_prefer_overspread = true`）。
用法示例见 `docs/hyprland-mangowc-config.zh.md`。

### 6. 下拉窗口（`src/dropdown.c`、`include/dropdown.h`，全新文件）

Yakuake/niri 风格的通用下拉窗口：任意 GUI 窗口都可隐藏到 scratchpad 并作为面板重新滑入。

- 新命令 `dropdown toggle|show|hide|recall`，自动支持 criteria 前缀（如
  `[instance="drop"] dropdown toggle`）。无 criteria 的 `toggle` 为焦点优先：
  焦点窗口是可见下拉窗口则隐藏；否则把焦点窗口转换为下拉并显示。
  `dropdown recall` 重新打开最近隐藏的下拉窗口，用于隐藏后快速找回。
- 单次调用可带几何参数：`dropdown toggle "bottom,0.25,0.4,right"`
  （字段 `position,height,width,align`，可留空，缺省取配置/全宽/居中）。
  覆盖值保存在客户端上，后续无参调用沿用，隐藏时按客户端自身的位置滑出。
- 新增 `~/.config/i3/bin/i3-dropdown` 启动/切换助手（Python 3 标准库）：
  `-i 实例名 [-W] [-s 几何] -- 命令...`，可选把焦点工作区 id 追加到实例名，
  替换命令里的 `{instance}` 占位符；窗口不存在时启动并轮询 `dropdown show`。
  纯通用键位不需要它；用户的 quake-term 规则已迁移为通用 `$mod+\` /
  `$mod+Shift+\` 绑定。
- 几何：默认顶部/底部、工作区全宽、高度为 `ws->rect`（含 outer gaps）的
  `dropdown_height` 比例，位置由 `dropdown_position top|bottom` 控制。
  在此基础上再按 `gaps inner`（上限 16px）四边内缩：单个下拉与多窗口 band 共用
  同一条外边距；多窗口槽位也按该内缩区域等分，槽间距相同。
- 多窗口平铺时跳过 `floating_check_size()`，避免应用 size hint 导致 tile 宽高不齐，
  每个槽位都使用精确的等宽等高矩形。
- 新增 `~/.config/i3/bin/i3-dropdown-toggle`（Python 3 标准库）：焦点窗口是可见
  下拉时先用 dmenu 询问 `Hide dropdown?`（Yes/No），确认后按原窗口 id 的 criteria
  隐藏；普通窗口直接执行 `dropdown toggle` 转换并显示，不弹确认。用户的
  `$mod+\` 已改为 `exec` 该脚本；`dropdown hide` 命令本身仍是非交互的。
- 隐藏时先按 `animation dropdown` 的时长/曲线滑出屏幕，动画结束回调
  （`animation.c` 的 `dropdown_hide_pending`）才移入 `__i3_scratch`，进程保持存活；
  显示时从屏幕外滑回目标矩形。`animations off` 或时长为 0 时立即生效。
- 隐藏完成后由 `con_move_to_workspace` 恢复焦点到原来的可见窗口。
- 多窗口下拉：同一工作区上多个可见 dropdown 会自动平铺（最多 4 个横向等分，
  超过 4 个改为两行），显示/隐藏任意一个时重排其余窗口，互不重叠；多个窗口
  共享由全局 `dropdown_height` / `dropdown_position` 决定的 band，只有单个可见
  下拉时才使用每客户端的几何覆盖。重排只在显示/隐藏时执行。
- Alt+Tab 集成：`switcher` 在普通候选之后追加隐藏的下拉窗口（位于 `__i3_scratch`
  且带 `dropdown` 标记），标题显示为 `[hidden] ...`；提交隐藏候选时调用
  `dropdown_show()` 将其作为下拉面板重新显示并重排，而不是仅聚焦 scratchpad
  容器。普通 scratchpad 窗口不会出现在列表中；隐藏进程始终存活。
- IPC `get_tree` 为每个容器输出 `dropdown` 布尔字段。
- 限制：`dropdown` 标记不参与 restart/load_layout 持久化，重启后窗口退化为普通
  scratchpad 窗口。

### 7. `i3ext { ... }` Lua 配置块（`src/i3ext.c`、`include/i3ext.h`，全新文件）

把 fork 专属的外观/动画/下拉/布局参数和扩展键位从 i3 配置命名空间中独立出来，
放进一个顶层 Lua 5.4 配置块；块外只保留上游 i3 指令、上游命令键位和外部工具键位。

- 嵌入系统 Lua 5.4（meson 依赖 `lua5.4 >= 5.4`）；`i3 -C` 会实际执行并校验
  块内 Lua。每个块使用独立、沙箱化的 state：只加载 base/table/string/math/utf8，
  移除 `os`、`io`、`package`、`debug`、`require`、`load`、`loadfile`、`dofile`、
  `loadstring`、`collectgarbage`、`print`。
- 两阶段预处理：Pass A 在原始文件上定位块行范围，避免块内的 `set`/续行被变量
  处理误改；Pass B 在变量替换后的 buffer 上抽取 Lua、替换为内部
  `__i3ext_exec <id>` 标记并保持行号。块按原位置执行，维持“后写覆盖”语义，
  include 和多个块均支持。
- Lua-aware 扫描器理解单/双引号字符串与转义、`--` 行注释、`--[[...]]` /
  `--[=[...]=]` 长注释、`[[...]]` / `[=[...]=]` 长字符串、任意层嵌套表、CRLF。
  块必须顶层、开括号在 `i3ext` 行末、闭括号独占一行。
- 新增 `binds` 列表：fork 扩展键位也迁入块内，复用 `configure_binding()`
  直接注册。`key` 按最后一个 `+` 拆成修饰键与键名/键码，支持
  `release = true` 和 `type = "bindcode"`（最后一段为数字键码）。
  `binds` 在固定顺序中最先应用；与原生 `bindsym` 重复时由
  `check_for_duplicate_bindings()` 检出并使 `i3 -C` 失败。首版不支持
  criteria 前缀和 mode 内键位，这类绑定继续使用原生写法。
- 新增 `theme` / `themes` / `bar`：palette 定义在 `themes.<name>` 中，
  同时覆盖窗口 titlebar/border 与 i3bar 颜色；`theme = "..."` 一行切换，
  省略时默认 `gruber_darker`。所有主题都会完整校验；i3bar 颜色应用到
  `barconfigs` 中的所有 bar，原生 `bar { ... }` 必须写在 `i3ext` 之前。
  顶层 `client` / `bar` 仍可作为主题后的覆盖项。用户配置内置
  `gruber_darker`、`md3`、`onedark`、`dracula`、`catppuccin_mocha`。
- 两阶段应用：先校验未知字段/类型/范围/枚举/十六进制颜色，再按固定顺序调用
  既有 `cfg_*` setter：**binds** → 主题 client → 主题 bar → gaps → decoration
  → opacity → titlebar → 顶层 client → 顶层 bar → animations → dropdown
  → layouts。未知键和类型/范围错误是致命错误，使 `i3 -C` 失败。
- `animations.<类型>` 的 `duration` 省略时沿用原值（不清零），`enabled = false`
  可关闭单个类型；`open`/`close` 分别映射 `windowsIn`/`windowsOut`，
  `fade_in` 映射 `fadeIn`；曲线先注册后应用。
- `layouts.scroller.ignore_proportion_single` 映射到内部取反后的
  `cfg_scroller_proportion_single`；`layouts.master.orientation` 是 i3ext 新增
  能力，直接写 `config.master_orientation`。
- IPC `get_config` 的 `raw_contents` / `variable_replaced_contents` 仍返回用户
  写入的 `i3ext { ... }`，不会暴露内部标记。
- 原生 fork 指令全部保留，可继续使用；两者混用时按出现顺序后者覆盖。
  详细语法、完整 schema、沙箱与错误定位见
  `docs/hyprland-mangowc-config.zh.md`。

## 二、性能优化（本次）

渲染路径是 fork 的性能关键：动画开启时 `tree_render()` 以 60 Hz 运行，
任何每帧开销都会被放大 60 倍。本次做了三处优化：

### 1. 纯透明度动画不再触发全量重绘（`src/animation.c`）— 最大收益

原实现中 `animation_tick_cb` 只要帧内有任何活跃动画（包括纯 fade）就调用
`tree_render()`——即 60 Hz 全树重算 + 全部 decoration 重绘 + X 推送，
而透明度变化实际只改 X property，无需重绘。

改动：tick 内跟踪插值几何是否变化（`anim_rect_equals`），**仅几何变化才
`tree_render()`**。效果：

- 纯 fade（窗口淡入、dim 切换）从每帧全量重绘降为零重绘；
- 几何动画最后一帧若 eased 值已收敛到目标，也不再多做一次重绘。

### 2. dwindle / scroller 布局 O(n²) → O(n)（`src/render.c`、`include/render.h`）

原实现每个子容器渲染时都从头遍历整个容器重算矩形（scroller 每列还要
`con_inside_focused` 深度扫描 + 宽度/位置两次全列遍历），单次渲染为 O(n²)。

改动：仿照 i3 既有的 `precalculate_sizes()` 惯例，新增
`precalculate_layout_rects()`，在 `render_con()` 进入子循环前**一次性**算出全部
子矩形存入 `render_params.layout_rects`（`free_params` 统一释放），
`render_con_dwindle` / `render_con_scroller` 退化为 O(1) 查表。

### 3. scroller 渲染消除每帧堆分配抖动（`src/render.c`）

原实现每次渲染每个 scroller 容器 `smalloc` 两组 int 数组再 `free`
（60 Hz × 每容器 2 次分配/释放）。改动：≤128 列走栈上缓冲，
更大规模回退堆分配，预计算重构后此缓冲只分配一次/容器。

### 未改动但已评估为非热点

- `x_set_rounded_shape` 的静态矩形缓存（已优化良好）；
- cairo 渐变 pattern 每次绘制创建/销毁（cairo 内部廉价）；
- `bezier_eval` 牛顿迭代（每帧每窗口 8 次以内，可忽略）；
- `con_get_workspace` 的 O(深度) 调用（仅事件驱动路径）。

### 4. frame buffer 脏标记（`src/x.c`、`include/data.h`）

`Con.frame_dirty`：仅当某容器的 frame_buffer 在本次渲染中真正被重绘
（装饰参数变化 / pixmap 重建 / 焦点或颜色变化）才执行 CopyArea 推送到
frame 窗口；`x_deco_recurse` 的父容器拷贝同样按脏标记门控。
60 Hz 动画时未动的窗口零 X 流量，damage 只出现在动画窗口上，
每帧成本从 O(全部像素) 降为 O(动画窗口像素)，嵌套 X 服务器下收益最大。

### 5. 动画帧稳态调度（`src/animation.c`）

帧定时器原为固定 1/60 间隔的重复定时器，渲染耗时会整体顺延后续帧。
改为每帧结束时按"剩余预算"设置 repeat（`1/60 - 本帧渲染耗时`），
保持 60 Hz 墙钟节拍，动画更均匀；无可动画时照旧停表零轮询。

### 6. scroller 黑块修复、autofit 与越界列 pixmap 回收

frame 背景像素改用 `client.background` 主题色，消除黑块与残影；
`scroller_autofit`（默认 yes）：列窄于屏幕时按比例铺满、列宽不超过屏幕、贴边锚定；
完全滚出可视区的列不再分配装饰 pixmap（单列约 3.5 MB），滚回来时自动重建。

另外修复了 **只有一个窗口时应用 `layout scroller|dwindle|master` 无效**的问题：
原实现把布局直接设在窗口这个叶子容器上（叶子没有子节点，布局永远不生效），
之后打开的新窗口仍按普通 split 平铺。现在会把唯一的窗口包进一个布局容器
（`workspace_encapsulate`），后续窗口自然成为同一布局的列/子容器。

### 7. Alt+Tab 切换器：选错窗口、松手不生效、预览黑块

现象是"按 Alt+Tab 想切到 firefox，结果还在 zen（或别的窗口）"，只在 3 个以上窗口时明显。
两条主要原因都修掉了：

- **自动重复（auto-repeat）把选择器推过头**（主因）：按住 Tab 时服务器按重复速率再发若干次
  KeyPress，选择环从上一个窗口继续前进——3 个窗口时正好又落回"原本要离开的那个"。
  现在用一个"按键是否仍按下"的状态机过滤重复（同时兼容 XKB detectable auto-repeat 的
  press-only 和传统模式的 release/press 同时间戳对），**按一次 Tab 只走一格**。
- **修饰键释放事件丢失**：Alt+Tab 快速轻点时，release 往往在 `xcb_grab_keyboard` 生效前就
  已投递出去，切换器永远等不到确认事件，面板挂着不提交（用户看到"没反应"）。
  现在面板打开期间用 libev 定时器（50 ms）轮询 `xcb_query_keymap`，一旦确认修饰键全部松开就提交；
  打开瞬间也会先查一次，轻点几乎无延迟。`sticky` 模式不受影响。
- 防御性修复：面板销毁时指针落到下方窗口所产生的 EnterNotify，在 `focus_follows_mouse`
  （i3 默认开启）下会把焦点设回鼠标下的窗口、静默撤销刚才的选择；现在按
  `handle_unmap_notify_event()` 的做法忽略与 `DestroyWindow` 同序列号的 EnterNotify
  （带 GetInputFocus 往返，避免误伤后续真实事件，见 i3 #609），面板自身的 EnterNotify
  也不再落到 `check_crossing_screen_boundary()`。提交前 `focused_id = XCB_NONE`，
  强制 `x_push_changes()` 真正下发 `XSetInputFocus`。
- 候选不再要求 `con->mapped`：tabbed/stacked 里被隐藏的窗口也能被 Alt+Tab 选中
  （没有预览，退回标题块）；提交时按窗口 id 重新查找容器，避免候选在此期间被关闭的悬垂指针；
  改用 `con_activate()`，浮动窗口会一并置顶。
- **全浮动工作区修复**：候选收集原先用 `con_is_leaf()` 判断叶子，而只有浮动窗口的
  workspace 的 `nodes_head` 为空（浮动容器在 `floating_head`），会被误判成叶子并直接
  返回，导致“所有窗口都浮动时 Alt+Tab 打不开”。现在以 `CT_CON` 作为终止条件，并
  始终沿 `focus_head`（MRU 顺序，浮动与平铺容器都在其中）递归；实测两个浮动窗口
  Alt+Tab 能正常显示缩略图与标题栏。
- Shift 只负责反向循环：按下不再被当作"取消"（`Mod1+Tab` 后再按 Shift 以前会直接关掉面板且不切换），
  松开 Shift 也不再提交；Alt/Meta/Super/Ctrl 松开仍为确认，符合 mangowc 语义。
- 预览抓图：直接抓窗口可能得到黑图——无合成器时被遮挡区域内容未定义，depth-32 窗口 alpha 为 0
  则整块透明。现在对采样判定为"全黑/全透明"的抓图退回为窗口 frame 区域的屏幕快照
  （按 root 尺寸裁剪，恒定按不透明 RGB24 解析）。
- 实测（Chromium 150 / Firefox Nightly / Zen，均在 `--nest` 内）：20 次连续
  Alt+Tab 全部命中预期窗口；快速轻点与按住 Tab 均只走一格；浮动窗口遮挡时
  预览改用屏幕快照，无黑块。

### 8. i3bar 状态栏性能优化（`i3bar/src/*`、`libi3/font.c`）

i3bar 在每次状态更新（默认 1 秒一次）和每次工作区事件时都要重新校准、重绘所有 block，
是持续存在的小热点。本次从“每次更新/每次绘制”两个维度削减分配与测量：

- **Pango 布局与 cairo 上下文复用**（`libi3/font.c`）：原先 `predict_text_width_pango`
  和 `draw_text_pango` 每次调用都新建 `cairo_image_surface` + `cairo_t` + `PangoLayout` 并
  在返回前销毁。现在各保留一个测量布局和一个绘制布局（另加测量用 1×1 surface + cr），
  仅在字体描述、DPI 或绘制目标 surface 变化时重建（`pango_cache_reset()` 在
  `free_font()` / `free_font_struct()` 里失效）。文本测量仍用
  `pango_cairo_update_layout`，返回前 `cairo_new_path()` 避免缓存上下文留下活动路径，
  以免后续 `cairo_surface_flush()` 报错。i3 核心（窗口标题、边框）共用此代码，同样受益。
- **状态更新不再逐 block 拷贝**（`i3bar/src/child.c`）：`copy_statusline` 原来把每个
  `status_block` 连同字符串重新分配、memcpy 一份；改为 `move_statusline()` 用
  TAILQ_REMOVE/INSERT_TAIL 直接搬运节点，零分配、零拷贝。
- **JSON key 不再分配字符串**（`i3bar/src/child.c`、`i3bar/src/workspaces.c`）：原来
  `yajl_map_key` 里 `sstrdup` 每个 key、标量回调里再 `strcmp`、map 结束时 `free`。
  现在 key 直接映射成编译期 enum（`block_key_t` / `ws_key`），标量回调只做
  `switch`；比较用长度 + `strncasecmp`/`memcmp`，不分配。未知 key 的处理与原
  “取消解析”语义一致。
- **stdin 读取缓冲复用**（`i3bar/src/child.c`）：`get_buffer` 改为持久、按需翻倍的
  静态缓冲，不再每次 `malloc`/`free`；`ws_stdin_io_cb` 用 `strchr` 原地切分换行，
  取代 `g_strsplit`/`g_strfreev` 的整块字符串分配。
- **跨输出共享测量结果**（`i3bar/src/xcb.c`、`i3bar/include/common.h`）：
  `render_desc` 增加 `text_width` + `width_valid`，`predict_block_length` 只在首次
  测量并把裸宽度缓存；多显示器时同一次 `draw_bars` 不再对同一 block 重复
  `predict_text_width`，也不再把 border 宽度重复累加。每次 `draw_bars` 开始时统一
  失效缓存。
- **单次 flush**（`i3bar/src/xcb.c`）：`redraw_bars` 在所有输出循环结束后统一
  `xcb_flush`，避免每个输出一次往返。

> 顺带修复一个上游遗留的 yajl 2 兼容问题：i3bar 按 yajl 1 的“流式多文档”语义编写，
> 而 yajl 2 在第一个顶层 JSON 文档结束后再喂数据会报 `trailing garbage`，导致
> **JSON 格式状态命令除第一次更新外全部解析失败**。在 `start_child` 里显式
> `yajl_config(parser, yajl_allow_multiple_values, 1)`（与 `parse_json_header.c`
> 已有的 `yajl_allow_trailing_garbage` 一致）。用三帧 JSON（普通 block / pango
> markup / border + background + min_width + align + short_text）实测，三帧均正确渲染。

### 9. i3 核心渲染与推送路径优化（`src/gaps.c`、`src/render.c`、`src/x.c`、`src/con.c`、`src/animation.c`）

前 1–7 项主要针对动画与布局计算本身，本节处理 `tree_render()` → `x_push_changes()`
这条每帧必经链路上仍然存在的三类额外开销：重复的全树扫描、线性查找、以及可避免的
堆分配与 X 请求。

- **gaps 计算按渲染轮次缓存**（`src/gaps.c`、`include/gaps.h`、`src/render.c`）：
  `calculate_effective_gaps()` 每次都要 `con_num_visible_children()` 遍历整个工作区，
  而 `render_con()` 对树中每个容器都会调用它（工作区自身 + 每个判断 inset 的容器），
  单次渲染是 O(n²)。一轮渲染期间树、焦点与配置都不会变化，故新增
  `gaps_render_cache_begin()` / `gaps_render_cache_end()`，由 `render_con()` 用
  `render_depth` 在最外层进入/退出时开、关缓存，按工作区缓存 `gaps_t`。
  同理 `gaps_has_adjacent_container()` 原本每个方向都要调两次
  `con_get_fullscreen_con()`（每个 inset 容器 4 个方向 = 8 次全树 BFS），现在在缓存
  生效期间按工作区只查一次全屏容器并复用。
- **`state_for_frame()` 线性扫描改哈希表**（`src/x.c`）：`x_push_node()`、
  `set_hidden_state()`、`set_maximized_state()`、`set_shape_state()`、`x_raise_con()`
  每次推送都会对每个容器查一次 con_state，原实现是 `state_head` 的
  `CIRCLEQ_FOREACH`，使单次 push 为 O(n²)。新增开放寻址（线性探测、乘法散列、
  删除用墓碑标记）的 `state_map`，在 `x_con_init()` 插入、`_x_con_kill()` 删除，
  75% 装载时扩容并重哈希，查询降为均摊 O(1)；`state_head` 保留给需要遍历的旧逻辑。
- **`con_get_fullscreen_con()` 去掉逐节点 malloc**（`src/con.c`）：原 BFS 每访问一个
  节点就 `smalloc` 一个 `struct bfs_entry` 并 `free`，而该函数在渲染/inset 判断里被高频
  调用。改为跨调用复用的静态 `Con *` 队列（容量 64 起、不足翻倍），BFS 顺序不变，
  每次调用零分配。
- **渲染临时数组改栈上缓冲**（`src/render.c`）：`precalculate_sizes()` /
  `precalculate_layout_rects()` 的返回值原来总是 `smalloc`，每渲染每个容器两次堆分配。
  现在 `render_con()` 提供 `sizes_stack` / `rects_stack`（16 个元素，覆盖绝大多数容器），
  子容器数不超过该值时直接用栈缓冲、不再分配；超出才回退堆分配，由 `free_params`
  按 `heap_allocated` 标记释放。
- **`_NET_FRAME_EXTENTS` 仅在变化时写入**（`src/x.c`）：该 property 只在边框样式变化时
  才变，但原来每次 push 对每个窗口都无条件 `xcb_change_property`。现在在 `con_state`
  里缓存上一次的四个值并 `memcmp` 比较，未变则跳过；`x_reinit()`（换了 client window）
  与 `x_move_win()`（frame 里换了窗口）会置 `frame_extents_valid = false` 强制重发。
- **动画 tick 单次遍历**（`src/animation.c`）：`animation_tick_cb()` 原本在遍历
  `all_cons` 处理完所有动画后，再调用一次 `animation_running()` 又整树遍历一遍。
  现在在 tick 循环内用 `still_running` 记录是否仍有活跃动画，省掉每帧一次全树扫描。
- **scroller 焦点列检测修复**（`src/render.c`）：`precalculate_layout_rects()` 原来用
  `cur == focused || con_inside_focused(cur)` 判断焦点在第几列，但
  `con_inside_focused(cur)` 问的是“cur 是否在 focused 内部”（即 focused 是 cur 的
  祖先），方向正好相反。`layout scroller` 会把工作区原有的顶层容器整体包裹进
  scroller 容器，列本身常常是容器而不是窗口；此时该条件永远不成立，`focus_index`
  退回 0，视图始终钉在第一列，切换焦点看起来毫无反应（`--nest` 里尤其明显）。
  改为 `con_has_parent(focused, cur)`（focused 是否位于该列内部），焦点列才会被
  正确滚动/居中。

### 10. 帧提交、动画时序与状态栏（本轮）

参考 Clarion 的帧预算与“一帧一次提交”思路，继续削减每帧的 X 请求、堆分配与
无效工作：

- **事件掩码按需切换**（`src/x.c`）：`x_push_changes()` 原先每次推送都先把所有
  已映射窗口的 event mask 改成 `SUBSTRUCTURE_REDIRECT`（去掉 EnterWindow），
  推送完再改回 `FRAME_EVENT_MASK`，即每帧 `2×窗口数` 次
  `ChangeWindowAttributes`。现在先用一次轻量预扫描判断本次推送是否会重排窗口栈、
  映射/取消映射窗口（以及图形 resize、tiling drag 临时去掉 EnterWindow 的情况），
  只有确实需要时才做这两轮全树切换。普通几何动画/焦点/标题变化每帧省掉 2N 个
  X 请求。
- **每帧只 flush 一次**（`src/x.c`）：`x_push_node()` 在动画窗口的 pixmap 拷贝与
  改尺寸前后各调用了一次 `xcb_flush()`，工作区滑动时是每帧 `2×动画窗口数` 次
  系统调用。X11 请求在连接内保序（服务器一定先拷贝再 resize），因此改为把整帧
  请求留在同一个缓冲区，由 `x_push_changes()`（或直接调用 `x_push_node()` 的
  拖拽路径）统一提交，帧的原子性也更好。
- **热路径复用 con_state**（`src/x.c`）：`set_hidden_state()`、
  `set_maximized_state()`、`set_shape_state()` 原先每个容器都要再做一次
  `state_for_frame()` 哈希查找；现在由 `x_push_node()` 把已查到的 `con_state`
  直接传入，每帧少 3N 次哈希探测。
- **自适应帧预算**（`src/animation.c`）：帧定时器用“上一帧实际耗时”决定下一次
  sleep。现在维护一个立即上涨、按 1/16 缓慢衰减的耗时估计（同 Clarion 的 latch
  budget），避免偶尔一个重帧就把后续整段动画的节拍顶偏；持续的重负载仍会被
  如实计入。
- **透明度去重与首帧淡入**（`src/animation.c`、`include/data.h`）：容器记录上次
  写入的 opacity，值未变化时不再重复写客户端与 frame 的
  `_NET_WM_WINDOW_OPACITY`；窗口打开动画在开始时立即写入 `opacity_from`，
  消除“第一帧全不透明、下一帧突然变淡”的闪烁。`x_reinit()` 会失效该缓存。
  另外新增 `animation_reapply_opacity()`，配置 reload 后立即按新配置重算所有窗口
  的透明度——之前关闭 `dim_inactive` 或调回 1.0 时，旧窗口会一直停在变暗状态，
  直到下一次焦点切换。
- **工作区滑动可中断**（`src/x.c`）：连续快速切换工作区时，新滑动从“当前可见
  位置”继续，而不是每次都从屏幕外重新开始，切换过程不再跳变。
- **新增内置缓动曲线**（`src/animation.c`）：`easeOutBack`、`easeInOutBack`、
  `easeOutCirc`，用于窗口打开/工作区切换的轻微回弹（贝塞尔表达，行为接近
  Clarion 的 spring 轨迹但不需要额外状态机）。淡入淡出仍把 eased 值夹到
  `[0,1]`，避免过冲导致透明度钳制后停顿。
- **i3bar 相同状态更新直接跳过**（`i3bar/src/child.c`）：对完整的状态负载做
  64 位 FNV-1a 指纹，与上一次完全一致时不解析 JSON、不重建 block、不重绘。
  只在负载以 `]`/`}`/`,`/换行结尾（完整更新）时记录，避免误跳分片。
- **i3statusgo 去掉 gopsutil 与子进程**（`i3statusgo/cmd/i3statusgo.go`）：CPU、
  内存、磁盘、网速、电池全部改为直接读 `/proc`、`/sys` 与 `statfs`；SSID 用
  `SIOCGIWESSID` ioctl、信号强度读 `/proc/net/wireless`，稳态不再 fork `nmcli`；
  CPU 采样不再每次创建 goroutine + `time.After`。第三方依赖清零，二进制
  4.1 MB → 2.0 MB，实测 RSS ≈ 8 MB（原 ≈ 9–11 MB），WiFi 信息按 10 s、本机 IP
  按 30 s 缓存。

## 三、性能画像与内存

- **内存**：Xephyr 1280×800、5 窗口、渐变 + 圆角 + 动画全开，i3 RSS ≈ **35 MB**
  （VSZ ≈ 50 MB），与上游 i3 同量级；fork 新增的每容器状态
  （`con_anim`、`con_state.shape_rect` 等）为几十字节级，无独立大块分配，
  无泄漏（动画结束即停表，状态复用）。
- **CPU**：静态待机时动画定时器停止，与上游一致为零轮询；
  纯 fade 场景（如 `dim_inactive` 聚焦切换）在优化后不再产生 60 Hz 重绘，
  这是 fork 在上游之上最主要的运行时 CPU 开销来源，已被消除。

## 四、本次新增：配置热重载与 nest 测试模式

### 1. 配置自动热重载（`src/config_watcher.c`、`include/config_watcher.h`，全新文件）

Hyprland 风格：保存配置文件即自动生效，无需手动 `$mod+Shift+c` 或 IPC reload。

- **监听范围**：主配置文件 + 全部 `include` 递归引入的文件（watch 父目录的
  `IN_MODIFY/IN_CLOSE_WRITE/IN_MOVED_TO/IN_CREATE`，过滤文件名）。
  监听目录而非文件本身，因为编辑器常用 rename(2) 原子替换保存。
- **防抖**：200 ms 内的多事件合并为一次 reload（vim 写入 + rename 之类的
  事件风暴只触发一次）。
- **先校验后应用**：reload 前先 fork+exec 自身以 `-C -c <path>` 离线校验
  （必须在子进程做——解析失败会把存活进程的 Config 结构体写脏）。
  校验失败：弹 i3-nagbar 报错，**保留旧配置继续运行**；修复后保存即恢复。
- **生命周期**：初始加载后 `config_watcher_init()` 挂到 main_loop（ev_io）；
  每次 reload（含手动 reload）后 `config_watcher_rearm()` 重建监听列表
  （include 集合可能变化）；inotify 不可用时优雅降级为仅手动 reload。
- **报错 nagbar 的清理**：热重载失败弹出的 nagbar 使用 watcher 自己的 pid，
  原来的 `i3-msg restart` 只清理 config/command 两类 nagbar，升级后红色
  错误条会残留到新会话。新增 `config_watcher_kill_nagbar()`，在 restart、
  exit 以及手动 reload 成功时一并清理。

### 2. `--nest` 测试模式增强（`src/main.c`、`src/bindings.c`）

`i3 --nest[=DISPLAY]`：在私有 Xephyr 窗口（默认自动挑选空闲 display，从 `:99`
向下找；窗口 1280x800）里跑一个完整 i3 会话，用于在其它桌面下安全测试。

- **不再生成任何 Alt 别名**：早期实现用 `I3_NEST` 环境变量把所有 Mod4 绑定
  复制成一份 Mod1（Alt）副本，导致“nest 里按 Alt+Return 也会打开终端”这种
  与真机不一致的行为。该机制（`duplicate_bindings_for_nest_mode()`）已整体
  删除，嵌套会话里的键位与真机配置逐字一致：`$mod` 就是 `$mod`。
- **键盘交接给 Xephyr**：Xephyr 在其窗口获得焦点时会发起真正的
  `XGrabKeyboard`，抓取期间宿主 WM 的 passive grab（含宿主的 Alt+Tab）不再
  生效。因此启动后父进程会等待 Xephyr 窗口 map，用独立的宿主 X 连接
  `xcb_set_input_focus` 把焦点交给它，并把指针挪到窗口中心（兼容
  `focus_follows_mouse`），无需用户先点一下窗口。窗口用
  `-name i3-nest-<display>` 标记（WM_CLASS），多开时不会认错窗口。
- **静态与隔离**：嵌套会话有独立的 display、独立 `XDG_RUNTIME_DIR`、独立
  dbus-daemon；`unset WAYLAND_DISPLAY`/`WAYLAND_SOCKET` 并设
  `XDG_SESSION_TYPE=x11`，客户端不会跑回宿主 Wayland 桌面。Linux 下 Xephyr
  默认抓取键盘，`Ctrl+Shift` 切换抓取/释放，启动时在 stderr 说明。
- **进程级隔离（PID namespace）**：嵌套实例经
  `unshare --user --map-current-user --pid --fork --mount-proc` 重执行（
  `nest_enter_pid_namespace()`），拥有私有 `/proc`——嵌套会话里的
  `killall`/`pkill`/`ps` 只能看到嵌套进程（此前在嵌套终端里跑
  `killall st` 会把宿主终端一起杀掉），也无法按 PID 触达宿主进程。
  内层 i3 成为 namespace 的 PID 1，孤儿进程由 libev 默认 loop 统一
  回收（与上游行为一致），无僵尸积累。Xephyr 仍在 namespace 外由
  父进程管理，经共享的 `/tmp/.X11-unix` socket 连接。
  实现细节：必须在进入 unshare 前用 `readlink(/proc/self/exe)` 解析
  i3 真实路径（exec 时刻 `/proc/self` 指向 unshare 自己，直接传
  `/proc/self/exe` 会递归执行 unshare）；re-exec 参数中过滤
  `--nest*`，附带修复了 nest 内就地 restart 会重入 `nest_run()` 而失败
  的问题。unshare 缺失或非特权 user namespace 被禁用（sysctl/AppArmor）
  时自动降级为无 namespace 运行（探测子进程静默失败后打日志）。
- **已知限制**：宿主若是 Wayland compositor，其 compositor 层的全局快捷键
  在 XWayland 之前处理，Xephyr 抓取无法拦截；这一点无法从 i3 一侧解决。
- 退出时自动清理 Xephyr 并透传退出码；启动前规避本机有缺陷的
  `/usr/local` libxkbcommon（re-exec 前置 `/usr/lib/x86_64-linux-gnu`）。

### 3. 视觉默认值（`src/config.c`）

开箱即 Hyprland 风格（显式配置指令始终覆盖）：`rounding 12`、
`gaps inner 4 / outer 8`、默认边框宽 1px。回到上游观感设 `rounding 0`
并把 gaps 归零即可。

### 4. Alt+Tab 切换器主题（`src/switcher.c`、`src/config_directives.c`、`src/i3ext.c`）

早期实现把 `client.focused.border`（通常是高亮强调色）当作共享标题栏底色，
再配 `client.focused.text` 浅色文字，对比度极低，标题几乎看不清。

- **默认配色改为标题栏配对**：标题栏底色用 `client.focused.background`、
  文字用 `client.focused.text`；无缩略图贴图用 `client.unfocused.background`
  与 `client.unfocused.text`；面板底色用 `client.background`。
- **新增三条原生指令**：`switcher_background <color>`、
  `switcher_bar <background> <text>`、`switcher_tile <background> <text>`，
  未配置的项逐项回退到上述 `client.*` 颜色。
- **主题系统扩展**：`i3ext` 的 `themes.<name>` 新增 `switcher` 子表
  （`background`、`bar`、`tile`），也支持顶层 `i3ext.switcher` 覆盖；
  `etc/config` 的五套 palette（gruber_darker / md3 / onedark / dracula /
  catppuccin_mocha）都补上了对应配色，切主题即切换切换器配色。
- 缩略图选中/未选中边框仍走 `client.focused` / `client.unfocused` 的边框或
  渐变，保持与窗口装饰一致。

### 5. i3bar 状态行（`src/status_line.c`、`include/status_line.h`，全新文件）

类似 Emacs mode line 的状态显示，但**输出到 i3bar 的 binding-mode 区块**，
不创建屏幕 overlay，因此不会遮挡窗口内容：

```text
[resize] ws:1 | scroller | st tiled | keys: Mod4+Shift+r | last: reload
```

- 复用 `I3_IPC_EVENT_MODE` 事件（`{"change": "...", "pango_markup": false}`），
  由 i3bar 现有的 binding-mode 绘制路径渲染，不需要修改 i3bar；
- 字段：binding mode、工作区、焦点窗口所在容器布局
  （splith/splitv/tabbed/stacked/scroller/dwindle/master）、焦点窗口标题、
  `tiled/floating/fullscreen`、最后一次匹配的键位（`key_hints`）、最近执行的命令；
- `key_hints`（默认 off）：在 `key_press.c` 里把匹配到的 Binding 的
  `event_state_mask` + `symbol` 格式化成 `Mod4+Shift+r`，类似 screenkey。
  由于 i3 只接收被 grab 的按键，这里只显示已配置的 keybinding；
- 每个 `tree_render()` 结束后刷新，并在 `parse_command()` 后立即刷新，
  保证 `nop`、mode 切换这类不触发渲染的动作也能看到；
- 文字不变时不重复发送 IPC；
- 关闭时发送当前 binding mode（通常是 `default`），i3bar 会隐藏该区块；
- 新配置指令 `status_line on|off`（默认 on），推荐写进 `i3ext { status_line = true }`；
- `i3ext { key_hints = true }` 开启键位显示，不提供原生指令。

## 五、上游 TODO 处理状态

对仓库内全部上游遗留 TODO（`grep -irn 'TODO'`）逐条评估，结果分三类。

### 已修复（本次）

| TODO 位置 | 处理 |
|-----------|------|
| `src/x.c:1309`（零尺寸进入 pixmap 创建路径） | pixmap 创建条件补上 `rect.width > 0 && rect.height > 0`，零尺寸容器（不可见）不再创建 1×1 兜底 pixmap；`MAX(...,1)` 保留为防御措施 |
| `src/x.c:1336`（L_TABBED 是否同样处理） | 答案固化进注释：**否**——tabbed 布局所有 tab 装饰均可见，全部子容器需立即渲染装饰；stacked 仅焦点子容器可见 |
| `src/con.c:1226`（分配到已全屏容器时不推送状态） | `manage_window()` 在放置后检测 `nc->fullscreen_mode != CF_NONE` 且窗口自身未请求全屏时补推 `_NET_WM_STATE`（避免与 `con_toggle_fullscreen` 路径重复 APPEND atom） |
| `i3bar/src/ipc.c:45`（命令 reply 错误处理） | 新增 `check_reply_for_errors()`：yajl 解析 `{"success","error"}`，失败打 ELOG（命令与订阅 reply 共用） |
| `i3bar/src/ipc.c:65`（订阅 reply 错误处理） | 同上 |
| `src/ipc.c:254`（gaps "outer" 字段兼容性） | 无需改行为（i3ipc 兼容是刻意为之），注释固化为设计说明 |
| `include/scratchpad.h:7`（缺描述） | 补全头文件说明 |

### 已知决策、维持现状

- `src/x.c:1520` `is_con_attached`：等待上游修复 #1185 后移除，fork 不先行删除。
- `src/restore_layout.c:178`（watch 符号更大字体）：装饰性小项，涉及加载
  第二套字体，收益低；恢复布局界面仅短暂可见。

### 架构级重构，fork 不处理（与 fork 目标无关）

`tree.c:82/470`、`con.c:530/607/977/1574/2304`、`load_layout.c:17`、
`commands_parser.c:180/267`、`render.c:433`、`randr.c:856`、`x.c:109/142`、
`floating.c:284`、`restore_layout.c:135`、`include/tree.h:15`、
`include/con.h:266/368`、`parser-specs/config.spec:12`、
`contrib/gtk-tree-watch.pl:185`——均为上游作者留的通用性重构备忘
（遍历算法、解析器可测试性、per-connection 字体等），改动风险大于收益。

## 六、已知问题

- **启动 SIGSEGV（环境问题，非代码问题）**：`load_keymap()` → `xkb_x11_keymap_new_from_device`
  崩溃于 libxkbcommon 的 `atom_intern`。根因已定位到 `/usr/local` 下**两次安装混用**：
  core 库是 `libxkbcommon.so.0.14.0`（1.14.0-beta1，`/usr/local/lib/x86_64-linux-gnu`），
  而 `libxkbcommon-x11.so.0` 仍指向 0.13.1（`/usr/local/lib`），二者结构体布局不一致，
  x11 组件写坏 `xkb_context`，随后 core 内部的 atom 表崩溃。
  i3 的 `RUNPATH=/usr/local/lib/x86_64-linux-gnu:/usr/local/lib` 让它优先于系统库加载，
  所以**重新编译/链接后必然启动即崩**。系统里的 1.7.0-2（core + x11 配套）是好的。
  规避：`export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu` 后再启动 i3（已验证），
  或把 `/usr/local` 下的 libxkbcommon / libxkbcommon-x11 重装成同一版本（更干净）。
  已用 git stash 隔离验证：未修改的 fork 代码同样崩溃，与 fork 改动无关。
- 工作区根目录的历史 `core.*` 均为上述问题产物，可安全删除。
