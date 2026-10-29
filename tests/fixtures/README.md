# Test fixtures

The real family photo used for acceptance evidence is
`/home/lavx/Downloads/IMG20260830150231.jpg` (3072 × 4096, Display P3). It is a
personal file and is **not** copied into this repository. Tests read it in place
and only ever open it read-only; the hash is checked before and after use.

Synthetic fixtures are generated at test time into a temporary directory:

- `p3-fields.png` — four flat colour fields in Display P3, used to compare the
  LittleCMS conversion against Qt's independent colour conversion and to prove
  the exported PDF carries converted sRGB pixels.
- `plain.png` — no embedded profile, used to prove the documented sRGB fallback.
- `base.jpg` / `broken-profile.jpg` — a JPEG with a syntactically present but
  unparsable ICC APP2 segment, used to prove that a damaged profile never
  produces guessed colours.

Generated fixtures are never committed. Exported PDF/PNG evidence goes to
`build/evidence/` and stays out of Git as well.
