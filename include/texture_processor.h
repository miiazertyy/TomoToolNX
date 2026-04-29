#pragma once
// texture_processor.h
// Port of LivinTheDreamToolkit TextureProcessor.cs → C++17
// BC1/BC3 decode/encode + NX block-linear swizzle + zstd wrap/unwrap

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <stdexcept>
#include <cmath>
#include <algorithm>

// ─── Simple RGBA image ───────────────────────────────────────────────────────

struct RgbaImage {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major

    RgbaImage() = default;
    RgbaImage(int w, int h) : width(w), height(h), pixels(w * h * 4, 0) {}

    uint8_t* row(int y)             { return pixels.data() + y * width * 4; }
    const uint8_t* row(int y) const { return pixels.data() + y * width * 4; }
};

// ─── TextureProcessor ────────────────────────────────────────────────────────

namespace TextureProcessor {

    static const int DefaultBlockHeight = 16;
    static const int ThumbBlockHeight   = 8;
    static const int ZstdLevel         = 3;

    enum class TextureKind { Canvas, Ugctex, Thumb };
    enum class TextureFormat { Bc1, Bc3 };

    struct UgctexLayout {
        int Width, Height;
        int SwizzleBlocksWide, SwizzleBlocksTall;
        int BlockHeight;
        TextureFormat Format;

        int BytesPerBlock() const { return Format == TextureFormat::Bc3 ? 16 : 8; }
    };

    // Determine kind from filename
    TextureKind DetectKind(const std::string& filename);

    // Detect layout from decompressed byte count
    UgctexLayout DetectUgctexLayout(size_t decompressedBytes);

    // ── Zstd I/O ──
    std::vector<uint8_t> ZstdDecompress(const std::string& path);
    std::vector<uint8_t> ZstdDecompressBuffer(const uint8_t* data, size_t size);
    std::vector<uint8_t> ZstdCompress(const std::vector<uint8_t>& data, int level = ZstdLevel);

    // ── Decode ──
    // Returns empty string on success, error message on failure.
    std::string DecodeFile(const std::string& path, RgbaImage& out, bool noSrgb = false);
    std::string DecodeCanvas(const std::vector<uint8_t>& raw, RgbaImage& out, bool noSrgb = false);
    std::string DecodeUgctex(const std::vector<uint8_t>& raw, RgbaImage& out, bool noSrgb = false);
    std::string DecodeThumb(const std::vector<uint8_t>& raw, RgbaImage& out, bool noSrgb = false);

    // ── Encode / Import ──
    struct ImportOptions {
        std::string pngPath;
        std::string destStem;
        bool writeCanvas     = true;
        bool writeThumb      = false;
        bool noSrgb          = false;
        std::string originalUgctexPath;
    };

    // Returns empty string on success, error message on failure.
    std::string ImportPng(const ImportOptions& opts);

    // ── Internal helpers (exposed for testing) ──
    std::vector<uint8_t> DeswizzleBlockLinear(const std::vector<uint8_t>& data,
                                               int width, int height,
                                               int bpe, int blockHeight);
    std::vector<uint8_t> SwizzleBlockLinear(const std::vector<uint8_t>& data,
                                             int width, int height,
                                             int bpe, int blockHeight,
                                             const std::vector<uint8_t>* baseBuffer = nullptr);

    std::vector<uint8_t> Bc1Decode(const std::vector<uint8_t>& blocks, int w, int h);
    std::vector<uint8_t> Bc1Encode(const std::vector<uint8_t>& rgba, int w, int h);
    std::vector<uint8_t> Bc3Decode(const std::vector<uint8_t>& blocks, int w, int h);
    std::vector<uint8_t> Bc3Encode(const std::vector<uint8_t>& rgba, int w, int h);

    void ConvertLinearToSrgb(std::vector<uint8_t>& rgba);
    void ConvertSrgbToLinear(std::vector<uint8_t>& rgba);

    std::string GetBaseName(const std::string& filename);

} // namespace TextureProcessor
