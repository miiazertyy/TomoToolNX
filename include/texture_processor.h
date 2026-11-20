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
    // Custom: full port of ltd-save-editor's bc_encode.ts. Generates a
    //         bounding-box candidate and three PCA-scaled candidates in both
    //         4-color and 3-color modes, scores each in sRGB space with
    //         gamma-derivative² perceptual weighting, iteratively refits
    //         endpoints by weighted least squares (up to 4 rounds), then
    //         perturbs the RGB565 endpoints ±1 in each 5/6-bit channel until
    //         no further improvement (up to 4 rounds).
    // PCA:    single-pass principal-axis fit (8 power-iteration steps), no
    //         candidate scoring or refinement. Faster fallback.
    // Rgbcx:  Richard Geldreich's rgbcx (libs/rgbcx), level 10, on the
    //         linear values, as ltd-save-editor's "rgbcx" choice; blocks with
    //         see-through pixels go through Custom (rgbcx ignores alpha).
    enum class Bc1Encoder { Custom, PCA, Rgbcx };
    enum class Bc1Mode   { Auto, FourColor, ThreeColor };
    // How the source image is scaled to fit the target texture size.
    // - Cover:   scale up to fill, crop the overflow. Preserves aspect ratio.
    // - Contain: scale down to fit inside, letterbox the remainder with the matte color.
    // - Fill:    stretch to the target W/H ignoring aspect ratio.
    enum class FitMode   { Cover, Contain, Fill };
    // Canvas dimensions for the imported texture's .canvas.zs. The game uses
    // different in-memory canvas sizes for different UGC mesh types; we can't
    // reliably detect "this slot is a book" or "this is a TV" from byte count
    // alone (multiple meshes share the same ugctex byte budget), so the user
    // picks the shape explicitly in the texture-tab settings.
    // - Square: 256×256 — the default, used by food/clothing/goods/etc.
    // - Book:   256×512 — taller; book covers open up to twice the height.
    // - Tv:     256×128 — shorter; the TV mesh maps a wide screen onto a
    //                     half-height canvas.
    enum class CanvasShape { Square, Book, Tv };

    struct Matte { uint8_t r = 0, g = 0, b = 0, a = 0; };  // a==0 means transparent letterbox

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
        // Path to the existing .canvas.zs for this slot. When set, the new
        // canvas inherits its dimensions instead of defaulting to 256×256 —
        // required for non-square UGC items (books, etc.) whose canvases
        // are 256-wide × N-tall.
        std::string originalCanvasPath;
        std::string thumbPath; // actual thumb file path — write destination when set
        Bc1Encoder  encoder  = Bc1Encoder::Custom;
        Bc1Mode     bc1Mode  = Bc1Mode::Auto;
        FitMode     fitMode  = FitMode::Cover;   // matches upstream default
        Matte       matte    = {};               // transparent unless fitMode == Contain
        // When set to anything other than CanvasShape::Square, overrides the
        // canvas dimensions chosen from `originalCanvasPath`. Caller-provided
        // explicit override for slots whose mesh expects a non-square canvas
        // (books, TVs, etc.) — see the CanvasShape enum above.
        CanvasShape canvasShape = CanvasShape::Square;
        // Turned clockwise this many quarter turns, then mirrored left to
        // right (flipH) and/or top to bottom (flipV), before anything else.
        int  rotate = 0;
        bool flipH  = false;
        bool flipV  = false;
    };

    // The picture turned/mirrored as above.
    RgbaImage Transformed(const RgbaImage& src, int quarterTurns, bool flipH, bool flipV);
    // The picture fitted into W x H the way an import fits it: the fit mode
    // and matte (Contain), in linear light, averaging every source pixel when
    // shrinking and blending the nearest four when growing; with shapeBars,
    // stretched into the book/TV cut-out with black bars around. sRGB.
    RgbaImage FitForTexture(const RgbaImage& src, int W, int H, const ImportOptions& opts, bool shapeBars);
    // What the game will hold at W x H, in sRGB: turned, fitted (with the
    // bars), alpha cut at half, through the block encoder the import uses
    // (BC1 with the options' encoder and mode, or BC3) and read back. The
    // previews draw exactly this, so they match what an import writes.
    RgbaImage PreviewTexture(const RgbaImage& src, int W, int H, TextureFormat fmt, const ImportOptions& opts);
    // The size and format of the ugctex an import into this slot writes
    // (from the slot's current file; 512 x 512 BC1 without one).
    UgctexLayout LayoutOfFile(const std::string& ugctexPath);
    // A PNG, JPEG or WebP picture as sRGB RGBA. "" or an error.
    std::string LoadImageFile(const std::string& path, RgbaImage& out);

    // ── Designs as the game stores them ──
    // A design is a slot's own files: <stem>.ugctex.zs (always), <stem>.canvas.zs
    // and <stem>_Thumb.ugctex.zs; people trade them zipped or loose.
    struct DesignFiles { std::string stem; std::vector<uint8_t> canvas, ugctex, thumb; };
    // A zip archive's files (stored or deflated), by name. "" or an error.
    std::string ReadZip(const std::vector<uint8_t>& zip, std::vector<std::pair<std::string, std::vector<uint8_t>>>& out);
    // The first design among named files (folders in the names are ignored).
    bool FindDesign(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, DesignFiles& d);
    // Writes a design into a slot (destStem: folder + "/" + stem). Each file must
    // be what its name says, and the ugctex must have the slot's own size and
    // format (a design for another shape would come out scrambled). A missing
    // canvas (when the slot has one) or thumb (when thumbPath is set) is made
    // from the ugctex. "" or an error.
    std::string ImportDesign(const DesignFiles& d, const std::string& destStem, const std::string& originalUgctexPath,
                             const std::string& thumbPath, bool writeCanvas);

    // Returns empty string on success, error message on failure.
    std::string ImportPng(const ImportOptions& opts);
    // Same as ImportPng but accepts an already-decoded sRGB RgbaImage (skips PNG load).
    std::string ImportRgbaImage(const RgbaImage& src, const ImportOptions& opts);

    // ── Internal helpers (exposed for testing) ──
    std::vector<uint8_t> DeswizzleBlockLinear(const std::vector<uint8_t>& data,
                                               int width, int height,
                                               int bpe, int blockHeight);
    std::vector<uint8_t> SwizzleBlockLinear(const std::vector<uint8_t>& data,
                                             int width, int height,
                                             int bpe, int blockHeight,
                                             const std::vector<uint8_t>* baseBuffer = nullptr);

    std::vector<uint8_t> Bc1Decode(const std::vector<uint8_t>& blocks, int w, int h);
    std::vector<uint8_t> Bc1Encode(const std::vector<uint8_t>& rgba, int w, int h,
                                    Bc1Encoder enc  = Bc1Encoder::Custom,
                                    Bc1Mode    mode = Bc1Mode::Auto);
    std::vector<uint8_t> Bc3Decode(const std::vector<uint8_t>& blocks, int w, int h);
    std::vector<uint8_t> Bc3Encode(const std::vector<uint8_t>& rgba, int w, int h);

    void ConvertLinearToSrgb(std::vector<uint8_t>& rgba);
    void ConvertSrgbToLinear(std::vector<uint8_t>& rgba);

    std::string GetBaseName(const std::string& filename);

} // namespace TextureProcessor
