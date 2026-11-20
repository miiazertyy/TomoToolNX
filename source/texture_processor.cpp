// texture_processor.cpp
// C++17 port of LivinTheDreamToolkit TextureProcessor.cs
// No exceptions (-fno-exceptions compatible). All errors returned as strings.

#include "texture_processor.h"
#include <zstd.h>
#include <zlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdint>

#define RGBCX_USE_SMALLER_TABLES 1
#include "../libs/rgbcx/rgbcx.h"

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

// A slot's thumbnail: always the game's own <stem>_Thumb.ugctex.zs, and the
// other file too when the slot has one under another name (an older tool's
// <stem>_Thumb_ugctex.zs), so whatever reads it sees the same picture.
static bool WriteThumb(const std::string& destStem, const std::string& known, const std::vector<uint8_t>& data, std::string& err) {
    const std::string game = destStem + "_Thumb.ugctex.zs";
    if (!WriteFile(game, data, err)) return false;
    if (!known.empty() && known != game) {
        FILE* f = fopen(known.c_str(), "rb");
        if (f) { fclose(f); if (!WriteFile(known, data, err)) return false; }
    }
    return true;
}

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
    bool wantLinear = (enc == Bc1Encoder::PCA || enc == Bc1Encoder::Rgbcx);
    if (enc == Bc1Encoder::Rgbcx) {
        static const bool ready = (rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal), true);
        (void)ready;
    }
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
        else if (enc == Bc1Encoder::Rgbcx) {
            // rgbcx ignores alpha: blocks with see-through pixels need the
            // 3-colour cut-out, which Custom does.
            bool cut = false;
            for (int i = 0; i < 16 && !cut; i++) cut = linBlk[i * 4 + 3] < 128;
            if (cut) custom_bc::EncodeColorBlock(srgbBlk, mode, /*allowAlpha=*/true, out.data(), off);
            else rgbcx::encode_bc1(10, out.data() + off, linBlk, mode != Bc1Mode::FourColor, false);
        } else
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

// ─────────────────────────────────────────────────────────────────────────────
// Turning and fitting a picture
// ─────────────────────────────────────────────────────────────────────────────

RgbaImage Transformed(const RgbaImage& src, int quarterTurns, bool flipH, bool flipV) {
    const int q = ((quarterTurns % 4) + 4) % 4;
    const int W = src.width, H = src.height;
    RgbaImage out(q % 2 ? H : W, q % 2 ? W : H);
    for (int y = 0; y < out.height; y++)
        for (int x = 0; x < out.width; x++) {
            // Undo the mirrors (they come last), then the turn.
            const int ux = flipH ? out.width - 1 - x : x;
            const int uy = flipV ? out.height - 1 - y : y;
            int sx, sy;
            switch (q) {
                case 0:  sx = ux;         sy = uy;         break;
                case 1:  sx = uy;         sy = H - 1 - ux; break;   // clockwise
                case 2:  sx = W - 1 - ux; sy = H - 1 - uy; break;
                default: sx = W - 1 - uy; sy = ux;         break;   // counter-clockwise
            }
            memcpy(out.pixels.data() + ((size_t)y * out.width + x) * 4, src.pixels.data() + ((size_t)sy * W + sx) * 4, 4);
        }
    return out;
}

// sRGB byte → linear light (0..1), and back.
static float s_linF[256];
static bool  s_linFBuilt = false;
static void BuildLinF() {
    if (s_linFBuilt) return;
    for (int i = 0; i < 256; i++) {
        const double c = i / 255.0;
        s_linF[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    s_linFBuilt = true;
}
static inline uint8_t ToSrgb8(float l) {
    if (!(l > 0.f)) return 0;
    if (l >= 1.f) return 255;
    const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.f / 2.4f) - 0.055f;
    return (uint8_t)std::max(0, std::min(255, (int)lroundf(c * 255.f)));
}

// One axis of a resize. For each destination pixel: the source pixels it
// reads with their weights (summing to 1), and how much of the pixel the
// picture covers (Contain can leave the edge pixels partly matte).
struct AxisTaps {
    std::vector<int> start, count, idx;
    std::vector<float> w, cover;
};
// Destination pixels [0, dn); the picture lands on [d0, d0 + dlen) and shows
// the source span [s0, s0 + slen) of a line of n pixels.
static AxisTaps MakeTaps(int dn, double d0, double dlen, double s0, double slen, int n) {
    AxisTaps t;
    t.start.resize(dn); t.count.resize(dn); t.cover.resize(dn);
    const double step = slen / dlen;                       // source pixels per destination pixel
    const int lo = std::max(0, (int)std::floor(s0));
    const int hi = std::min(n - 1, (int)std::ceil(s0 + slen) - 1);
    for (int o = 0; o < dn; o++) {
        const double c0 = std::max((double)o, d0), c1 = std::min((double)o + 1.0, d0 + dlen);
        t.start[o] = (int)t.idx.size();
        t.count[o] = 0;
        t.cover[o] = (float)std::max(0.0, std::min(1.0, c1 - c0));
        if (c1 <= c0 || hi < lo) continue;
        const size_t first = t.idx.size();
        double sum = 0;
        if (step > 1.0) {
            // Shrinking: the average of all the source under it (area).
            const double u0 = s0 + (c0 - d0) * step, u1 = s0 + (c1 - d0) * step;
            const int i1 = std::min(hi, (int)std::ceil(u1) - 1);
            for (int i = std::max(lo, (int)std::floor(u0)); i <= i1; i++) {
                const double wa = std::min((double)i + 1.0, u1) - std::max((double)i, u0);
                if (wa > 0) { t.idx.push_back(i); t.w.push_back((float)wa); sum += wa; }
            }
        } else {
            // Growing: between the two nearest source pixels (bilinear).
            const double u = s0 + ((c0 + c1) * 0.5 - d0) * step - 0.5;
            const int i0 = (int)std::floor(u);
            const double f = u - i0;
            const int ia = std::max(lo, std::min(hi, i0)), ib = std::max(lo, std::min(hi, i0 + 1));
            if (ia == ib) { t.idx.push_back(ia); t.w.push_back(1.f); }
            else { t.idx.push_back(ia); t.w.push_back((float)(1.0 - f)); t.idx.push_back(ib); t.w.push_back((float)f); }
            sum = 1.0;
        }
        if (sum <= 0) {                                    // a sliver: the nearest pixel
            t.idx.resize(first); t.w.resize(first);
            t.idx.push_back(std::max(lo, std::min(hi, (int)std::floor(s0 + ((c0 + c1) * 0.5 - d0) * step))));
            t.w.push_back(1.f);
            sum = 1.0;
        }
        for (size_t k = first; k < t.w.size(); k++) t.w[k] = (float)(t.w[k] / sum);
        t.count[o] = (int)(t.idx.size() - first);
    }
    return t;
}

// Resize in linear light with premultiplied alpha (see-through pixels lend
// no colour to their neighbours): cover crops the middle, contain fits the
// whole picture over the matte, fill stretches.
static RgbaImage ResizeWithFit(const RgbaImage& src, int dw, int dh,
                                FitMode mode, const Matte& matte) {
    BuildLinF();
    RgbaImage dst(dw, dh);
    if (src.width <= 0 || src.height <= 0 || dw <= 0 || dh <= 0) return dst;
    double s0x = 0, s0y = 0, slx = src.width, sly = src.height;
    double d0x = 0, d0y = 0, dlx = dw, dly = dh;
    if (mode == FitMode::Contain) {
        const double k = std::min((double)dw / src.width, (double)dh / src.height);
        dlx = src.width * k; dly = src.height * k;
        d0x = (dw - dlx) / 2.0; d0y = (dh - dly) / 2.0;
    } else if (mode == FitMode::Cover) {
        const double k = std::max((double)dw / src.width, (double)dh / src.height);
        slx = dw / k; sly = dh / k;
        s0x = (src.width - slx) / 2.0; s0y = (src.height - sly) / 2.0;
    }
    const AxisTaps tx = MakeTaps(dw, d0x, dlx, s0x, slx, src.width);
    const AxisTaps ty = MakeTaps(dh, d0y, dly, s0y, sly, src.height);
    // The destination rows each source row feeds, and how much.
    std::vector<std::vector<std::pair<int, float>>> feeds(src.height);
    for (int oy = 0; oy < dh; oy++)
        for (int k = 0; k < ty.count[oy]; k++)
            feeds[ty.idx[ty.start[oy] + k]].push_back({ oy, ty.w[ty.start[oy] + k] });
    // Rows one at a time: across first, then into the rows they feed.
    std::vector<float> acc((size_t)dw * dh * 4, 0.f), line((size_t)src.width * 4), across((size_t)dw * 4);
    for (int sy = 0; sy < src.height; sy++) {
        if (feeds[sy].empty()) continue;
        const uint8_t* p = src.row(sy);
        for (int x = 0; x < src.width; x++, p += 4) {
            const float a = p[3] * (1.f / 255.f);
            float* l = &line[(size_t)x * 4];
            l[0] = s_linF[p[0]] * a; l[1] = s_linF[p[1]] * a; l[2] = s_linF[p[2]] * a; l[3] = a;
        }
        for (int ox = 0; ox < dw; ox++) {
            float r = 0, g = 0, b = 0, a = 0;
            const int st = tx.start[ox];
            for (int k = 0; k < tx.count[ox]; k++) {
                const float* l = &line[(size_t)tx.idx[st + k] * 4];
                const float wv = tx.w[st + k];
                r += l[0] * wv; g += l[1] * wv; b += l[2] * wv; a += l[3] * wv;
            }
            float* o = &across[(size_t)ox * 4];
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
        for (const auto& f : feeds[sy]) {
            float* d = &acc[(size_t)f.first * dw * 4];
            for (int i = 0; i < dw * 4; i++) d[i] += across[i] * f.second;
        }
    }
    // Over the matte (Contain), then back to sRGB.
    float mr = 0, mg = 0, mb = 0, ma = 0;
    if (mode == FitMode::Contain && matte.a > 0) {
        ma = matte.a / 255.f;
        mr = s_linF[matte.r] * ma; mg = s_linF[matte.g] * ma; mb = s_linF[matte.b] * ma;
    }
    for (int oy = 0; oy < dh; oy++) {
        uint8_t* o = dst.row(oy);
        for (int ox = 0; ox < dw; ox++, o += 4) {
            const float cov = tx.cover[ox] * ty.cover[oy];
            const float* a4 = &acc[((size_t)oy * dw + ox) * 4];
            float r = a4[0] * cov, g = a4[1] * cov, b = a4[2] * cov, a = a4[3] * cov;
            r += mr * (1.f - a); g += mg * (1.f - a); b += mb * (1.f - a); a += ma * (1.f - a);
            if (a > 1e-6f) { o[0] = ToSrgb8(r / a); o[1] = ToSrgb8(g / a); o[2] = ToSrgb8(b / a); }
            else { o[0] = o[1] = o[2] = 0; }
            o[3] = (uint8_t)std::max(0, std::min(255, (int)lroundf(a * 255.f)));
        }
    }
    return dst;
}

// Book is ~3:4 portrait (256×256 → 200-wide inner), TV is 16:9 wide
// (256×256 → 144-tall inner). The remaining area gets filled with opaque
// black bars so the in-game mesh, which renders the canvas 1:1, shows the
// picture in the right aspect with pillarbox / letterbox bars around it.
// (Pre-stretching the whole square came back squashed: the mesh does not
// stretch.) Multiples of 4 keep BC blocks aligned.
static void InnerSizeFor(CanvasShape shape, int boxW, int boxH, int& iw, int& ih) {
    iw = boxW; ih = boxH;
    if (shape == CanvasShape::Book)    iw = ((boxW * 200) / 256) & ~3;
    else if (shape == CanvasShape::Tv) ih = ((boxH * 144) / 256) & ~3;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
}
static RgbaImage PasteCentered(const RgbaImage& inner, int outW, int outH) {
    RgbaImage out(outW, outH);
    for (int i = 0; i < outW * outH; i++) out.pixels[(size_t)i * 4 + 3] = 255;   // opaque black bars
    const int offX = (outW - inner.width) / 2, offY = (outH - inner.height) / 2;
    for (int y = 0; y < inner.height; y++) {
        const int dy = y + offY;
        if (dy < 0 || dy >= outH) continue;
        const int copyW = std::min(inner.width, outW - offX);
        if (copyW <= 0) continue;
        memcpy(out.pixels.data() + ((size_t)dy * outW + std::max(0, offX)) * 4,
               inner.pixels.data() + ((size_t)y * inner.width + std::max(0, -offX)) * 4, (size_t)copyW * 4);
    }
    return out;
}

RgbaImage FitForTexture(const RgbaImage& src, int W, int H, const ImportOptions& opts, bool shapeBars) {
    int iw = W, ih = H;
    if (shapeBars) InnerSizeFor(opts.canvasShape, W, H, iw, ih);
    if (iw == W && ih == H)
        return (src.width == W && src.height == H) ? src : ResizeWithFit(src, W, H, opts.fitMode, opts.matte);
    // Book / TV: the picture fills the cut-out (cropping would lose part of
    // it), so the fit is Fill there whatever the setting.
    return PasteCentered(ResizeWithFit(src, iw, ih, FitMode::Fill, opts.matte), W, H);
}

static bool Turned(const ImportOptions& o) { return (o.rotate % 4) != 0 || o.flipH || o.flipV; }

RgbaImage PreviewTexture(const RgbaImage& src0, int W, int H, TextureFormat fmt, const ImportOptions& opts) {
    RgbaImage turned;
    const RgbaImage* src = &src0;
    if (Turned(opts)) { turned = Transformed(src0, opts.rotate, opts.flipH, opts.flipV); src = &turned; }
    RgbaImage img = FitForTexture(*src, W, H, opts, true);
    ThresholdAlpha(img.pixels);
    std::vector<uint8_t> px = (fmt == TextureFormat::Bc3)
        ? Bc3Decode(Bc3Encode(img.pixels, W, H), W, H)
        : Bc1Decode(Bc1Encode(img.pixels, W, H, opts.encoder, opts.bc1Mode), W, H);
    if (px.size() == img.pixels.size()) {
        ConvertLinearToSrgb(px);            // the blocks hold linear values
        img.pixels = std::move(px);
    }
    return img;
}

UgctexLayout LayoutOfFile(const std::string& ugctexPath) {
    UgctexLayout layout{512, 512, 128, 128, 16, TextureFormat::Bc1};
    if (ugctexPath.empty()) return layout;
    std::vector<uint8_t> raw;
    std::string err;
    UgctexLayout found{};
    if (ZstdDecompressFile(ugctexPath, raw, err) && DetectUgctexLayoutInner(raw.size(), found, err)) layout = found;
    return layout;
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

std::string LoadImageFile(const std::string& path, RgbaImage& out) {
    std::string err;
    return LoadPng(path, out, err) ? std::string() : err;
}

// ─────────────────────────────────────────────────────────────────────────────
// ImportRgbaImage / ImportPng  (no exceptions)
// ─────────────────────────────────────────────────────────────────────────────

std::string ImportRgbaImage(const RgbaImage& src0, const ImportOptions& opts) {
    std::string err;

    // Turned / mirrored first, as the preview shows it.
    RgbaImage turnedImg;
    const RgbaImage& src = Turned(opts) ? (turnedImg = Transformed(src0, opts.rotate, opts.flipH, opts.flipV)) : src0;

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

    // ── Canvas ──
    if (opts.writeCanvas) {
        // Upstream ltd-save-editor's encoder writes every UGC canvas as
        // 256×256 regardless of mesh type (see codec.ts:258); we match
        // that. For Book / TV the picture goes into the book/TV-shaped
        // cut-out with opaque black bars around (FitForTexture).
        const int cW = 256, cH = 256;
        RgbaImage ci = FitForTexture(src, cW, cH, opts, true);
        auto rgba = ci.pixels;
        ThresholdAlpha(rgba);
        ConvertSrgbToLinear(rgba);
        auto sw = SwizzleBlockLinear(rgba, cW, cH, 4, DefaultBlockHeight);
        auto comp = ZstdCompress(sw);
        if (!WriteFile(opts.destStem + ".canvas.zs", comp, err)) return err;
    }

    // ── Ugctex ──
    // BC1/BC3 encoders take sRGB-u8 directly (they handle sRGB↔linear
    // conversion themselves with float precision). Only the canvas, which is
    // stored as linear RGBA8 on disk, needs the pre-linearization step.
    {
        int uW=layout.Width, uH=layout.Height;
        // Same bars as the canvas: the ugctex sits on the same mesh and is
        // drawn at a distance, so it needs the same aspect.
        RgbaImage ui = FitForTexture(src, uW, uH, opts, true);
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
        RgbaImage ti = FitForTexture(src, tW, tH, opts, false);
        auto rgba = ti.pixels;
        auto blocks = Bc3Encode(rgba, tW, tH);
        auto sw     = SwizzleBlockLinear(blocks, tW/4, tH/4, 16, ThumbBlockHeight);
        auto comp   = ZstdCompress(sw);
        if (!WriteThumb(opts.destStem, opts.thumbPath, comp, err)) return err;
    }

    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Designs as the game stores them (zipped or loose)
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t Rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t Rd32(const uint8_t* p) { return Rd16(p) | (Rd16(p + 2) << 16); }

std::string ReadZip(const std::vector<uint8_t>& zip, std::vector<std::pair<std::string, std::vector<uint8_t>>>& out) {
    const size_t n = zip.size();
    if (n < 22) return "Not a zip file";
    // The end of the central directory, searched from the end (comment <= 64 KB).
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;)
        if (Rd32(&zip[i]) == 0x06054b50u) { eocd = i; break; }
    if (eocd == std::string::npos) return "Not a zip file";
    const uint32_t count = Rd16(&zip[eocd + 10]), cdSize = Rd32(&zip[eocd + 12]), cdOff = Rd32(&zip[eocd + 16]);
    if ((size_t)cdOff + cdSize > n || count > 256) return "Damaged zip file";
    size_t p = cdOff;
    for (uint32_t k = 0; k < count; k++) {
        if (p + 46 > n || Rd32(&zip[p]) != 0x02014b50u) return "Damaged zip file";
        const uint32_t method = Rd16(&zip[p + 10]), csize = Rd32(&zip[p + 20]), usize = Rd32(&zip[p + 24]);
        const uint32_t nameLen = Rd16(&zip[p + 28]), extraLen = Rd16(&zip[p + 30]), commentLen = Rd16(&zip[p + 32]);
        const uint32_t local = Rd32(&zip[p + 42]);
        if (p + 46 + nameLen > n) return "Damaged zip file";
        const std::string name((const char*)&zip[p + 46], nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.back() == '/') continue;                 // a folder
        if (usize > 16u * 1024 * 1024) continue;                           // nothing a design holds
        if ((size_t)local + 30 > n || Rd32(&zip[local]) != 0x04034b50u) return "Damaged zip file";
        const size_t data = (size_t)local + 30 + Rd16(&zip[local + 26]) + Rd16(&zip[local + 28]);
        if (data + csize > n) return "Damaged zip file";
        std::vector<uint8_t> bytes;
        if (method == 0) bytes.assign(zip.begin() + data, zip.begin() + data + csize);
        else if (method == 8) {
            bytes.resize(usize);
            z_stream zs{};
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return "Cannot unpack the zip file";
            zs.next_in = (Bytef*)&zip[data]; zs.avail_in = csize;
            zs.next_out = bytes.data(); zs.avail_out = usize;
            const int rc = inflate(&zs, Z_FINISH);
            const bool ok = rc == Z_STREAM_END && zs.total_out == usize;
            inflateEnd(&zs);
            if (!ok) return "Cannot unpack " + name;
        } else continue;                                                    // other methods: skipped
        out.push_back({ name, std::move(bytes) });
    }
    return "";
}

bool FindDesign(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, DesignFiles& d) {
    auto base = [](const std::string& s) { const size_t q = s.find_last_of("/\\"); return q == std::string::npos ? s : s.substr(q + 1); };
    auto ends = [](const std::string& s, const char* suf) { const size_t m = strlen(suf); return s.size() > m && s.compare(s.size() - m, m, suf) == 0; };
    // The stem of the first ugctex that is not a thumb.
    for (const auto& f : files) {
        const std::string b = base(f.first);
        if (ends(b, ".ugctex.zs") && !ends(b, "_Thumb.ugctex.zs")) { d.stem = b.substr(0, b.size() - 10); d.ugctex = f.second; break; }
    }
    if (d.stem.empty()) return false;
    for (const auto& f : files) {
        const std::string b = base(f.first);
        if (b == d.stem + ".canvas.zs") d.canvas = f.second;
        else if (b == d.stem + "_Thumb.ugctex.zs") d.thumb = f.second;
    }
    return true;
}

std::string ImportDesign(const DesignFiles& d, const std::string& destStem, const std::string& originalUgctexPath,
                         const std::string& thumbPath, bool writeCanvas) {
    std::string err;
    auto unz = [](const std::vector<uint8_t>& z) { return z.empty() ? std::vector<uint8_t>() : ZstdDecompressBuffer(z.data(), z.size()); };
    // The ugctex: a layout the game knows, the same as the slot's.
    const std::vector<uint8_t> uraw = unz(d.ugctex);
    UgctexLayout lay{};
    if (uraw.empty() || !DetectUgctexLayoutInner(uraw.size(), lay, err)) return "The design's .ugctex.zs is damaged";
    if (!originalUgctexPath.empty()) {
        const UgctexLayout slot = LayoutOfFile(originalUgctexPath);
        if (slot.Width != lay.Width || slot.Height != lay.Height || slot.Format != lay.Format)
            return "This design is " + std::to_string(lay.Width) + "x" + std::to_string(lay.Height) + ", this slot takes " +
                   std::to_string(slot.Width) + "x" + std::to_string(slot.Height) + ": it is for another kind of item";
    }
    // The canvas: square, or 256 wide.
    if (!d.canvas.empty()) {
        const std::vector<uint8_t> c = unz(d.canvas);
        const size_t px = c.size() / 4;
        const size_t side = (size_t)std::sqrt((double)px);
        if (c.empty() || c.size() % 4 || !(side * side == px || px % 256 == 0)) return "The design's .canvas.zs is damaged";
    }
    if (!d.thumb.empty()) {
        const std::vector<uint8_t> t = unz(d.thumb);
        const size_t blocks = t.size() / 16, gs = (size_t)std::sqrt((double)blocks);
        if (t.empty() || t.size() % 16 || gs * gs != blocks) return "The design's _Thumb.ugctex.zs is damaged";
    }
    // What is missing comes from the ugctex.
    RgbaImage pic;
    if ((writeCanvas && d.canvas.empty()) || (!thumbPath.empty() && d.thumb.empty())) {
        if (!DecodeUgctex(uraw, pic, false).empty() || pic.width <= 0) return "The design's .ugctex.zs is damaged";
    }
    if (!WriteFile(destStem + ".ugctex.zs", d.ugctex, err)) return err;
    if (writeCanvas) {
        if (!d.canvas.empty()) { if (!WriteFile(destStem + ".canvas.zs", d.canvas, err)) return err; }
        else {
            ImportOptions o;
            o.fitMode = FitMode::Fill;
            auto rgba = FitForTexture(pic, 256, 256, o, false).pixels;
            ConvertSrgbToLinear(rgba);
            if (!WriteFile(destStem + ".canvas.zs", ZstdCompress(SwizzleBlockLinear(rgba, 256, 256, 4, DefaultBlockHeight)), err)) return err;
        }
    }
    if (!thumbPath.empty()) {
        if (!d.thumb.empty()) { if (!WriteThumb(destStem, thumbPath, d.thumb, err)) return err; }
        else {
            ImportOptions o;
            o.fitMode = FitMode::Fill;
            const auto blocks = Bc3Encode(FitForTexture(pic, 256, 256, o, false).pixels, 256, 256);
            if (!WriteThumb(destStem, thumbPath, ZstdCompress(SwizzleBlockLinear(blocks, 64, 64, 16, ThumbBlockHeight)), err)) return err;
        }
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
