# Startup animation preference

Windows desktop users can toggle **启动主题动画** in **外观与背景**.
The preference is enabled by default and applies at the next app launch.
Disabling motion preserves a static startup status and normal timeout/error
handling; it does not delay readiness or disable the system reduced-motion rule.

Authenticated GET/PUT `/v1/desktop/startup-animation` reads/writes a strict
boolean `enabled`. Persistence is per Windows user, in
`%LOCALAPPDATA%/cognitive-os-agent/desktop-startup-animation`, so the desktop shell
can read it before the backend has started. Unsupported platforms report
`available:false` and reject writes. Browser wallpaper settings are independent.

`tests/test_startup_animation.py` uses an isolated LOCALAPPDATA directory and a
private Runtime to test defaults, disabling, invalid values, restart persistence
and re-enabling. It does not change the user's actual preference.

This setting is a post-v3.0.0 change. The v3.0.0 release remains pinned to the
user-selected commit `83f7edc556c030d3a568d66846a34addf7685459`.
