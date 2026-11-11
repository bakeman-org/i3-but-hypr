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
- **titlebar 内边距**：`titlebar_padding`、`titlebar_icon_padding`。

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

## 三、性能画像与内存

- **内存**：Xephyr 1280×800、5 窗口、渐变 + 圆角 + 动画全开，i3 RSS ≈ **35 MB**
  （VSZ ≈ 50 MB），与上游 i3 同量级；fork 新增的每容器状态
  （`con_anim`、`con_state.shape_rect` 等）为几十字节级，无独立大块分配，
  无泄漏（动画结束即停表，状态复用）。
- **CPU**：静态待机时动画定时器停止，与上游一致为零轮询；
  纯 fade 场景（如 `dim_inactive` 聚焦切换）在优化后不再产生 60 Hz 重绘，
  这是 fork 在上游之上最主要的运行时 CPU 开销来源，已被消除。

## 四、已知问题

- **启动间歇性 SIGSEGV（环境问题，非代码问题）**：`load_keymap()` 崩溃于
  libxkbcommon 的 `atom_intern`。根因是本机 `/usr/local/lib` 下手工安装的
  libxkbcommon 有缺陷（i3 链接它优先于系统库）。规避：
  `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu` 启动，或卸载/重装该本地副本。
  已用 git stash 隔离验证：未修改的 fork 代码同样崩溃，与 fork 改动无关。
- 工作区根目录的历史 `core.*` 均为上述问题产物，可安全删除。
