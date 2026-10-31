# Effect textures

Five 512 x 512 in-house tiles used by the `texture` stage of the grading
engine. They are procedural and fully deterministic: no third-party texture
pack, no scanned material, nothing to license. The project licence (MIT) covers
them.

| Asset | Character | Used by |
| --- | --- | --- |
| `paper-fine.png` | Fine, near-white paper grain | `print.sepia` |
| `paper-news.png` | Coarser newsprint with horizontal fibres and a faint screen | reserved for the newsprint recipes |
| `paper-soft.png` | Smooth, low-frequency paper cloud | reserved for the pastel/chalk recipes |
| `paper-wash.png` | Mottled wash with visible fibre | reserved for the ink-wash recipes |
| `dust-fine.png` | Near-white base with sparse dust and fine scratches | reserved for the dust recipes |

The stage samples the midpoint of each tile, so every image is a greyscale
texture stored as RGB (the three channels are equal). Values sit near white
because the default blend is `multiply`; `dust-fine` carries the darkest
specks at roughly 55 % grey.

## Regenerating

```sh
python3 resources/textures/generate_textures.py
```

The script uses a fixed seed and periodic (FFT) blur, so reruns are
byte-identical and every tile wraps seamlessly. The embedded copies are
compiled from `resources/resources.qrc`; a missing asset is a hard error in
`effectTexture`, not a silent identity.

Generated files (SHA-256 of the checked-in PNGs):

```
03359ee971c525d03c421c5e5fd8e8ba094b70c36faeefdb7b34739689cb7adc  dust-fine.png
bb0727fab03b4784d900b0fc29f344fa0df6dbd10375cf17113f1f1f9db9594c  paper-fine.png
f112267b3cd2a62c9601519e4402e355142fe143a5250d8dda3e04c9144b970f  paper-news.png
a21553f048cfbf68b75781f7e6a7d48be820618ee659e725717e59dcf653f72d  paper-soft.png
3221e3f6b93a906f3fcc641a2c811a817740b8fc789bf208e53e65676a7c2b6e  paper-wash.png
```
