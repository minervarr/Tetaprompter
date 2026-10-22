# Fonts

These faces are copied from the `minervarr/fonts` submodule so the app can be
built and run without that large repository checked out:

- `ui/NewCMSans10-{Regular,Bold,Oblique,BoldOblique}.otf` — New Computer
  Modern Sans 10 (GUST Font License).
- `mono/NewCMMono10-{Regular,Bold}.otf` — New Computer Modern Mono 10 (GUST
  Font License).

RasterFont bakes actual coverage at each used size; the Raster and Bold/Italic/
Math styles map to the four UI faces and the mono face. The original Python app
used the system Adwaita Sans/Cantarell/Pango stack; this port ships its faces so
the Vulkan renderer is independent of fontconfig.
