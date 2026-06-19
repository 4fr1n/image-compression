/**
 * jpeg_compress.cpp
 * A JPEG-like image compression tool implementing:
 *   1. PPM image I/O (simple portable format, no external libs needed)
 *   2. RGB → YCbCr color space conversion
 *   3. 4:2:0 chroma subsampling
 *   4. 8x8 block DCT (Discrete Cosine Transform)
 *   5. Quantization (with configurable quality factor)
 *   6. Zigzag scan + Run-Length Encoding (RLE) of AC coefficients
 *   7. Full decompression pipeline back to PPM
 *
 * Usage:
 *   Compress:   ./jpeg_compress c input.ppm output.jc <quality 1-100>
 *   Decompress: ./jpeg_compress d input.jc  output.ppm
 *
 * Build:
 *   g++ -O2 -std=c++17 -o jpeg_compress jpeg_compress.cpp
 *
 * To test with a sample PPM:
 *   Use GIMP, ImageMagick ("convert input.png input.ppm"), or generate one below.
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <cassert>

// ─────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────
static const int BLOCK = 8;
static const double PI = std::acos(-1.0);

// Standard JPEG luminance quantization table (quality=50 baseline)
static const int LUMA_QTABLE[BLOCK][BLOCK] = {
    {16,11,10,16,24,40,51,61},
    {12,12,14,19,26,58,60,55},
    {14,13,16,24,40,57,69,56},
    {14,17,22,29,51,87,80,62},
    {18,22,37,56,68,109,103,77},
    {24,35,55,64,81,104,113,92},
    {49,64,78,87,103,121,120,101},
    {72,92,95,98,112,100,103,99}
};

// Standard JPEG chrominance quantization table
static const int CHROMA_QTABLE[BLOCK][BLOCK] = {
    {17,18,24,47,99,99,99,99},
    {18,21,26,66,99,99,99,99},
    {24,26,56,99,99,99,99,99},
    {47,66,99,99,99,99,99,99},
    {99,99,99,99,99,99,99,99},
    {99,99,99,99,99,99,99,99},
    {99,99,99,99,99,99,99,99},
    {99,99,99,99,99,99,99,99}
};

// Zigzag scan order for an 8x8 block (maps linear index → (row,col))
static const int ZIGZAG[64][2] = {
    {0,0},{0,1},{1,0},{2,0},{1,1},{0,2},{0,3},{1,2},
    {2,1},{3,0},{4,0},{3,1},{2,2},{1,3},{0,4},{0,5},
    {1,4},{2,3},{3,2},{4,1},{5,0},{6,0},{5,1},{4,2},
    {3,3},{2,4},{1,5},{0,6},{0,7},{1,6},{2,5},{3,4},
    {4,3},{5,2},{6,1},{7,0},{7,1},{6,2},{5,3},{4,4},
    {3,5},{2,6},{1,7},{2,7},{3,6},{4,5},{5,4},{6,3},
    {7,2},{7,3},{6,4},{5,5},{4,6},{3,7},{4,7},{5,6},
    {6,5},{7,4},{7,5},{6,6},{5,7},{6,7},{7,6},{7,7}
};

// ─────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────
inline uint8_t clamp_u8(double v) {
    return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(v))));
}

// Scale a standard JPEG qtable by quality factor (1-100)
void scale_qtable(const int src[BLOCK][BLOCK], int dst[BLOCK][BLOCK], int quality) {
    quality = std::max(1, std::min(100, quality));
    int scale = (quality < 50) ? (5000 / quality) : (200 - 2 * quality);
    for (int r = 0; r < BLOCK; ++r)
        for (int c = 0; c < BLOCK; ++c)
            dst[r][c] = std::max(1, std::min(255, (src[r][c] * scale + 50) / 100));
}

// ─────────────────────────────────────────────
// PPM I/O  (supports P6 binary PPM)
// ─────────────────────────────────────────────
struct Image {
    int width, height;
    std::vector<uint8_t> data; // interleaved R,G,B

    uint8_t& r(int row, int col) { return data[(row * width + col) * 3 + 0]; }
    uint8_t& g(int row, int col) { return data[(row * width + col) * 3 + 1]; }
    uint8_t& b(int row, int col) { return data[(row * width + col) * 3 + 2]; }
    uint8_t  r(int row, int col) const { return data[(row * width + col) * 3 + 0]; }
    uint8_t  g(int row, int col) const { return data[(row * width + col) * 3 + 1]; }
    uint8_t  b(int row, int col) const { return data[(row * width + col) * 3 + 2]; }
};

Image read_ppm(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open PPM: " + path);
    std::string magic;
    f >> magic;
    if (magic != "P6") throw std::runtime_error("Only P6 (binary) PPM supported.");
    Image img;
    int maxval;
    f >> img.width >> img.height >> maxval;
    f.ignore(1); // skip single whitespace after header
    img.data.resize(img.width * img.height * 3);
    f.read(reinterpret_cast<char*>(img.data.data()), img.data.size());
    return img;
}

void write_ppm(const std::string& path, const Image& img) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << img.width << " " << img.height << "\n255\n";
    f.write(reinterpret_cast<const char*>(img.data.data()), img.data.size());
}

// ─────────────────────────────────────────────
// Color Space Conversion
// ─────────────────────────────────────────────
struct YCbCr { double Y, Cb, Cr; };

YCbCr rgb_to_ycbcr(uint8_t R, uint8_t G, uint8_t B) {
    double Y  =  0.299   * R + 0.587   * G + 0.114   * B;
    double Cb = -0.16874 * R - 0.33126 * G + 0.5     * B + 128.0;
    double Cr =  0.5     * R - 0.41869 * G - 0.08131 * B + 128.0;
    return {Y, Cb, Cr};
}

void ycbcr_to_rgb(double Y, double Cb, double Cr,
                  uint8_t& R, uint8_t& G, uint8_t& B) {
    Cb -= 128.0; Cr -= 128.0;
    R = clamp_u8(Y                + 1.40200 * Cr);
    G = clamp_u8(Y - 0.34414 * Cb - 0.71414 * Cr);
    B = clamp_u8(Y + 1.77200 * Cb);
}

// ─────────────────────────────────────────────
// DCT / IDCT  (2D, unnormalized, on [-128,127] shifted block)
// ─────────────────────────────────────────────
// Input block[r][c] should have 128 subtracted (level shift).
void dct2d(double block[BLOCK][BLOCK]) {
    double tmp[BLOCK][BLOCK] = {};
    // Row DCT
    for (int r = 0; r < BLOCK; ++r) {
        for (int u = 0; u < BLOCK; ++u) {
            double sum = 0;
            for (int x = 0; x < BLOCK; ++x)
                sum += block[r][x] * std::cos((2*x+1)*u*PI/16.0);
            double cu = (u == 0) ? 1.0/std::sqrt(2.0) : 1.0;
            tmp[r][u] = 0.5 * cu * sum;
        }
    }
    // Col DCT
    for (int u = 0; u < BLOCK; ++u) {
        for (int v = 0; v < BLOCK; ++v) {
            double sum = 0;
            for (int y = 0; y < BLOCK; ++y)
                sum += tmp[y][u] * std::cos((2*y+1)*v*PI/16.0);
            double cv = (v == 0) ? 1.0/std::sqrt(2.0) : 1.0;
            block[v][u] = 0.5 * cv * sum;
        }
    }
}

void idct2d(double block[BLOCK][BLOCK]) {
    double tmp[BLOCK][BLOCK] = {};
    // Col IDCT
    for (int x = 0; x < BLOCK; ++x) {
        for (int y = 0; y < BLOCK; ++y) {
            double sum = 0;
            for (int v = 0; v < BLOCK; ++v) {
                double cv = (v == 0) ? 1.0/std::sqrt(2.0) : 1.0;
                sum += cv * block[v][x] * std::cos((2*y+1)*v*PI/16.0);
            }
            tmp[y][x] = 0.5 * sum;
        }
    }
    // Row IDCT
    for (int y = 0; y < BLOCK; ++y) {
        for (int x = 0; x < BLOCK; ++x) {
            double sum = 0;
            for (int u = 0; u < BLOCK; ++u) {
                double cu = (u == 0) ? 1.0/std::sqrt(2.0) : 1.0;
                sum += cu * tmp[y][u] * std::cos((2*x+1)*u*PI/16.0);
            }
            block[y][x] = 0.5 * sum;
        }
    }
}

// ─────────────────────────────────────────────
// Quantize / Dequantize a block
// ─────────────────────────────────────────────
void quantize(double block[BLOCK][BLOCK], int qblock[BLOCK][BLOCK],
              const int qtable[BLOCK][BLOCK]) {
    for (int r = 0; r < BLOCK; ++r)
        for (int c = 0; c < BLOCK; ++c)
            qblock[r][c] = static_cast<int>(std::round(block[r][c] / qtable[r][c]));
}

void dequantize(const int qblock[BLOCK][BLOCK], double block[BLOCK][BLOCK],
                const int qtable[BLOCK][BLOCK]) {
    for (int r = 0; r < BLOCK; ++r)
        for (int c = 0; c < BLOCK; ++c)
            block[r][c] = qblock[r][c] * qtable[r][c];
}

// ─────────────────────────────────────────────
// Zigzag → linear array (64 values)
// ─────────────────────────────────────────────
void zigzag_encode(const int qblock[BLOCK][BLOCK], int out[64]) {
    for (int i = 0; i < 64; ++i)
        out[i] = qblock[ZIGZAG[i][0]][ZIGZAG[i][1]];
}

void zigzag_decode(const int in[64], int qblock[BLOCK][BLOCK]) {
    for (int i = 0; i < 64; ++i)
        qblock[ZIGZAG[i][0]][ZIGZAG[i][1]] = in[i];
}

// ─────────────────────────────────────────────
// Simple binary file writer / reader
// ─────────────────────────────────────────────
struct BitWriter {
    std::ofstream& f;
    void write_i32(int32_t v) { f.write(reinterpret_cast<char*>(&v), 4); }
    void write_i16(int16_t v) { f.write(reinterpret_cast<char*>(&v), 2); }
    void write_u8 (uint8_t v) { f.write(reinterpret_cast<char*>(&v), 1); }
};

struct BitReader {
    std::ifstream& f;
    int32_t read_i32() { int32_t v; f.read(reinterpret_cast<char*>(&v), 4); return v; }
    int16_t read_i16() { int16_t v; f.read(reinterpret_cast<char*>(&v), 2); return v; }
    uint8_t read_u8 () { uint8_t v; f.read(reinterpret_cast<char*>(&v), 1); return v; }
};

// ─────────────────────────────────────────────
// RLE on zigzag coefficients
//   Format: (run_of_zeros : u8, value : i16) pairs, terminated by (0,0)
//   DC is stored separately as i16.
// ─────────────────────────────────────────────
void rle_encode_block(const int zz[64], BitWriter& bw) {
    // DC coefficient
    bw.write_i16(static_cast<int16_t>(zz[0]));
    // AC coefficients
    int zeros = 0;
    for (int i = 1; i < 64; ++i) {
        if (zz[i] == 0) {
            ++zeros;
        } else {
            while (zeros > 15) { // JPEG uses 4-bit run, so flush at 16
                bw.write_u8(15); bw.write_i16(0);
                zeros -= 16;
            }
            bw.write_u8(static_cast<uint8_t>(zeros));
            bw.write_i16(static_cast<int16_t>(zz[i]));
            zeros = 0;
        }
    }
    // EOB marker
    bw.write_u8(0); bw.write_i16(0);
}

void rle_decode_block(int zz[64], BitReader& br) {
    zz[0] = br.read_i16(); // DC
    int i = 1;
    while (i < 64) {
        uint8_t run = br.read_u8();
        int16_t val = br.read_i16();
        if (run == 0 && val == 0) { // EOB
            while (i < 64) zz[i++] = 0;
            break;
        }
        for (int z = 0; z < run && i < 64; ++z) zz[i++] = 0;
        if (i < 64) zz[i++] = val;
    }
}

// ─────────────────────────────────────────────
// Channel (plane) helpers
// ─────────────────────────────────────────────
// Pad channel to multiple of 8
std::vector<double> pad_channel(const std::vector<double>& ch,
                                int w, int h, int& pw, int& ph) {
    pw = ((w + 7) / 8) * 8;
    ph = ((h + 7) / 8) * 8;
    std::vector<double> out(pw * ph, 0.0);
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c)
            out[r * pw + c] = ch[r * w + c];
        // replicate right edge
        for (int c = w; c < pw; ++c)
            out[r * pw + c] = ch[r * w + (w - 1)];
    }
    // replicate bottom edge
    for (int r = h; r < ph; ++r)
        for (int c = 0; c < pw; ++c)
            out[r * pw + c] = out[(h - 1) * pw + c];
    return out;
}

// Subsample (2x2 average) a channel
std::vector<double> subsample(const std::vector<double>& ch, int w, int h,
                              int& sw, int& sh) {
    sw = w / 2; sh = h / 2;
    std::vector<double> out(sw * sh);
    for (int r = 0; r < sh; ++r)
        for (int c = 0; c < sw; ++c)
            out[r * sw + c] = (ch[(2*r)*w + 2*c]   + ch[(2*r)*w + 2*c+1] +
                                ch[(2*r+1)*w + 2*c] + ch[(2*r+1)*w + 2*c+1]) / 4.0;
    return out;
}

// Upsample (nearest-neighbor 2x) a channel
std::vector<double> upsample(const std::vector<double>& ch, int sw, int sh) {
    int w = sw * 2, h = sh * 2;
    std::vector<double> out(w * h);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            out[r * w + c] = ch[(r/2) * sw + (c/2)];
    return out;
}

// Encode one channel plane to file
void encode_channel(const std::vector<double>& ch, int pw, int ph,
                    const int qtable[BLOCK][BLOCK], BitWriter& bw) {
    int brows = ph / BLOCK, bcols = pw / BLOCK;
    for (int br = 0; br < brows; ++br) {
        for (int bc = 0; bc < bcols; ++bc) {
            double block[BLOCK][BLOCK];
            for (int r = 0; r < BLOCK; ++r)
                for (int c = 0; c < BLOCK; ++c)
                    block[r][c] = ch[(br*BLOCK+r)*pw + (bc*BLOCK+c)] - 128.0;
            dct2d(block);
            int qblock[BLOCK][BLOCK];
            quantize(block, qblock, qtable);
            int zz[64];
            zigzag_encode(qblock, zz);
            rle_encode_block(zz, bw);
        }
    }
}

// Decode one channel plane from file
std::vector<double> decode_channel(int pw, int ph,
                                   const int qtable[BLOCK][BLOCK], BitReader& br) {
    std::vector<double> ch(pw * ph);
    int brows = ph / BLOCK, bcols = pw / BLOCK;
    for (int br2 = 0; br2 < brows; ++br2) {
        for (int bc = 0; bc < bcols; ++bc) {
            int zz[64];
            rle_decode_block(zz, br);
            int qblock[BLOCK][BLOCK];
            zigzag_decode(zz, qblock);
            double block[BLOCK][BLOCK];
            dequantize(qblock, block, qtable);
            idct2d(block);
            for (int r = 0; r < BLOCK; ++r)
                for (int c = 0; c < BLOCK; ++c)
                    ch[(br2*BLOCK+r)*pw + (bc*BLOCK+c)] =
                        std::max(0.0, std::min(255.0, block[r][c] + 128.0));
        }
    }
    return ch;
}

// ─────────────────────────────────────────────
// Compress
// ─────────────────────────────────────────────
void compress(const std::string& src_path,
              const std::string& dst_path,
              int quality) {
    Image img = read_ppm(src_path);
    int W = img.width, H = img.height;

    // Split into Y, Cb, Cr channels
    std::vector<double> Y(W*H), Cb(W*H), Cr(W*H);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            auto [y,cb,cr] = rgb_to_ycbcr(img.r(r,c), img.g(r,c), img.b(r,c));
            Y[r*W+c] = y; Cb[r*W+c] = cb; Cr[r*W+c] = cr;
        }

    // Pad Y to 8x8 multiples
    int pW, pH;
    auto Yp = pad_channel(Y, W, H, pW, pH);

    // Subsample Cb and Cr (4:2:0)
    int sW, sH, spW, spH;
    auto Cbs = subsample(Cb, W, H, sW, sH);
    auto Crs = subsample(Cr, W, H, sW, sH);
    auto Cbp = pad_channel(Cbs, sW, sH, spW, spH);
    auto Crp = pad_channel(Crs, sW, sH, spW, spH);

    // Scale quantization tables
    int lq[BLOCK][BLOCK], cq[BLOCK][BLOCK];
    scale_qtable(LUMA_QTABLE,   lq, quality);
    scale_qtable(CHROMA_QTABLE, cq, quality);

    std::ofstream f(dst_path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write: " + dst_path);
    BitWriter bw{f};

    // Header: magic, dimensions, quality
    f.write("JC10", 4);
    bw.write_i32(W); bw.write_i32(H); bw.write_i32(quality);

    // Encode channels
    encode_channel(Yp,  pW,  pH,  lq, bw);
    encode_channel(Cbp, spW, spH, cq, bw);
    encode_channel(Crp, spW, spH, cq, bw);

    long long in_size  = (long long)W * H * 3;
    long long out_size = (long long)f.tellp();
    std::cout << "Compressed: " << in_size << " → " << out_size
              << " bytes  (" << std::fixed
              << (100.0 * out_size / in_size) << "% of original)\n";
}

// ─────────────────────────────────────────────
// Decompress
// ─────────────────────────────────────────────
void decompress(const std::string& src_path, const std::string& dst_path) {
    std::ifstream f(src_path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open: " + src_path);
    BitReader br{f};

    char magic[5] = {};
    f.read(magic, 4);
    if (std::string(magic) != "JC10")
        throw std::runtime_error("Not a JC10 file.");

    int W = br.read_i32(), H = br.read_i32(), quality = br.read_i32();
    std::cout << "Decompressing " << W << "x" << H
              << " quality=" << quality << "\n";

    int pW = ((W+7)/8)*8, pH = ((H+7)/8)*8;
    int sW = W/2, sH = H/2;
    int spW = ((sW+7)/8)*8, spH = ((sH+7)/8)*8;

    int lq[BLOCK][BLOCK], cq[BLOCK][BLOCK];
    scale_qtable(LUMA_QTABLE,   lq, quality);
    scale_qtable(CHROMA_QTABLE, cq, quality);

    auto Yp  = decode_channel(pW,  pH,  lq, br);
    auto Cbp = decode_channel(spW, spH, cq, br);
    auto Crp = decode_channel(spW, spH, cq, br);

    // Upsample chroma back to full size
    auto CbUp = upsample(Cbp, spW, spH); // now spW*2 x spH*2
    auto CrUp = upsample(Crp, spW, spH);
    int upW = spW * 2, upH = spH * 2;

    Image out;
    out.width = W; out.height = H;
    out.data.resize(W * H * 3);

    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            double y  = Yp [r * pW + c];
            // Chroma might be slightly larger due to padding; clamp index
            int cr2 = std::min(r, upH - 1);
            int cc2 = std::min(c, upW - 1);
            double cb = CbUp[cr2 * upW + cc2];
            double cr_ = CrUp[cr2 * upW + cc2];
            ycbcr_to_rgb(y, cb, cr_, out.r(r,c), out.g(r,c), out.b(r,c));
        }
    }
    write_ppm(dst_path, out);
    std::cout << "Written: " << dst_path << "\n";
}

// ─────────────────────────────────────────────
// Generate a test PPM (optional helper)
// ─────────────────────────────────────────────
void generate_test_ppm(const std::string& path, int W = 256, int H = 256) {
    Image img; img.width = W; img.height = H;
    img.data.resize(W * H * 3);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            img.r(r,c) = static_cast<uint8_t>((c * 255) / W);
            img.g(r,c) = static_cast<uint8_t>((r * 255) / H);
            img.b(r,c) = static_cast<uint8_t>(128 + 127*std::sin(r*c*0.01));
        }
    write_ppm(path, img);
    std::cout << "Generated test image: " << path << " (" << W << "x" << H << ")\n";
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr <<
            "Usage:\n"
            "  jpeg_compress gen  output.ppm          -- generate a test PPM\n"
            "  jpeg_compress c    input.ppm output.jc [quality=75]\n"
            "  jpeg_compress d    input.jc  output.ppm\n";
        return 1;
    }

    std::string mode = argv[1];
    try {
        if (mode == "gen") {
            if (argc < 3) { std::cerr << "Need output path\n"; return 1; }
            generate_test_ppm(argv[2]);
        } else if (mode == "c") {
            if (argc < 4) { std::cerr << "Need input and output paths\n"; return 1; }
            int quality = (argc >= 5) ? std::stoi(argv[4]) : 75;
            compress(argv[2], argv[3], quality);
        } else if (mode == "d") {
            if (argc < 4) { std::cerr << "Need input and output paths\n"; return 1; }
            decompress(argv[2], argv[3]);
        } else {
            std::cerr << "Unknown mode: " << mode << "\n"; return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n"; return 1;
    }
    return 0;
}
