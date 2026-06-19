# jpeg-compress

A from-scratch, dependency-free JPEG-style image compression tool written in C++17. It implements the full classical compression pipeline — color space conversion, chroma subsampling, DCT, quantization, zigzag scanning, and RLE — producing its own compact binary format (`.jc`) and reconstructing images back to PPM.

Built as a learning-focused reference implementation: every stage maps directly to a real step in the JPEG standard, with readable, well-commented code.

---

## Features

- Full compression and decompression pipeline
- Configurable quality factor (1–100)
- 4:2:0 chroma subsampling
- 8×8 block 2D DCT and inverse DCT
- Standard JPEG luminance and chrominance quantization tables
- Zigzag scan + run-length encoding of AC coefficients
- Zero external dependencies — pure C++17
- Built-in test image generator

---

## Build

```bash
g++ -O2 -std=c++17 -o jpeg_compress jpeg_compress.cpp
```

Tested with GCC 11+ and Clang 14+. No libraries required.

---

## Usage

**Generate a synthetic test image (no image needed to get started):**
```bash
./jpeg_compress gen test.ppm
```

**Compress a PPM image:**
```bash
./jpeg_compress c input.ppm output.jc <quality>
# quality: integer from 1 (smallest file) to 100 (best quality). Default: 75
```

**Decompress back to PPM:**
```bash
./jpeg_compress d output.jc restored.ppm
```

**Convert other image formats to PPM using ImageMagick:**
```bash
convert photo.jpg photo.ppm
convert restored.ppm restored.png
```

---

## How It Works

The compressor follows the same pipeline as the JPEG standard, stage by stage.

### 1. RGB → YCbCr Color Space Conversion

Raw pixels arrive as red, green, and blue values. These are converted into the YCbCr color space, which separates the image into:

- **Y** — luminance (brightness). The human visual system is highly sensitive to this.
- **Cb** — blue-difference chroma
- **Cr** — red-difference chroma

This separation is the foundation of lossy compression: because we see detail in brightness much more sharply than in color, the chroma channels can be aggressively compressed without the loss being visible.

```
Y  =  0.299·R + 0.587·G + 0.114·B
Cb = -0.169·R - 0.331·G + 0.500·B + 128
Cr =  0.500·R - 0.419·G - 0.081·B + 128
```

### 2. 4:2:0 Chroma Subsampling

The Cb and Cr channels are downsampled to half resolution in both dimensions (2×2 averaging), reducing their size to one quarter. The Y channel is kept at full resolution.

This mimics how the human eye works: spatial acuity for color is far lower than for luminance. Subsampling the chroma channels therefore removes data that we largely can't see, achieving significant size reduction before any transform even happens.

During decompression, the chroma channels are upsampled back to full resolution via nearest-neighbor interpolation before recombining with Y.

### 3. 8×8 Block DCT

Each channel is divided into non-overlapping 8×8 pixel blocks. A 128-level shift is applied (centering values around zero), and then the **2D Discrete Cosine Transform** is applied to each block.

The DCT transforms spatial pixel values into frequency coefficients:
- The top-left coefficient (DC) represents the average value of the block.
- The remaining 63 coefficients (AC) represent progressively higher-frequency detail.

Natural images tend to concentrate most of their energy in the low-frequency coefficients, leaving the high-frequency ones small or near zero. This is what makes DCT so effective as a compression step.

The formula for the 2D DCT-II used here:

```
F(u,v) = (1/4) · C(u) · C(v) · ΣΣ f(x,y) · cos((2x+1)uπ/16) · cos((2y+1)vπ/16)
```

where `C(k) = 1/√2` if `k = 0`, else `1`.

### 4. Quantization

Each DCT coefficient is divided by a corresponding value in a **quantization table** and rounded to the nearest integer. This is the primary lossy step.

The tool uses the standard JPEG quantization tables, which assign smaller divisors to low-frequency coefficients (preserving important detail) and larger divisors to high-frequency ones (aggressively discarding imperceptible information). The tables are scaled by a quality factor:

```
scale = (quality < 50) ? 5000 / quality : 200 - 2 * quality
qvalue = clamp(1, 255, (base_value × scale + 50) / 100)
```

At low quality settings, most high-frequency coefficients round to zero. At high quality, the tables have little effect and almost all data is preserved.

Dequantization on the decoder side simply multiplies back by the same table, recovering approximate (not exact) original coefficients.

### 5. Zigzag Scan

After quantization, the 8×8 block of coefficients is read out in a **zigzag order** — traversing diagonals from the top-left (low frequency) corner to the bottom-right (high frequency) corner.

```
 0  1  5  6 14 15 27 28
 2  4  7 13 16 26 29 42
 3  8 12 17 25 30 41 43
 9 11 18 24 31 40 44 53
10 19 23 32 39 45 52 54
20 22 33 38 46 51 55 60
21 34 37 47 50 56 59 61
35 36 48 49 57 58 62 63
```

This ordering groups the near-zero high-frequency values together at the end of the array, setting up the next stage for efficient encoding.

### 6. Run-Length Encoding (RLE)

The zigzag sequence is encoded using run-length encoding on the AC coefficients. Each non-zero value is stored as a `(run, value)` pair, where `run` is the count of preceding zeros. An end-of-block (EOB) marker signals that all remaining coefficients in the block are zero.

The DC coefficient (first value in the zigzag) is stored separately as a raw 16-bit integer before the AC run-length stream.

This stage is lossless. Together with zigzag ordering, it efficiently eliminates the long runs of zeros produced by quantization.

> Real JPEG goes further and applies Huffman coding on top of the RLE stream. This implementation uses direct binary output, which keeps the code simple while still demonstrating the complete pipeline.

### 7. Decompression

Decompression is the exact reverse:

1. Read header and reconstruct scaled quantization tables
2. Read RLE stream → reconstruct zigzag array
3. Inverse zigzag → 8×8 quantized block
4. Dequantize → floating-point DCT coefficients
5. 2D inverse DCT → spatial pixel block (add 128 level shift back)
6. Upsample Cb and Cr channels 2×
7. YCbCr → RGB for every pixel
8. Write PPM

---

## File Format (`.jc`)

The output format is a custom binary format with a minimal header:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 bytes | Magic: `JC10` |
| 4 | 4 bytes | Image width (int32) |
| 8 | 4 bytes | Image height (int32) |
| 12 | 4 bytes | Quality factor (int32) |
| 16+ | variable | Y channel RLE blocks, then Cb, then Cr |

Each block in the stream is encoded as: `DC (i16)` followed by `(run: u8, value: i16)` pairs, terminated by `(0, 0)`.

---

## Quality vs. Size Examples

Results will vary by image content. As a rough guide on a typical photograph:

| Quality | Typical compression ratio | Visual quality |
|---------|--------------------------|----------------|
| 10 | ~25:1 | Heavy blocking artifacts |
| 50 | ~10:1 | Visible but acceptable |
| 75 | ~5:1 | Good — close to original |
| 95 | ~2:1 | Near-lossless to the eye |

---

## Project Structure

```
.
├── jpeg_compress.cpp    # Entire implementation, single file
└── README.md
```

---

## Limitations

- Input format: **P6 binary PPM only**. Use ImageMagick to convert from JPEG/PNG/etc.
- No Huffman coding (AC coefficients are raw binary, not entropy-coded)
- No progressive encoding
- Image dimensions are padded to the nearest 8-pixel boundary internally and cropped on decode
- No EXIF or metadata support

---

## References

- Wallace, G.K. — *The JPEG Still Picture Compression Standard* (1991)
- [JPEG standard overview, Wikipedia](https://en.wikipedia.org/wiki/JPEG)
- Pennebaker & Mitchell — *JPEG: Still Image Data Compression Standard* (1992)

---
