// texture_processor.cpp
// C++17 port of LivinTheDreamToolkit TextureProcessor.cs
// No exceptions (-fno-exceptions compatible). All errors returned as strings.

#include "texture_processor.h"
#include <zstd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdint>

namespace TextureProcessor {

// ─────────────────────────────────────────────────────────────────────────────
// sRGB LUTs
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t s_srgbToLinear[256];
static uint8_t s_linearToSrgb[256];
static bool    s_lutsBuilt = false;

static void BuildLuts() {
    if (s_lutsBuilt) return;
    for (int i = 0; i < 256; i++) {
        double s = i / 255.0;
        double lin = (s <= 0.04045) ? s / 12.92
                                     : std::pow((s + 0.055) / 1.055, 2.4);
        s_srgbToLinear[i] = (uint8_t)std::clamp((int)std::round(lin * 255.0), 0, 255);

        // i represents a linear value (0–255 → 0.0–1.0), convert to sRGB
        double linVal = i / 255.0;
        double sg = (linVal <= 0.0031308) ? linVal * 12.92
                                           : 1.055 * std::pow(linVal, 1.0 / 2.4) - 0.055;
        s_linearToSrgb[i] = (uint8_t)std::clamp((int)std::round(sg * 255.0), 0, 255);
    }
    s_lutsBuilt = true;
}

void ConvertSrgbToLinear(std::vector<uint8_t>& rgba) {
    BuildLuts();
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i]   = s_srgbToLinear[rgba[i]];
        rgba[i+1] = s_srgbToLinear[rgba[i+1]];
        rgba[i+2] = s_srgbToLinear[rgba[i+2]];
    }
}

void ConvertLinearToSrgb(std::vector<uint8_t>& rgba) {
    BuildLuts();
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i]   = s_linearToSrgb[rgba[i]];
        rgba[i+1] = s_linearToSrgb[rgba[i+1]];
        rgba[i+2] = s_linearToSrgb[rgba[i+2]];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Filename helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string Basename(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

TextureKind DetectKind(const std::string& filename) {
    std::string lower = ToLower(Basename(filename));
    if (lower.find("thumb") != std::string::npos) return TextureKind::Thumb;
    if (lower.find("ugctex") != std::string::npos) return TextureKind::Ugctex;
    return TextureKind::Canvas;
}

std::string GetBaseName(const std::string& filename) {
    std::string name = Basename(filename);
    if (EndsWith(name, ".zs")) name = name.substr(0, name.size() - 3);
    for (const char* suf : {".canvas", ".ugctex", "_canvas", "_ugctex"}) {
        if (EndsWith(ToLower(name), suf)) {
            name = name.substr(0, name.size() - strlen(suf));
            break;
        }
    }
    return name;
}

// Returns true and fills layout, or returns false with errOut set
static bool DetectUgctexLayoutInner(size_t bytes, UgctexLayout& layout, std::string& errOut) {
    switch (bytes) {
        case 131072: layout = {512, 512, 128, 128, 16, TextureFormat::Bc1}; return true;
        case  98304: layout = {384, 384,  96, 128, 16, TextureFormat::Bc1}; return true;
        case  65536: layout = {256, 256,  64,  64,  8, TextureFormat::Bc3}; return true;
        default:
            errOut = "Unknown ugctex size: " + std::to_string(bytes) + " bytes";
            return false;
    }
}

UgctexLayout DetectUgctexLayout(size_t bytes) {
    UgctexLayout layout{};
    std::string err;
    DetectUgctexLayoutInner(bytes, layout, err);
    return layout;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zstd I/O  (no exceptions – return error via bool/string)
// ─────────────────────────────────────────────────────────────────────────────

static bool ZstdDecompressInner(const uint8_t* data, size_t size,
                                 std::vector<uint8_t>& out, std::string& errOut) {
    unsigned long long bound = ZSTD_getFrameContentSize(data, size);
    if (bound == ZSTD_CONTENTSIZE_ERROR) {
        errOut = "Not a valid zstd frame"; return false;
    }
    if (bound == ZSTD_CONTENTSIZE_UNKNOWN) bound = 4 * 1024 * 1024;

    out.resize((size_t)bound);
    size_t result = ZSTD_decompress(out.data(), out.size(), data, size);
    if (ZSTD_isError(result)) {
        errOut = std::string("Zstd decompress: ") + ZSTD_getErrorName(result);
        return false;
    }
    out.resize(result);
    return true;
}

std::vector<uint8_t> ZstdDecompressBuffer(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    std::string err;
    ZstdDecompressInner(data, size, out, err);
    return out;
}

std::vector<uint8_t> ZstdDecompress(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> compressed(sz);
    f.read((char*)compressed.data(), sz);
    return ZstdDecompressBuffer(compressed.data(), compressed.size());
}

static bool ZstdDecompressFile(const std::string& path,
                                std::vector<uint8_t>& out, std::string& errOut) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { errOut = "Cannot open: " + path; return false; }
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> compressed(sz);
    f.read((char*)compressed.data(), sz);
    return ZstdDecompressInner(compressed.data(), compressed.size(), out, errOut);
}

std::vector<uint8_t> ZstdCompress(const std::vector<uint8_t>& data, int level) {
    size_t bound = ZSTD_compressBound(data.size());
    std::vector<uint8_t> out(bound);
    size_t result = ZSTD_compress(out.data(), bound, data.data(), data.size(), level);
    if (ZSTD_isError(result)) return {};
    out.resize(result);
    return out;
}

static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data,
                      std::string& errOut) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { errOut = "Cannot write: " + path; return false; }
    f.write((const char*)data.data(), data.size());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block-linear swizzle
// ─────────────────────────────────────────────────────────────────────────────

static int DivRoundUp(int n, int d) { return (n + d - 1) / d; }

static int GobAddress(int x, int y, int widthInGobs, int bpe, int blockHeight) {
    int xBytes = x * bpe;
    int gobAddress =
          (y / (8 * blockHeight)) * 512 * blockHeight * widthInGobs
        + (xBytes / 64) * 512 * blockHeight
        + ((y % (8 * blockHeight)) / 8) * 512;
    int xInGob = xBytes % 64;
    int yInGob = y % 8;
    return gobAddress
        + ((xInGob % 64) / 32) * 256
        + ((yInGob % 8) / 2) * 64
        + ((xInGob % 32) / 16) * 32
        + (yInGob % 2) * 16
        + (xInGob % 16);
}

std::vector<uint8_t> DeswizzleBlockLinear(const std::vector<uint8_t>& data,
                                           int width, int height,
                                           int bpe, int blockHeight) {
    int widthInGobs  = DivRoundUp(width * bpe, 64);
    int paddedHeight = DivRoundUp(height, 8 * blockHeight) * (8 * blockHeight);
    size_t paddedSize = (size_t)widthInGobs * paddedHeight * 64;

    const uint8_t* src = data.data();
    std::vector<uint8_t> padded;
    if (data.size() < paddedSize) {
        padded.resize(paddedSize, 0);
        memcpy(padded.data(), data.data(), data.size());
        src = padded.data();
    }

    std::vector<uint8_t> output((size_t)width * height * bpe);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int sw  = GobAddress(x, y, widthInGobs, bpe, blockHeight);
            int lin = (y * width + x) * bpe;
            memcpy(output.data() + lin, src + sw, bpe);
        }
    }
    return output;
}

std::vector<uint8_t> SwizzleBlockLinear(const std::vector<uint8_t>& data,
                                         int width, int height,
                                         int bpe, int blockHeight,
                                         const std::vector<uint8_t>* baseBuffer) {
    int widthInGobs  = DivRoundUp(width * bpe, 64);
    int paddedHeight = DivRoundUp(height, 8 * blockHeight) * (8 * blockHeight);
    size_t paddedSize = (size_t)widthInGobs * paddedHeight * 64;

    std::vector<uint8_t> output;
    if (baseBuffer && baseBuffer->size() == paddedSize)
        output = *baseBuffer;
    else
        output.resize(paddedSize, 0);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int lin = (y * width + x) * bpe;
            int sw  = GobAddress(x, y, widthInGobs, bpe, blockHeight);
            memcpy(output.data() + sw, data.data() + lin, bpe);
        }
    }
    return output;
}

// ─────────────────────────────────────────────────────────────────────────────
// RGB565
// ─────────────────────────────────────────────────────────────────────────────

struct Rgb { uint8_t r, g, b; };

static Rgb Rgb565Decode(uint16_t c) {
    return {
        (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
        (uint8_t)(((c >>  5) & 0x3F) * 255 / 63),
        (uint8_t)( (c        & 0x1F) * 255 / 31)
    };
}

static uint16_t Rgb565Encode(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r * 31 + 127) / 255) << 11 |
                      ((g * 63 + 127) / 255) << 5  |
                      ((b * 31 + 127) / 255));
}

static int ColorDistSq(int r1,int g1,int b1, int r2,int g2,int b2) {
    int dr=r1-r2, dg=g1-g2, db=b1-b2;
    return dr*dr + dg*dg + db*db;
}

// ─────────────────────────────────────────────────────────────────────────────
// BC1
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> Bc1Decode(const std::vector<uint8_t>& bd, int W, int H) {
    int bx=W/4, by=H/4;
    std::vector<uint8_t> out((size_t)W*H*4);
    uint8_t pal[16];
    for (int iy=0;iy<by;iy++) for (int ix=0;ix<bx;ix++) {
        const uint8_t* b = bd.data()+(iy*bx+ix)*8;
        uint16_t c0r,c1r; memcpy(&c0r,b,2); memcpy(&c1r,b+2,2);
        uint32_t idx; memcpy(&idx,b+4,4);
        auto [r0,g0,b0]=Rgb565Decode(c0r);
        auto [r1,g1,b1]=Rgb565Decode(c1r);
        pal[0]=r0;pal[1]=g0;pal[2]=b0;pal[3]=255;
        pal[4]=r1;pal[5]=g1;pal[6]=b1;pal[7]=255;
        if(c0r>c1r){
            pal[8]=(uint8_t)((2*r0+r1)/3);pal[9]=(uint8_t)((2*g0+g1)/3);
            pal[10]=(uint8_t)((2*b0+b1)/3);pal[11]=255;
            pal[12]=(uint8_t)((r0+2*r1)/3);pal[13]=(uint8_t)((g0+2*g1)/3);
            pal[14]=(uint8_t)((b0+2*b1)/3);pal[15]=255;
        } else {
            pal[8]=(uint8_t)((r0+r1)/2);pal[9]=(uint8_t)((g0+g1)/2);
            pal[10]=(uint8_t)((b0+b1)/2);pal[11]=255;
            pal[12]=pal[13]=pal[14]=pal[15]=0;
        }
        for(int row=0;row<4;row++) for(int col=0;col<4;col++){
            int pi=(idx>>(2*(row*4+col)))&3;
            int dst=((iy*4+row)*W+(ix*4+col))*4;
            memcpy(out.data()+dst,pal+pi*4,4);
        }
    }
    return out;
}

static void AssignBc1Indices(const uint8_t blk[64], uint16_t c0, uint16_t c1,
                              bool use3Color, uint8_t* out, int off) {
    auto [r0,g0,b0]=Rgb565Decode(c0); auto [r1,g1,b1]=Rgb565Decode(c1);
    int pr2,pg2,pb2,pr3,pg3,pb3;
    if(c0>c1){pr2=(2*r0+r1)/3;pg2=(2*g0+g1)/3;pb2=(2*b0+b1)/3;
              pr3=(r0+2*r1)/3;pg3=(g0+2*g1)/3;pb3=(b0+2*b1)/3;}
    else{pr2=(r0+r1)/2;pg2=(g0+g1)/2;pb2=(b0+b1)/2;pr3=pg3=pb3=0;}
    uint32_t indices=0;
    for(int i=0;i<16;i++){
        const uint8_t* p=blk+i*4;
        int r=p[0],g=p[1],b=p[2],a=p[3],best;
        if(a<128&&use3Color){best=3;}
        else{
            int d0=ColorDistSq(r,g,b,r0,g0,b0),d1=ColorDistSq(r,g,b,r1,g1,b1),
                d2=ColorDistSq(r,g,b,pr2,pg2,pb2); best=0; int bd=d0;
            if(d1<bd){bd=d1;best=1;} if(d2<bd){bd=d2;best=2;}
            if(!use3Color){int d3=ColorDistSq(r,g,b,pr3,pg3,pb3);if(d3<bd)best=3;}
        }
        indices|=(uint32_t)(best<<(2*i));
    }
    memcpy(out+off,&c0,2); memcpy(out+off+2,&c1,2); memcpy(out+off+4,&indices,4);
}

static void EnforceMode(uint16_t& c0, uint16_t& c1, bool use3Color) {
    if(use3Color){ if(c0>c1) std::swap(c0,c1); if(c0==c1){if(c1<0xFFFF)c1++;else if(c0>0)c0--;} }
    else { if(c0<c1) std::swap(c0,c1); if(c0==c1){if(c0<0xFFFF)c0++;else c1--;} }
}

// ─────────────────────────────────────────────────────────────────────────────
// Custom BC1/BC3 color encoder
//
// Faithful C++ port of alexislours/ltd-save-editor's bc_encode.ts (AGPL-3.0).
// What it does, in order, per 4×4 block:
//   1. Compute the bounding box (per-channel min/max RGB) of opaque pixels.
//   2. Compute the principal color axis via 8 steps of power-iteration on the
//      3×3 covariance matrix.
//   3. Project pixels onto the axis to get min/max extents, then generate
//      candidate endpoint pairs: bounding-box and PCA at three scale factors
//      (k = 1.0, 1.5, 2.0), in both 4-color and 3-color modes (subject to the
//      requested bc1Mode and whether the block has alpha).
//   4. Score each candidate by squared sRGB distance — the palette is built
//      from RGB565 endpoints in linear space and then mapped back to sRGB via
//      a LUT for the comparison, which is the "sRGB-aware error metric" Rich
//      Geldreich describes in his BC1 writeups.
//   5. Iteratively refine each candidate (up to 4 rounds) by solving a 2-by-2
//      weighted least-squares system per channel for new endpoints; weights
//      are (dS/dL)² evaluated at the linear pixel value, which compensates for
//      the sRGB nonlinearity when the math is done in linear space.
//   6. After the best candidate is locked in, perturb its RGB565 endpoints ±1
//      in each 5/6-bit channel and keep any change that lowers error (up to 4
//      rounds of full ±1 sweeps).
//   7. Assign per-pixel indices by sRGB distance to the final palette.
//
// The 3-color mode (c0 ≤ c1) reserves palette entry 3 for fully transparent;
// 4-color mode (c0 > c1) uses entry 3 as a second interpolated color.
//
// All math is scalar f64; the upstream code uses WASM f64x2 SIMD purely for
// throughput on browser CPUs, not because anything is genuinely vectorised at
// the algorithm level. The Switch runs ARM Cortex-A57, so we keep it scalar.
// ─────────────────────────────────────────────────────────────────────────────

// (dS/dL)² perceptual weight LUT, indexed by linear value × 255. Used by
// LsRefit to weight the per-channel least-squares system so that an error in
// linear space approximates the corresponding error in sRGB space.
static float s_gammaDeriv2[256];
// sRGB(L) for L = i/255, normalized to [0,1]. Used by EvaluateBc1Mode to map
// a palette built in linear space back to sRGB for the squared-distance error
// metric. (Same numerical mapping as s_linearToSrgb, but in [0,1] floats.)
static float s_linearToSrgbF[256];
static bool  s_perceptualLutsBuilt = false;

static void BuildPerceptualLuts() {
    if (s_perceptualLutsBuilt) return;
    for (int i = 0; i < 256; i++) {
        double L = (double)i / 255.0;
        double dS_dL = (L <= 0.0031308)
            ? 12.92
            : (1.055 / 2.4) * std::pow(L, 1.0 / 2.4 - 1.0);
        s_gammaDeriv2[i] = (float)(dS_dL * dS_dL);
        double sV = (L <= 0.0031308)
            ? L * 12.92
            : 1.055 * std::pow(L, 1.0 / 2.4) - 0.055;
        if (sV < 0.0) sV = 0.0; else if (sV > 1.0) sV = 1.0;
        s_linearToSrgbF[i] = (float)sV;
    }
    s_perceptualLutsBuilt = true;
}

static inline double GammaWeight(double L) {
    int idx = (int)(L * 255.0 + 0.5);
    if (idx < 0) idx = 0; else if (idx > 255) idx = 255;
    return (double)s_gammaDeriv2[idx];
}
static inline double LinearToSrgbF(double L) {
    int idx = (int)(L * 255.0 + 0.5);
    if (idx < 0) idx = 0; else if (idx > 255) idx = 255;
    return (double)s_linearToSrgbF[idx];
}

namespace custom_bc {

// Per-block scratchpad. Lives on the stack so the encoder is reentrant.
struct BlockCtx {
    float  pix[16][4];          // linear RGBA (alpha is 1.0 when allowAlpha=false)
    float  srgb[16][3];          // sRGB normalized to [0,1]
    float  weights[16][3];       // (dS/dL)² per channel per pixel
    int    bestC0, bestC1;
    double bestErr;
    bool   bestIdx3;
    bool   haveBest;
    float  bestPal[4][4];
    float  bestPalSrgb[4][3];
    float  altPal[4][4];
    float  altPalSrgb[4][3];
    bool   evalIdx3;
    double refE0r, refE0g, refE0b;
    double refE1r, refE1g, refE1b;
    int    seenC0[16], seenC1[16];
    bool   seenT[16];
    int    seenN;
};

static inline double Clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }

static inline int Rgb565QuantizeF(double r, double g, double b) {
    int r5 = (int)std::round(Clamp01(r) * 31.0);
    int g6 = (int)std::round(Clamp01(g) * 63.0);
    int b5 = (int)std::round(Clamp01(b) * 31.0);
    return ((r5 << 11) | (g6 << 5) | b5) & 0xFFFF;
}

// Builds the 4-entry palette implied by an RGB565 endpoint pair. The DDS
// convention is: c0 > c1 → 4-color mode (entry 3 is the second blend); c0 ≤
// c1 → 3-color mode (entry 3 is fully transparent, used for alpha cutout).
// Returns true for 3-color mode.
static bool BuildBc1Palette(int c0, int c1, float pal[4][4]) {
    int r0 = ((c0 >> 11) & 0x1F) * 255 / 31;
    int g0 = ((c0 >>  5) & 0x3F) * 255 / 63;
    int b0 =  (c0        & 0x1F) * 255 / 31;
    int r1 = ((c1 >> 11) & 0x1F) * 255 / 31;
    int g1 = ((c1 >>  5) & 0x3F) * 255 / 63;
    int b1 =  (c1        & 0x1F) * 255 / 31;
    pal[0][0] = r0 / 255.f; pal[0][1] = g0 / 255.f; pal[0][2] = b0 / 255.f; pal[0][3] = 1.f;
    pal[1][0] = r1 / 255.f; pal[1][1] = g1 / 255.f; pal[1][2] = b1 / 255.f; pal[1][3] = 1.f;
    if (c0 > c1) {
        int p2r = (2*r0 + r1) / 3, p2g = (2*g0 + g1) / 3, p2b = (2*b0 + b1) / 3;
        int p3r = (r0 + 2*r1) / 3, p3g = (g0 + 2*g1) / 3, p3b = (b0 + 2*b1) / 3;
        pal[2][0] = p2r / 255.f; pal[2][1] = p2g / 255.f; pal[2][2] = p2b / 255.f; pal[2][3] = 1.f;
        pal[3][0] = p3r / 255.f; pal[3][1] = p3g / 255.f; pal[3][2] = p3b / 255.f; pal[3][3] = 1.f;
        return false;
    }
    int p2r = (r0 + r1) >> 1, p2g = (g0 + g1) >> 1, p2b = (b0 + b1) >> 1;
    pal[2][0] = p2r / 255.f; pal[2][1] = p2g / 255.f; pal[2][2] = p2b / 255.f; pal[2][3] = 1.f;
    pal[3][0] = 0.f; pal[3][1] = 0.f; pal[3][2] = 0.f; pal[3][3] = 0.f;
    return true;
}

static void BuildBc1PaletteSrgb(const float pal[4][4], float palSrgb[4][3]) {
    for (int k = 0; k < 4; k++) {
        palSrgb[k][0] = (float)LinearToSrgbF((double)pal[k][0]);
        palSrgb[k][1] = (float)LinearToSrgbF((double)pal[k][1]);
        palSrgb[k][2] = (float)LinearToSrgbF((double)pal[k][2]);
    }
}

// Sum of squared sRGB distances from each opaque pixel to its nearest palette
// entry. In 3-color mode, transparent pixels are skipped (cost zero).
static double EvaluateBc1Mode(BlockCtx& ctx, int c0, int c1, float pal[4][4], float palSrgb[4][3]) {
    bool idx3 = BuildBc1Palette(c0, c1, pal);
    BuildBc1PaletteSrgb(pal, palSrgb);
    double err = 0.0;
    int paletteEntries = idx3 ? 3 : 4;
    for (int i = 0; i < 16; i++) {
        if (idx3 && ctx.pix[i][3] < 0.5f) continue;
        double sr = (double)ctx.srgb[i][0];
        double sg = (double)ctx.srgb[i][1];
        double sb = (double)ctx.srgb[i][2];
        double best = 1e300;
        for (int k = 0; k < paletteEntries; k++) {
            double dr = sr - (double)palSrgb[k][0];
            double dg = sg - (double)palSrgb[k][1];
            double db = sb - (double)palSrgb[k][2];
            double d = dr*dr + dg*dg + db*db;
            if (d < best) best = d;
        }
        err += best;
    }
    ctx.evalIdx3 = idx3;
    return err;
}

// Weighted least-squares refit: for each linear-space channel, solve the
// 2-by-2 system Σwᵢ·[w0ᵢ²  w0ᵢw1ᵢ; w0ᵢw1ᵢ  w1ᵢ²]·[e0;e1] = Σwᵢ·[w0ᵢ;w1ᵢ]·pᵢ
// where (w0ᵢ, w1ᵢ) are the blend weights implied by pixel i's currently
// assigned palette entry (1,0)/(2/3,1/3)/(1/3,2/3)/(0,1) in 4-color mode or
// (1,0)/(0,1)/(1/2,1/2) in 3-color mode. wᵢ comes from the perceptual weight
// LUT so the linear-space system approximates an sRGB-space fit.
// Refit fails if any per-channel determinant is singular.
static bool LsRefit(BlockCtx& ctx, const float palSrgb[4][3], bool idx3IsTransparent) {
    double s00r=0,s01r=0,s11r=0,s0r=0,s1r=0;
    double s00g=0,s01g=0,s11g=0,s0g=0,s1g=0;
    double s00b=0,s01b=0,s11b=0,s0b=0,s1b=0;
    for (int i = 0; i < 16; i++) {
        if (ctx.pix[i][3] < 0.5f && idx3IsTransparent) continue;
        double sr = (double)ctx.srgb[i][0];
        double sg = (double)ctx.srgb[i][1];
        double sb = (double)ctx.srgb[i][2];
        // Find nearest palette entry in sRGB
        double d0 = (sr - palSrgb[0][0])*(sr - palSrgb[0][0])
                  + (sg - palSrgb[0][1])*(sg - palSrgb[0][1])
                  + (sb - palSrgb[0][2])*(sb - palSrgb[0][2]);
        double d1 = (sr - palSrgb[1][0])*(sr - palSrgb[1][0])
                  + (sg - palSrgb[1][1])*(sg - palSrgb[1][1])
                  + (sb - palSrgb[1][2])*(sb - palSrgb[1][2]);
        double d2 = (sr - palSrgb[2][0])*(sr - palSrgb[2][0])
                  + (sg - palSrgb[2][1])*(sg - palSrgb[2][1])
                  + (sb - palSrgb[2][2])*(sb - palSrgb[2][2]);
        int bestK = 0; double bestD = d0;
        if (d1 < bestD) { bestD = d1; bestK = 1; }
        if (d2 < bestD) { bestD = d2; bestK = 2; }
        if (!idx3IsTransparent) {
            double d3 = (sr - palSrgb[3][0])*(sr - palSrgb[3][0])
                      + (sg - palSrgb[3][1])*(sg - palSrgb[3][1])
                      + (sb - palSrgb[3][2])*(sb - palSrgb[3][2]);
            if (d3 < bestD) { bestK = 3; }
        }
        double w0, w1;
        if (idx3IsTransparent) {
            if      (bestK == 0) { w0 = 1.0; w1 = 0.0; }
            else if (bestK == 1) { w0 = 0.0; w1 = 1.0; }
            else                 { w0 = 0.5; w1 = 0.5; }   // midpoint
        } else {
            if      (bestK == 0) { w0 = 1.0;     w1 = 0.0;     }
            else if (bestK == 1) { w0 = 0.0;     w1 = 1.0;     }
            else if (bestK == 2) { w0 = 2.0/3.0; w1 = 1.0/3.0; }
            else                 { w0 = 1.0/3.0; w1 = 2.0/3.0; }
        }
        double w00 = w0*w0, w01 = w0*w1, w11 = w1*w1;
        double r = (double)ctx.pix[i][0];
        double g = (double)ctx.pix[i][1];
        double b = (double)ctx.pix[i][2];
        double wr = (double)ctx.weights[i][0];
        double wg = (double)ctx.weights[i][1];
        double wb = (double)ctx.weights[i][2];
        s00r += wr*w00; s01r += wr*w01; s11r += wr*w11; s0r += wr*w0*r; s1r += wr*w1*r;
        s00g += wg*w00; s01g += wg*w01; s11g += wg*w11; s0g += wg*w0*g; s1g += wg*w1*g;
        s00b += wb*w00; s01b += wb*w01; s11b += wb*w11; s0b += wb*w0*b; s1b += wb*w1*b;
    }
    double detR = s00r*s11r - s01r*s01r;
    double detG = s00g*s11g - s01g*s01g;
    double detB = s00b*s11b - s01b*s01b;
    if (detR <= 1e-9 || detG <= 1e-9 || detB <= 1e-9) return false;
    ctx.refE0r = Clamp01((s11r*s0r - s01r*s1r) / detR);
    ctx.refE0g = Clamp01((s11g*s0g - s01g*s1g) / detG);
    ctx.refE0b = Clamp01((s11b*s0b - s01b*s1b) / detB);
    ctx.refE1r = Clamp01((s00r*s1r - s01r*s0r) / detR);
    ctx.refE1g = Clamp01((s00g*s1g - s01g*s0g) / detG);
    ctx.refE1b = Clamp01((s00b*s1b - s01b*s0b) / detB);
    return true;
}

static uint32_t QuantizeAndOrder(double e0r, double e0g, double e0b,
                                  double e1r, double e1g, double e1b, bool threeColor) {
    int c0 = Rgb565QuantizeF(e0r, e0g, e0b);
    int c1 = Rgb565QuantizeF(e1r, e1g, e1b);
    if (threeColor) {
        if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }
    } else {
        if (c0 < c1) { int t = c0; c0 = c1; c1 = t; }
        if (c0 == c1) { if (c0 < 0xFFFF) c0++; else c1--; }
    }
    return ((uint32_t)(c0 & 0xFFFF)) | ((uint32_t)(c1 & 0xFFFF) << 16);
}

static int PerturbRgb565(int c, int channel, int delta) {
    int r = (c >> 11) & 0x1F;
    int g = (c >>  5) & 0x3F;
    int b =  c        & 0x1F;
    if      (channel == 0) { int nr = r + delta; if (nr < 0 || nr > 31) return -1; r = nr; }
    else if (channel == 1) { int ng = g + delta; if (ng < 0 || ng > 63) return -1; g = ng; }
    else                   { int nb = b + delta; if (nb < 0 || nb > 31) return -1; b = nb; }
    return ((r << 11) | (g << 5) | b) & 0xFFFF;
}

// Consider one endpoint candidate: dedupe against previously-seen (c0,c1,mode)
// triples, score, refine via LS up to 4 times, and update bestC0/C1/Err if the
// final refined result improves on it.
static void Consider(BlockCtx& ctx, double e0r, double e0g, double e0b,
                                     double e1r, double e1g, double e1b,
                                     bool threeColor) {
    double r0 = Clamp01(e0r), g0 = Clamp01(e0g), b0 = Clamp01(e0b);
    double r1 = Clamp01(e1r), g1 = Clamp01(e1g), b1 = Clamp01(e1b);
    uint32_t pk = QuantizeAndOrder(r0, g0, b0, r1, g1, b1, threeColor);
    int c0 = (int)(pk & 0xFFFF);
    int c1 = (int)((pk >> 16) & 0xFFFF);
    for (int s = 0; s < ctx.seenN; s++) {
        if (ctx.seenC0[s] == c0 && ctx.seenC1[s] == c1 && ctx.seenT[s] == threeColor) return;
    }
    if (ctx.seenN < 16) {
        ctx.seenC0[ctx.seenN] = c0; ctx.seenC1[ctx.seenN] = c1;
        ctx.seenT[ctx.seenN]  = threeColor;
        ctx.seenN++;
    }
    double curErr = EvaluateBc1Mode(ctx, c0, c1, ctx.altPal, ctx.altPalSrgb);
    bool curIdx3 = threeColor;
    for (int iter = 0; iter < 4; iter++) {
        if (!LsRefit(ctx, ctx.altPalSrgb, curIdx3)) break;
        uint32_t p = QuantizeAndOrder(ctx.refE0r, ctx.refE0g, ctx.refE0b,
                                       ctx.refE1r, ctx.refE1g, ctx.refE1b, threeColor);
        int nc0 = (int)(p & 0xFFFF);
        int nc1 = (int)((p >> 16) & 0xFFFF);
        if (nc0 == c0 && nc1 == c1) break;
        double newErr = EvaluateBc1Mode(ctx, nc0, nc1, ctx.altPal, ctx.altPalSrgb);
        if (newErr >= curErr) {
            // Refit made it worse: restore the palette state for the c0,c1
            // we're keeping (subsequent code may inspect altPal*Srgb*).
            EvaluateBc1Mode(ctx, c0, c1, ctx.altPal, ctx.altPalSrgb);
            break;
        }
        c0 = nc0; c1 = nc1; curErr = newErr;
        curIdx3 = ctx.evalIdx3;
    }
    if (!ctx.haveBest || curErr < ctx.bestErr) {
        ctx.bestC0 = c0; ctx.bestC1 = c1;
        ctx.bestErr = curErr;
        ctx.bestIdx3 = curIdx3;
        ctx.haveBest = true;
        memcpy(ctx.bestPal,     ctx.altPal,     sizeof(ctx.bestPal));
        memcpy(ctx.bestPalSrgb, ctx.altPalSrgb, sizeof(ctx.bestPalSrgb));
    }
}

// RGB565 endpoint perturbation: ±1 in each 5/6-bit channel of each endpoint,
// up to 4 rounds. Skips perturbations that would flip the mode (3-color vs
// 4-color) since that changes the meaning of palette entry 3.
static void RefineBc1Endpoints(BlockCtx& ctx) {
    int bestC0 = ctx.bestC0, bestC1 = ctx.bestC1;
    double bestErr = ctx.bestErr;
    bool idx3 = ctx.bestIdx3;
    for (int round = 0; round < 4; round++) {
        bool improved = false;
        for (int which = 0; which < 2; which++) {
            for (int ch = 0; ch < 3; ch++) {
                for (int d = -1; d <= 1; d += 2) {
                    int nc0 = bestC0, nc1 = bestC1;
                    if (which == 0) {
                        int p = PerturbRgb565(bestC0, ch, d);
                        if (p < 0) continue;
                        nc0 = p;
                    } else {
                        int p = PerturbRgb565(bestC1, ch, d);
                        if (p < 0) continue;
                        nc1 = p;
                    }
                    if (idx3) { if (nc0 > nc1) continue; }
                    else      { if (nc0 <= nc1) continue; }
                    double err = EvaluateBc1Mode(ctx, nc0, nc1, ctx.altPal, ctx.altPalSrgb);
                    if (ctx.evalIdx3 != idx3) continue;
                    if (err < bestErr) {
                        bestC0 = nc0; bestC1 = nc1; bestErr = err;
                        memcpy(ctx.bestPal,     ctx.altPal,     sizeof(ctx.bestPal));
                        memcpy(ctx.bestPalSrgb, ctx.altPalSrgb, sizeof(ctx.bestPalSrgb));
                        improved = true;
                    }
                }
            }
        }
        if (!improved) break;
    }
    ctx.bestC0 = bestC0; ctx.bestC1 = bestC1; ctx.bestErr = bestErr;
}

static const double PCA_K[3] = { 1.0, 1.5, 2.0 };

// Top-level endpoint search. Builds bounding-box and PCA candidates, scores
// and refines each, then runs RGB565 perturbation on the best.
static void ChooseBc1Endpoints(BlockCtx& ctx, bool allowAlpha, Bc1Mode bc1Mode) {
    int n = 0;
    double mr=0, mg=0, mb=0;
    double minR=1e300, minG=1e300, minB=1e300;
    double maxR=-1e300, maxG=-1e300, maxB=-1e300;
    bool hasAlpha = false;
    for (int i = 0; i < 16; i++) {
        if (ctx.pix[i][3] < 0.5f) { hasAlpha = true; continue; }
        n++;
        double r = ctx.pix[i][0], g = ctx.pix[i][1], b = ctx.pix[i][2];
        mr += r; mg += g; mb += b;
        if (r < minR) minR = r;
        if (g < minG) minG = g;
        if (b < minB) minB = b;
        if (r > maxR) maxR = r;
        if (g > maxG) maxG = g;
        if (b > maxB) maxB = b;
    }
    if (n == 0) {
        // Block is fully transparent. Emit a 3-color block with both endpoints
        // chosen so all 16 indices end up = 3 (transparent black).
        ctx.bestC0 = 0; ctx.bestC1 = 0xFFFF;
        ctx.bestIdx3 = true; ctx.bestErr = 0.0;
        ctx.haveBest = true;
        BuildBc1Palette(0, 0xFFFF, ctx.bestPal);
        BuildBc1PaletteSrgb(ctx.bestPal, ctx.bestPalSrgb);
        return;
    }
    mr /= n; mg /= n; mb /= n;

    double cxx=0, cxy=0, cxz=0, cyy=0, cyz=0, czz=0;
    for (int i = 0; i < 16; i++) {
        if (ctx.pix[i][3] < 0.5f) continue;
        double dr = ctx.pix[i][0] - mr;
        double dg = ctx.pix[i][1] - mg;
        double db = ctx.pix[i][2] - mb;
        cxx += dr*dr; cxy += dr*dg; cxz += dr*db;
        cyy += dg*dg; cyz += dg*db; czz += db*db;
    }
    // Power iteration with infinity-norm. The L∞ rescale (rather than L2) lets
    // the axis blow up component-wise during convergence and is what upstream
    // uses; the final result is L2-normalised at the end.
    double ax=1, ay=1, az=1;
    for (int it = 0; it < 8; it++) {
        double nx = cxx*ax + cxy*ay + cxz*az;
        double ny = cxy*ax + cyy*ay + cyz*az;
        double nz = cxz*ax + cyz*ay + czz*az;
        double mx = std::fabs(nx), my = std::fabs(ny), mz = std::fabs(nz);
        double m = mx; if (my > m) m = my; if (mz > m) m = mz;
        if (m == 0.0) { ax = 1; ay = 1; az = 1; break; }
        ax = nx / m; ay = ny / m; az = nz / m;
    }
    double norm = std::sqrt(ax*ax + ay*ay + az*az);
    if (norm > 0.0) { ax /= norm; ay /= norm; az /= norm; }

    double minP=1e300, maxP=-1e300;
    for (int i = 0; i < 16; i++) {
        if (ctx.pix[i][3] < 0.5f) continue;
        double dr = ctx.pix[i][0] - mr;
        double dg = ctx.pix[i][1] - mg;
        double db = ctx.pix[i][2] - mb;
        double p = dr*ax + dg*ay + db*az;
        if (p < minP) minP = p;
        if (p > maxP) maxP = p;
    }

    bool requireThreeColor = allowAlpha && hasAlpha;
    bool tryFour  = !requireThreeColor && bc1Mode != Bc1Mode::ThreeColor;
    bool tryThree =  bc1Mode != Bc1Mode::FourColor || requireThreeColor;

    ctx.bestC0 = 0; ctx.bestC1 = 0;
    ctx.bestErr = 1e300; ctx.bestIdx3 = false;
    ctx.haveBest = false; ctx.seenN = 0;

    if (tryFour) {
        Consider(ctx, maxR, maxG, maxB, minR, minG, minB, false);
        for (int ki = 0; ki < 3; ki++) {
            double k = PCA_K[ki];
            Consider(ctx,
                mr + maxP*ax*k, mg + maxP*ay*k, mb + maxP*az*k,
                mr + minP*ax*k, mg + minP*ay*k, mb + minP*az*k, false);
        }
    }
    if (tryThree) {
        for (int ki = 0; ki < 3; ki++) {
            double k = PCA_K[ki];
            Consider(ctx,
                mr + maxP*ax*k, mg + maxP*ay*k, mb + maxP*az*k,
                mr + minP*ax*k, mg + minP*ay*k, mb + minP*az*k, true);
        }
        Consider(ctx, maxR, maxG, maxB, minR, minG, minB, true);
    }

    if (!ctx.haveBest) {
        // Every candidate above got deduped (rare degenerate case). Fall back
        // to the plain PCA endpoint pair in 4-color mode.
        Consider(ctx,
            mr + maxP*ax, mg + maxP*ay, mb + maxP*az,
            mr + minP*ax, mg + minP*ay, mb + minP*az, false);
    }

    RefineBc1Endpoints(ctx);
}

static int AssignIndex(const BlockCtx& ctx, double sr, double sg, double sb, double a, bool idx3IsTransparent) {
    if (a < 0.5 && idx3IsTransparent) return 3;
    double d0 = (sr - ctx.bestPalSrgb[0][0])*(sr - ctx.bestPalSrgb[0][0])
              + (sg - ctx.bestPalSrgb[0][1])*(sg - ctx.bestPalSrgb[0][1])
              + (sb - ctx.bestPalSrgb[0][2])*(sb - ctx.bestPalSrgb[0][2]);
    double d1 = (sr - ctx.bestPalSrgb[1][0])*(sr - ctx.bestPalSrgb[1][0])
              + (sg - ctx.bestPalSrgb[1][1])*(sg - ctx.bestPalSrgb[1][1])
              + (sb - ctx.bestPalSrgb[1][2])*(sb - ctx.bestPalSrgb[1][2]);
    double d2 = (sr - ctx.bestPalSrgb[2][0])*(sr - ctx.bestPalSrgb[2][0])
              + (sg - ctx.bestPalSrgb[2][1])*(sg - ctx.bestPalSrgb[2][1])
              + (sb - ctx.bestPalSrgb[2][2])*(sb - ctx.bestPalSrgb[2][2]);
    int best = 0; double bestD = d0;
    if (d1 < bestD) { bestD = d1; best = 1; }
    if (d2 < bestD) { bestD = d2; best = 2; }
    if (!idx3IsTransparent) {
        double d3 = (sr - ctx.bestPalSrgb[3][0])*(sr - ctx.bestPalSrgb[3][0])
                  + (sg - ctx.bestPalSrgb[3][1])*(sg - ctx.bestPalSrgb[3][1])
                  + (sb - ctx.bestPalSrgb[3][2])*(sb - ctx.bestPalSrgb[3][2]);
        if (d3 < bestD) { best = 3; }
    }
    return best;
}

// Encode a single 4×4 color block. Used by both BC1 (bytesPerBlock=8, colorOff=0)
// and BC3 (bytesPerBlock=16, colorOff=8 — color block follows the alpha block).
static void EncodeColorBlock(const uint8_t srgbBlk[64], Bc1Mode mode, bool allowAlpha,
                              uint8_t* out, int off) {
    BlockCtx ctx;
    for (int i = 0; i < 16; i++) {
        // Convert sRGB-u8 to linear via the float-precision LUT for highest
        // accuracy in the endpoint math (the upstream encoder also stores the
        // linear pixels in f32).
        double sr = (double)srgbBlk[i*4 + 0] / 255.0;
        double sg = (double)srgbBlk[i*4 + 1] / 255.0;
        double sb = (double)srgbBlk[i*4 + 2] / 255.0;
        double lr = (sr <= 0.04045) ? sr / 12.92 : std::pow((sr + 0.055) / 1.055, 2.4);
        double lg = (sg <= 0.04045) ? sg / 12.92 : std::pow((sg + 0.055) / 1.055, 2.4);
        double lb = (sb <= 0.04045) ? sb / 12.92 : std::pow((sb + 0.055) / 1.055, 2.4);
        double a  = (double)srgbBlk[i*4 + 3] / 255.0;
        ctx.pix[i][0]  = (float)lr;
        ctx.pix[i][1]  = (float)lg;
        ctx.pix[i][2]  = (float)lb;
        ctx.pix[i][3]  = allowAlpha ? (float)a : 1.0f;
        ctx.srgb[i][0] = (float)sr;
        ctx.srgb[i][1] = (float)sg;
        ctx.srgb[i][2] = (float)sb;
        ctx.weights[i][0] = (float)GammaWeight(lr);
        ctx.weights[i][1] = (float)GammaWeight(lg);
        ctx.weights[i][2] = (float)GammaWeight(lb);
    }
    ChooseBc1Endpoints(ctx, allowAlpha, mode);
    uint32_t indices = 0;
    for (int i = 0; i < 16; i++) {
        int k = AssignIndex(ctx,
            (double)ctx.srgb[i][0], (double)ctx.srgb[i][1], (double)ctx.srgb[i][2],
            (double)ctx.pix[i][3], ctx.bestIdx3);
        indices |= ((uint32_t)k) << (2 * i);
    }
    uint16_t c0w = (uint16_t)ctx.bestC0;
    uint16_t c1w = (uint16_t)ctx.bestC1;
    memcpy(out + off,     &c0w,     2);
    memcpy(out + off + 2, &c1w,     2);
    memcpy(out + off + 4, &indices, 4);
}

// Encode the BC3 alpha sub-block. Picks between the 8-value alpha palette
// (using the block's min/max as endpoints) and the 6-value palette (using
// the inner-bounds — i.e. the min/max of strictly-between-0-and-1 pixels —
// as endpoints, with 0 and 255 added as fixed entries 6 and 7) and keeps
// whichever scores lower MSE. This is the same heuristic upstream uses.
static void EncodeBc3AlphaBlock(const uint8_t srgbBlk[64], uint8_t* out, int off) {
    double a[16];
    double minA = 1.0, maxA = 0.0;
    double innerMin = 1.0, innerMax = 0.0;
    bool hasInner = false;
    for (int i = 0; i < 16; i++) {
        double v = (double)srgbBlk[i*4 + 3] / 255.0;
        a[i] = v;
        if (v < minA) minA = v;
        if (v > maxA) maxA = v;
        if (v > 0.0 && v < 1.0) {
            if (v < innerMin) innerMin = v;
            if (v > innerMax) innerMax = v;
            hasInner = true;
        }
    }
    int a0_8 = (int)std::round(maxA * 255.0);
    int a1_8 = (int)std::round(minA * 255.0);
    double pal8[8];
    pal8[0] = a0_8 / 255.0; pal8[1] = a1_8 / 255.0;
    if (a0_8 > a1_8) {
        pal8[2] = (double)(6*a0_8 + 1*a1_8) / 7.0 / 255.0;
        pal8[3] = (double)(5*a0_8 + 2*a1_8) / 7.0 / 255.0;
        pal8[4] = (double)(4*a0_8 + 3*a1_8) / 7.0 / 255.0;
        pal8[5] = (double)(3*a0_8 + 4*a1_8) / 7.0 / 255.0;
        pal8[6] = (double)(2*a0_8 + 5*a1_8) / 7.0 / 255.0;
        pal8[7] = (double)(1*a0_8 + 6*a1_8) / 7.0 / 255.0;
    } else {
        for (int k = 2; k < 8; k++) pal8[k] = pal8[0];
    }

    double sixSrcMin = hasInner ? innerMin : minA;
    double sixSrcMax = hasInner ? innerMax : maxA;
    int a0_6 = (int)std::round(sixSrcMin * 255.0);
    int a1_6 = (int)std::round(sixSrcMax * 255.0);
    bool useSix = a0_6 < a1_6;
    double pal6[8] = {0,0,0,0,0,0,0,0};
    if (useSix) {
        pal6[0] = a0_6 / 255.0; pal6[1] = a1_6 / 255.0;
        pal6[2] = (double)(4*a0_6 + 1*a1_6) / 5.0 / 255.0;
        pal6[3] = (double)(3*a0_6 + 2*a1_6) / 5.0 / 255.0;
        pal6[4] = (double)(2*a0_6 + 3*a1_6) / 5.0 / 255.0;
        pal6[5] = (double)(1*a0_6 + 4*a1_6) / 5.0 / 255.0;
        pal6[6] = 0.0; pal6[7] = 1.0;
    }
    double err8 = 0.0, err6 = 0.0;
    uint8_t idx8[16], idx6[16] = {0};
    for (int i = 0; i < 16; i++) {
        double v = a[i];
        int b8 = 0; double dv = v - pal8[0]; double d8 = dv*dv;
        for (int k = 1; k < 8; k++) {
            double dk = v - pal8[k]; double d = dk*dk;
            if (d < d8) { d8 = d; b8 = k; }
        }
        idx8[i] = (uint8_t)b8;
        err8 += d8;
        if (useSix) {
            int b6 = 0; double dv6 = v - pal6[0]; double d6 = dv6*dv6;
            for (int k = 1; k < 8; k++) {
                double dk = v - pal6[k]; double d = dk*dk;
                if (d < d6) { d6 = d; b6 = k; }
            }
            idx6[i] = (uint8_t)b6;
            err6 += d6;
        }
    }
    bool sixWins = useSix && err6 < err8;
    int a0Out = sixWins ? a0_6 : a0_8;
    int a1Out = sixWins ? a1_6 : a1_8;
    const uint8_t* idx = sixWins ? idx6 : idx8;
    uint64_t aib = 0;
    for (int i = 0; i < 16; i++) aib |= ((uint64_t)idx[i]) << (3 * i);
    out[off + 0] = (uint8_t)a0Out;
    out[off + 1] = (uint8_t)a1Out;
    for (int i = 0; i < 6; i++) out[off + 2 + i] = (uint8_t)((aib >> (8 * i)) & 0xFF);
}

} // namespace custom_bc

// PCA endpoint fit: 8 power-iteration steps to find the principal color axis,
// then project pixels onto it for min/max endpoints. Better than bounding-box
// on gradients, but with no candidate scoring and no iterative refinement —
// this is *not* a port of rgbcx, just the PCA step it builds on.
static void Bc1EncodeBlockPCA(const uint8_t blk[64], Bc1Mode mode,
                               uint8_t* out, int off) {
    int oc=0; bool hasAlpha=false;
    for(int i=0;i<16;i++){if(blk[i*4+3]<128){hasAlpha=true;}else{oc++;}}
    if(oc==0){ memset(out+off,0,4); memset(out+off+4,0xFF,4); return; }

    bool use3 = (mode==Bc1Mode::ThreeColor) || (mode==Bc1Mode::Auto && hasAlpha);
    if(mode==Bc1Mode::FourColor) use3=false;

    // Compute mean of opaque pixels
    float mr=0,mg=0,mb=0;
    for(int i=0;i<16;i++){
        if(blk[i*4+3]<128) continue;
        mr+=blk[i*4]; mg+=blk[i*4+1]; mb+=blk[i*4+2];
    }
    mr/=oc; mg/=oc; mb/=oc;

    // Compute 3x3 covariance matrix
    float cov[3][3]={};
    for(int i=0;i<16;i++){
        if(blk[i*4+3]<128) continue;
        float dr=blk[i*4]-mr, dg=blk[i*4+1]-mg, db=blk[i*4+2]-mb;
        cov[0][0]+=dr*dr; cov[0][1]+=dr*dg; cov[0][2]+=dr*db;
        cov[1][1]+=dg*dg; cov[1][2]+=dg*db;
        cov[2][2]+=db*db;
    }
    cov[1][0]=cov[0][1]; cov[2][0]=cov[0][2]; cov[2][1]=cov[1][2];

    // Power iteration to find principal axis (8 steps)
    float ax=0.577f,ay=0.577f,az=0.577f;
    for(int it=0;it<8;it++){
        float nx=cov[0][0]*ax+cov[0][1]*ay+cov[0][2]*az;
        float ny=cov[1][0]*ax+cov[1][1]*ay+cov[1][2]*az;
        float nz=cov[2][0]*ax+cov[2][1]*ay+cov[2][2]*az;
        float len=sqrtf(nx*nx+ny*ny+nz*nz);
        if(len<1e-6f) break;
        ax=nx/len; ay=ny/len; az=nz/len;
    }

    // Project pixels onto axis; find min/max
    float minT=1e30f,maxT=-1e30f;
    for(int i=0;i<16;i++){
        if(blk[i*4+3]<128) continue;
        float t=(blk[i*4]-mr)*ax+(blk[i*4+1]-mg)*ay+(blk[i*4+2]-mb)*az;
        if(t<minT) minT=t;
        if(t>maxT) maxT=t;
    }

    auto clamp8=[](float v)->uint8_t{return(uint8_t)std::max(0.f,std::min(255.f,v));};
    uint16_t c0,c1;
    if(maxT==minT){
        // Solid color block — both endpoints identical, assign directly
        c0=c1=Rgb565Encode(clamp8(mr),clamp8(mg),clamp8(mb));
    } else {
        c0=Rgb565Encode(clamp8(mr+maxT*ax),clamp8(mg+maxT*ay),clamp8(mb+maxT*az));
        c1=Rgb565Encode(clamp8(mr+minT*ax),clamp8(mg+minT*ay),clamp8(mb+minT*az));
    }
    EnforceMode(c0,c1,use3);
    AssignBc1Indices(blk,c0,c1,use3,out,off);
}

// Bc1Encode now takes sRGB-u8 RGBA, not linear-u8. The Custom encoder needs
// the original sRGB values for its error metric, so the caller must not
// pre-linearize the image (see ImportRgbaImage).
std::vector<uint8_t> Bc1Encode(const std::vector<uint8_t>& srgbRgba, int W, int H,
                                Bc1Encoder enc, Bc1Mode mode) {
    BuildLuts();
    BuildPerceptualLuts();
    int bx=W/4, by=H/4;
    std::vector<uint8_t> out((size_t)bx*by*8);
    uint8_t srgbBlk[64];
    uint8_t linBlk[64];
    bool wantLinear = (enc == Bc1Encoder::PCA);
    for (int iy = 0; iy < by; iy++) for (int ix = 0; ix < bx; ix++) {
        for (int row = 0; row < 4; row++) for (int col = 0; col < 4; col++) {
            int src = ((iy*4 + row) * W + (ix*4 + col)) * 4;
            int dst = (row * 4 + col) * 4;
            memcpy(srgbBlk + dst, srgbRgba.data() + src, 4);
            if (wantLinear) {
                // Cheap path: u8 sRGB → u8 linear via the precomputed LUT.
                // Only the PCA fallback needs this; Custom does its own
                // higher-precision conversion inside EncodeColorBlock.
                linBlk[dst + 0] = s_srgbToLinear[srgbBlk[dst + 0]];
                linBlk[dst + 1] = s_srgbToLinear[srgbBlk[dst + 1]];
                linBlk[dst + 2] = s_srgbToLinear[srgbBlk[dst + 2]];
                linBlk[dst + 3] = srgbBlk[dst + 3];
            }
        }
        int off = (iy * bx + ix) * 8;
        if (enc == Bc1Encoder::PCA)
            Bc1EncodeBlockPCA(linBlk, mode, out.data(), off);
        else
            custom_bc::EncodeColorBlock(srgbBlk, mode, /*allowAlpha=*/true, out.data(), off);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// BC3
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> Bc3Decode(const std::vector<uint8_t>& bd, int W, int H) {
    int bx=W/4,by=H/4;
    std::vector<uint8_t> out((size_t)W*H*4);
    uint8_t alphas[8];
    for(int iy=0;iy<by;iy++) for(int ix=0;ix<bx;ix++){
        const uint8_t* b=bd.data()+(iy*bx+ix)*16;
        uint8_t a0=b[0],a1=b[1];
        uint64_t aib=0; for(int i=0;i<6;i++) aib|=(uint64_t)b[2+i]<<(8*i);
        alphas[0]=a0;alphas[1]=a1;
        if(a0>a1){
            alphas[2]=(uint8_t)((6*a0+a1)/7);alphas[3]=(uint8_t)((5*a0+2*a1)/7);
            alphas[4]=(uint8_t)((4*a0+3*a1)/7);alphas[5]=(uint8_t)((3*a0+4*a1)/7);
            alphas[6]=(uint8_t)((2*a0+5*a1)/7);alphas[7]=(uint8_t)((a0+6*a1)/7);
        } else {
            alphas[2]=(uint8_t)((4*a0+a1)/5);alphas[3]=(uint8_t)((3*a0+2*a1)/5);
            alphas[4]=(uint8_t)((2*a0+3*a1)/5);alphas[5]=(uint8_t)((a0+4*a1)/5);
            alphas[6]=0;alphas[7]=255;
        }
        uint16_t c0r,c1r; memcpy(&c0r,b+8,2); memcpy(&c1r,b+10,2);
        uint32_t ci; memcpy(&ci,b+12,4);
        auto [r0,g0,b0]=Rgb565Decode(c0r); auto [r1,g1,b1]=Rgb565Decode(c1r);
        uint8_t pr[4]={r0,r1,(uint8_t)((2*r0+r1)/3),(uint8_t)((r0+2*r1)/3)};
        uint8_t pg[4]={g0,g1,(uint8_t)((2*g0+g1)/3),(uint8_t)((g0+2*g1)/3)};
        uint8_t pb[4]={b0,b1,(uint8_t)((2*b0+b1)/3),(uint8_t)((b0+2*b1)/3)};
        for(int row=0;row<4;row++) for(int col=0;col<4;col++){
            int pi=row*4+col;
            int dst=((iy*4+row)*W+(ix*4+col))*4;
            out[dst]=pr[(ci>>(2*pi))&3];out[dst+1]=pg[(ci>>(2*pi))&3];
            out[dst+2]=pb[(ci>>(2*pi))&3];out[dst+3]=alphas[(aib>>(3*pi))&7];
        }
    }
    return out;
}

// Bc3Encode now takes sRGB-u8 RGBA. The color block goes through the Custom
// encoder (4-color mode, no alpha cutout since BC3 carries alpha separately).
// The alpha block uses the 8-value vs 6-value selection from bc_encode.ts.
std::vector<uint8_t> Bc3Encode(const std::vector<uint8_t>& srgbRgba, int W, int H) {
    BuildLuts();
    BuildPerceptualLuts();
    int bx = W/4, by = H/4;
    std::vector<uint8_t> out((size_t)bx*by*16);
    uint8_t srgbBlk[64];
    for (int iy = 0; iy < by; iy++) for (int ix = 0; ix < bx; ix++) {
        for (int row = 0; row < 4; row++) for (int col = 0; col < 4; col++) {
            memcpy(srgbBlk + (row*4 + col)*4,
                   srgbRgba.data() + ((iy*4 + row)*W + (ix*4 + col))*4, 4);
        }
        int off = (iy*bx + ix) * 16;
        custom_bc::EncodeBc3AlphaBlock(srgbBlk, out.data(), off);
        custom_bc::EncodeColorBlock(srgbBlk, Bc1Mode::FourColor, /*allowAlpha=*/false,
                                     out.data(), off + 8);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Decode pipeline  (no exceptions)
// ─────────────────────────────────────────────────────────────────────────────

std::string DecodeCanvas(const std::vector<uint8_t>& raw, RgbaImage& out, bool noSrgb) {
    int totalPx = (int)(raw.size() / 4);
    int side = (int)std::sqrt((double)totalPx);
    int w = (side*side == totalPx) ? side : 256;
    int h = totalPx / w;
    if ((size_t)(w*h*4) != raw.size())
        return "Canvas: unexpected size " + std::to_string(raw.size());

    auto pixels = DeswizzleBlockLinear(raw, w, h, 4, DefaultBlockHeight);
    if (!noSrgb) ConvertLinearToSrgb(pixels);

    out.width  = w;
    out.height = h;
    out.pixels = std::move(pixels);
    return "";
}

std::string DecodeUgctex(const std::vector<uint8_t>& raw, RgbaImage& out, bool noSrgb) {
    UgctexLayout layout{};
    std::string err;
    if (!DetectUgctexLayoutInner(raw.size(), layout, err)) return err;

    int vbw = layout.Width  / 4;
    int vbh = layout.Height / 4;
    auto blocks = DeswizzleBlockLinear(raw, vbw, vbh, layout.BytesPerBlock(), layout.BlockHeight);

    std::vector<uint8_t> rgba = (layout.Format == TextureFormat::Bc3)
        ? Bc3Decode(blocks, layout.Width, layout.Height)
        : Bc1Decode(blocks, layout.Width, layout.Height);

    if (!noSrgb) ConvertLinearToSrgb(rgba);

    out.width  = layout.Width;
    out.height = layout.Height;
    out.pixels = std::move(rgba);
    return "";
}

std::string DecodeThumb(const std::vector<uint8_t>& raw, RgbaImage& out, bool noSrgb) {
    int totalBlocks = (int)(raw.size() / 16);
    int gs = (int)std::sqrt((double)totalBlocks);
    if (gs*gs != totalBlocks)
        return "Thumb: not square BC3 (" + std::to_string(raw.size()) + " bytes)";

    int tw = gs*4, th = gs*4;
    auto blocks = DeswizzleBlockLinear(raw, gs, gs, 16, ThumbBlockHeight);
    auto rgba   = Bc3Decode(blocks, tw, th);
    if (!noSrgb) ConvertLinearToSrgb(rgba);

    out.width  = tw;
    out.height = th;
    out.pixels = std::move(rgba);
    return "";
}

std::string DecodeFile(const std::string& path, RgbaImage& out, bool noSrgb) {
    std::vector<uint8_t> raw;
    std::string err;
    if (!ZstdDecompressFile(path, raw, err)) return err;

    TextureKind kind = DetectKind(path);
    switch (kind) {
        case TextureKind::Thumb:  return DecodeThumb(raw, out, noSrgb);
        case TextureKind::Ugctex: return DecodeUgctex(raw, out, noSrgb);
        default:                  return DecodeCanvas(raw, out, noSrgb);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PNG I/O via SDL2_image  (no exceptions)
// ─────────────────────────────────────────────────────────────────────────────


// Threshold alpha to eliminate semi-transparent edge fringing.
// Pixels with alpha < threshold become fully transparent (discarded).
// Pixels with alpha >= threshold become fully opaque.
// This removes the white outline caused by background-removal tools leaving
// semi-transparent fringe pixels with light/white color values.
static void ThresholdAlpha(std::vector<uint8_t>& rgba, uint8_t threshold=128) {
    for (size_t i = 3; i < rgba.size(); i += 4)
        rgba[i] = (rgba[i] >= threshold) ? 255 : 0;
}

static RgbaImage ResizeBilinear(const RgbaImage& src, int dw, int dh) {
    RgbaImage dst(dw, dh);
    for (int dy = 0; dy < dh; dy++) {
        float fy = (dy + 0.5f) * src.height / dh - 0.5f;
        if (fy < 0) fy = 0;
        int y0 = (int)fy, y1 = y0 + 1;
        if (y1 >= src.height) y1 = src.height - 1;
        float wy = fy - y0;
        for (int dx = 0; dx < dw; dx++) {
            float fx = (dx + 0.5f) * src.width / dw - 0.5f;
            if (fx < 0) fx = 0;
            int x0 = (int)fx, x1 = x0 + 1;
            if (x1 >= src.width) x1 = src.width - 1;
            float wx = fx - x0;
            uint8_t* p = dst.row(dy) + dx * 4;
            for (int c = 0; c < 4; c++) {
                float v = src.row(y0)[x0*4+c]*(1-wx)*(1-wy)
                        + src.row(y0)[x1*4+c]*wx*(1-wy)
                        + src.row(y1)[x0*4+c]*(1-wx)*wy
                        + src.row(y1)[x1*4+c]*wx*wy;
                p[c] = (uint8_t)(v + 0.5f);
            }
        }
    }
    return dst;
}

static RgbaImage ResizeNearest(const RgbaImage& src, int dw, int dh) {
    RgbaImage dst(dw, dh); // zero-initialized = fully transparent
    float scale = std::min((float)dw / src.width, (float)dh / src.height);
    int scaledW = (int)(src.width  * scale);
    int scaledH = (int)(src.height * scale);
    int offX = (dw - scaledW) / 2;
    int offY = (dh - scaledH) / 2;
    for (int y = 0; y < scaledH; y++) {
        int sy = y * src.height / scaledH;
        for (int x = 0; x < scaledW; x++) {
            int sx = x * src.width / scaledW;
            memcpy(dst.pixels.data() + ((offY + y) * dw + (offX + x)) * 4,
                   src.pixels.data() + (sy * src.width + sx) * 4, 4);
        }
    }
    return dst;
}

// Single-stop resize that handles the three fit modes (cover / contain / fill)
// plus an optional matte color for contain's letterbox. Cover crops; contain
// letterboxes; fill stretches. All use nearest-neighbour to match the existing
// import pipeline.
static RgbaImage ResizeWithFit(const RgbaImage& src, int dw, int dh,
                                FitMode mode, const Matte& matte) {
    RgbaImage dst(dw, dh);
    // Pre-fill with matte for letterbox case; cover/fill rewrite the whole canvas.
    if (mode == FitMode::Contain && matte.a > 0) {
        for (int i = 0; i < dw * dh; i++) {
            dst.pixels[i*4+0] = matte.r;
            dst.pixels[i*4+1] = matte.g;
            dst.pixels[i*4+2] = matte.b;
            dst.pixels[i*4+3] = matte.a;
        }
    }
    if (mode == FitMode::Fill) {
        for (int y = 0; y < dh; y++) {
            int sy = y * src.height / dh;
            for (int x = 0; x < dw; x++) {
                int sx = x * src.width / dw;
                memcpy(dst.pixels.data() + (y * dw + x) * 4,
                       src.pixels.data() + (sy * src.width + sx) * 4, 4);
            }
        }
        return dst;
    }
    if (mode == FitMode::Cover) {
        // Source rect that maps to the full target: scale = max(dw/sw, dh/sh)
        // → crop the longer source axis.
        float scale = std::max((float)dw / src.width, (float)dh / src.height);
        int cropW = (int)((float)dw / scale + 0.5f);
        int cropH = (int)((float)dh / scale + 0.5f);
        if (cropW < 1) cropW = 1;
        if (cropH < 1) cropH = 1;
        int cropX = (src.width  - cropW) / 2;
        int cropY = (src.height - cropH) / 2;
        for (int y = 0; y < dh; y++) {
            int sy = cropY + y * cropH / dh;
            if (sy < 0) sy = 0; if (sy >= src.height) sy = src.height - 1;
            for (int x = 0; x < dw; x++) {
                int sx = cropX + x * cropW / dw;
                if (sx < 0) sx = 0; if (sx >= src.width) sx = src.width - 1;
                memcpy(dst.pixels.data() + (y * dw + x) * 4,
                       src.pixels.data() + (sy * src.width + sx) * 4, 4);
            }
        }
        return dst;
    }
    // Contain: same as ResizeNearest but on top of the pre-filled matte canvas.
    float scale = std::min((float)dw / src.width, (float)dh / src.height);
    int scaledW = std::max(1, (int)(src.width  * scale));
    int scaledH = std::max(1, (int)(src.height * scale));
    int offX = (dw - scaledW) / 2;
    int offY = (dh - scaledH) / 2;
    for (int y = 0; y < scaledH; y++) {
        int sy = y * src.height / scaledH;
        for (int x = 0; x < scaledW; x++) {
            int sx = x * src.width / scaledW;
            memcpy(dst.pixels.data() + ((offY + y) * dw + (offX + x)) * 4,
                   src.pixels.data() + (sy * src.width + sx) * 4, 4);
        }
    }
    return dst;
}

static bool LoadPng(const std::string& path, RgbaImage& out, std::string& errOut) {
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) { errOut = std::string("IMG_Load: ") + IMG_GetError(); return false; }

    SDL_Surface* conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surf);
    if (!conv) { errOut = std::string("SDL_ConvertSurface: ") + SDL_GetError(); return false; }

    out.width  = conv->w;
    out.height = conv->h;
    out.pixels.resize((size_t)conv->w * conv->h * 4);
    SDL_LockSurface(conv);
    for (int y=0;y<conv->h;y++)
        memcpy(out.pixels.data()+y*conv->w*4, (uint8_t*)conv->pixels+y*conv->pitch, conv->w*4);
    SDL_UnlockSurface(conv);
    SDL_FreeSurface(conv);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ImportRgbaImage / ImportPng  (no exceptions)
// ─────────────────────────────────────────────────────────────────────────────

std::string ImportRgbaImage(const RgbaImage& src, const ImportOptions& opts) {
    std::string err;

    // Detect layout from original ugctex
    UgctexLayout layout{512, 512, 128, 128, 16, TextureFormat::Bc1};
    std::vector<uint8_t> originalSwizzled;

    if (!opts.originalUgctexPath.empty()) {
        std::vector<uint8_t> raw;
        if (ZstdDecompressFile(opts.originalUgctexPath, raw, err)) {
            UgctexLayout detected{};
            std::string lerr;
            if (DetectUgctexLayoutInner(raw.size(), detected, lerr)) {
                layout = detected;
                originalSwizzled = std::move(raw);
            }
        }
    }

    // Shape → inner content aspect inside the always-square target buffer.
    // Book is ~3:4 portrait (256×256 → 200-wide inner), TV is 16:9 wide
    // (256×256 → 144-tall inner). The remaining area gets filled with
    // opaque black bars so the in-game mesh — which renders the canvas
    // 1:1 — shows the source image fitted into the right aspect with
    // pillarbox / letterbox bars around it. Earlier attempts pre-stretched
    // the source into the full square assuming the mesh would un-stretch
    // it; that was wrong (the mesh doesn't stretch), which is why imports
    // came back visibly squashed.
    auto innerSizeFor = [&](CanvasShape shape, int boxW, int boxH,
                            int& iw, int& ih) {
        if (shape == CanvasShape::Book) {
            // ~3:4 (200/256). Round to multiples of 4 so BC blocks stay
            // aligned when this size is used for the ugctex pass too.
            iw = ((boxW * 200) / 256) & ~3;
            ih = boxH;
        } else if (shape == CanvasShape::Tv) {
            // 16:9 — derived from height so the wider edge sits at the
            // canvas's full width.
            iw = boxW;
            ih = ((boxH * 144) / 256) & ~3;
        } else {
            iw = boxW;
            ih = boxH;
        }
        if (iw < 1) iw = 1;
        if (ih < 1) ih = 1;
    };
    auto pasteCentered = [&](const RgbaImage& inner, int outW, int outH,
                             bool transparentBars) -> RgbaImage {
        RgbaImage out(outW, outH);
        if (!transparentBars) {
            for (int i = 0; i < outW * outH; i++) {
                out.pixels[i*4+0] = 0;
                out.pixels[i*4+1] = 0;
                out.pixels[i*4+2] = 0;
                out.pixels[i*4+3] = 255;
            }
        }
        int offX = (outW - inner.width) / 2;
        int offY = (outH - inner.height) / 2;
        for (int y = 0; y < inner.height; y++) {
            int dy = y + offY;
            if (dy < 0 || dy >= outH) continue;
            int copyW = std::min(inner.width, outW - offX);
            if (copyW <= 0) continue;
            memcpy(out.pixels.data() + (dy * outW + std::max(0, offX)) * 4,
                   inner.pixels.data() + (y * inner.width + std::max(0, -offX)) * 4,
                   (size_t)copyW * 4);
        }
        return out;
    };

    // ── Canvas ──
    if (opts.writeCanvas) {
        // Upstream ltd-save-editor's encoder writes every UGC canvas as
        // 256×256 regardless of mesh type (see codec.ts:258); we match
        // that. For Book / TV the source is fit-resized into an inner
        // book/TV-shaped rectangle and pasted centered into 256×256, with
        // the rest of the buffer left as opaque-black bars.
        const int cW = 256, cH = 256;
        int innerW, innerH;
        innerSizeFor(opts.canvasShape, cW, cH, innerW, innerH);
        RgbaImage ci;
        if (innerW == cW && innerH == cH) {
            // Square — preserve the existing fit-mode behaviour.
            ci = (src.width != cW || src.height != cH)
                 ? ResizeWithFit(src, cW, cH, opts.fitMode, opts.matte)
                 : src;
        } else {
            // Book / TV — stretch the source to exactly fill the inner
            // rect, then pillarbox / letterbox the result. The user
            // explicitly wants the image to fill the book / TV cutout
            // (cropping it would lose content), so we override the fit
            // mode to Fill here regardless of the canvas matte preset.
            RgbaImage inner = ResizeWithFit(src, innerW, innerH,
                                            FitMode::Fill, opts.matte);
            ci = pasteCentered(inner, cW, cH, /*transparentBars=*/false);
        }
        auto rgba = ci.pixels;
        ThresholdAlpha(rgba);
        ConvertSrgbToLinear(rgba);
        auto sw = SwizzleBlockLinear(rgba, cW, cH, 4, DefaultBlockHeight);
        auto comp = ZstdCompress(sw);
        if (!WriteFile(opts.destStem + ".canvas.zs", comp, err)) return err;
    }

    // ── Ugctex ──
    // BC1/BC3 encoders now take sRGB-u8 directly (they handle sRGB↔linear
    // conversion themselves with float precision). Only the canvas, which is
    // stored as linear RGBA8 on disk, needs the pre-linearization step.
    {
        int uW=layout.Width, uH=layout.Height;
        // Same letterbox/pillarbox treatment as the canvas — the ugctex
        // sits on the same mesh and gets rendered at distance, so it
        // needs to show the same aspect with the same black bars.
        int innerW, innerH;
        innerSizeFor(opts.canvasShape, uW, uH, innerW, innerH);
        RgbaImage ui;
        if (innerW == uW && innerH == uH) {
            ui = (src.width != uW || src.height != uH)
                 ? ResizeWithFit(src, uW, uH, opts.fitMode, opts.matte)
                 : src;
        } else {
            // Same Fill override as the canvas pass above — keeps the
            // ugctex (used at distance) visually consistent with the
            // canvas (used at close range).
            RgbaImage inner = ResizeWithFit(src, innerW, innerH,
                                            FitMode::Fill, opts.matte);
            ui = pasteCentered(inner, uW, uH, /*transparentBars=*/false);
        }
        auto rgba = ui.pixels;
        ThresholdAlpha(rgba);

        std::vector<uint8_t> blocks = (layout.Format == TextureFormat::Bc3)
            ? Bc3Encode(rgba, uW, uH)
            : Bc1Encode(rgba, uW, uH, opts.encoder, opts.bc1Mode);

        int vbw=uW/4, vbh=uH/4;
        const std::vector<uint8_t>* base = originalSwizzled.empty() ? nullptr : &originalSwizzled;
        auto sw   = SwizzleBlockLinear(blocks, vbw, vbh, layout.BytesPerBlock(), layout.BlockHeight, base);
        auto comp = ZstdCompress(sw);
        if (!WriteFile(opts.destStem + ".ugctex.zs", comp, err)) return err;
    }

    // ── Thumb ──
    if (opts.writeThumb) {
        const int tW=256, tH=256;
        RgbaImage ti = (src.width!=tW||src.height!=tH)
            ? ResizeWithFit(src, tW, tH, opts.fitMode, opts.matte)
            : src;
        auto rgba = ti.pixels;
        auto blocks = Bc3Encode(rgba, tW, tH);
        auto sw     = SwizzleBlockLinear(blocks, tW/4, tH/4, 16, ThumbBlockHeight);
        auto comp   = ZstdCompress(sw);
        std::string thumbDest = opts.thumbPath.empty()
            ? opts.destStem + "_Thumb.ugctex.zs"
            : opts.thumbPath;
        if (!WriteFile(thumbDest, comp, err)) return err;
    }

    return "";
}

std::string ImportPng(const ImportOptions& opts) {
    RgbaImage src;
    std::string err;
    if (!LoadPng(opts.pngPath, src, err)) return err;
    return ImportRgbaImage(src, opts);
}

} // namespace TextureProcessor
