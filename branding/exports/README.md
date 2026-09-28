# Inventatory animated mark — exports

Rendered frame-by-frame from the SVG sources in `../animations/` (30 fps, pixel-exact: 48 px cells at 512, 24 px at 256, 96 px at 1080).
Animations: `boot` (4.0 s), `fill` (3.4 s), `locate` (4.0 s), `lowstock` (1.6 s), `cursor` (1.1 s). All loop forever.

| Folder | Format | Background | Sizes | Use it for |
|---|---|---|---|---|
| `../animations/*.svg` | Animated SVG | transparent | any | Website, README (best quality, tiny) |
| `apng/` | APNG (`.png`) | transparent | 256, 512 | GitHub README, anywhere PNG works; shows first frame where APNG isn't supported |
| `webp/` | Animated WebP, lossless | transparent | 256, 512 | Website / README alternative, smallest raster |
| `gif/` | GIF, 25 fps | dark `#0D1010` | 256, 512 | Discord, forums, chat apps — universal fallback |
| `video/*.mp4` | H.264 | dark `#0D1010` | 1080×1080 | YouTube, social media, video editors |
| `video/*-alpha.webm` | VP9 with alpha | transparent | 1080×1080 | Overlays in OBS / browsers / editors that support WebM alpha |

GIFs have a solid background because GIF only supports on/off transparency — the 18 % "empty slot" state can't be shown on a transparent GIF.

## README snippet

```html
<p align="center">
  <img src="branding/animations/inventatory-boot.svg" width="160" alt="Inventatory">
</p>
```

Palette (from `src/app/settings/AppSettings.h`): Interactive `#58B9B0` → FocusText `#B9E7DD`, WarningText `#D8B56B`, PrimaryText `#F1EEE5`, CanvasBg `#0D1010`.
