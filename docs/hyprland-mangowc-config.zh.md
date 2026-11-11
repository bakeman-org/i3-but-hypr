# i3 (hypri3 定制版) 配置规格 —— Hyprland / mangowc 移植功能

本文档描述本分支在标准 i3 配置语法之上移植的 Hyprland 与 mangowc 风格特性，
包括最终的配置指令（config directives）与运行时命令（commands）。

## 一、外观类配置指令

### 间距（Hyprland 风格别名）

```text
gaps_in <px>
gaps_out <px>
```

- 等价于标准写法 `gaps inner <n>px` / `gaps outer <n>px`。
- 默认值与 i3 上游一致（inner 2, outer 0）。

### 圆角

```text
rounding <0-100>          # 亦可用别名 border_radius
```

- 单位为逻辑像素，作用于窗口边框与标题栏的圆角。
- 默认 `0`（直角）。

### 边框宽度（Hyprland 风格别名）

```text
border_size <0-100>
```

- 等价于同时设置平铺与浮动窗口的边框宽度。
- 边框样式（有无标题栏）由 `titlebar` 控制，见下文。

### 标题栏

```text
titlebar_height <0-200>
titlebar_padding <0-100>
titlebar normal|none
```

- `titlebar_height`：标题栏高度，`0` 表示不保留标题栏空间；默认 `0`。
- `titlebar_padding`：标题栏内边距；默认 `2`。
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
  `easeOutExpo`、`easeInOutCubic`、`easeInOutQuint`、`easeInOutSine`。
  可用 `bezier` 自定义（最多 16 条，同名覆盖）。
- `animation` 类型（大小写不敏感）：

| 类型 | 别名 | 作用 |
|------|------|------|
| `windows` | `windowsmove` | 窗口移动/几何动画 |
| `windowsIn` | `windowopen` | 窗口打开动画 |
| `windowsOut` | `windowclose` | 窗口关闭动画 |
| `fadeIn` | `fade` / `fadeOut` | 焦点切换淡入淡出 |
| `workspaces` | — | 工作区切换动画 |
| `global` | — | 一次性设置以上全部 |

- 字段均可省略：开关默认 `on`，时长缺省为 `0`（沿用对应默认），曲线缺省沿用原值。
- 默认值：总开关 `animations on`，windows 180ms、open 220ms、close 150ms、
  workspaces 300ms，曲线 `easeOutQuint`。

示例：

```text
animations on
bezier myQuint, 0.23, 1.0, 0.32, 1.0
animation windows, 1, 180, myQuint
animation workspaces, 1, 300, easeOutQuint
```

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
```

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
```

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

绑定示例：

```text
bindsym $mod+m layout master
bindsym $mod+t layout dwindle
bindsym $mod+s layout scroller
bindsym $mod+g layoutmsg swapwithmaster
bindsym $mod+o layoutmsg focusmaster
bindsym $mod+y layoutmsg togglesplit
bindsym $mod+equal layoutmsg proportion +0.05
bindsym $mod+minus layoutmsg proportion -0.05
bindsym $mod+c centerwindow
```

## 五、兼容性说明

- 以上全部为**增量**语法，标准 i3 配置无需修改即可继续使用。
- `client.*_gradient` 的旧三参数语法已删除，迁移时需改为单字符串形式。
- `gaps_in` / `gaps_out` 从标准 `gaps` 状态中拆出为独立顶层指令，
  与 `gaps inner|outer` 可混用（后写者生效）。
