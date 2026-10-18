# afterglow brand

The mark: a sun setting into the sea, rings of light rising around it. *Afterglow* is the light left
after sunset, and this project is the Quest 1's life after Meta's support ended.

| File | What |
|---|---|
| `afterglow-mark.svg` | The flat logo |
| `preview.html` | The flat logo animated, and how it sits in the headset (`preview.html?t=3000` freezes at 3 s) |
| `boot3d/scene.py` | The 3D boot animation, built entirely in Blender Python: sea, sun, rings, caption, camera and timeline. Renders mono previews or stereo (63 mm, parallel cameras) |
| `fonts/` | Montserrat Light (OFL), with overlapping contours merged for Blender |

The boot animation is a 3 s intro followed by a 2.5 s loop; every looping property has that period,
so the last frame wraps seamlessly to frame 109. Rendered stereo at 900×900 per eye it packs to about
25 MB (`tools/pack_anim.py`).

```sh
blender -b --factory-startup -P brand/boot3d/scene.py -- --out out/boot3d/preview --res 600   # mono preview
blender -b --factory-startup -P brand/boot3d/scene.py -- --out out/boot3d/stereo --res 900 --samples 48 --stereo
```
