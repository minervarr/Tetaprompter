# Framework patches

Two framework commits are not yet on `origin/main`; the superproject pins the
commits used by this build, and these patches bring a fresh clone to the same
state.

## App_shell

```sh
git clone https://github.com/minervarr/App_shell.git
cd App_shell
for p in /path/to/Teleprompter_Vk/patches/000[123]-*.patch; do
    git apply "$p"
done
```

1. `Host::setFullscreen(bool)` and its Wayland implementation, plus
   `app_shell_restore_console()` for CLI modes whose stdout is a pipe.
2. `WindowOptions::output` selects a Wayland connector for fullscreen, so the
   teleprompter uses `DP-2` without disturbing a monitor being used for other
   work.
3. `WaylandDisplay::dispatch()` polls the host's timerfd too, so the app's
   16 ms scroll timer is not paced by the display's 250 ms idle timeout
   (which had made a 60 Hz scroll run at ~4 Hz).

## Vk_Canvas_Lb_LAW

```sh
git clone https://github.com/minervarr/Vk_Canvas_Lb_LAW.git
cd Vk_Canvas_Lb_LAW
git apply /path/to/Teleprompter_Vk/patches/vk-canvas-0001-*.patch
```

4. `WaylandDisplay::dispatch(int timeout_ms, int extra_fd = -1)` adds one
   optional fd to the display poll. The default remains the old behaviour.

The nested `vulkan_font_engine`, FreeType and msdfgen are used at the
`main` commits recorded by Vk_Canvas.
