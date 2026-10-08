#!/usr/bin/env python3
"""Encode a PNG or JPEG as a single-surface 3840x2160 BC7_UNORM DX10 DDS.

Mode 6 only. That is the layout pic0.dds and pic1.dds need: one 2D image,
no mipmaps, DXGI format 98.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

WEIGHTS4 = np.array(
    [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64],
    dtype=np.float32,
)


def quantize_batch(endpoint):
    color = np.clip(np.rint(endpoint), 0, 255)
    best_error = None
    best = None
    for parity in (0, 1):
        quantized = np.clip(np.rint((color - parity) / 2.0) * 2.0 + parity, 0, 255)
        error = ((quantized - color) ** 2).sum(axis=1)
        picked = best_error is None
        if not picked:
            picked = error < best_error
        if best is None:
            best_error = error
            best = (
                np.full(len(color), parity, dtype=np.uint8),
                (quantized.astype(np.uint16) >> 1).astype(np.uint8),
                quantized.astype(np.float32),
            )
            continue
        parity_out, bits_out, values_out = best
        parity_out[error < best_error] = parity
        bits_out[error < best_error] = (quantized[error < best_error].astype(np.uint16) >> 1).astype(np.uint8)
        values_out[error < best_error] = quantized[error < best_error]
        best_error = np.minimum(best_error, error)
        best = (parity_out, bits_out, values_out)
    return best


def encode(image):
    array = np.asarray(image, dtype=np.float32)
    height, width, _ = array.shape
    blocks = array.reshape(height // 4, 4, width // 4, 4, 4)
    blocks = blocks.transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    parts = []
    step = 8192
    total = len(blocks)
    for start in range(0, total, step):
        block = blocks[start:start + step]
        low, high = endpoints(block)
        parity_low, low7, low_q = quantize_batch(low)
        parity_high, high7, high_q = quantize_batch(high)
        choice = indices(block, low_q, high_q)
        swap = choice[:, 0] >= 8
        if np.any(swap):
            low7[swap], high7[swap] = high7[swap].copy(), low7[swap].copy()
            parity_low[swap], parity_high[swap] = parity_high[swap].copy(), parity_low[swap].copy()
            choice[swap] = 15 - choice[swap]
        parts.append(pack_mode6(low7, high7, parity_low, parity_high, choice))
        print(f"{min(start + step, total)}/{total}", flush=True)
    return b"".join(parts)


def endpoints(block):
    mean = block.mean(axis=1)
    centered = block - mean[:, None, :]
    covariance = np.einsum("nij,nik->njk", centered, centered, optimize=True)
    axis = np.ones((block.shape[0], 4), dtype=np.float32)
    for _ in range(6):
        axis = np.einsum("nij,nj->ni", covariance, axis, optimize=True)
        axis /= np.linalg.norm(axis, axis=1, keepdims=True) + 1e-8
    projected = np.einsum("nij,nj->ni", centered, axis, optimize=True)
    low = mean + axis * projected.min(axis=1)[:, None]
    high = mean + axis * projected.max(axis=1)[:, None]
    return np.clip(low, 0, 255), np.clip(high, 0, 255)


def indices(block, low, high):
    span = WEIGHTS4[None, :, None]
    reconstructed = ((64.0 - span) * low[:, None, :] + span * high[:, None, :] + 32.0) / 64.0
    error = ((block[:, :, None, :] - reconstructed[:, None, :, :]) ** 2).sum(axis=3)
    return error.argmin(axis=2).astype(np.uint8)


def pack_mode6(low7, high7, plow, phigh, index):
    count = index.shape[0]
    words = np.zeros(count, dtype=object)
    for i in range(count):
        value = 0
        shift = 0

        def put(bits, width):
            nonlocal value, shift
            value |= int(bits) << shift
            shift += width

        put(0x3F, 7)
        for channel in range(4):
            put(int(low7[i, channel]), 7)
            put(int(high7[i, channel]), 7)
        put(int(plow[i]), 1)
        put(int(phigh[i]), 1)
        for pixel in range(16):
            width = 3 if pixel == 0 else 4
            put(int(index[i, pixel]) & ((1 << width) - 1), width)
        words[i] = value
    out = bytearray(count * 16)
    for i, value in enumerate(words):
        out[i * 16:(i + 1) * 16] = int(value).to_bytes(16, "little")
    return bytes(out)


def dds_header(width, height, payload_size):
    header = bytearray(148)
    header[0:4] = b"DDS "
    header[4:8] = (124).to_bytes(4, "little")
    header[8:12] = (0x000A1007).to_bytes(4, "little")
    header[12:16] = height.to_bytes(4, "little")
    header[16:20] = width.to_bytes(4, "little")
    header[20:24] = payload_size.to_bytes(4, "little")
    header[28:32] = (1).to_bytes(4, "little")
    header[76:80] = (32).to_bytes(4, "little")
    header[80:84] = (0x4).to_bytes(4, "little")
    header[84:88] = b"DX10"
    header[108:112] = (0x1000).to_bytes(4, "little")
    header[128:132] = (98).to_bytes(4, "little")
    header[132:136] = (3).to_bytes(4, "little")
    header[140:144] = (1).to_bytes(4, "little")
    return bytes(header)


def main():
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    image = Image.open(src).convert("RGBA").resize((3840, 2160), Image.Resampling.LANCZOS)
    payload = encode(np.asarray(image))
    expected = (3840 // 4) * (2160 // 4) * 16
    if len(payload) != expected:
        raise SystemExit(f"payload {len(payload)} != {expected}")
    dst.write_bytes(dds_header(3840, 2160, len(payload)) + payload)
    print(dst, dst.stat().st_size)


if __name__ == "__main__":
    main()
