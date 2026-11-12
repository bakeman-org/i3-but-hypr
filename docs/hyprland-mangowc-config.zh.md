# i3 (hypri3 定制版) 配置规格 —— Hyprland / mangowc 移植功能

本文档描述本分支在标准 i3 配置语法之上移植的 Hyprland 与 mangowc 风格特性，
包括最终的配置指令（config directives）与运行时命令（commands）。

## 零、`i3ext { ... }`：Lua 配置块

本分支把 fork 专属的外观、动画、下拉窗口和布局参数集中到一个顶层配置块中。
块内是**真正的 Lua 5.4**，由内嵌的 Lua 解释器执行：

```lua
i3ext {
    gaps = { inner = 8, outer = 0 },
    decoration = { rounding = 8, border_size = 2 },
}
```

### 语法规则

- `i3ext` 必须是顶层块，不能写在 `bar { ... }` 或 `mode ... { ... }` 内。
- 开括号 `{` 必须是 `i3ext` 行最后一个非空白字符；闭括号 `}` 必须独占一行
  （其后只允许空白或 `--` 注释）。不支持单行块。
- 字段之间必须用逗号；注释必须使用 Lua 的 `--`，不能用 `#`。
- 变量替换（`set $foo ...`）照常生效，块内也可以写 `$foo`。
- 块按出现位置执行：块后的原生指令覆盖块内设置，块后的 `i3ext` 块也会覆盖
  前一个块；与旧配置的“后写覆盖”语义一致。
- 每个块拥有独立、沙箱化的 Lua state，块之间不共享全局变量。
- 块本身是一个 Lua table 表达式（编译为一次 `i3ext { ... }` 调用）；字段值可以用
  Lua 表达式（算术、字符串拼接、函数调用等），但不能在 table 构造器内部写
  `local`、`if`、`for` 等语句。

### 沙箱

块内只能使用 `base`、`table`、`string`、`math`、`utf8`。以下内容被移除：
`os`、`io`、`package`、`debug`、`require`、`load`、`loadfile`、`dofile`、
`loadstring`、`collectgarbage`、`print`。块内可以调用全局函数
`i3ext(table)`（配置块本身会被编译为一次 `i3ext { ... }` 调用）。

### 扩展键位 `binds`

fork 扩展键位也写在块内；块外只保留上游 i3 配置、上游命令键位和外部工具键位：

```lua
binds = {
    { key = "Mod4+Shift+t", command = "titlebar toggle" },
    { key = "Mod4+Ctrl+1", command = "snap top-left" },
    { key = "Mod1+Tab", command = "switcher next" },
    { key = "Mod4+d", command = "exec dmenu_run", release = true },
}
```

- `key` 是完整字符串，按**最后一个** `+` 拆成修饰键和键名/键码：
  `Mod4+Shift+t` → modifiers `Mod4+Shift`、key `t`。
- `command` 原样交给 i3 命令解析器。
- `release = true` 对应原生 `bindsym --release`。
- 可选 `type = "bindcode"`，此时 `key` 的最后一段必须是数字键码
  （如 `Mod4+24`）；默认 `type = "bindsym"`。
- 首版不支持 criteria 前缀和 mode 内键位，这类绑定继续使用原生 `bindsym`。
- `binds` 在固定顺序中**最先**应用；与原生 `bindsym` 重复时由
  `check_for_duplicate_bindings()` 报告并使 `i3 -C` 失败。
- 原生写法示例（留在块外）：`bindsym $mod+u border toggle`、
  `bindsym $mod+Return exec st`、`bindsym $mod+j focus left`。

### 完整 schema

所有字段均可省略：

```lua
i3ext {
    binds = {
        { key = "Mod4+backslash", command = "exec --no-startup-id /home/etcix/.config/i3/bin/i3-dropdown-toggle" },
        { key = "Mod4+Shift+backslash", command = "dropdown recall" },
        { key = "Mod4+Shift+t", command = "titlebar toggle" },
    },
    gaps = {
        inner = 8,
        outer = 0,
        -- 可选：horizontal / vertical / top / right / bottom / left
    },
    decoration = {
        rounding = 8,              -- 0..100
        border_size = 2,           -- 0..100
    },
    opacity = {
        active = 1.0,              -- 0.0..1.0
        inactive = 0.92,           -- 0.0..1.0
        dim_inactive = true,
        dim_strength = 0.10,       -- 0.0..1.0
    },
    titlebar = {
        align = "center",          -- left | center | right
        height = 20,               -- 0..200
        padding = 4,               -- 0..100
        font = "pango:monospace 24",
        visibility = "normal",     -- normal | none
    },
    client = {
        background = "#11111b",    -- #rrggbb 或 #rrggbbaa
        colors = {
            -- border / background / text / indicator / child_border
            focused = { "#1e1e2e", "#1414b0ff", "#cdd6f4", "#f9e2af", "#1e1e2e" },
            focused_inactive = { "#181825", "#010156ff", "#bac2de", "#313244", "#181825" },
            unfocused = { "#181825", "#810639ff", "#9399b2", "#313244", "#181825" },
            urgent = { "#11111b", "#11111b", "#f38ba8", "#f38ba8", "#f38ba8" },
        },
        gradients = {
            -- start / end / angle（角度可省略，默认 45）
            focused = { "#adecc9ff", "#561ac5ff", 45 },
            focused_inactive = { "#313244", "#0b0bb3ff", 45 },
            unfocused = { "#313244", "#9f08c9ff", 45 },
            urgent = { "#f38ba8", "#ff5555", 90 },
        },
    },
    animations = {
        enabled = true,
        beziers = {
            myQuint = { 0.23, 1.0, 0.32, 1.0 },
        },
        windows = { duration = 180, curve = "myQuint" },
        open = { duration = 220, curve = "easeOutQuint" },
        close = { duration = 150, curve = "easeOutQuint" },
        fade_in = true,
        workspaces = { duration = 300, curve = "easeOutQuint" },
        dropdown = { duration = 250, curve = "easeOutQuint" },
    },
    dropdown = {
        height = 0.40,             -- 0.1..1.0
        position = "top",          -- top | bottom
    },
    layouts = {
        master = { ratio = 0.6, orientation = "left" },   -- left|right|top|bottom
        dwindle = { split_ratio = 0.5 },                  -- 0.1..0.9
        scroller = {
            proportion = 0.9,                  -- 0.1..1.0
            structs = 20,                      -- 0..1000 px
            focus_center = false,
            prefer_center = false,
            prefer_overspread = true,
            ignore_proportion_single = true,
            default_proportion_single = 1.0,   -- 0.1..1.0
            autofit = true,
        },
    },
}
```

约定：

- `animations.<类型>`：`duration` 省略时沿用当前配置值（不会清零）；
  `enabled = false` 关闭该类型；`open` 映射到 `windowsIn`，`close` 映射到
  `windowsOut`，`fade_in` 映射到 `fadeIn`。贝塞尔曲线先注册、后应用动画。
- `layouts.scroller.ignore_proportion_single` 对应旧原生指令的
  `scroller_ignore_proportion_single`（内部取反后存入
  `scroller_proportion_single`）。
- 应用顺序固定为：`binds` → `gaps` → `decoration` → `opacity` → `titlebar` →
  `client.background` → `client.colors` → `client.gradients` → `animations` →
  `dropdown` → `layouts`，因此与顺序相关的副作用稳定可复现。

### 错误与校验

- `i3 -C` 会执行并校验块内的 Lua；未知字段、类型错误、范围错误、Lua 语法错误
  和沙箱越界都会使校验失败。
- Lua 语法/运行时错误精确到 `文件名:行号`，例如：
  `~/.config/i3/config:225: attempt to index a nil value (global 'os')`。
- schema 校验错误包含完整路径，例如
  `~/.config/i3/config:225: i3ext.dropdown.height: expected number`；
  由于 Lua 无法提供表字段的源码行号，这类错误定位到 `i3ext` 块起始行，
  字段路径已足以定位问题。
- 热重载会先在子进程中校验；校验失败时保留旧配置。

### 与原生指令的关系

旧的原生 fork 指令（`gaps_in`、`rounding`、`border_size`、
`active_opacity`、`titlebar_*`、`client.*_gradient`、`animations`、`bezier`、
`animation`、`dropdown_height`、`dropdown_position`、`master_ratio`、
`dwindle_split_ratio`、`scroller_*` 等）仍然保留并可继续使用。两者可以混用，
按出现顺序后者覆盖前者。新配置推荐统一使用 `i3ext`：fork 扩展设置与键位都在
块内，块外只保留上游 i3 配置、上游命令键位和外部工具键位；上游命令键位以及
带 criteria / mode 的 fork 键位仍写在原生配置中。

## 一、外观类配置指令

### 间距（Hyprland 风格别名）

```text
gaps_in <px>
gaps_out <px>
```

- 等价于标准写法 `gaps inner <n>px` / `gaps outer <n>px`，可混用（后写者生效）。
- 默认：`gaps_in 4`、`gaps_out 8`（fork 开箱观感；上游为 inner 2 / outer 0）。

### 圆角

```text
rounding <0-100>          # 亦可用别名 border_radius
```

- 单位为逻辑像素，作用于窗口边框与标题栏的圆角。
- 默认 `12`（fork 开箱观感；回到上游直角观感设 `rounding 0`）。

### 边框宽度（Hyprland 风格别名）

```text
border_size <0-100>
```

- 等价于同时设置平铺与浮动窗口的边框宽度。
- 边框样式（有无标题栏）由 `titlebar` 控制，见下文。

### 标题栏

```text
titlebar_font <font>      # 例如 titlebar_font pango:monospace bold 9
titlebar_height <0-200>
titlebar_padding <0-100>
titlebar normal|none
```

- `titlebar_font`：标题栏专用字体（含字号），缺省回退全局 `font`。
  与全局 `font` 的资源完全独立（使用点动态选择，reload 安全）。
- `titlebar_height`：标题栏高度，`0` 表示不保留标题栏空间；默认 `0`。
  显式设置时也不会低于字体所需高度（避免标题被裁剪）。
- `titlebar_padding`：标题栏内边距；默认 `2`。
  图标内边距 `titlebar_icon_padding` 是内部默认值（2px），暂无配置指令。
- `titlebar none`：完全隐藏标题栏（Hyprland 无边框观感），同时
  将默认边框样式切换为 `pixel`；`titlebar normal` 恢复。

### 不透明度与暗化

```text
active_opacity <0.0-1.0>
inactive_opacity <0.0-1.0>
dim_inactive yes|no
dim_strength <0.0-1.0>
```

- 默认：`active_opacity 1.0`、`inactive_opacity 1.0`、
  `dim_inactive no`、`dim_strength 0.15`。

### 渐变边框（最终语法：单个带引号字符串）

```text
client.focused_gradient          "#起始色 结束色 [角度]"
client.focused_inactive_gradient "#起始色 结束色 [角度]"
client.unfocused_gradient        "#起始色 结束色 [角度]"
client.urgent_gradient           "#起始色 结束色 [角度]"
```

- 颜色为 `#rrggbb` 或 `#rrggbbaa`；角度为整数（度），可省略，默认 `45`。
- 渐变替代对应状态下窗口边框/标题栏的纯色
  （`client.focused` 等颜色指令仍作为回退与状态栏配色使用）。
- 示例：

```text
client.focused_gradient          "#33ccff #00ff99 [45]"
client.unfocused_gradient        "#444455 #222233"
client.urgent_gradient           "#ff5555 #ff9955 [90]"
```

> 注意：旧的三参数写法 `client.focused_gradient <start> <end> [angle]`
> 已移除，必须使用上述单个字符串形式。

## 二、动画

```text
animations on|off
bezier <名称>, <x1>, <y1>, <x2>, <y2>
animation <类型>, <开关>, <时长 ms>, <曲线名>
```

- 内置贝塞尔曲线：`default`、`linear`、`easeOutCubic`、`easeOutQuint`、
  `easeOutExpo`、`easeOutBack`、`easeInOutBack`、`easeOutCirc`、
  `easeInOutCubic`、`easeInOutQuint`、`easeInOutSine`。
  其中 `easeOutBack` / `easeInOutBack` 会轻微过冲（适合窗口打开、工作区切换），
  淡入淡出仍会自动夹到 `[0,1]`。
  可用 `bezier` 自定义（最多 16 条，同名覆盖）。
- `animation` 类型（大小写不敏感）：

| 类型 | 别名 | 作用 |
|------|------|------|
| `windows` | `windowsmove` | 窗口移动/几何动画 |
| `windowsIn` | `windowopen` | 窗口打开动画 |
| `windowsOut` | `windowclose` | 窗口关闭动画 |
| `fadeIn` | `fade` / `fadeOut` | 焦点切换淡入淡出 |
| `workspaces` | — | 工作区切换动画 |
| `dropdown` | — | 下拉窗口滑入/滑出动画 |
| `global` | — | 一次性设置以上全部 |

- 字段均可省略：开关默认 `on`，时长缺省为 `0`（沿用对应默认），曲线缺省沿用原值。
- 默认值：总开关 `animations on`，windows 180ms、open 220ms、close 150ms、
  workspaces 300ms、dropdown 250ms，曲线 `easeOutQuint`。

示例：

```text
animations on
bezier myQuint, 0.23, 1.0, 0.32, 1.0
animation windows, 1, 180, myQuint
animation workspaces, 1, 300, easeOutQuint
```

### 下拉窗口（dropdown）

Yakuake/niri 风格的通用下拉窗口：任意 GUI 窗口都可以向上（或向下）滑出屏幕，
进程保持存活于 scratchpad，再次触发时作为面板滑回。

```text
dropdown_height <0.1-1.0>      # 面板高度占工作区高度的比例，默认 0.5
dropdown_position top|bottom   # 从顶部或底部滑入，默认 top
animation dropdown, <开关>, <时长 ms>, <曲线名>   # 默认 250ms / easeOutQuint
```

- 几何使用工作区 `ws->rect`（已包含 outer gaps），并在此基础上再按 `gaps inner`
  向四边内缩同样的距离（下称 dropdown 间距，取值 0–16px）：单窗口下拉和多窗口
  band 共用这条外边距，因此都不会贴着屏幕边缘。`dropdown_height` /
  `dropdown_position` 给出默认值；单次调用还可以用几何参数覆盖（见下）。
- 单个下拉的宽高按内缩后区域计算；`align left|center|right` 与
  `position top|bottom` 也都在该区域内定位。
- 隐藏时先播放滑出动画，动画结束后才移入 `__i3_scratch`；显示时从屏幕外滑到目标位置。
  `animations off` 或时长为 0 时立即显示/隐藏。
- 多个下拉窗口同时显示时自动平铺，互不遮挡：同一工作区上最多 4 个时横向等分，
  超过 4 个改为两行；它们共享同一条全局 band（高度取 `dropdown_height`，位置取
  `dropdown_position`），band 同样位于内缩区域内，槽与槽之间使用相同的 dropdown
  间距。多窗口时不再对每个 tile 调用 `floating_check_size()`，避免应用 size hint
  造成宽高不齐，保证每个 tile 都是精确的等宽等高槽位。此时单客户端的
  position/height/width/align 覆盖不生效。
  显示或隐藏任意一个都会立即重排其余可见下拉；重新排布只发生在显示/隐藏时，
  不会产生每帧开销。
- 只有一个可见下拉时，继续使用下面列出的每客户端几何覆盖；新显示的下拉从各自
  槽位的屏幕外滑入。
- 隐藏的下拉窗口仍驻留在 `__i3_scratch`，进程保持存活（Yakuake 语义），不会被杀死。
  用 `dropdown recall` 或 Alt+Tab 重新显示。
- Alt+Tab（`switcher`）会列出隐藏的下拉窗口，标题前带 `[hidden]` 标记；选中并提交后
  该窗口作为下拉面板重新显示，而不是仅仅聚焦 scratchpad 容器。普通 scratchpad
  窗口（未标记为 dropdown）不会出现在 Alt+Tab 列表中。要真正结束隐藏的下拉进程，
  请使用 `kill` 或先显示再正常关闭。
- 命令（支持标准 criteria 前缀）：

```text
dropdown toggle [<position>,<height>,<width>,<align>]   # 焦点优先：转换/收起当前窗口
dropdown show   [<position>,<height>,<width>,<align>]   # 显示当前焦点窗口
dropdown hide                                           # 隐藏当前可见的下拉窗口（忽略几何参数）
dropdown recall [<position>,<height>,<width>,<align>]   # 召回最近隐藏的下拉窗口
[instance="drop"] dropdown toggle "bottom,0.25,0.4,right"
```

几何参数是一个可选的、逗号分隔的字符串，四个字段都可以留空：

| 字段 | 取值 | 缺省 |
|------|------|------|
| position | `top` / `bottom` | 配置 `dropdown_position` |
| height | `0.05`–`1.0`（占工作区高度比例） | 配置 `dropdown_height` |
| width | `0.05`–`1.0`（占工作区宽度比例） | `1.0`（全宽） |
| align | `left` / `center` / `right` | `center` |

- 字段为空（如 `"bottom,,0.4,"`）时沿用上表缺省；非法字段会记 ELOG 并忽略。
- 覆盖值保存在该客户端上：后续不带几何参数的 `dropdown toggle` 会继续使用，
  直到下一次带参数调用将其改写。
- 通用用法：先用 `$mod+Return` 等正常方式启动任意 GUI 程序，再按 `$mod+\`
  把它变成下拉面板；再按一次收起。GPUI/终端/浏览器等都可以。
  下面的绑定即可覆盖原来 quake-term 的两条规则：

```lua
i3ext {
    binds = {
        -- 当前焦点窗口：普通窗口直接转换并显示；收起下拉前用 dmenu 确认
        { key = "Mod4+backslash", command = "exec --no-startup-id /home/etcix/.config/i3/bin/i3-dropdown-toggle" },
        -- 召回最近隐藏的下拉窗口（可连续用于多个应用）
        { key = "Mod4+Shift+backslash", command = "dropdown recall" },
    },
}
```

原生 `bindsym` 仍可绑定同样的命令，但本 fork 推荐把扩展键位写在 `i3ext.binds`；
块外只保留上游 i3 和外部工具键位。

- `~/.config/i3/bin/i3-dropdown-toggle` 是 `$mod+\` 使用的确认助手（Python 3 标准库）：
  焦点窗口是可见下拉时，用 dmenu 询问 `Hide dropdown?`（Yes/No），只有选择 Yes
  才隐藏；普通窗口则直接执行 `dropdown toggle` 完成转换并显示，不弹确认。
  隐藏时按原窗口 id（criteria）下发命令，因此 dmenu 改变焦点也不会收错窗口。
  命令本身 `dropdown hide` 以及不带 criteria 的 `dropdown toggle` 仍然是非交互的，
  脚本只是键位层面的包装。

- 需要按应用固定几何时，仍可用 criteria：
  `[instance="quake-alacritty"] dropdown toggle "bottom,0.25,0.4,right"`。
- `~/.config/i3/bin/i3-dropdown` 是一个可选的启动/切换助手：
  `-i/--instance` 指定 WM_CLASS 实例名，`-W` 追加当前焦点工作区 id（如
  `quake-st:1`），`-s/--spec` 传给 `dropdown` 命令，`--` 之后是要启动的命令；
  命令参数中的字面量 `{instance}` 会被替换为最终实例名。窗口已存在时直接切换，
  否则启动命令并在 5 秒内轮询 `dropdown show` 直到窗口出现。纯通用键位不需要它。

无 criteria 的 `dropdown toggle` 是焦点优先：焦点窗口是可见下拉时收起它，否则把当前
焦点窗口转换为下拉并显示。隐藏后焦点交还给原来的可见窗口；用 `dropdown recall`
重新打开最近隐藏的那个下拉窗口。

> 已知限制：下拉标记不做 restart/配置重载持久化，i3 重启后窗口恢复为普通 scratchpad 窗口。

## 三、布局

### 布局类型

`layout` 命令在标准布局（`default|stacked|stacking|tabbed|splitv|splith`）之外新增：

- `layout dwindle` —— 螺旋平铺（Hyprland dwindle）
- `layout master` —— 主从布局（Hyprland master）
- `layout scroller` —— 滚动列布局（mangowc scroller）

`workspace_layout` 同样接受 `dwindle`、`master`。

### 布局参数

```text
master_ratio <0.1-0.9>            # master 布局主区宽度占比，默认 0.6
dwindle_split_ratio <0.1-0.9>     # dwindle 首次分割比例，默认 0.5
```

### scroller 布局参数（全部来自 mangowc）

```text
scroller_proportion <0.1-1.0>             # 焦点列宽度占比，默认 0.9
scroller_structs <px>                     # 可见区域两侧留白，默认 20
scroller_focus_center yes|no              # 焦点列永远居中，默认 no
scroller_prefer_center yes|no             # 允许时优先居中，默认 no
scroller_prefer_overspread yes|no         # 焦点列在边缘时锚定该边缘，默认 yes
scroller_ignore_proportion_single yes|no  # 单列工作区忽略比例设置，默认 no
scroller_default_proportion_single <0.1-1.0>  # 单列时的占比，默认 1.0
scroller_autofit yes|no                   # 自动铺满可视区，默认 yes
```

`scroller_autofit`（默认开启）保证可视区不留黑边：列宽按比例放大铺满可视区
（比例关系不变）、列永远不会比屏幕更宽、焦点列锚定屏幕边缘时贴边
（`scroller_structs` 不再在屏幕边缘留出底色条）。

焦点列定位优先级：`scroller_focus_center` > 单列 > `scroller_prefer_overspread`
（锚定最近边缘）> `scroller_prefer_center`；均未命中时锚定最近边缘。
超出可见区域的列坐标允许为负，由 X server 截断裁剪（等同合成器的越界裁剪）。

## 四、运行时命令（用于 bindsym / i3-msg）

### 顶层命令

```text
focusmaster       # 焦点移到 master 布局的主窗口
swapwithmaster    # 当前窗口与主窗口交换
togglesplit       # dwindle 下切换分割方向（末块回退到前一个分割）
pseudo            # dwindle 伪平铺切换
centerwindow      # 将当前窗口居中
cyclenext         # 循环聚焦下一个窗口
cycleprev         # 循环聚焦上一个窗口
snap left|right|up|down                            # 浮动窗口贴边；平铺时等价 focus
snap top-left|top-right|bottom-left|bottom-right   # 直接贴到四分屏
switcher next|prev                                 # Alt+Tab 缩略图切换器（打开/循环）
switcher commit|close                              # 确认 / 取消
dropdown toggle|show|hide [<spec>]                 # 下拉窗口（支持 criteria 与几何参数）
```

`snap` 的行为（KDE Quick Tile 风格）：

| 当前状态 | 按 `snap left` | 再按 `snap up` | 再按 `snap up` |
| --- | --- | --- | --- |
| 居中浮动窗口 | 左半屏 | 左上四分屏 | 上半屏（全宽） |

焦点在平铺窗口时，`snap left|right|up|down` 等同于 `focus left|right|up|down`，
所以可以把方向键直接换成 `snap`，浮动/平铺两种状态都顺手。

`switcher` 的交互（mangowc 语义）：

| 操作 | 效果 |
| --- | --- |
| `Alt+Tab` / `Alt+Shift+Tab` | 打开面板（候选项是当前工作区的 MRU 顺序），并前进/后退一格 |
| 按住 `Alt` 连点 `Tab` | 循环选择；过程中按下/松开 `Shift` 只切换方向，不会取消或提交 |
| 松开 `Alt`/`Super`/`Ctrl` | 确认：聚焦高亮窗口 |
| `Enter` / `Esc` | 确认 / 取消 |
| 其他任意键 | 取消（`sticky` 模式下忽略） |

被 tabbed/stacked 布局隐藏的窗口同样会出现在候选里（没有实时预览时退回标题块），
因此 Alt+Tab 不会被 i3 的分页器"吃掉"窗口。切换器自身不会改动焦点以外的状态。

选择器**忽略按键自动重复**：按住 `Tab` 不会连续跳格，一次按下只前进一格（想多走就多按几次），
避免"按住一下就跑过头、松手落在原本要离开的窗口上"。若修饰键释放事件丢失（快速轻点或
别家客户端抢了键盘抓取），50 ms 内的轮询会自动确认，面板不会挂在屏幕上。

### layoutmsg 消息（Hyprland 风格）

```text
layoutmsg togglesplit
layoutmsg pseudo
layoutmsg focusmaster
layoutmsg swapwithmaster
layoutmsg orientationleft|orientationright|orientationtop|orientationbottom   # 仅 master
layoutmsg setproportion <0.1-1.0>     # 仅 scroller，绝对设置焦点列占比
layoutmsg proportion <+0.05 / -0.05>  # 仅 scroller，相对调整焦点列占比
layoutmsg cyclenext
```

绑定示例（推荐写在 `i3ext.binds`）：

```lua
i3ext {
    binds = {
        { key = "Mod4+m", command = "layout master" },
        { key = "Mod4+t", command = "layout dwindle" },
        { key = "Mod4+s", command = "layout scroller" },
        { key = "Mod4+g", command = "layoutmsg swapwithmaster" },
        { key = "Mod4+o", command = "layoutmsg focusmaster" },
        { key = "Mod4+y", command = "layoutmsg togglesplit" },
        { key = "Mod4+equal", command = "layoutmsg proportion +0.05" },
        { key = "Mod4+minus", command = "layoutmsg proportion -0.05" },
        { key = "Mod4+c", command = "centerwindow" },
        -- 浮动窗口贴边（平铺时自动退化为 focus 移动）
        { key = "Mod4+Left", command = "snap left" },
        { key = "Mod4+Right", command = "snap right" },
        { key = "Mod4+Up", command = "snap up" },
        { key = "Mod4+Down", command = "snap down" },
        -- 可选：直接贴四角
        { key = "Mod4+Ctrl+Left", command = "snap top-left" },
        { key = "Mod4+Ctrl+Right", command = "snap bottom-right" },
        -- Alt+Tab 缩略图切换器
        { key = "Mod1+Tab", command = "switcher next" },
        { key = "Mod1+Shift+Tab", command = "switcher prev" },
    },
}
```

原生 `bindsym` 仍可直接绑定这些命令（例如在旧配置或 include 文件中），
但本 fork 推荐统一放在 `i3ext.binds`。

## 五、默认值与热重载

### 开箱默认值（Hyprland 风格）

以下默认值在每次（重）加载配置时生效，显式指令始终覆盖：

| 项 | 默认值 | 回到上游观感 |
|----|--------|--------------|
| `rounding` | `12` | `rounding 0` |
| `gaps inner/outer` | `4 / 8` | `gaps inner 0; gaps outer 0` |
| 默认边框宽 | `1px`（平铺与浮动） | 上游为 `2px` |
| `title_align` | `center` | `title_align left` |
| 标题栏配色 | 深色面板（背景 `#11111b`/`#181825`，文字 `#cdd6f4`，accent `#f9e2af`） | 用 `client.*` 颜色指令覆盖 |
| `titlebar_font` | 回退全局 `font` | — |
| 动画 | 总开关开，windows 180ms / open 220ms / close 150ms / workspaces 300ms / dropdown 250ms，`easeOutQuint` | `animations off` |
| 下拉窗口 | `dropdown_height 0.5`、`dropdown_position top` | `dropdown_position bottom` |

### 配置热重载（Hyprland 风格）

- 保存配置文件即自动生效（inotify 监听主配置 + 全部 `include` 引入的文件），
  无需手动 `$mod+Shift+c` 或 IPC reload；inotify 不可用时自动降级为手动 reload。
- 200 ms 内的多次保存合并为一次 reload（防抖）。
- **先校验后应用**：reload 前先以 `-C` 离线校验；校验失败弹出 nagbar 报错
  并**保留旧配置继续运行**，修复后保存即自动恢复。
- 每次 reload 后监听列表按新的 `include` 集合重建。

## 六、兼容性说明

- 以上全部为**增量**语法，标准 i3 配置无需修改即可继续使用。
- `client.*_gradient` 的旧三参数语法已删除，迁移时需改为单字符串形式。
- `gaps_in` / `gaps_out` 从标准 `gaps` 状态中拆出为独立顶层指令，
  与 `gaps inner|outer` 可混用（后写者生效）。
