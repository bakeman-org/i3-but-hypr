# i3-but-hypr 配置规范（中文）

本文档是 `i3-but-hypr`（上游 i3 4.25 + Hyprland / mangowc / niri 特性移植）的**完整配置参考**。
覆盖配置文件的语法、每一条配置指令的取值枚举、默认值与实际效果，并**明确标注哪些是上游指令、哪些是本 fork 的扩展**。

- 实现依据：`parser-specs/config.spec`（语法）、`src/config_directives.c`（取值校验与生效逻辑）、
  `src/config.c`（默认值）、`src/i3ext.c`（Lua 配置块）。
- 扩展功能（动画、下拉窗口、圆角、渐变、scroller 布局等）的专题说明与运行时命令，
  另见 [hyprland-mangowc-config.zh.md](hyprland-mangowc-config.zh.md)。
- 本文只描述**配置指令**；`i3-msg` / 键位里使用的**命令**（如 `layout scroller`、`dropdown toggle`、
  `switcher next`）不在此展开。

标记约定：

| 标记 | 含义 |
|------|------|
| `[上游]` | 上游 i3 已有指令，语义未改变 |
| `[扩展]` | 本 fork 新增的指令或新增的取值 |
| `[上游/扩展]` | 指令来自上游，但本 fork 增加了取值或改变了默认值 |

---

## 一、配置文件基础

### 1.1 文件查找顺序

按以下顺序查找第一个存在的文件：

1. `$XDG_CONFIG_HOME/i3/config`（通常是 `~/.config/i3/config`）
2. `~/.i3/config`
3. `$XDG_CONFIG_DIRS/i3/config`（通常是 `/etc/xdg/i3/config`）
4. 编译期 `SYSCONFDIR/i3/config`（通常是 `/etc/i3/config`）

也可以用 `i3 -c <path>` 指定。`i3 -C -c <path>` 只做**离线校验**（不启动窗口管理器），
校验失败返回非 0。

### 1.2 语法规则

- 一行一条指令；行尾 `#` 起为注释（`#` 必须位于行首或前面有空白）。
- 变量：`set $name value`。变量在做**文本替换**，只影响**其后**出现的行（包括 `include` 引入的文件）。
  同一个变量重复 `set` 时以后者为准。
- `set_from_resource $name <resource> <fallback>`：从 X 资源数据库读取，读不到时使用 fallback。
- `include <pattern>`：按 shell 规则（`wordexp`，支持 `~` 与通配符）展开，可引入多个文件；
  已被引入过的真实路径会被自动跳过（避免重复）。被引入文件与主文件**共享变量表**。
- 布尔值：真值词表共 **6** 个（大小写不敏感）：`1`、`yes`、`true`、`on`、`enable`、`active`；
  假值词表 **6** 个：`0`、`no`、`false`、`off`、`disable`、`inactive`。
  注意：多数布尔指令内部只判断“是否为真值”，其余任何字符串都按 false 处理，不会报错。
- 数值：`px` 表示**逻辑像素**，会按 `Xft.dpi` 缩放为物理像素；部分指令可写可选的 `px` 后缀，
  部分指令（扩展指令）只接受裸数字。
- 颜色：`#rrggbb` 或 `#rrggbbaa`。
- 枚举值大小写：多数枚举用 `strcasecmp`（大小写不敏感），少数（如 `gaps` 的 scope、
  `layout_from_name` 的 `default`）区分大小写，下文逐条注明。

### 1.3 热重载与配置校验 `[扩展]`

```text
hot_reload on|off        # 默认 on
status_line on|off       # 默认 on
```

- 开启后监听主配置文件与全部 `include` 文件所在目录，保存即自动重载（200 ms 防抖）。
- 重载前会在子进程里用 `i3 -C` 校验新配置；校验失败则**保留旧配置**并用 `i3-nagbar` 弹出错误。
- 关闭后只能手动触发重载（`i3-msg reload`）或重启（`i3-msg restart`）。
- 编辑器中“写临时文件再 rename”的原子保存方式也能被捕获。
- `status_line`：在焦点输出顶部显示一条常驻状态行（类似 Emacs mode line），
  实时反映 WM 当前状态与最近一次动作，便于调试：
  `[default] ws:1 layout:scroller st tiled last:focus right`。
  字段依次为 binding mode（`indicator` 色）、工作区、焦点窗口所在容器的布局
  （splith/splitv/tabbed/stacked/scroller/dwindle/master）、焦点窗口标题、
  `tiled/floating/fullscreen`、最后执行的命令。每个渲染周期结束后刷新，
  文字变化时才重绘；窗口为 override-redirect，不抢焦点，Shape 扩展可用时
  设置空输入区域以允许鼠标穿透。

---

## 二、指令总览

### 2.1 扩展指令清单 `[扩展]`

下表全部是本 fork 新增的**原生语法**指令（不含 `i3ext` Lua 块；Lua 块见第十二节）：

| 指令 | 类别 | 一句话说明 |
|------|------|-----------|
| `gaps_in <px>` | 间距 | `gaps inner` 的别名 |
| `gaps_out <px>` | 间距 | `gaps outer` 的别名 |
| `rounding` / `border_radius` `<0-100>` | 装饰 | 窗口圆角半径 |
| `border_size <0-100>` | 装饰 | 同时设置平铺/浮动边框宽度 |
| `titlebar_font <font>` | 装饰 | 标题栏独立字体 |
| `titlebar_height <0-200>` | 装饰 | 标题栏高度 |
| `titlebar_padding <0-100>` | 装饰 | 标题栏左右内边距 |
| `titlebar normal\|none` | 装饰 | 显示/隐藏标题栏 |
| `active_opacity <0.0-1.0>` | 透明度 | 焦点窗口不透明度 |
| `inactive_opacity <0.0-1.0>` | 透明度 | 非焦点窗口不透明度 |
| `dim_inactive yes\|no` | 透明度 | 非焦点窗口变暗 |
| `dim_strength <0.0-1.0>` | 透明度 | 变暗强度 |
| `client.<state>_gradient "<c1> <c2> [角度]"` | 装饰 | 渐变边框（4 种状态） |
| `animations yes\|no` | 动画 | 动画总开关 |
| `animation <类型>, <开关>, <时长>, <曲线>` | 动画 | 按类型配置动画 |
| `bezier <名称>, <x1>, <y1>, <x2>, <y2>` | 动画 | 自定义贝塞尔曲线 |
| `dropdown_height <0.1-1.0>` | 下拉窗口 | 面板高度比例 |
| `dropdown_position top\|bottom` | 下拉窗口 | 滑入方向 |
| `master_ratio <0.1-0.9>` | 布局 | master 布局主区比例 |
| `dwindle_split_ratio <0.1-0.9>` | 布局 | dwindle 布局分割比例 |
| `scroller_proportion <0.1-1.0>` | 布局 | scroller 列宽比例 |
| `scroller_structs <0-1000>` | 布局 | scroller 两侧留白 |
| `scroller_focus_center yes\|no` | 布局 | 总是居中焦点列 |
| `scroller_prefer_center yes\|no` | 布局 | 焦点列不完整时居中 |
| `scroller_prefer_overspread yes\|no` | 布局 | 首尾列铺满 |
| `scroller_ignore_proportion_single yes\|no` | 布局 | 单窗口时忽略列宽比例 |
| `scroller_default_proportion_single <0.1-1.0>` | 布局 | 单窗口比例 |
| `scroller_autofit yes\|no` | 布局 | 列宽自适应铺满 |
| `switcher_background <color>` | 主题 | Alt+Tab 切换器面板底色 |
| `switcher_bar <bg> <text>` | 主题 | 切换器底部标题栏底色 / 文字色 |
| `switcher_tile <bg> <text>` | 主题 | 无缩略图窗口的贴图底色 / 文字色 |
| `hot_reload on\|off` | 系统 | 配置热重载（默认 on） |
| `status_line on\|off` | 系统 | 顶部常驻状态行（默认 on） |
| `i3ext { ... }` | 综合 | Lua 配置块（第十二节） |

### 2.2 新增取值 `[扩展]`

- `workspace_layout` 新增 `dwindle`、`master`（并接受 `stacked` 作为 `stacking` 的别名）。
- `layout` 命令支持 `scroller`（但 `workspace_layout` 配置**不接受** `scroller`，见 4.2）。

---

## 三、装饰与外观

### 3.1 `font` `[上游]`

```text
font <font>
```

- 全局字体，Pango 描述串，如 `pango:monospace 10`；不写 `pango:` 前缀时按 X 核心字体 + 字号解析。
- 未配置时使用 `fixed`（`src/config.c`）。
- 影响：标题栏文字、i3bar 默认字体（bar 内可单独覆盖）、nagbar。

### 3.2 `titlebar_font` `[扩展]`

```text
titlebar_font <font>     # 默认回退到 font
```

- 只影响窗口标题栏，与全局 `font` 使用两份独立的字体资源（reload 安全）。
- 未配置时标题栏使用 `font`。

### 3.3 `default_border` / `new_window` / `default_floating_border` / `new_float` `[上游]`

```text
default_border          normal|pixel <px>|1pixel|none
new_window              normal|pixel <px>|1pixel|none
default_floating_border normal|pixel <px>|1pixel|none
new_float               normal|pixel <px>|1pixel|none
```

- 前两个作用于**平铺**窗口，后两个是它们的别名，作用于之后的**新窗口**；
  带 `floating` 的两个作用于**浮动**窗口。
- 边框样式枚举共 **4** 项：

| 值 | 是否带标题栏 | 边框宽度 |
|----|-------------|---------|
| `normal` | 是 | 默认 2（本 fork 默认值被改为 1 逻辑像素），可写 `normal <px>` |
| `pixel` | 否 | 可写 `pixel <px>`，只画边框 |
| `1pixel` | 否 | 固定 1 逻辑像素 |
| `none` | 否 | 0 |

- 默认：平铺 `normal` 宽 1 逻辑像素，浮动 `normal` 宽 1 逻辑像素。
- 注意：`titlebar none` 会把默认样式改为 `pixel`，`titlebar normal` 改回 `normal`（见 3.7）。

### 3.4 `hide_edge_borders` `[上游]`

```text
hide_edge_borders none|vertical|horizontal|both|smart|smart_no_gaps
hide_edge_borders <bool>        # 兼容旧写法
```

- 枚举共 **6** 项：

| 值 | 效果 |
|----|------|
| `none` | 不隐藏（默认） |
| `vertical` | 隐藏左右屏幕边缘的边框 |
| `horizontal` | 隐藏上下屏幕边缘的边框 |
| `both` | 同时隐藏上下左右边缘边框 |
| `smart` | 仅当该工作区只有一个容器时隐藏其边缘边框 |
| `smart_no_gaps` | 同 `smart`，但仅在**没有** inner/outer gaps 时生效 |

- 旧布尔写法：语法只接受真值词表中的 6 个值，等价于 `vertical`；写 `no` 等假值会**解析失败**。
- 与 `smart_borders` 互斥：`hide_edge_borders` 非 `none` 时 `smart_borders` 会被忽略并报错。

### 3.5 `smart_borders` `[上游，已废弃]`

```text
smart_borders on|no_gaps
```

- 只有 **2** 个有效取值：真值词表（等价 `hide_edge_borders smart`）与 `no_gaps`
  （等价 `hide_edge_borders smart_no_gaps`）。
- 只有在 `hide_edge_borders` 仍为默认值 `none` 时才生效，否则打印错误并忽略。
- 假值不是合法输入（`smart_borders no` 会解析失败），无法用它反向关闭。
- 新配置请直接使用 `hide_edge_borders`。

### 3.6 圆角 `[扩展]`

```text
rounding <0-100>          # 别名：border_radius
```

- 单位：逻辑像素；`0` 恢复上游直角观感。
- 范围 **0–100**，超出范围打印错误并忽略该行。
- 默认：**12**（fork 开箱观感）。
- 效果：对窗口装饰（边框/标题栏）做圆角裁剪，使用 X Shape 扩展；同时影响 frame 与子窗口的可见形状。

### 3.7 边框宽度 `[扩展]`

```text
border_size <0-100>
```

- 同时设置 `default_border_width` 与 `default_floating_border_width`（逻辑像素）。
- 只改宽度，不改“是否有标题栏”；是否有标题栏由 `default_border` / `titlebar` 决定。
- 默认：1 逻辑像素。
- 范围 **0–100**。

### 3.8 标题栏高度 `[扩展]`

```text
titlebar_height <0-200>
```

- 逻辑像素。`0` 表示由字体高度自动推导（默认）。
- 非 0 时也不会低于字体所需高度，避免文字被裁剪。
- 范围 **0–200**。

### 3.9 标题栏内边距 `[扩展]`

```text
titlebar_padding <0-100>
```

- 标题文字左右的水平内边距，逻辑像素，默认 **2**。
- 窗口图标的内边距由内部常量 `titlebar_icon_padding`（2px）控制，暂无配置指令。

### 3.10 标题栏显示 `[扩展]`

```text
titlebar normal|none
```

- 枚举 **2** 项：
  - `normal`（也接受 `on`）：显示标题栏，并把默认边框样式设为 `normal`。
  - `none`（也接受 `off`、`hidden`）：完全隐藏标题栏，并把默认边框样式设为 `pixel`，
    得到 Hyprland 式“只有边框”的外观。
- 默认：`normal`（但 `titlebar_height 0` 时标题栏仍占字体高度）。

### 3.11 客户端配色 `[上游]`

```text
client.background <color>
client.focused           <border> <background> <text> [<indicator> [<child_border>]]
client.focused_inactive  <border> <background> <text> [<indicator> [<child_border>]]
client.focused_tab_title <border> <background> <text>
client.unfocused         <border> <background> <text> [<indicator> [<child_border>]]
client.urgent            <border> <background> <text> [<indicator> [<child_border>]]
client.placeholder       <border> <background> <text> [<indicator> [<child_border>]]
```

- 颜色类共 **7** 个：`background`（单色）+ 6 个五元组类。
- 五元组顺序：`border`、`background`、`text`、可选 `indicator`、可选 `child_border`；
  省略 `child_border` 时自动取 `background`。
- `client.focused_tab_title` 语法上可写满 5 个值，但 `indicator` / `child_border` 对它**没有效果**，
  写了会打印警告（不致命）。
- `client.placeholder` 的 border 与 indicator 实际不参与绘制。
- 默认值（本 fork）：`background #11111b`；`focused` 为 `#181825 #181825 #cdd6f4 #f9e2af`；
  `focused_inactive` 为 `#181825 #181825 #a6adc8 #313244`；
  `unfocused` 为 `#11111b #11111b #6c7086 #313244`；
  `urgent` 为 `#11111b #181825 #f38ba8 #f38ba8`。

### 3.12 渐变边框 `[扩展]`

```text
client.focused_gradient          "<起始色> <结束色> [角度]"
client.focused_inactive_gradient "<起始色> <结束色> [角度]"
client.unfocused_gradient        "<起始色> <结束色> [角度]"
client.urgent_gradient           "<起始色> <结束色> [角度]"
```

- 必须是**一个带引号的字符串**，内部空格或逗号分隔：两个颜色 + 可选角度（整数，单位度）。
- 颜色 `#rrggbb` 或 `#rrggbbaa`；角度缺省 **45**，并按 360 取模归一化到 `0–359`。
- 生效范围：对应状态的窗口边框（替代纯色 `client.<state>` 的 border 颜色）；标题栏底色不受影响。
- 只定义了某几个状态时，其余状态仍使用纯色，可混用。
- 解析失败（颜色不是 `#` 开头、字段不足）打印错误并忽略该行。

### 3.13 Alt+Tab 切换器配色 `[扩展]`

```text
switcher_background <color>
switcher_bar        <background> <text>
switcher_tile       <background> <text>
```

- `switcher_background`：切换器面板底色，默认取 `client.background`。
- `switcher_bar`：底部共享标题栏的底色与文字色，默认取 `client.focused` 的
  `background` / `text`（**不是** `border`：边框通常是高亮强调色，用作文字底色时
  对比度很低，标题几乎看不清）。
- `switcher_tile`：没有缩略图的窗口贴图底色与文字色，默认取 `client.unfocused` 的
  `background` / `text`。
- 三项互相独立，未配置的项逐项回退到对应 `client.*` 颜色；切换调色板时切换器自动跟随。
- 缩略图的选中 / 未选中边框仍使用 `client.focused` / `client.unfocused` 的边框或渐变
  （见 3.12），因此切换器整体与窗口边框风格一致。
- 也可写在 `i3ext.themes.<name>.switcher` 里，见 12.3；原生指令与 Lua 块按出现顺序覆盖。

### 3.14 不透明度与暗化 `[扩展]`

```text
active_opacity   <0.0-1.0>     # 默认 1.0
inactive_opacity <0.0-1.0>     # 默认 1.0
dim_inactive     yes|no        # 默认 no
dim_strength     <0.0-1.0>     # 默认 0.15
```

- 数值自动夹到 `[0.0, 1.0]`（超出不报错，直接截断）。
- `active_opacity` / `inactive_opacity`：通过 `_NET_WM_WINDOW_OPACITY` 设置窗口整体不透明度，
  **需要合成器**（picom / xcompmgr 等）才有可见效果。
- `dim_inactive`：在**有焦点的输出**上，把非焦点窗口的透明度再乘以 `(1 - dim_strength)`，
  实现 Hyprland 式“聚焦高亮、其余变暗”。
- 焦点切换时透明度变化会走淡入淡出（受 `animation fadeIn` 控制）。

---

## 四、间距与布局

### 4.1 `gaps` `[上游]`

```text
gaps inner|outer|horizontal|vertical|top|right|bottom|left <n>[px]
```

- scope 枚举共 **8** 项，含义：

| scope | 作用范围 |
|-------|---------|
| `inner` | 相邻容器之间 |
| `outer` | 工作区四边 |
| `horizontal` | 左右（等价于同时设置 left + right） |
| `vertical` | 上下（等价于同时设置 top + bottom） |
| `top` / `right` / `bottom` / `left` | 单边 |

- 默认（本 fork）：`inner 4`，`outer 8`（即 top/right/bottom/left 各 8）。
- scope 比较**区分大小写**；`px` 后缀可省略。
- 同一 scope 重复设置时后者覆盖；`horizontal` 之后再写 `left` 只覆盖左边。

### 4.2 Hyprland 风格别名 `[扩展]`

```text
gaps_in <px>      # = gaps inner <px>
gaps_out <px>     # = gaps outer <px>
```

- 与 `gaps` 完全等价，可混用，按出现顺序后者生效。
- 只接受裸数字，不支持 `px` 后缀。

### 4.3 `smart_gaps` `[上游]`

```text
smart_gaps on|off|inverse_outer
```

- 枚举 **3** 项：

| 值 | 效果 |
|----|------|
| `off` | 始终显示 gaps（默认） |
| `on` | 工作区只有一个可见容器时，所有 gaps 归零 |
| `inverse_outer` | 只有一个可见容器时**只保留** inner gaps，outer 归零 |

- “只有一个可见容器”包含“唯一子容器是 tabbed/stacked”的情况。

### 4.4 `default_orientation` `[上游]`

```text
default_orientation horizontal|vertical|auto
```

- 枚举 **3** 项；`auto` 等价于“按屏幕长宽比决定”，为**默认值**（内部 `NO_ORIENTATION`）。
- 影响新工作区第一个 split 容器的方向。

### 4.5 `workspace_layout` `[上游/扩展]`

```text
workspace_layout default|stacking|stacked|tabbed|dwindle|master
```

- 配置解析器接受的取值共 **6** 个字符串：`default`、`stacking`、`stacked`（`stacking` 的别名）、
  `tabbed`、`dwindle` `[扩展]`、`master` `[扩展]`，对应 **5** 种实际布局。
- 默认 `default`（普通水平/垂直分割），对之后**新创建**的工作区生效。
- 注意：`scroller` 布局虽然可通过 `layout scroller` 命令启用，但**不能**写在本指令里；
  写 `scroller` 会被解析器拒绝。
- `layout_from_name` 内部还认识 `splith`、`splitv`，但本指令的语法不接受。

### 4.6 `dwindle_split_ratio` `[扩展]`

```text
dwindle_split_ratio <0.1-0.9>     # 默认 0.5
```

- dwindle 布局每次分割时，第一个（上/左）子容器占可用空间的比例。
- 越接近 0.9，新窗口越小；越接近 0.1，新窗口越大。
- 范围 **0.1–0.9**，超出打印错误并忽略。

### 4.7 `master_ratio` `[扩展]`

```text
master_ratio <0.1-0.9>            # 默认 0.6
```

- master 布局中主区域（master area）占可用宽/高的比例。
- 范围 **0.1–0.9**。
- master 区域方位（left/right/top/bottom）没有对应的原生指令，默认 left；
  需要时通过 `i3ext.layouts.master.orientation` 配置（第十二节）。

### 4.8 scroller 布局参数 `[扩展]`

```text
scroller_proportion <0.1-1.0>              # 默认 0.9
scroller_structs <0-1000>                  # 默认 20（逻辑像素）
scroller_focus_center yes|no               # 默认 no
scroller_prefer_center yes|no              # 默认 no
scroller_prefer_overspread yes|no          # 默认 yes
scroller_ignore_proportion_single yes|no   # 未设置时行为同 yes
scroller_default_proportion_single <0.1-1.0>  # 默认 1.0
scroller_autofit yes|no                    # 默认 yes
```

逐项说明：

- `scroller_proportion`：每列宽度 = 可用宽度 × 该比例；范围 0.1–1.0。
- `scroller_structs`：可视区左右两侧保留的空白（逻辑像素），0–1000。
- `scroller_focus_center`：焦点列**总是**居中，而不是只在将要移出可视区时才滚动。
- `scroller_prefer_center`：焦点列不能完整显示时，将其居中。
- `scroller_prefer_overspread`：焦点列位于首列/末列时，将其**贴靠屏幕边缘**而不是居中，
  让相邻列自然铺开（默认 `yes`）。
- `scroller_ignore_proportion_single`：单窗口时是否忽略 `scroller_proportion`。
  默认行为等同于 `yes`：单窗口铺满整屏（与上游一致）。
  写 `no` 时单窗口也使用 `scroller_default_proportion_single`（默认 1.0，仍铺满；
  把它改小才会看到按比例留白）。内部存储的是它的反值 `scroller_proportion_single`。
- `scroller_default_proportion_single`：单窗口且不忽略比例时使用的比例，范围 0.1–1.0，默认 1.0。
- `scroller_autofit`：列宽自适应可视区——列不会宽于屏幕；所有列总宽不足时按比例铺满；
  焦点列贴紧滚动到的屏幕边缘，避免侧边留下未绘制条带。默认开启。

---

## 五、焦点与窗口行为

### 5.1 `focus_follows_mouse` `[上游]`

```text
focus_follows_mouse yes|no      # 默认 yes
```

- 鼠标移入某窗口即聚焦。任何非真值字符串都表示关闭。
- 关闭后只能靠键位/点击改变焦点。

### 5.2 `focus_wrapping` `[上游]`

```text
focus_wrapping on|off|force|workspace      # 默认 on
force_focus_wrapping yes|no                # 旧写法，等价 force
```

- 枚举 **4** 项：

| 值 | 效果 |
|----|------|
| `on` | 焦点到达容器边缘时，选择“父容器方向的相邻容器” |
| `off` | 到达边缘后不再移动 |
| `force` | 总是回绕到容器的另一端 |
| `workspace` | 回绕范围限制在当前工作区 |

- `force_focus_wrapping no` 只会撤销 `force`，不会把 `on` 改成 `off`。

### 5.3 `mouse_warping` `[上游]`

```text
mouse_warping none|output
```

- 枚举 **2** 项：

| 值 | 效果 |
|----|------|
| `output` | 焦点切到另一个输出时，把鼠标指针移到该窗口中心（默认） |
| `none` | 从不移动指针 |

### 5.4 `focus_on_window_activation` `[上游]`

```text
focus_on_window_activation smart|urgent|focus|none
```

- 枚举 **4** 项，决定窗口发送 `_NET_ACTIVE_WINDOW` 时的行为：

| 值 | 效果 |
|----|------|
| `smart` | 目标工作区可见则聚焦，否则只设置 urgency 提示（默认） |
| `urgent` | 总是只设置 urgency，不抢焦点 |
| `focus` | 总是立即聚焦（可能切换工作区） |
| `none` | 完全忽略请求（既不聚焦也不设 urgency） |

### 5.5 `popup_during_fullscreen` `[上游]`

```text
popup_during_fullscreen smart|leave_fullscreen|ignore|all
```

- 枚举 **4** 项：

| 值 | 效果 |
|----|------|
| `smart` | 只显示属于全屏窗口的弹窗（默认） |
| `leave_fullscreen` | 任何弹窗出现都退出全屏 |
| `ignore` | 不映射弹窗 |
| `all` | 显示所有浮动窗口/弹窗 |

- 无法识别的值会**回退为 `smart`**（不报错）。

### 5.6 `tiling_drag` `[上游]`

```text
tiling_drag off|modifier|titlebar
tiling_drag swap_modifier <modifier>
```

- 模式枚举 **3** 项：

| 值 | 效果 |
|----|------|
| `off`（及任何假值） | 禁用平铺拖拽 |
| `modifier` | 按住 `floating_modifier` + 鼠标左键拖动平铺窗口（默认） |
| `titlebar` | 直接拖标题栏 |

- 实现里还支持 `modifier,titlebar` / `titlebar,modifier`（两者皆可），
  但配置语法不接受该写法。
- `swap_modifier`：拖拽换位时额外需要的修饰键，默认 `Shift`；
  取值与 `floating_modifier` 相同（见 5.7），可用 `+` 组合多个。

### 5.7 `floating_modifier` `[上游]`

```text
floating_modifier <modifier>[+<modifier>...]
```

- 修饰键枚举共 **8** 个：`Mod1`、`Mod2`、`Mod3`、`Mod4`、`Mod5`、`Shift`、`Control`、`Ctrl`。
- 可写多个，用 `+` 连接（如 `Mod1+Shift`）；变量替换后 `$mod` 也可用。
- 默认：`Mod4`（即 Super）。
- 效果：按住该修饰键 + 鼠标左键可移动浮动窗口，右键可调整大小，并用于平铺拖拽。

### 5.8 浮动窗口尺寸限制 `[上游]`

```text
floating_minimum_size <width> x <height>     # 默认 0 x 0 → sane 下限 75 x 50
floating_maximum_size <width> x <height>     # 默认 0 x 0 → 限制为所有输出合并尺寸
```

- 单位逻辑像素；`x` 两侧可带空格。
- 值为 `-1` 表示**关闭**该方向的限制（不检查）。
- 值为 `0` 表示使用内置合理值：最小值 75x50，最大值取所有输出合并后的宽高。
- 其他正数按绝对值处理。
- 只对**新映射**的浮动窗口及显式 `floating enable` 生效。

### 5.9 `show_marks` `[上游]`

```text
show_marks yes|no        # 默认 yes
```

- 是否在标题栏显示 mark 名。以 `_` 开头的 mark 永远不显示。

### 5.10 `title_align` `[上游]`

```text
title_align left|center|right      # 默认 center
```

- 枚举 **3** 项；窗口标题在标题栏内的水平对齐。

### 5.11 `force_display_urgency_hint` `[上游]`

```text
force_display_urgency_hint <ms> ms      # 默认 500
```

- 单位毫秒（必须带 `ms` 后缀），内部换算为秒。
- 效果：切换到带 urgency 的工作区后，延迟多久清除 urgency 标记；
  设为 `0 ms` 表示立即清除。

### 5.12 `workspace_auto_back_and_forth` `[上游]`

```text
workspace_auto_back_and_forth yes|no      # 默认 no
```

- 开启后，再次切换到当前工作区会切回上一个工作区（两工作区快速来回切换）。

### 5.13 `no_focus` `[上游]`

```text
no_focus [<criteria>]
```

- 匹配的窗口**不会**在映射时获得焦点（但仍会加入布局）。
- criteria 为空会打印错误并忽略。

### 5.14 `for_window` `[上游]`

```text
for_window [<criteria>] <命令>
```

- 窗口映射时若匹配 criteria，则执行给定命令（命令语法与键位绑定相同）。
- criteria 为空或非法时忽略并打印错误。

### 5.15 `assign` `[上游]`

```text
assign [<criteria>] [→] workspace <name>
assign [<criteria>] [→] workspace number <n>
assign [<criteria>] [→] output <name>
```

- `→` 可省略。
- `workspace number <n>` 只把窗口放到编号为 n 的工作区，不改变其名称。
- 不支持在 `assign` 中使用 `floating` / `tiling` / `all` 窗口模式 criteria（会报错）。
- criteria 为空时忽略。

### 5.16 criteria 语法 `[上游]`

```text
[class="..." instance="..." title="..." ...]
```

- 可用的匹配键共 **13** 个：

| 键 | 含义 |
|----|------|
| `class` | WM_CLASS 的 class 部分 |
| `instance` | WM_CLASS 的 instance 部分 |
| `window_role` | WM_WINDOW_ROLE |
| `con_id` | 容器 ID |
| `id` | X11 窗口 ID |
| `window_type` | `_NET_WM_WINDOW_TYPE`，取值见下 |
| `con_mark` | mark 名 |
| `title` | 窗口标题 |
| `urgent` | urgency 状态：`latest`/`newest`/`recent`/`last` 或 `oldest`/`first`（**6** 个别名，2 种含义） |
| `workspace` | 所在工作区名 |
| `machine` | WM_CLIENT_MACHINE |
| `floating_from` | 浮动来源：`auto` 或 `user` |
| `tiling_from` | 平铺来源：`auto` 或 `user` |

- 另有 **3** 个窗口模式匹配（只用于 `for_window` / `no_focus`，不用于 `assign`）：
  `tiling`、`floating`、`all`。
- `window_type` 的枚举共 **10** 项：`normal`、`dialog`、`utility`、`toolbar`、`splash`、
  `menu`、`dropdown_menu`、`popup_menu`、`tooltip`、`notification`。
- `floating_from` / `tiling_from` 的取值枚举 **2** 项：`auto`、`user`，可加引号也可不加。
- 同一 criteria 内的多个键是“与”关系；多个 criteria 块之间是“或”关系（分别写多条指令）。
- 值中的特殊字符（空格、引号）需用双引号包裹。

---

## 六、工作区

### 6.1 工作区到输出绑定 `[上游]`

```text
workspace <name> output <output> [<output> ...]
```

- 一行可列多个输出（按顺序作为候选）。
- 同一个工作区重复绑定到输出会打印错误并忽略后一条。
- 工作区名可以是数字或任意字符串。

### 6.2 工作区级 gaps `[上游]`

```text
workspace <name> gaps inner|outer|horizontal|vertical|top|right|bottom|left <n>[px]
```

- scope 枚举与全局 `gaps` 相同（**8** 项）。
- 同一工作区可写多行、不同 scope 叠加；同一 scope 后者覆盖。
- 工作区级 gaps 优先于全局 `gaps` / `gaps_in` / `gaps_out`。

### 6.3 启动工作区 `[上游]`

工作区名以数字开头时（如 `1:web`），启动时按编号排序；不带数字的工作区按创建顺序排列。
`workspace_auto_back_and_forth` 与 `focus_on_window_activation` 会影响切换行为。

---

## 七、键位绑定与模式

### 7.1 `bindsym` / `bindcode` / `bind` `[上游]`

```text
bindsym [--release] [--border] [--whole-window] [--exclude-titlebar] <modifiers>+<key> <command>
bindcode [--release] [--border] [--whole-window] [--exclude-titlebar] <modifiers>+<keycode> <command>
bind     ...   # 等价 bindsym
```

- 修饰键枚举共 **15** 个：`Mod1`、`Mod2`、`Mod3`、`Mod4`、`Mod5`、`Shift`、`Control`、
  `Ctrl`、`Mode_switch`、`Group1`、`Group2`、`Group3`、`Group4`、`$mod`（变量替换后为实际修饰键）。
  其中 `Group2` 与 `Mode_switch` 等价、`Ctrl` 与 `Control` 等价。
- `--release`：按键释放时执行（用于 push-to-talk 等）。
- `--border` / `--whole-window` / `--exclude-titlebar`：鼠标绑定的命中区域控制。
- 键名区分大小写；特殊键（`Return`、`space`、`Left` 等）按 XKB keysym 名称书写。
- 重复绑定会在 `i3 -C` 时报错（`check_for_duplicate_bindings`）。

### 7.2 `mode` `[上游]`

```text
mode [--pango_markup] <name> {
    bindsym ...
}
```

- 定义一个键位模式；名称不能是保留字 `default`。
- `--pango_markup`：该模式的名称支持 Pango 标记（i3bar 显示模式名时使用）。
- 进入模式用命令 `mode "<name>"`；`mode default` 返回默认模式。
- 同名模式定义多次会打印错误。
- 模式块内只接受 `bindsym` / `bindcode` / `bind` 与注释；块内的 `set` / `#` 行会被**忽略**
  （不会定义变量），如需变量请在块外定义。

### 7.3 `exec` / `exec_always` `[上游]`

```text
exec [--no-startup-id] <command>
exec_always [--no-startup-id] <command>
```

- `exec`：仅在 i3 首次启动时执行；`exec_always`：每次 reload 都执行。
- 命令经 shell 执行；`--no-startup-id` 关闭启动通知（用于不兼容的应用，避免鼠标变成 watch 光标）。

---

## 八、i3bar（`bar { ... }`）

### 8.1 通用

```text
bar {
    ...
}
```

- 一个配置文件可以有多个 `bar` 块；每个块生成一份独立 bar 配置。
- 未写 `id` 时自动生成 `bar-<序号>`（从 0 开始）。
- 本 fork 的 `i3ext` 主题会应用到**所有** bar 配置，因此 `bar { }` 块必须写在 `i3ext` 之前。

### 8.2 可用键

| 键 | 取值 | 默认 | 效果 |
|----|------|------|------|
| `id` | 字符串 | `bar-<n>` | bar 唯一标识，供 `i3bar --bar_id` 使用 |
| `i3bar_command` | 字符串 | `i3bar` | 启动 i3bar 的命令 |
| `status_command` | 字符串 | 无 | 状态栏命令（如 `i3status`），经 shell 执行 |
| `workspace_command` `[上游]` | 字符串 | 无 | 自定义工作区按钮生成命令，替代内置工作区按钮 |
| `socket_path` | 路径 | 无 | i3 IPC socket 路径（不推荐，通常自动发现） |
| `mode` | `dock` / `hide` / `invisible`（**3** 项） | `dock` | `dock` 常驻；`hide` 按住 `modifier` 才显示；`invisible` 永不显示 |
| `hidden_state` | `hide` / `show`（**2** 项） | `hide` | 初始隐藏状态；只在 `mode hide` 时有意义 |
| `modifier` | `off` / `none` / 修饰键组合 | `Mod4` | `hide` 模式下唤出 bar 的修饰键 |
| `bindsym` | `button<N> [--release] <命令>` | 无 | 鼠标点击 bar 时执行的命令 |
| `position` | `top` / `bottom`（**2** 项） | `bottom` | bar 位置 |
| `output` | 输出名，可写多行 | 主输出 | bar 显示在哪些输出 |
| `tray_output` | `none` / `primary` / 输出名 | 主输出 | 系统托盘显示位置；`none` 禁用托盘 |
| `tray_padding` | 数字 [+`px`] | 2 | 托盘图标内边距 |
| `font` | Pango 描述 | 全局 `font` | bar 字体 |
| `separator_symbol` | 字符串 | 竖线 | 状态块之间的分隔符 |
| `binding_mode_indicator` | 布尔 | `yes` | 是否显示当前模式名 |
| `workspace_buttons` | 布尔 | `yes` | 是否显示工作区按钮 |
| `workspace_min_width` | 数字 [+`px`] | 0 | 工作区按钮最小宽度 |
| `strip_workspace_numbers` | 布尔 | `no` | 去掉工作区名开头的数字与冒号 |
| `strip_workspace_name` | 布尔 | `no` | 只显示工作区编号 |
| `verbose` | 布尔 | `no` | 输出调试信息 |
| `height` | 数字 | 0（自动） | bar 高度（像素） |
| `padding` | 1–4 个数字 | 0 | 依次为 上/右/下/左，或 1 个=全部、2 个=上下/左右、3 个=上/左右/下 |
| `colors { ... }` | 见 8.4 | 见 8.4 | 配色 |

- `wheel_up_cmd` / `wheel_down_cmd`：已废弃，分别等价于 `bindsym button4` / `bindsym button5`，
  使用时会打印弃用警告。
- `bindsym` 只接受 `button<N>`（`N >= 1`）；同一按钮 + 同一 `--release` 状态重复定义会忽略后者。

### 8.3 `workspace_command` 说明 `[上游]`

- 配置后，i3bar 不再使用内置的工作区按钮，而是执行该命令并读取其输出的 JSON 工作区列表。
- 适合把工作区按钮完全交给外部脚本渲染。

### 8.4 `colors { ... }`

```text
colors {
    background          <color>
    statusline          <color>
    separator           <color>
    focused_background  <color>
    focused_statusline  <color>
    focused_separator   <color>

    focused_workspace   <border> <background> <text>
    active_workspace    <border> <background> <text>
    inactive_workspace  <border> <background> <text>
    urgent_workspace    <border> <background> <text>
    binding_mode        <border> <background> <text>
}
```

- 单色键共 **6** 个；三元组键共 **5** 个。
- 三元组也兼容旧的两值写法 `<text> <background>`（此时无边框色）。
- 默认：`background #000000`、`statusline #ffffff`、`separator #666666`；
  工作区三元组使用内置 i3 默认配色。
- 该配色属于 i3bar 自身；本 fork 的 `i3ext` 主题会覆盖它（所有 bar 配置）。

---

## 九、动画 `[扩展]`

### 9.1 总开关

```text
animations yes|no        # 默认 yes
```

- 关闭后所有几何/透明度动画立即跳到终态，动画定时器停止（零轮询）。

### 9.2 自定义贝塞尔曲线

```text
bezier <name>, <x1>, <y1>, <x2>, <y2>
```

- 定义一条 cubic-bezier 曲线（控制点 `(0,0)`、`(x1,y1)`、`(x2,y2)`、`(1,1)`）。
- 名称区分大小写，不能包含逗号或空白；同名覆盖；最多 **16** 条自定义曲线。
- 内置曲线共 **8** 条：

| 名称 | 特点 |
|------|------|
| `default` | 默认缓出 |
| `linear` | 匀速 |
| `easeOutCubic` | 缓出 |
| `easeOutQuint` | 更强的缓出（默认动画曲线） |
| `easeOutExpo` | 极强缓出 |
| `easeInOutCubic` | 缓入缓出 |
| `easeInOutQuint` | 更平滑的缓入缓出 |
| `easeInOutSine` | 正弦缓入缓出 |

- 曲线名找不到时回退到 `default`。

### 9.3 按类型配置动画

```text
animation <type>, <enabled>, <duration ms>, <curve>
```

- 逗号分隔，字段可省略（省略表示沿用当前值；`enabled` 省略视为 on；`duration` 省略视为 0）。
- 类型名大小写不敏感；可写名称共 **12** 个（含别名），归为 **6** 组动画 + `global`：

| 类型（及别名） | 作用 | 默认时长 | 默认曲线 |
|---------------|------|---------|---------|
| `windows` / `windowsmove` | 窗口移动/几何变化 | 180 ms | `easeOutQuint` |
| `windowsIn` / `windowopen` | 窗口打开 | 220 ms | `easeOutQuint` |
| `windowsOut` / `windowclose` | 窗口关闭 | 150 ms | `easeOutQuint` |
| `fadeIn` / `fade` / `fadeOut` | 焦点切换淡入淡出（布尔开关） | — | — |
| `workspaces` | 工作区切换 | 300 ms | `easeOutQuint` |
| `dropdown` | 下拉窗口滑入/滑出 | 250 ms | `easeOutQuint` |
| `global` | 一次性设置以上全部 | — | — |

- `duration` 为 0 或该项 `enabled` 为 false 时，该类型立即完成（无动画）。
- 未知类型会打印错误并忽略该行。

---

## 十、下拉窗口 `[扩展]`

```text
dropdown_height <0.1-1.0>      # 默认 0.5
dropdown_position top|bottom   # 默认 top
```

- `dropdown_height`：面板高度占工作区高度的比例，范围 **0.1–1.0**（超出报错并忽略）。
- `dropdown_position`：枚举 **2** 项，从顶部或底部滑入；非法值报错并忽略。
- 下拉窗口的显示/隐藏、单次几何覆盖等运行时行为由命令控制：
  `dropdown toggle|show|hide|recall [<position>,<height>,<width>,<align>]`（见专题文档）。
- 动画使用 `animation dropdown` 的时长与曲线。

---

## 十一、会话、IPC 与调试

| 指令 | 取值 | 默认 | 说明 |
|------|------|------|------|
| `ipc_socket` / `ipc-socket` | 路径 | 自动 | i3 IPC socket 路径 |
| `ipc_kill_timeout` | 毫秒 | 10000 | 关闭客户端时等待其退出的超时，超时后强杀 |
| `restart_state` | 路径 | 无 | 测试用：指定 restart 状态文件 |
| `fake_outputs` / `fake-outputs` | 输出列表 | 无 | 测试用：伪造输出几何 |
| `force_xinerama` / `force-xinerama` | 布尔 | no | 强制使用 Xinerama 而不是 RandR（仅启动时生效） |
| `disable_randr15` / `disable-randr15` | 布尔 | no | 不查询 RandR 1.5 输出信息 |

---

## 十二、`i3ext { ... }`：Lua 配置块 `[扩展]`

`i3ext` 是本 fork 的综合配置入口：块内是**真正的 Lua 5.4**，可写扩展指令、键位与主题。
语法、沙箱、应用顺序与错误的完整说明见 [hyprland-mangowc-config.zh.md 第零节](hyprland-mangowc-config.zh.md)。
此处给出**字段枚举与取值范围总表**。

### 12.1 语法要点

```lua
i3ext {
    gaps = { inner = 8, outer = 0 },
    decoration = { rounding = 8, border_size = 2 },
}
```

- 必须是顶层块；开括号必须是该行最后一个非空白字符，闭括号独占一行；字段用逗号分隔；
  注释用 Lua 的 `--`（不能用 `#`）。
- 变量替换（`$foo`）照常生效。
- 块按出现位置执行：块后的原生指令覆盖块内设置，反之亦然。
- 每个块独立沙箱，只暴露 `base`、`table`、`string`、`math`、`utf8`；
  移除 `os`、`io`、`package`、`debug`、`require`、`load*`、`dofile`、`print` 等。
- `i3 -C` 会执行并校验块内 Lua；未知字段、类型错误、范围错误都会导致校验失败。

### 12.2 顶层键（共 13 个）

`binds`、`theme`、`themes`、`bar`、`gaps`、`decoration`、`opacity`、
`titlebar`、`client`、`switcher`、`animations`、`dropdown`、`layouts`。

未列出的顶层键会报 `unknown field` 并使校验失败。

### 12.3 字段总表

| 路径 | 类型 / 枚举 | 范围 | 对应原生指令 |
|------|------------|------|-------------|
| `binds[]` | 表数组，键 `key`/`command`/`release`/`type` | — | `bindsym` |
| `binds[].type` | `bindsym` / `bindcode`（**2** 项） | — | `bindsym` / `bindcode` |
| `theme` | 主题名字符串 | — | —（仅 i3ext 主题） |
| `themes.<name>.client` | 形状同顶层 `client` | — | 见 `client` |
| `themes.<name>.bar` | 形状同顶层 `bar` | — | 见 `bar` |
| `themes.<name>.switcher` | 形状同顶层 `switcher` | — | 见 `switcher` |
| `bar` | 11 个键（6 单色 + 5 三元组） | 颜色字符串 | bar `colors` |
| `gaps` | 8 个键：`inner`/`outer`/`horizontal`/`vertical`/`top`/`right`/`bottom`/`left` | 0–1000 整数 | `gaps` / `gaps_in` / `gaps_out` |
| `decoration.rounding` | 整数 | 0–100 | `rounding` |
| `decoration.border_size` | 整数 | 0–100 | `border_size` |
| `opacity.active` | 数字 | 0.0–1.0 | `active_opacity` |
| `opacity.inactive` | 数字 | 0.0–1.0 | `inactive_opacity` |
| `opacity.dim_inactive` | 布尔 | — | `dim_inactive` |
| `opacity.dim_strength` | 数字 | 0.0–1.0 | `dim_strength` |
| `titlebar.align` | `left` / `center` / `right`（**3** 项） | — | `title_align` |
| `titlebar.height` | 整数 | 0–200 | `titlebar_height` |
| `titlebar.padding` | 整数 | 0–100 | `titlebar_padding` |
| `titlebar.font` | 字符串 | — | `titlebar_font` |
| `titlebar.visibility` | `normal` / `none`（**2** 项） | — | `titlebar` |
| `client.background` | 颜色 | — | `client.background` |
| `client.colors.<state>` | 5 色数组 | — | `client.<state>` |
| `client.gradients.<state>` | `{起始色, 结束色[, 角度]}` | 角度整数 | `client.<state>_gradient` |
| `client.colors.*` / `client.gradients.*` 的 state | `focused` / `focused_inactive` / `unfocused` / `urgent`（**4** 项） | — | 同上 |
| `switcher.background` | 颜色 | — | `switcher_background` |
| `switcher.bar` | 2 色数组 `{底色, 文字色}` | — | `switcher_bar` |
| `switcher.tile` | 2 色数组 `{底色, 文字色}` | — | `switcher_tile` |
| `animations.enabled` | 布尔 | — | `animations` |
| `animations.beziers.<name>` | 4 个数字的数组 | — | `bezier` |
| `animations.windows` / `open` / `close` / `workspaces` / `dropdown` | 表：`duration` / `curve` / `enabled` | — | `animation` |
| `animations.fade_in` | 布尔 | — | `animation fadeIn` |
| `dropdown.height` | 数字 | 0.1–1.0 | `dropdown_height` |
| `dropdown.position` | `top` / `bottom`（**2** 项） | — | `dropdown_position` |
| `layouts.master.ratio` | 数字 | 0.1–0.9 | `master_ratio` |
| `layouts.master.orientation` | `left` / `right` / `top` / `bottom`（**4** 项） | — | 无原生指令 |
| `layouts.dwindle.split_ratio` | 数字 | 0.1–0.9 | `dwindle_split_ratio` |
| `layouts.scroller.proportion` | 数字 | 0.1–1.0 | `scroller_proportion` |
| `layouts.scroller.structs` | 整数 | 0–1000 | `scroller_structs` |
| `layouts.scroller.focus_center` | 布尔 | — | `scroller_focus_center` |
| `layouts.scroller.prefer_center` | 布尔 | — | `scroller_prefer_center` |
| `layouts.scroller.prefer_overspread` | 布尔 | — | `scroller_prefer_overspread` |
| `layouts.scroller.ignore_proportion_single` | 布尔 | — | `scroller_ignore_proportion_single` |
| `layouts.scroller.default_proportion_single` | 数字 | 0.1–1.0 | `scroller_default_proportion_single` |
| `layouts.scroller.autofit` | 布尔 | — | `scroller_autofit` |

### 12.4 应用顺序

固定为：

`binds` → 主题 `client` → 主题 `bar` → 主题 `switcher` → `gaps` → `decoration` →
`opacity` → `titlebar` → 顶层 `client` → 顶层 `bar` → 顶层 `switcher` →
`animations` → `dropdown` → `layouts`。

因此顶层 `client` / `bar` / `switcher` 覆盖主题；`layouts` 最后应用。

### 12.5 与原生指令的关系

- 第十二节列出的原生扩展指令**全部保留**，可与 `i3ext` 混用，按出现顺序后者覆盖前者。
- 推荐统一使用 `i3ext`；块外保留上游 i3 配置、上游命令键位与带 criteria / mode 的键位。

---

## 十三、枚举速查表

| 枚举 | 项数 | 取值 |
|------|------|------|
| 布尔真值词 | 6 | `1` `yes` `true` `on` `enable` `active` |
| 布尔假值词 | 6 | `0` `no` `false` `off` `disable` `inactive` |
| `gaps` scope | 8 | `inner` `outer` `horizontal` `vertical` `top` `right` `bottom` `left` |
| `default_orientation` | 3 | `horizontal` `vertical` `auto` |
| `workspace_layout` | 6 字符串 / 5 布局 | `default` `stacking` `stacked` `tabbed` `dwindle` `master` |
| `default_border` 样式 | 4 | `normal` `pixel` `1pixel` `none` |
| `hide_edge_borders` | 6 | `none` `vertical` `horizontal` `both` `smart` `smart_no_gaps` |
| `smart_borders` | 2 | 真值词 `no_gaps` |
| `smart_gaps` | 3 | `on` `off` `inverse_outer` |
| `focus_wrapping` | 4 | `on` `off` `force` `workspace` |
| `mouse_warping` | 2 | `none` `output` |
| `focus_on_window_activation` | 4 | `smart` `urgent` `focus` `none` |
| `popup_during_fullscreen` | 4 | `smart` `leave_fullscreen` `ignore` `all` |
| `tiling_drag` | 3 | `off` `modifier` `titlebar` |
| 修饰键 | 8 | `Mod1`–`Mod5` `Shift` `Control`/`Ctrl`（键位另含 `Mode_switch` `Group1`–`Group4` `$mod`） |
| `title_align` | 3 | `left` `center` `right` |
| criteria 键 | 13 | `class` `instance` `window_role` `con_id` `id` `window_type` `con_mark` `title` `urgent` `workspace` `machine` `floating_from` `tiling_from` |
| criteria 窗口模式 | 3 | `tiling` `floating` `all` |
| `window_type` | 10 | `normal` `dialog` `utility` `toolbar` `splash` `menu` `dropdown_menu` `popup_menu` `tooltip` `notification` |
| `_from` 来源 | 2 | `auto` `user` |
| bar `mode` | 3 | `dock` `hide` `invisible` |
| bar `hidden_state` | 2 | `hide` `show` |
| bar `position` | 2 | `top` `bottom` |
| 动画类型（可写名称 / 分组） | 12 / 6+1 | `windows` `windowsmove` `windowsin` `windowopen` `windowsout` `windowclose` `fade` `fadein` `fadeout` `workspaces` `dropdown` `global` |
| 内置贝塞尔曲线 | 8 | `default` `linear` `easeOutCubic` `easeOutQuint` `easeOutExpo` `easeInOutCubic` `easeInOutQuint` `easeInOutSine` |
| `dropdown_position` | 2 | `top` `bottom` |
| master 方位（仅 i3ext） | 4 | `left` `right` `top` `bottom` |
| i3ext 顶层键 | 13 | `binds` `theme` `themes` `bar` `gaps` `decoration` `opacity` `titlebar` `client` `switcher` `animations` `dropdown` `layouts` |

---

## 十四、默认值总表（本 fork）

| 项目 | 默认值 | 与上游差异 |
|------|--------|-----------|
| 字体 | `fixed` | — |
| 标题栏字体 | 回退 `font` | 扩展 |
| 平铺/浮动边框 | `normal`，宽 1 逻辑像素 | 上游默认宽 2 |
| `rounding` | 12 | 扩展 |
| `titlebar_height` | 0（按字体推导） | 扩展 |
| `titlebar_padding` | 2 | 扩展 |
| `titlebar` 可见性 | `normal` | 扩展 |
| `gaps inner` | 4 | 上游 2 |
| `gaps outer` | 8（四边各 8） | 上游 0 |
| `smart_gaps` | off | — |
| `default_orientation` | auto | — |
| `workspace_layout` | default | — |
| `dwindle_split_ratio` | 0.5 | 扩展 |
| `master_ratio` | 0.6 | 扩展 |
| `scroller_proportion` | 0.9 | 扩展 |
| `scroller_structs` | 20 | 扩展 |
| `scroller_focus_center` | no | 扩展 |
| `scroller_prefer_center` | no | 扩展 |
| `scroller_prefer_overspread` | yes | 扩展 |
| `scroller_ignore_proportion_single` | 未设置（行为同 yes） | 扩展 |
| `scroller_default_proportion_single` | 1.0 | 扩展 |
| `scroller_autofit` | yes | 扩展 |
| `active_opacity` / `inactive_opacity` | 1.0 / 1.0 | 扩展 |
| `dim_inactive` / `dim_strength` | no / 0.15 | 扩展 |
| `animations` | yes | 扩展 |
| 动画时长 | windows 180 / open 220 / close 150 / workspaces 300 / dropdown 250 ms | 扩展 |
| 动画曲线 | `easeOutQuint` | 扩展 |
| `dropdown_height` / `dropdown_position` | 0.5 / `top` | 扩展 |
| `hot_reload` | yes | 扩展 |
| `status_line` | yes | 扩展 |
| `focus_follows_mouse` | yes | — |
| `focus_wrapping` | on | — |
| `mouse_warping` | output | — |
| `focus_on_window_activation` | smart | — |
| `popup_during_fullscreen` | smart | — |
| `tiling_drag` | modifier | — |
| `tiling_drag swap_modifier` | Shift | — |
| `floating_modifier` | Mod4 | — |
| `floating_minimum_size` | 0 x 0（实际生效 75 x 50） | — |
| `floating_maximum_size` | 0 x 0（限制为输出合并尺寸） | — |
| `show_marks` | yes | — |
| `title_align` | center | — |
| `force_display_urgency_hint` | 500 ms | — |
| `workspace_auto_back_and_forth` | no | — |
| `hide_edge_borders` | none | — |
| `ipc_kill_timeout` | 1000 ms | — |
| bar `mode` | dock | — |
| bar `hidden_state` | hide（仅 `mode hide` 生效） | — |
| bar `position` | bottom | — |
| bar `modifier` | Mod4 | — |
| bar `tray_padding` | 2 | — |
| bar `workspace_buttons` | yes | — |
| bar `binding_mode_indicator` | yes | — |

---

## 十五、最小可用示例

```text
# 基础
set $mod Mod4
font pango:monospace 10
titlebar_font pango:monospace 9

# 外观
rounding 8
border_size 2
gaps_in 6
gaps_out 10
titlebar normal

# 行为
focus_follows_mouse yes
focus_wrapping on
workspace_layout dwindle
dwindle_split_ratio 0.5

# 动画
animations yes
animation windows, 1, 180, easeOutQuint
animation workspaces, 1, 300, easeOutQuint

# 键位
bindsym $mod+Return exec st
bindsym $mod+Shift+q kill
bindsym $mod+1 workspace number 1

# 工作区绑定
workspace 1 output DP-1
workspace 2 output DP-1

# bar
bar {
    status_command i3status
    position top
    tray_output primary
}

# 扩展综合配置（必须写在 bar 之后）
i3ext {
    decoration = { rounding = 8, border_size = 2 },
    opacity = { active = 1.0, inactive = 0.95, dim_inactive = true, dim_strength = 0.1 },
    layouts = { scroller = { proportion = 0.9, autofit = true } },
    binds = {
        { key = "Mod4+Shift+t", command = "titlebar toggle" },
        { key = "Mod1+Tab", command = "switcher next" },
    },
}
```
