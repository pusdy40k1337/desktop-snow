# Assets

The app needs no image files: every visual is drawn procedurally with Direct2D.
PNG files placed here (they are copied next to the exe by `build.cmd`, into
`dist\assets\`) replace the procedural drawing for that item:

| File | Replaces | Placement |
|---|---|---|
| `tree.png` | tree body (trunk + foliage) | bottom centre of the image = base of the trunk; scaled to the tree height (≈230 px at 100%). Ornaments, lights and the star are still drawn on top, so leave the foliage roughly triangular with the apex at the top centre. |
| `snowman.png` | whole snowman | bottom centre = feet on the window edge; scaled to ≈92 px at 100%. |
| `santa.png` | Santa peeking in | facing left (he leans in from the right edge); mirrored for the left edge; scaled to ≈150 px tall. |
| `train.png` | the whole gift train | facing right (locomotive at the right); mirrored for leftward trips; bottom = rails (taskbar top); ≈90 px tall. |
| `yeti.png` | the yeti | facing right; bottom centre = feet; ≈110 px tall. |

Transparent PNG (32-bit RGBA) works best; any size, scaled with the aspect ratio kept.

The original project mentions an `Assets` folder of about 40 items. It was not in
the repository when this was written. Once it is committed, each file will be
mapped to a feature here instead of guessing names.
