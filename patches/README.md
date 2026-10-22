# Framework patches

The product pins two local App_shell commits that are not yet on
`origin/main`. They are small, independently useful, and kept here so a fresh
clone of App_shell can be brought to the same state:

```sh
git clone https://github.com/minervarr/App_shell.git
cd App_shell
git apply /path/to/Teleprompter_Vk/patches/0001-*.patch
git apply /path/to/Teleprompter_Vk/patches/0002-*.patch
```

1. `Host::setFullscreen(bool)` and its Wayland implementation, plus
   `app_shell_restore_console()` for CLI modes whose stdout is a pipe.
2. `WindowOptions::output` selects a Wayland connector for fullscreen, so the
   teleprompter can use `DP-2` without disturbing a monitor being used for
   other work.

Vk_Canvas_Lb_LAW is used at its upstream `main` commit; no patch is needed.
