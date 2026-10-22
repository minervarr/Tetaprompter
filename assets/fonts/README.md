# Fonts

- `ui/NewCMSans10-{Regular,Bold,Oblique,BoldOblique}.otf` — New Computer
  Modern Sans 10 (GUST Font License).
- `mono/NewCMMono10-{Regular,Bold}.otf` — New Computer Modern Mono 10 (GUST
  Font License).
- `openDix/OpenDyslexic-{Regular,Bold,Italic,BoldItalic}.otf` — OpenDyslexic
  by Abbie Gonzalez, SIL Open Font License 1.1. See `openDix/OFL.txt`.

The app has two presets, `newcm` and `opendyslexic`. OpenDyslexic is used at a
0.70 size factor because its glyphs are wider; New Computer Modern is
registered as a per-style fallback for the em dash, ellipsis and arrows it does
not cover. Stage directions stay in the mono face.

RasterFont bakes actual coverage at each used size; the Raster, Bold/Italic/Math
styles map to the four UI faces and the mono face. The original Python app used
the system Adwaita Sans/Cantarell/Pango stack; this port ships its faces so the
Vulkan renderer is independent of fontconfig.
