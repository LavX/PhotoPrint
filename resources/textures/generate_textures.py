#!/usr/bin/env python3
"""Deterministic generator for the five 512 x 512 paper and dust tiles.

The assets are authored in-house: no third-party texture pack is involved, so
there is nothing to license and nothing to attribute. Running this script twice
on the same machine writes byte-identical PNGs. The engine samples the tiles as
greyscale, so the red, green and blue channels are equal.

    python3 resources/textures/generate_textures.py
"""

from pathlib import Path

import numpy as np
from PIL import Image

SIZE = 512
OUT = Path(__file__).resolve().parent


def periodic_blur(values: np.ndarray, sigma: float) -> np.ndarray:
    """Gaussian blur on a wrapping grid, so every tile is seamless."""
    freqs = np.fft.fftfreq(values.shape[0])
    fy = np.exp(-2.0 * (np.pi * sigma * freqs) ** 2)
    fx = np.exp(-2.0 * (np.pi * sigma * freqs) ** 2)
    kernel = np.outer(fy, fx)
    return np.real(np.fft.ifft2(np.fft.fft2(values) * kernel))


def noise(rng: np.random.Generator, sigma: float) -> np.ndarray:
    field = periodic_blur(rng.standard_normal((SIZE, SIZE)), sigma)
    field -= field.mean()
    peak = np.abs(field).max()
    return field / peak if peak > 0 else field


def save(name: str, values: np.ndarray) -> None:
    values = np.clip(values, 0.0, 1.0)
    grey = np.round(values * 255.0).astype(np.uint8)
    rgb = np.stack([grey, grey, grey], axis=-1)
    Image.fromarray(rgb, mode="RGB").save(OUT / f"{name}.png", optimize=True)
    print(f"wrote {name}.png")


def paper_fine() -> np.ndarray:
    rng = np.random.default_rng(20261031)
    fine = noise(rng, 0.7)
    medium = noise(rng, 2.4)
    return 0.945 + 0.030 * fine + 0.018 * medium


def paper_news() -> np.ndarray:
    rng = np.random.default_rng(20261101)
    fibre = rng.standard_normal((SIZE, SIZE))
    # Horizontal fibres: blur across x far more than across y.
    freqs = np.fft.fftfreq(SIZE)
    gy = np.exp(-2.0 * (np.pi * 0.6 * freqs) ** 2)
    gx = np.exp(-2.0 * (np.pi * 6.0 * freqs) ** 2)
    fibre = np.real(np.fft.ifft2(np.fft.fft2(fibre) * np.outer(gy, gx)))
    fibre -= fibre.mean()
    fibre /= max(np.abs(fibre).max(), 1e-6)
    grain = noise(rng, 1.1)
    screen = np.sin(2.0 * np.pi * np.arange(SIZE) / 4.0)
    dots = np.outer(np.ones(SIZE), screen) * 0.006
    return 0.925 + 0.035 * fibre + 0.022 * grain + dots


def paper_soft() -> np.ndarray:
    rng = np.random.default_rng(20261102)
    cloud = noise(rng, 9.0)
    detail = noise(rng, 2.0)
    return 0.955 + 0.030 * cloud + 0.010 * detail


def paper_wash() -> np.ndarray:
    rng = np.random.default_rng(20261103)
    broad = noise(rng, 13.0)
    mid = noise(rng, 4.0)
    grain = noise(rng, 0.9)
    return 0.915 + 0.055 * broad + 0.020 * mid + 0.012 * grain


def dust_fine() -> np.ndarray:
    rng = np.random.default_rng(20261104)
    base = 0.970 + 0.014 * noise(rng, 0.8)
    # Sparse specks and two fine scratches, all placed by the seeded generator.
    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    for _ in range(140):
        cx = int(rng.integers(0, SIZE))
        cy = int(rng.integers(0, SIZE))
        radius = float(rng.uniform(0.6, 2.4))
        depth = float(rng.uniform(0.10, 0.32))
        wrapped = ((xx - cx + SIZE // 2) % SIZE - SIZE // 2) ** 2 + (
            (yy - cy + SIZE // 2) % SIZE - SIZE // 2
        ) ** 2
        base -= depth * np.exp(-wrapped / (2.0 * radius * radius))
    for _ in range(4):
        y0 = int(rng.integers(0, SIZE))
        x0 = int(rng.integers(0, SIZE))
        length = int(rng.integers(40, 180))
        angle = float(rng.uniform(-0.5, 0.5)) + np.pi / 2.0
        depth = float(rng.uniform(0.06, 0.14))
        for step in range(length):
            x = int(round(x0 + step * np.cos(angle))) % SIZE
            y = int(round(y0 + step * np.sin(angle))) % SIZE
            base[y, x] -= depth
    return base


def main() -> None:
    save("paper-fine", paper_fine())
    save("paper-news", paper_news())
    save("paper-soft", paper_soft())
    save("paper-wash", paper_wash())
    save("dust-fine", dust_fine())


if __name__ == "__main__":
    main()
