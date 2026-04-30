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

        double sg = (lin <= 0.0031308) ? lin * 12.92
                                       : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
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

static void Bc1EncodeBlock(const uint8_t blk[64], bool hasAlpha,
                            uint8_t* out, int off) {
    int minR=255,minG=255,minB=255,maxR=0,maxG=0,maxB=0,oc=0;
    for(int i=0;i<16;i++){
        const uint8_t* p=blk+i*4;
        if(p[3]<128) continue; oc++;
        if(p[0]<minR)minR=p[0]; if(p[0]>maxR)maxR=p[0];
        if(p[1]<minG)minG=p[1]; if(p[1]>maxG)maxG=p[1];
        if(p[2]<minB)minB=p[2]; if(p[2]>maxB)maxB=p[2];
    }
    if(oc==0){ memset(out+off,0,4); memset(out+off+4,0xFF,4); return; }
    uint16_t c0=Rgb565Encode((uint8_t)maxR,(uint8_t)maxG,(uint8_t)maxB);
    uint16_t c1=Rgb565Encode((uint8_t)minR,(uint8_t)minG,(uint8_t)minB);
    if(hasAlpha){ if(c0>c1) std::swap(c0,c1); if(c0==c1){if(c1<0xFFFF)c1++;else if(c0>0)c0--;} }
    else { if(c0<c1) std::swap(c0,c1); if(c0==c1){if(c0<0xFFFF)c0++;else c1--;} }
    auto [r0,g0,b0]=Rgb565Decode(c0); auto [r1,g1,b1]=Rgb565Decode(c1);
    int pr2,pg2,pb2,pr3,pg3,pb3; bool t3;
    if(c0>c1){pr2=(2*r0+r1)/3;pg2=(2*g0+g1)/3;pb2=(2*b0+b1)/3;
              pr3=(r0+2*r1)/3;pg3=(g0+2*g1)/3;pb3=(b0+2*b1)/3;t3=false;}
    else{pr2=(r0+r1)/2;pg2=(g0+g1)/2;pb2=(b0+b1)/2;pr3=pg3=pb3=0;t3=true;}
    uint32_t indices=0;
    for(int i=0;i<16;i++){
        const uint8_t* p=blk+i*4;
        int r=p[0],g=p[1],b=p[2],a=p[3],best;
        if(a<128&&t3){best=3;}
        else{
            int d0=ColorDistSq(r,g,b,r0,g0,b0),d1=ColorDistSq(r,g,b,r1,g1,b1),
                d2=ColorDistSq(r,g,b,pr2,pg2,pb2); best=0; int bd=d0;
            if(d1<bd){bd=d1;best=1;} if(d2<bd){bd=d2;best=2;}
            if(!t3){int d3=ColorDistSq(r,g,b,pr3,pg3,pb3);if(d3<bd)best=3;}
        }
        indices|=(uint32_t)(best<<(2*i));
    }
    memcpy(out+off,&c0,2); memcpy(out+off+2,&c1,2); memcpy(out+off+4,&indices,4);
}

std::vector<uint8_t> Bc1Encode(const std::vector<uint8_t>& rgba, int W, int H) {
    int bx=W/4,by=H/4;
    std::vector<uint8_t> out((size_t)bx*by*8);
    uint8_t blk[64];
    for(int iy=0;iy<by;iy++) for(int ix=0;ix<bx;ix++){
        bool ha=false;
        for(int row=0;row<4;row++) for(int col=0;col<4;col++){
            int src=((iy*4+row)*W+(ix*4+col))*4;
            int dst=(row*4+col)*4;
            memcpy(blk+dst,rgba.data()+src,4);
            if(rgba[src+3]<128) ha=true;
        }
        Bc1EncodeBlock(blk,ha,out.data(),(iy*bx+ix)*8);
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

static void Bc3EncodeBlock(const uint8_t blk[64], uint8_t* out, int off) {
    int minA=255,maxA=0;
    for(int i=0;i<16;i++){int a=blk[i*4+3];if(a<minA)minA=a;if(a>maxA)maxA=a;}
    uint8_t a0=(uint8_t)maxA,a1=(uint8_t)minA;
    out[off]=a0;out[off+1]=a1;
    int pal[8];pal[0]=a0;pal[1]=a1;
    if(a0>a1){
        pal[2]=(6*a0+a1)/7;pal[3]=(5*a0+2*a1)/7;pal[4]=(4*a0+3*a1)/7;
        pal[5]=(3*a0+4*a1)/7;pal[6]=(2*a0+5*a1)/7;pal[7]=(a0+6*a1)/7;
    } else { pal[2]=pal[3]=pal[4]=pal[5]=a0;pal[6]=0;pal[7]=255; }
    uint64_t aib=0;
    for(int i=0;i<16;i++){
        int a=blk[i*4+3],best=0,bd=abs(a-pal[0]);
        for(int p=1;p<8;p++){int d=abs(a-pal[p]);if(d<bd){bd=d;best=p;}}
        aib|=(uint64_t)best<<(3*i);
    }
    for(int i=0;i<6;i++) out[off+2+i]=(uint8_t)((aib>>(8*i))&0xFF);
    int minR=255,minG=255,minB=255,maxR=0,maxG=0,maxB=0;
    for(int i=0;i<16;i++){
        int r=blk[i*4],g=blk[i*4+1],b=blk[i*4+2];
        if(r<minR)minR=r;if(r>maxR)maxR=r;
        if(g<minG)minG=g;if(g>maxG)maxG=g;
        if(b<minB)minB=b;if(b>maxB)maxB=b;
    }
    uint16_t c0=Rgb565Encode((uint8_t)maxR,(uint8_t)maxG,(uint8_t)maxB);
    uint16_t c1=Rgb565Encode((uint8_t)minR,(uint8_t)minG,(uint8_t)minB);
    if(c0<c1)std::swap(c0,c1);
    if(c0==c1){if(c0<0xFFFF)c0++;else c1--;}
    auto [r0,g0,b0]=Rgb565Decode(c0); auto [r1,g1,b1]=Rgb565Decode(c1);
    int pr2=(2*r0+r1)/3,pg2=(2*g0+g1)/3,pb2=(2*b0+b1)/3;
    int pr3=(r0+2*r1)/3,pg3=(g0+2*g1)/3,pb3=(b0+2*b1)/3;
    uint32_t ci=0;
    for(int i=0;i<16;i++){
        int r=blk[i*4],g=blk[i*4+1],b=blk[i*4+2];
        int d0=ColorDistSq(r,g,b,r0,g0,b0),d1=ColorDistSq(r,g,b,r1,g1,b1),
            d2=ColorDistSq(r,g,b,pr2,pg2,pb2),d3=ColorDistSq(r,g,b,pr3,pg3,pb3);
        int best=0,bd=d0;
        if(d1<bd){bd=d1;best=1;}if(d2<bd){bd=d2;best=2;}if(d3<bd){best=3;}
        ci|=(uint32_t)(best<<(2*i));
    }
    memcpy(out+off+8,&c0,2);memcpy(out+off+10,&c1,2);memcpy(out+off+12,&ci,4);
}

std::vector<uint8_t> Bc3Encode(const std::vector<uint8_t>& rgba, int W, int H) {
    int bx=W/4,by=H/4;
    std::vector<uint8_t> out((size_t)bx*by*16);
    uint8_t blk[64];
    for(int iy=0;iy<by;iy++) for(int ix=0;ix<bx;ix++){
        for(int row=0;row<4;row++) for(int col=0;col<4;col++)
            memcpy(blk+(row*4+col)*4,rgba.data()+((iy*4+row)*W+(ix*4+col))*4,4);
        Bc3EncodeBlock(blk,out.data(),(iy*bx+ix)*16);
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

static RgbaImage ResizeNearest(const RgbaImage& src, int dw, int dh) {
    RgbaImage dst(dw, dh);
    for (int y=0;y<dh;y++) {
        int sy = y * src.height / dh;
        for (int x=0;x<dw;x++) {
            int sx = x * src.width / dw;
            memcpy(dst.pixels.data()+(y*dw+x)*4,
                   src.pixels.data()+(sy*src.width+sx)*4, 4);
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
// ImportPng  (no exceptions)
// ─────────────────────────────────────────────────────────────────────────────

std::string ImportPng(const ImportOptions& opts) {
    RgbaImage src;
    std::string err;
    if (!LoadPng(opts.pngPath, src, err)) return err;

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
        const int cW=256, cH=256;
        RgbaImage ci = (src.width!=cW||src.height!=cH) ? ResizeNearest(src,cW,cH) : src;
        auto rgba = ci.pixels;
        ThresholdAlpha(rgba); // prevent holes from semi-transparent edges
        if (!opts.noSrgb) ConvertSrgbToLinear(rgba);
        auto sw = SwizzleBlockLinear(rgba, cW, cH, 4, DefaultBlockHeight);
        auto comp = ZstdCompress(sw);
        if (!WriteFile(opts.destStem + ".canvas.zs", comp, err)) return err;
    }

    // ── Ugctex ──
    {
        int uW=layout.Width, uH=layout.Height;
        RgbaImage ui = (src.width!=uW||src.height!=uH) ? ResizeNearest(src,uW,uH) : src;
        auto rgba = ui.pixels;
        ThresholdAlpha(rgba); // BC1 can't handle semi-transparent pixels
        if (!opts.noSrgb) ConvertSrgbToLinear(rgba);

        std::vector<uint8_t> blocks = (layout.Format == TextureFormat::Bc3)
            ? Bc3Encode(rgba, uW, uH)
            : Bc1Encode(rgba, uW, uH);

        int vbw=uW/4, vbh=uH/4;
        const std::vector<uint8_t>* base = originalSwizzled.empty() ? nullptr : &originalSwizzled;
        auto sw   = SwizzleBlockLinear(blocks, vbw, vbh, layout.BytesPerBlock(), layout.BlockHeight, base);
        auto comp = ZstdCompress(sw);
        if (!WriteFile(opts.destStem + ".ugctex.zs", comp, err)) return err;
    }

    // ── Thumb ──
    if (opts.writeThumb) {
        const int tW=256, tH=256;
        RgbaImage ti = (src.width!=tW||src.height!=tH) ? ResizeNearest(src,tW,tH) : src;
        auto rgba = ti.pixels;
        if (!opts.noSrgb) ConvertSrgbToLinear(rgba);
        auto blocks = Bc3Encode(rgba, tW, tH);
        auto sw     = SwizzleBlockLinear(blocks, tW/4, tH/4, 16, ThumbBlockHeight);
        auto comp   = ZstdCompress(sw);
        if (!WriteFile(opts.destStem + "_Thumb_ugctex.zs", comp, err)) return err;
    }

    return ""; // success
}

} // namespace TextureProcessor
