# vim:ts=2:sw=2:expandtab
#
# i3 - an improved tiling window manager
# © 2009 Michael Stapelberg and contributors (see also: LICENSE)
#
# parser-specs/config.spec: Specification file for generate-command-parser.pl
# which will generate the appropriate header files for our C parser.
#
# Use :source highlighting.vim in vim to get syntax highlighting
# for this file.

# TODO: should we implement an include statement for the criteria part so we DRY?

state INITIAL:
  # We have an end token here for all the commands which just call some
  # function without using an explicit 'end' token.
  end ->
  error ->
  '#'                                      -> IGNORE_LINE
  'set '                                   -> IGNORE_LINE
  'set	'                                  -> IGNORE_LINE
  'set_from_resource'                      -> IGNORE_LINE
  'include'                                -> INCLUDE
  '__i3ext_exec'                           -> I3EXT_EXEC
  bindtype = 'bindsym', 'bindcode', 'bind' -> BINDING
  'bar'                                    -> BARBRACE
  'font'                                   -> FONT
  'titlebar_font'                          -> TITLEBAR_FONT
  'mode_hint'                              -> MODE_HINT
  'mode'                                   -> MODENAME
  'gaps_in'                                -> GAPS_IN
  'gaps_out'                               -> GAPS_OUT
  'gaps'                                   -> GAPS
  'smart_borders'                          -> SMART_BORDERS
  'smart_gaps'                             -> SMART_GAPS
  'floating_minimum_size'                  -> FLOATING_MINIMUM_SIZE_WIDTH
  'floating_maximum_size'                  -> FLOATING_MAXIMUM_SIZE_WIDTH
  'floating_modifier'                      -> FLOATING_MODIFIER
  'default_orientation'                    -> DEFAULT_ORIENTATION
  'workspace_layout'                       -> WORKSPACE_LAYOUT
  windowtype = 'default_border', 'new_window', 'default_floating_border', 'new_float'
      -> DEFAULT_BORDER
  'hide_edge_borders'                      -> HIDE_EDGE_BORDERS
  'for_window'                             -> FOR_WINDOW
  'assign'                                 -> ASSIGN
  'no_focus'                               -> NO_FOCUS
  'focus_follows_mouse'                    -> FOCUS_FOLLOWS_MOUSE
  'mouse_warping'                          -> MOUSE_WARPING
  'focus_wrapping'                         -> FOCUS_WRAPPING
  'force_focus_wrapping'                   -> FORCE_FOCUS_WRAPPING
  'force_xinerama', 'force-xinerama'       -> FORCE_XINERAMA
  'disable_randr15', 'disable-randr15'     -> DISABLE_RANDR15
  'workspace_auto_back_and_forth'          -> WORKSPACE_BACK_AND_FORTH
  'fake_outputs', 'fake-outputs'           -> FAKE_OUTPUTS
  'force_display_urgency_hint'             -> FORCE_DISPLAY_URGENCY_HINT
  'focus_on_window_activation'             -> FOCUS_ON_WINDOW_ACTIVATION
  'title_align'                            -> TITLE_ALIGN
  'show_marks'                             -> SHOW_MARKS
  'hot_reload'                             -> HOT_RELOAD
  'workspace'                              -> WORKSPACE
  'animations'                             -> ANIMATIONS
  'animation'                              -> ANIMATION
  'dropdown_height'                        -> DROPDOWN_HEIGHT
  'dropdown_position'                      -> DROPDOWN_POSITION
  'bezier'                                 -> BEZIER
  'master_ratio'                           -> MASTER_RATIO
  'active_opacity'                         -> ACTIVE_OPACITY
  'inactive_opacity'                       -> INACTIVE_OPACITY
  'dim_inactive'                           -> DIM_INACTIVE
  'dim_strength'                           -> DIM_STRENGTH
  'rounding', 'border_radius'              -> ROUNDING
  'border_size'                            -> BORDER_SIZE
  'titlebar_height'                        -> TITLEBAR_HEIGHT
  'titlebar_padding'                       -> TITLEBAR_PADDING
  'titlebar'                               -> TITLEBAR_VISIBILITY
  'client.focused_gradient'                -> CLIENT_GRADIENT_FOCUSED
  'client.focused_inactive_gradient'       -> CLIENT_GRADIENT_FOCUSED_INACTIVE
  'client.unfocused_gradient'              -> CLIENT_GRADIENT_UNFOCUSED
  'client.urgent_gradient'                 -> CLIENT_GRADIENT_URGENT
  'dwindle_split_ratio'                    -> DWINDLE_SPLIT_RATIO
  'scroller_proportion'                    -> SCROLLER_PROPORTION
  'scroller_structs'                       -> SCROLLER_STRUCTS
  'scroller_focus_center'                  -> SCROLLER_FOCUS_CENTER
  'scroller_prefer_center'                 -> SCROLLER_PREFER_CENTER
  'scroller_prefer_overspread'             -> SCROLLER_PREFER_OVERSPREAD
  'scroller_ignore_proportion_single'      -> SCROLLER_IGNORE_PROPORTION_SINGLE
  'scroller_default_proportion_single'     -> SCROLLER_DEFAULT_PROPORTION_SINGLE
  'scroller_autofit'                       -> SCROLLER_AUTOFIT
  'switcher_background'                    -> SWITCHER_BACKGROUND
  'switcher_bar'                           -> SWITCHER_BAR
  'switcher_tile'                          -> SWITCHER_TILE
  'ipc_socket', 'ipc-socket'               -> IPC_SOCKET
  'ipc_kill_timeout'                       -> IPC_KILL_TIMEOUT
  'restart_state'                          -> RESTART_STATE
  'popup_during_fullscreen'                -> POPUP_DURING_FULLSCREEN
  'tiling_drag'                            -> TILING_DRAG
  exectype = 'exec_always', 'exec'         -> EXEC
  colorclass = 'client.background'
      -> COLOR_SINGLE
  colorclass = 'client.focused_inactive', 'client.focused_tab_title', 'client.focused', 'client.unfocused', 'client.urgent', 'client.placeholder'
      -> COLOR_BORDER

# We ignore comments and 'set' lines (variables).
state IGNORE_LINE:
  line
      -> INITIAL

# gaps inner|outer|horizontal|vertical|top|right|bottom|left <gap_size>[px]
state GAPS:
  scope = 'inner', 'outer', 'horizontal', 'vertical', 'top', 'right', 'bottom', 'left'
      -> GAPS_WITH_SCOPE

state GAPS_WITH_SCOPE:
  value = number
      -> GAPS_END

state GAPS_END:
  'px'
      ->
  end
      -> call cfg_gaps($workspace, $scope, &value)

# smart_borders true|false
# smart_borders no_gaps
state SMART_BORDERS:
  enabled = '1', 'yes', 'true', 'on', 'enable', 'active'
      -> call cfg_smart_borders($enabled)
  enabled = 'no_gaps'
      -> call cfg_smart_borders($enabled)

# smart_gaps on|off|inverse_outer
state SMART_GAPS:
  enabled = '1', 'yes', 'true', 'on', 'enable', 'active'
      -> call cfg_smart_gaps($enabled)
  enabled = '0', 'no', 'false', 'off', 'disable', 'inactive'
      -> call cfg_smart_gaps($enabled)
  enabled = 'inverse_outer'
      -> call cfg_smart_gaps($enabled)

# include <pattern>
state INCLUDE:
  pattern = string
      -> call cfg_include($pattern)

# floating_minimum_size <width> x <height>
state FLOATING_MINIMUM_SIZE_WIDTH:
  width = number
      -> FLOATING_MINIMUM_SIZE_X

state FLOATING_MINIMUM_SIZE_X:
  'x'
      -> FLOATING_MINIMUM_SIZE_HEIGHT

state FLOATING_MINIMUM_SIZE_HEIGHT:
  height = number
      -> call cfg_floating_minimum_size(&width, &height)

# floating_maximum_size <width> x <height>
state FLOATING_MAXIMUM_SIZE_WIDTH:
  width = number
      -> FLOATING_MAXIMUM_SIZE_X

state FLOATING_MAXIMUM_SIZE_X:
  'x'
      -> FLOATING_MAXIMUM_SIZE_HEIGHT

state FLOATING_MAXIMUM_SIZE_HEIGHT:
  height = number
      -> call cfg_floating_maximum_size(&width, &height)

# floating_modifier <modifier>
state FLOATING_MODIFIER:
  modifiers = 'Mod1', 'Mod2', 'Mod3', 'Mod4', 'Mod5', 'Shift', 'Control', 'Ctrl'
      ->
  '+'
      ->
  end
      -> call cfg_floating_modifier($modifiers)

# tiling_drag swap_modifier <modifier>
state TILING_DRAG_SWAP_MODIFIER:
  modifiers = 'Mod1', 'Mod2', 'Mod3', 'Mod4', 'Mod5', 'Shift', 'Control', 'Ctrl'
      ->
  '+'
      ->
  end
      -> call cfg_tiling_drag_swap_modifier($modifiers)

# default_orientation <horizontal|vertical|auto>
state DEFAULT_ORIENTATION:
  orientation = 'horizontal', 'vertical', 'auto'
      -> call cfg_default_orientation($orientation)

# workspace_layout <default|stacking|tabbed|dwindle|master>
state WORKSPACE_LAYOUT:
  layout = 'default', 'stacking', 'stacked', 'tabbed', 'dwindle', 'master'
      -> call cfg_workspace_layout($layout)

# <default_border|new_window> <normal|1pixel|none>
# <default_floating_border|new_float> <normal|1pixel|none>
state DEFAULT_BORDER:
  border = 'normal', 'pixel'
      -> DEFAULT_BORDER_PIXELS
  border = '1pixel', 'none'
      -> call cfg_default_border($windowtype, $border, -1)

state DEFAULT_BORDER_PIXELS:
  end
      -> call cfg_default_border($windowtype, $border, 2)
  width = number
      -> DEFAULT_BORDER_PIXELS_PX

state DEFAULT_BORDER_PIXELS_PX:
  'px'
      ->
  end
      -> call cfg_default_border($windowtype, $border, &width)

# hide_edge_borders <none|vertical|horizontal|both|smart|smart_no_gaps>
# also hide_edge_borders <bool> for compatibility
state HIDE_EDGE_BORDERS:
  hide_borders = 'none', 'vertical', 'horizontal', 'both', 'smart_no_gaps', 'smart'
      -> call cfg_hide_edge_borders($hide_borders)
  hide_borders = '1', 'yes', 'true', 'on', 'enable', 'active'
      -> call cfg_hide_edge_borders($hide_borders)

# for_window <criteria> command
state FOR_WINDOW:
  '['
      -> call cfg_criteria_init(FOR_WINDOW_COMMAND); CRITERIA

state FOR_WINDOW_COMMAND:
  command = string
      -> call cfg_for_window($command)

# assign <criteria> [→] [workspace | output] <name>
state ASSIGN:
  '['
      -> call cfg_criteria_init(ASSIGN_WORKSPACE); CRITERIA

state ASSIGN_WORKSPACE:
  '→'
      ->
  'output'
      -> ASSIGN_OUTPUT
  'workspace'
      ->
  'number'
      -> ASSIGN_WORKSPACE_NUMBER
  workspace = string
      -> call cfg_assign($workspace, 0)

state ASSIGN_OUTPUT:
  output = string
      -> call cfg_assign_output($output)

state ASSIGN_WORKSPACE_NUMBER:
  number = string
      -> call cfg_assign($number, 1)

# no_focus <criteria>
state NO_FOCUS:
  '['
      -> call cfg_criteria_init(NO_FOCUS_END); CRITERIA

state NO_FOCUS_END:
  end
      -> call cfg_no_focus()

# Criteria: Used by for_window and assign.
state CRITERIA:
  ctype = 'class'         -> CRITERION
  ctype = 'instance'      -> CRITERION
  ctype = 'window_role'   -> CRITERION
  ctype = 'con_id'        -> CRITERION
  ctype = 'id'            -> CRITERION
  ctype = 'window_type'   -> CRITERION
  ctype = 'con_mark'      -> CRITERION
  ctype = 'title'         -> CRITERION
  ctype = 'urgent'        -> CRITERION
  ctype = 'workspace'     -> CRITERION
  ctype = 'machine'     -> CRITERION
  ctype = 'floating_from' -> CRITERION_FROM
  ctype = 'tiling_from'   -> CRITERION_FROM
  ctype = 'tiling', 'floating', 'all'
      -> call cfg_criteria_add($ctype, NULL); CRITERIA
  ']'
      -> call cfg_criteria_pop_state()

state CRITERION:
  '=' -> CRITERION_STR

state CRITERION_FROM:
  '=' -> CRITERION_FROM_STR_START

state CRITERION_FROM_STR_START:
  '"' -> CRITERION_FROM_STR
  kind = 'auto', 'user'
    -> call cfg_criteria_add($ctype, $kind); CRITERIA

state CRITERION_FROM_STR:
  kind = 'auto', 'user'
    -> CRITERION_FROM_STR_END

state CRITERION_FROM_STR_END:
  '"'
    -> call cfg_criteria_add($ctype, $kind); CRITERIA

state CRITERION_STR:
  cvalue = word
      -> call cfg_criteria_add($ctype, $cvalue); CRITERIA

# focus_follows_mouse bool
state FOCUS_FOLLOWS_MOUSE:
  value = word
      -> call cfg_focus_follows_mouse($value)

# mouse_warping warping_t
state MOUSE_WARPING:
  value = 'none', 'output'
      -> call cfg_mouse_warping($value)

# focus_wrapping
state FOCUS_WRAPPING:
  value = '1', 'yes', 'true', 'on', 'enable', 'active', '0', 'no', 'false', 'off', 'disable', 'inactive', 'force', 'workspace'
      -> call cfg_focus_wrapping($value)

# force_focus_wrapping
state FORCE_FOCUS_WRAPPING:
  value = word
      -> call cfg_force_focus_wrapping($value)

# force_xinerama
state FORCE_XINERAMA:
  value = word
      -> call cfg_force_xinerama($value)

# disable_randr15
state DISABLE_RANDR15:
  value = word
      -> call cfg_disable_randr15($value)

# workspace_back_and_forth
state WORKSPACE_BACK_AND_FORTH:
  value = word
      -> call cfg_workspace_back_and_forth($value)


# fake_outputs (for testcases)
state FAKE_OUTPUTS:
  outputs = string
      -> call cfg_fake_outputs($outputs)

# force_display_urgency_hint <timeout> ms
state FORCE_DISPLAY_URGENCY_HINT:
  duration_ms = number
      -> FORCE_DISPLAY_URGENCY_HINT_MS

# title_align [left|center|right]
state TITLE_ALIGN:
  alignment = 'left', 'center', 'right'
      -> call cfg_title_align($alignment)

# show_marks
state SHOW_MARKS:
  value = word
      -> call cfg_show_marks($value)

# hot_reload <on|off>
state HOT_RELOAD:
  value = word
      -> call cfg_hot_reload($value)

# mode_hint <on|off>
state MODE_HINT:
  value = word
      -> call cfg_mode_hint($value)

state FORCE_DISPLAY_URGENCY_HINT_MS:
  'ms'
      ->
  end
      -> call cfg_force_display_urgency_hint(&duration_ms)

# focus_on_window_activation <smart|urgent|focus|none>
state FOCUS_ON_WINDOW_ACTIVATION:
  mode = word
      -> call cfg_focus_on_window_activation($mode)

# workspace <workspace> output <output>
# workspace <workspace> gaps inner|outer <px>
state WORKSPACE:
  workspace = word
    -> WORKSPACE_COMMAND

state WORKSPACE_COMMAND:
  'output'
      -> WORKSPACE_OUTPUT_WORD
  'gaps'
      -> GAPS

state WORKSPACE_OUTPUT_WORD:
  output = word
      -> call cfg_workspace($workspace, $output); WORKSPACE_OUTPUT_WORD
  end
      -> INITIAL

# ipc-socket <path>
state IPC_SOCKET:
  path = string
      -> call cfg_ipc_socket($path)

# ipc_kill_timeout
state IPC_KILL_TIMEOUT:
  timeout = number
      -> call cfg_ipc_kill_timeout(&timeout)

# restart_state <path> (for testcases)
state RESTART_STATE:
  path = string
      -> call cfg_restart_state($path)

# popup_during_fullscreen
state POPUP_DURING_FULLSCREEN:
  value = 'ignore', 'leave_fullscreen', 'all', 'smart'
      -> call cfg_popup_during_fullscreen($value)

state TILING_DRAG_MODE:
  value = 'modifier', 'titlebar'
      ->
  end
      -> call cfg_tiling_drag($value)

state TILING_DRAG:
  off = '0', 'no', 'false', 'off', 'disable', 'inactive'
      -> call cfg_tiling_drag($off)
  swap_modifier = 'swap_modifier'
      -> TILING_DRAG_SWAP_MODIFIER
  value = 'modifier', 'titlebar'
      -> TILING_DRAG_MODE

# client.background <hexcolor>
state COLOR_SINGLE:
  color = word
      -> call cfg_color_single($colorclass, $color)

# colorclass border background text indicator
state COLOR_BORDER:
  border = word
      -> COLOR_BACKGROUND

state COLOR_BACKGROUND:
  background = word
      -> COLOR_TEXT

state COLOR_TEXT:
  text = word
      -> COLOR_INDICATOR

state COLOR_INDICATOR:
  indicator = word
      -> COLOR_CHILD_BORDER
  end
      -> call cfg_color($colorclass, $border, $background, $text, NULL, NULL)

state COLOR_CHILD_BORDER:
  child_border = word
      -> call cfg_color($colorclass, $border, $background, $text, $indicator, $child_border)
  end
      -> call cfg_color($colorclass, $border, $background, $text, $indicator, NULL)

# [fork] switcher_background <hexcolor>: panel background of the Alt+Tab switcher
state SWITCHER_BACKGROUND:
  color = word
      -> call cfg_switcher_background($color)

# [fork] switcher_bar <background> <text>: shared title bar of the switcher
state SWITCHER_BAR:
  background = word
      -> SWITCHER_BAR_TEXT

state SWITCHER_BAR_TEXT:
  text = word
      -> call cfg_switcher_bar($background, $text)

# [fork] switcher_tile <background> <text>: fallback tiles without a preview
state SWITCHER_TILE:
  background = word
      -> SWITCHER_TILE_TEXT

state SWITCHER_TILE_TEXT:
  text = word
      -> call cfg_switcher_tile($background, $text)

# <exec|exec_always> [--no-startup-id] command
state EXEC:
  no_startup_id = '--no-startup-id'
      ->
  command = string
      -> call cfg_exec($exectype, $no_startup_id, $command)

# font font
state FONT:
  font = string
      -> call cfg_font($font)

# titlebar_font font
state TITLEBAR_FONT:
  font = string
      -> call cfg_titlebar_font($font)

# bindsym/bindcode
state BINDING:
  release = '--release'
      ->
  border = '--border'
      ->
  whole_window = '--whole-window'
      ->
  exclude_titlebar = '--exclude-titlebar'
      ->
  modifiers = 'Mod1', 'Mod2', 'Mod3', 'Mod4', 'Mod5', 'Shift', 'Control', 'Ctrl', 'Mode_switch', 'Group1', 'Group2', 'Group3', 'Group4', '$mod'
      ->
  '+'
      ->
  key = word
      -> BINDCOMMAND

state BINDCOMMAND:
  release = '--release'
      ->
  border = '--border'
      ->
  whole_window = '--whole-window'
      ->
  exclude_titlebar = '--exclude-titlebar'
      ->
  command = string
      -> call cfg_binding($bindtype, $modifiers, $key, $release, $border, $whole_window, $exclude_titlebar, $command)

################################################################################
# Mode configuration
################################################################################

state MODENAME:
  pango_markup = '--pango_markup'
      ->
  modename = word
      -> call cfg_enter_mode($pango_markup, $modename); MODEBRACE

state MODEBRACE:
  end
      ->
  '{'
      -> MODE

state MODE:
  end ->
  error ->
  '#' -> MODE_IGNORE_LINE
  'set' -> MODE_IGNORE_LINE
  bindtype = 'bindsym', 'bindcode', 'bind'
      -> MODE_BINDING
  '}'
      -> INITIAL

# We ignore comments and 'set' lines (variables).
state MODE_IGNORE_LINE:
  line
      -> MODE

state MODE_BINDING:
  release = '--release'
      ->
  border = '--border'
      ->
  whole_window = '--whole-window'
      ->
  exclude_titlebar = '--exclude-titlebar'
      ->
  modifiers = 'Mod1', 'Mod2', 'Mod3', 'Mod4', 'Mod5', 'Shift', 'Control', 'Ctrl', 'Mode_switch', 'Group1', 'Group2', 'Group3', 'Group4', '$mod'
      ->
  '+'
      ->
  key = word
      -> MODE_BINDCOMMAND

state MODE_BINDCOMMAND:
  release = '--release'
      ->
  border = '--border'
      ->
  whole_window = '--whole-window'
      ->
  exclude_titlebar = '--exclude-titlebar'
      ->
  command = string
      -> call cfg_mode_binding($bindtype, $modifiers, $key, $release, $border, $whole_window, $exclude_titlebar, $command); MODE

################################################################################
# Bar configuration (i3bar)
################################################################################

state BARBRACE:
  end
      ->
  '{'
      -> call cfg_bar_start(); BAR

state BAR:
  end ->
  error ->
  '#' -> BAR_IGNORE_LINE
  'set' -> BAR_IGNORE_LINE
  'i3bar_command'          -> BAR_BAR_COMMAND
  'status_command'         -> BAR_STATUS_COMMAND
  'workspace_command'      -> BAR_WORKSPACE_COMMAND
  'socket_path'            -> BAR_SOCKET_PATH
  'mode'                   -> BAR_MODE
  'hidden_state'           -> BAR_HIDDEN_STATE
  'id'                     -> BAR_ID
  'modifier'               -> BAR_MODIFIER
  'wheel_up_cmd'           -> BAR_WHEEL_UP_CMD
  'wheel_down_cmd'         -> BAR_WHEEL_DOWN_CMD
  'bindsym'                -> BAR_BINDSYM
  'position'               -> BAR_POSITION
  'output'                 -> BAR_OUTPUT
  'tray_output'            -> BAR_TRAY_OUTPUT
  'tray_padding'           -> BAR_TRAY_PADDING
  'font'                   -> BAR_FONT
  'separator_symbol'       -> BAR_SEPARATOR_SYMBOL
  'binding_mode_indicator' -> BAR_BINDING_MODE_INDICATOR
  'workspace_buttons'      -> BAR_WORKSPACE_BUTTONS
  'workspace_min_width'    -> BAR_WORKSPACE_MIN_WIDTH
  'strip_workspace_numbers' -> BAR_STRIP_WORKSPACE_NUMBERS
  'strip_workspace_name' -> BAR_STRIP_WORKSPACE_NAME
  'verbose'                -> BAR_VERBOSE
  'height'                 -> BAR_HEIGHT
  'padding'                -> BAR_PADDING
  'colors'                 -> BAR_COLORS_BRACE
  '}'
      -> call cfg_bar_finish(); INITIAL

# We ignore comments and 'set' lines (variables).
state BAR_IGNORE_LINE:
  line
      -> BAR

state BAR_BAR_COMMAND:
  command = string
      -> call cfg_bar_i3bar_command($command); BAR

state BAR_STATUS_COMMAND:
  command = string
      -> call cfg_bar_status_command($command); BAR

state BAR_WORKSPACE_COMMAND:
  command = string
      -> call cfg_bar_workspace_command($command); BAR

state BAR_SOCKET_PATH:
  path = string
      -> call cfg_bar_socket_path($path); BAR

state BAR_MODE:
  mode = 'dock', 'hide', 'invisible'
      -> call cfg_bar_mode($mode); BAR

state BAR_HIDDEN_STATE:
  hidden_state = 'hide', 'show'
      -> call cfg_bar_hidden_state($hidden_state); BAR

state BAR_ID:
  bar_id = word
      -> call cfg_bar_id($bar_id); BAR

state BAR_MODIFIER:
  'off', 'none'
      -> call cfg_bar_modifier(NULL); BAR
  modifiers = 'Mod1', 'Mod2', 'Mod3', 'Mod4', 'Mod5', 'Shift', 'Control', 'Ctrl'
      ->
  '+'
      ->
  end
      -> call cfg_bar_modifier($modifiers); BAR

state BAR_WHEEL_UP_CMD:
  command = string
      -> call cfg_bar_wheel_up_cmd($command); BAR

state BAR_WHEEL_DOWN_CMD:
  command = string
      -> call cfg_bar_wheel_down_cmd($command); BAR

state BAR_BINDSYM:
  release = '--release'
      ->
  button = word
      -> BAR_BINDSYM_COMMAND

state BAR_BINDSYM_COMMAND:
  release = '--release'
      ->
  command = string
      -> call cfg_bar_bindsym($button, $release, $command); BAR

state BAR_POSITION:
  position = 'top', 'bottom'
      -> call cfg_bar_position($position); BAR

state BAR_OUTPUT:
  output = word
      -> call cfg_bar_output($output); BAR

state BAR_TRAY_OUTPUT:
  output = word
      -> call cfg_bar_tray_output($output); BAR

state BAR_TRAY_PADDING:
  padding_px = number
      -> BAR_TRAY_PADDING_PX

state BAR_TRAY_PADDING_PX:
  'px'
      ->
  end
      -> call cfg_bar_tray_padding(&padding_px); BAR

state BAR_FONT:
  font = string
      -> call cfg_bar_font($font); BAR

state BAR_SEPARATOR_SYMBOL:
  separator = string
      -> call cfg_bar_separator_symbol($separator); BAR

state BAR_BINDING_MODE_INDICATOR:
  value = word
      -> call cfg_bar_binding_mode_indicator($value); BAR

state BAR_WORKSPACE_BUTTONS:
  value = word
      -> call cfg_bar_workspace_buttons($value); BAR

state BAR_WORKSPACE_MIN_WIDTH:
  width = number
      -> BAR_WORKSPACE_MIN_WIDTH_PX

state BAR_WORKSPACE_MIN_WIDTH_PX:
  'px'
      ->
  end
      -> call cfg_bar_workspace_min_width(&width); BAR

state BAR_STRIP_WORKSPACE_NUMBERS:
  value = word
      -> call cfg_bar_strip_workspace_numbers($value); BAR

state BAR_STRIP_WORKSPACE_NAME:
  value = word
      -> call cfg_bar_strip_workspace_name($value); BAR

state BAR_VERBOSE:
  value = word
      -> call cfg_bar_verbose($value); BAR

state BAR_HEIGHT:
  value = number
      -> call cfg_bar_height(&value); BAR

state BAR_PADDING:
  top_or_all = number
      -> BAR_PADDING_TOP

state BAR_PADDING_TOP:
  'px'
      ->
  right_or_right_and_left = number
      -> BAR_PADDING_RIGHT
  end
      -> call cfg_bar_padding_one(&top_or_all); BAR

state BAR_PADDING_RIGHT:
  'px'
      ->
  bottom = number
      -> BAR_PADDING_BOTTOM
  end
      -> call cfg_bar_padding_two(&top_or_all, &right_or_right_and_left); BAR

state BAR_PADDING_BOTTOM:
  'px'
      ->
  left = number
      -> BAR_PADDING_LEFT
  end
      -> call cfg_bar_padding_three(&top_or_all, &right_or_right_and_left, &bottom); BAR

state BAR_PADDING_LEFT:
  'px'
      ->
  end
      -> call cfg_bar_padding_four(&top_or_all, &right_or_right_and_left, &bottom, &left); BAR

state BAR_COLORS_BRACE:
  end
      ->
  '{'
      -> BAR_COLORS

state BAR_COLORS:
  end ->
  '#' -> BAR_COLORS_IGNORE_LINE
  'set' -> BAR_COLORS_IGNORE_LINE
  colorclass = 'background', 'statusline', 'separator', 'focused_background', 'focused_statusline', 'focused_separator'
      -> BAR_COLORS_SINGLE
  colorclass = 'focused_workspace', 'active_workspace', 'inactive_workspace', 'urgent_workspace', 'binding_mode'
      -> BAR_COLORS_BORDER
  '}'
      -> BAR

# We ignore comments and 'set' lines (variables).
state BAR_COLORS_IGNORE_LINE:
  line
      -> BAR_COLORS

state BAR_COLORS_SINGLE:
  color = word
      -> call cfg_bar_color_single($colorclass, $color); BAR_COLORS

state BAR_COLORS_BORDER:
  border = word
      -> BAR_COLORS_BACKGROUND

state BAR_COLORS_BACKGROUND:
  background = word
      -> BAR_COLORS_TEXT

state BAR_COLORS_TEXT:
  end
      -> call cfg_bar_color($colorclass, $border, $background, NULL); BAR_COLORS
  text = word
      -> call cfg_bar_color($colorclass, $border, $background, $text); BAR_COLORS

# animations <yes|no>
state ANIMATIONS:
  value = '1', 'yes', 'true', 'on', 'enable', 'active'
      -> call cfg_animations($value)
  value = '0', 'no', 'false', 'off', 'disable', 'inactive'
      -> call cfg_animations($value)

# animation <windows|workspaces|dropdown>, <enabled>, <duration in ms>, <curve>
state ANIMATION:
  spec = string
      -> call cfg_animation($spec)

# dropdown_height <0.1 - 1.0>
state DROPDOWN_HEIGHT:
  value = string
      -> call cfg_dropdown_height($value)

# dropdown_position top|bottom
state DROPDOWN_POSITION:
  value = 'top', 'bottom'
      -> call cfg_dropdown_position($value)

# bezier <name>, <x1>, <y1>, <x2>, <y2>
state BEZIER:
  spec = string
      -> call cfg_bezier($spec)

# master_ratio <0.1 - 0.9>
state MASTER_RATIO:
  value = string
      -> call cfg_master_ratio($value)

# active_opacity <0.0 - 1.0>
state ACTIVE_OPACITY:
  value = string
      -> call cfg_active_opacity($value)

# inactive_opacity <0.0 - 1.0>
state INACTIVE_OPACITY:
  value = string
      -> call cfg_inactive_opacity($value)

# dim_inactive <yes|no>
state DIM_INACTIVE:
  value = '1', 'yes', 'true', 'on', 'enable', 'active'
      -> call cfg_dim_inactive($value)
  value = '0', 'no', 'false', 'off', 'disable', 'inactive'
      -> call cfg_dim_inactive($value)

# dim_strength <0.0 - 1.0>
state DIM_STRENGTH:
  value = string
      -> call cfg_dim_strength($value)

# rounding <pixels>
state ROUNDING:
  value = string
      -> call cfg_rounding($value)

# border_size <pixels>
state BORDER_SIZE:
  value = string
      -> call cfg_border_size($value)

# gaps_in <pixels>
state GAPS_IN:
  value = string
      -> call cfg_gaps_in($value)

# gaps_out <pixels>
state GAPS_OUT:
  value = string
      -> call cfg_gaps_out($value)

# titlebar_height <pixels>
state TITLEBAR_HEIGHT:
  value = string
      -> call cfg_titlebar_height($value)

# titlebar_padding <pixels>
state TITLEBAR_PADDING:
  value = string
      -> call cfg_titlebar_padding($value)

# titlebar normal|none
state TITLEBAR_VISIBILITY:
  value = string
      -> call cfg_titlebar_visibility($value)

# dwindle_split_ratio <0.1 - 0.9>
state DWINDLE_SPLIT_RATIO:
  value = string
      -> call cfg_dwindle_split_ratio($value)

# scroller_proportion <0.1 - 1.0>
state SCROLLER_PROPORTION:
  value = string
      -> call cfg_scroller_proportion($value)

# scroller_structs <pixels>
state SCROLLER_STRUCTS:
  value = string
      -> call cfg_scroller_structs($value)

# scroller_focus_center yes|no
state SCROLLER_FOCUS_CENTER:
  value = string
      -> call cfg_scroller_focus_center($value)

# scroller_prefer_center yes|no
state SCROLLER_PREFER_CENTER:
  value = string
      -> call cfg_scroller_prefer_center($value)

# scroller_prefer_overspread yes|no
state SCROLLER_PREFER_OVERSPREAD:
  value = string
      -> call cfg_scroller_prefer_overspread($value)

# scroller_ignore_proportion_single yes|no
state SCROLLER_IGNORE_PROPORTION_SINGLE:
  value = string
      -> call cfg_scroller_proportion_single($value)

# scroller_default_proportion_single <0.1 - 1.0>
state SCROLLER_DEFAULT_PROPORTION_SINGLE:
  value = string
      -> call cfg_scroller_default_proportion_single($value)

# scroller_autofit yes|no
state SCROLLER_AUTOFIT:
  value = string
      -> call cfg_scroller_autofit($value)

# client.focused_gradient "#color #color [angle]"
state CLIENT_GRADIENT_FOCUSED:
  spec = string
      -> call cfg_client_gradient_focused($spec)

# client.focused_inactive_gradient "#color #color [angle]"
state CLIENT_GRADIENT_FOCUSED_INACTIVE:
  spec = string
      -> call cfg_client_gradient_focused_inactive($spec)

# client.unfocused_gradient "#color #color [angle]"
state CLIENT_GRADIENT_UNFOCUSED:
  spec = string
      -> call cfg_client_gradient_unfocused($spec)

# client.urgent_gradient "#color #color [angle]"
state CLIENT_GRADIENT_URGENT:
  spec = string
      -> call cfg_client_gradient_urgent($spec)

# Internal marker substituted for a top-level i3ext { ... } block.
state I3EXT_EXEC:
  index = number
      -> call cfg_i3ext_exec(&index)
