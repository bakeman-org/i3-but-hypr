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
  平铺窗口下 `left/right/up/down` 自动退化为 `focus` 移动，因此可以直接替换配置里的
  `bindsym $mod+Left focus left`。
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
- Shift 只负责反向循环：按下不再被当作"取消"（`Mod1+Tab` 后再按 Shift 以前会直接关掉面板且不切换），
  松开 Shift 也不再提交；Alt/Meta/Super/Ctrl 松开仍为确认，符合 mangowc 语义。
- 预览抓图：直接抓窗口可能得到黑图——无合成器时被遮挡区域内容未定义，depth-32 窗口 alpha 为 0
  则整块透明。现在对采样判定为"全黑/全透明"的抓图退回为窗口 frame 区域的屏幕快照
  （按 root 尺寸裁剪，恒定按不透明 RGB24 解析）。
- 实测（Chromium 150 / Firefox Nightly / Zen，均在 `--nest` 内）：20 次连续
  Alt+Tab 全部命中预期窗口；快速轻点与按住 Tab 均只走一格；浮动窗口遮挡时
  预览改用屏幕快照，无黑块。

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

### 2. `--nest` 测试模式增强（`src/main.c`、`src/bindings.c`）

`i3 --nest[=DISPLAY]`：在私有全屏 Xephyr（默认 `:99`）里跑一个完整 i3 会话，
用于在 Hyprland 下安全测试。

- **mod 冲突解决**（`duplicate_bindings_for_nest_mode()`）：Wayland 宿主的
  键位绑定在 seat 层先于 XWayland 处理，Xephyr 抓取无法绕过；而 XKB 层面
  交换 Alt/Super（setxkbmap altwin:swap_alt_win）在本机 TinyX Xephyr 上会
  留下脏的兼容修饰位（state 混入 Mod1|Mod2|Mod4，XGrabKey 精确匹配永不
  命中）。最终方案：nest 子进程设置 `I3_NEST=1` 环境变量，i3 在初始加载与
  每次 reload 后把所有 Mod4 键位绑定自动复制一份 Mod1（Alt）变体并一并
  translate/grab——物理 Alt 触发配置里 `$mod`（Super）的键位，配置文件
  零改动，Hyprland 只绑 Super 故零冲突。
- 退出时自动清理 Xephyr 并透传退出码；启动前规避本机有缺陷的
  `/usr/local` libxkbcommon（re-exec 前置 `/usr/lib/x86_64-linux-gnu`）。
- **会话隔离**：子进程额外 `unset WAYLAND_DISPLAY`/`WAYLAND_SOCKET` 并设
  `XDG_SESSION_TYPE=x11`，否则宿主 Wayland 会被客户端优先选中。

### 3. 视觉默认值（`src/config.c`）

开箱即 Hyprland 风格（显式配置指令始终覆盖）：`rounding 12`、
`gaps inner 4 / outer 8`、默认边框宽 1px。回到上游观感设 `rounding 0`
并把 gaps 归零即可。

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
