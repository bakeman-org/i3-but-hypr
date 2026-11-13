# NOTICE(AI SLOP)
- LLM driven code, if you do mind llm, please don't use or it will break your system and family.

# get .deb file from github release
- https://github.com/bakeman-org/i3-but-hypr

## develop notice
- i3-config-wizard and the man pages are removed, they are not maintained
- AnyEvent-I3 and the old Perl testcases are removed; only the C parser / i3ext
  tests remain (`meson test -C build`)
- the `debian/` folder is gone, releases are built and published by GitHub Actions
- `docs/` only holds this fork's own documentation; upstream docs are not updated
- `etc/config` is the reference config: copy it to `~/.config/i3/config` to try
  changes (`make __sample_config_push`), or copy your local config back with
  `make __sync_local_config`

# ref links & idea inspired by
- hyprland's animation ...
- mangowc's scroller
- niri's alt tab
- kde's snap layout
- clarion's optimization
- lua5.4
- part of code complete by codebuddy ide
- part of code complete by trae ide
