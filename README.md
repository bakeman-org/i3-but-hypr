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

## upgrading without logging out

Install the new release as usual, then run this **inside your session**:

```bash
sudo apt install ./i3-but-hypr_*.deb
i3-but-hypr-upgrade
```

`i3-msg restart` alone would already re-exec the new binary, but the new
configuration may contain directives that only the new binary understands; if
the old process were to reload it first, it would only show an error popup.
`i3-but-hypr-upgrade` avoids this by doing the steps in a safe order:

1. validate `/etc/i3/config` with the newly installed binary (`i3 -C`);
2. `i3-msg restart` — the session (layout and all windows) is kept, the new
   binary starts with the old configuration;
3. install `/etc/i3/config` as `~/.config/i3/config` (the previous file is kept
   as `config.bak.<timestamp>`) and reload it — now the **new** binary does the
   validation, so new directives parse fine.

If the old configuration is not compatible with the new binary, the order is
reversed (config first, then restart), so the upgrade still succeeds.

Options:

```text
i3-but-hypr-upgrade [-b BINARY] [-c CONFIG] [--keep-user-config] [--no-restart]
```

- `-c CONFIG` — use a different config as the new one (e.g. a repo checkout:
  `./etc/config`); default `/etc/i3/config`;
- `--keep-user-config` — restart only, do not touch `~/.config/i3/config`;
- `--no-restart` — update the configuration without a running session;
- `-b BINARY` — binary to switch to (default `i3` from `PATH`).

Configuration precedence reminder: i3 reads `~/.config/i3/config` **before**
`/etc/i3/config`, so changing the packaged config alone has no effect while a
user config exists. `i3-but-hypr-upgrade` keeps the two in sync; if you prefer
to keep personal changes, put them in `~/.config/i3/config` after the copied
content, or use `--keep-user-config` and merge manually.

# ref links & idea inspired by
- hyprland's animation ...
- mangowc's scroller
- niri's alt tab
- kde's snap layout
- clarion's optimization
- lua5.4
- part of code complete by codebuddy ide
- part of code complete by trae ide
