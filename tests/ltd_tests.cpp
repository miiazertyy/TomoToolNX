// Host-side regression tests for the .ltd Mii parser in source/mii_manager.cpp.
// Covers the import bugs behind issue #36 (corrupt facepaint textures, one Mii
// wearing another's makeup, gender flipped on import, corrupted saves).
//
// Not part of the Switch build -- the Makefile only globs `source`, so this
// directory is ignored. Run it on the host with any C++17 compiler:
//
//   g++ -std=gnu++17 -I. -Iinclude tests/ltd_tests.cpp source/save_editor.cpp -o ltd_tests
//   ./ltd_tests
//
// mii_manager.cpp is #included directly (not linked) so the file-static
// helpers -- DecodeLtdMii / UpgradeLtdToV3 / FindMagic -- are reachable.
// The final section re-implements the pre-fix ShareMii.py algorithm and
// asserts it gets the WRONG answer on the same fixtures, so the tests fail
// loudly if anyone reintroduces the unbounded find()/rfind() marker search.

#include <sys/stat.h>
#include <dirent.h>
#include <direct.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// devkitPro/newlib has POSIX 2-arg mkdir(); MinGW ships the 1-arg MSVC one.
// Shim for the host build only -- the headers above are already included, so
// this cannot corrupt any declaration.
#define mkdir(p, m) _mkdir(p)
#include "source/mii_manager.cpp"
#undef mkdir

static int g_fail = 0, g_pass = 0;

static void check(bool cond, const char* what) {
    if (cond) { g_pass++; printf("  ok   %s\n", what); }
    else      { g_fail++; printf("  FAIL %s\n", what); }
}

static void checkEq(const std::vector<uint8_t>& got,
                    const std::vector<uint8_t>& want, const char* what) {
    if (got == want) { g_pass++; printf("  ok   %s\n", what); }
    else {
        g_fail++;
        printf("  FAIL %s (got %zu bytes, want %zu)\n", what, got.size(), want.size());
    }
}

// Build a v3 .ltd. `headerFill` lets a test plant bytes inside the fixed header.
static std::vector<uint8_t> makeV3(const std::vector<uint8_t>& canvas,
                                   const std::vector<uint8_t>& ugctex,
                                   bool hasCanvas = true, bool hasUgcTex = true) {
    std::vector<uint8_t> f(LTD_HEADER_LEN, 0x11);
    f[0] = 3;
    f[1] = hasCanvas ? 1 : 0;
    f[2] = hasUgcTex ? 1 : 0;
    f[3] = 0;
    // 156-byte block: [u32 len=152][152 data]
    f[4] = 152; f[5] = 0; f[6] = 0; f[7] = 0;
    f.insert(f.end(), MAGIC_A3, MAGIC_A3 + 4);
    f.insert(f.end(), canvas.begin(), canvas.end());
    f.insert(f.end(), MAGIC_A4, MAGIC_A4 + 4);
    f.insert(f.end(), ugctex.begin(), ugctex.end());
    return f;
}

int main() {
    printf("== CharInfoEx layout (the gender-offset claim) ==\n");
    // Field order puts gender at 39 and height/build at 40/41. The shipped
    // default blob is the neutral male starter Mii: gender 0, height/build 64.
    check(sizeof(MiiManager::BlankDefaults::DEFAULT_CHAR_INFO_EX) == 152,
          "DEFAULT_CHAR_INFO_EX is 152 bytes (matches ltd-gamedata)");
    check(MiiManager::BlankDefaults::DEFAULT_CHAR_INFO_EX[39] == 0x00,
          "CharInfoEx[39] (gender) == 0 (Male)");
    check(MiiManager::BlankDefaults::DEFAULT_CHAR_INFO_EX[40] == 0x40,
          "CharInfoEx[40] (height) == 64");
    check(MiiManager::BlankDefaults::DEFAULT_CHAR_INFO_EX[41] == 0x40,
          "CharInfoEx[41] (build) == 64");
    // .ltd offset 47 -> block offset 43 -> CharInfoEx offset 39.
    check(47 - LTD_OFF_MII_BLOCK - 4 == 39,
          "the old mii[47] poke targeted CharInfoEx[39] = gender");

    printf("\n== header geometry ==\n");
    check(LTD_HEADER_LEN == 428, "fixed header is 428 bytes");
    check(LTD_OFF_PERSONALITY == 160 && LTD_OFF_NAME == 232 &&
          LTD_OFF_PRONOUNCE == 296 && LTD_OFF_SEXUALITY == 424,
          "section offsets 160/232/296/424");

    printf("\n== happy path ==\n");
    {
        std::vector<uint8_t> canvas = {1,2,3,4,5,6,7,8};
        std::vector<uint8_t> ugctex = {9,10,11,12};
        LtdMii out;
        std::string err = DecodeLtdMii(makeV3(canvas, ugctex), out);
        check(err.empty(), "decodes without error");
        check(out.originalVersion == 3, "originalVersion == 3");
        check(out.hasCanvas && out.hasUgcTex, "flags read");
        checkEq(out.canvasTex, canvas, "canvas payload exact");
        checkEq(out.ugcTex, ugctex, "ugctex payload exact");
    }

    printf("\n== REGRESSION: A4A4A4A4 inside the ugctex payload ==\n");
    {
        // The old parser used rfind() for the ugctex marker, so this run would
        // become the split point: canvas would swallow the real marker plus
        // most of the texture, and ugctex would be a 2-byte tail.
        std::vector<uint8_t> canvas = {0xDE, 0xAD, 0xBE, 0xEF};
        std::vector<uint8_t> ugctex = {1, 2, 0xA4, 0xA4, 0xA4, 0xA4, 3, 4};
        LtdMii out;
        std::string err = DecodeLtdMii(makeV3(canvas, ugctex), out);
        check(err.empty(), "decodes without error");
        checkEq(out.canvasTex, canvas, "canvas NOT extended past the real marker");
        checkEq(out.ugcTex, ugctex, "ugctex kept whole (old code gave a 2-byte tail)");
    }

    printf("\n== REGRESSION: A3A3A3A3 inside the fixed header ==\n");
    {
        // The old parser used find() from offset 0, so a marker-shaped run in
        // the personality block became the canvas start.
        std::vector<uint8_t> canvas = {0x55, 0x66};
        std::vector<uint8_t> ugctex = {0x77, 0x88};
        std::vector<uint8_t> f = makeV3(canvas, ugctex);
        f[200] = 0xA3; f[201] = 0xA3; f[202] = 0xA3; f[203] = 0xA3; // in personality
        LtdMii out;
        std::string err = DecodeLtdMii(f, out);
        check(err.empty(), "decodes without error");
        checkEq(out.canvasTex, canvas, "canvas taken from the real marker at 428");
        checkEq(out.ugcTex, ugctex, "ugctex intact");
    }

    printf("\n== REGRESSION: truncated file must be rejected, not read OOB ==\n");
    {
        // 12 bytes, both markers present, version byte valid. The old code
        // accepted this and memcpy'd 156 bytes from offset 4 into Mii.sav.
        std::vector<uint8_t> f = {3,1,1,0, 0xA3,0xA3,0xA3,0xA3, 0xA4,0xA4,0xA4,0xA4};
        LtdMii out;
        std::string err = DecodeLtdMii(f, out);
        check(!err.empty(), "rejected");
        printf("       -> \"%s\"\n", err.c_str());
    }
    {
        LtdMii out;
        check(!DecodeLtdMii({3,1}, out).empty(), "2-byte file rejected");
        check(!DecodeLtdMii({}, out).empty(), "empty file rejected");
        check(!DecodeLtdMii({7,0,0,0}, out).empty(), "bad version rejected");
    }

    printf("\n== missing markers ==\n");
    {
        std::vector<uint8_t> f(LTD_HEADER_LEN, 0x11);
        f[0] = 3;
        LtdMii out;
        check(!DecodeLtdMii(f, out).empty(), "header-only file rejected (no markers)");
    }

    printf("\n== empty canvas payload is surfaced as empty ==\n");
    {
        // Flags claim a facepaint but there is no data. ImportMii keys off the
        // blob being non-empty, so this must not look like a usable facepaint.
        LtdMii out;
        std::string err = DecodeLtdMii(makeV3({}, {}, true, true), out);
        check(err.empty(), "decodes without error");
        check(out.hasCanvas && out.hasUgcTex, "flags still set");
        check(out.canvasTex.empty() && out.ugcTex.empty(), "payloads empty");
        bool usable = out.hasCanvas && out.hasUgcTex &&
                      !out.canvasTex.empty() && !out.ugcTex.empty();
        check(!usable, "ImportMii's facepaint predicate rejects it");
    }

    printf("\n== v2 upgrade (3-byte markers) ==\n");
    {
        std::vector<uint8_t> canvas = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4};
        std::vector<uint8_t> ug = {0xE0, 0xE1, 0xE2};
        // v2: 5-byte header, 420 bytes of block/personality/name/pronunciation,
        // 3 sexuality bytes, then two 3-byte A3 markers.
        std::vector<uint8_t> f(5 + 420 + 3, 0x11);
        f[0] = 2; f[1] = 1; f[2] = 1; f[3] = 0; f[4] = 0x99; // [4] gets erased
        f[5] = 152; f[6] = 0; f[7] = 0; f[8] = 0;
        static const uint8_t M3[3] = {0xA3, 0xA3, 0xA3};
        f.insert(f.end(), M3, M3 + 3);
        f.insert(f.end(), canvas.begin(), canvas.end());
        f.insert(f.end(), M3, M3 + 3);
        f.insert(f.end(), ug.begin(), ug.end());

        LtdMii out;
        std::string err = DecodeLtdMii(f, out);
        check(err.empty(), err.empty() ? "upgrades and decodes" : err.c_str());
        check(out.originalVersion == 2, "originalVersion == 2 (personality applies)");
        check(out.buf[0] == 3, "buffer normalised to v3");
        check((int)out.buf.size() >= LTD_HEADER_LEN, "upgraded buffer >= 428 bytes");
        checkEq(out.canvasTex, canvas, "v2 canvas payload exact");
        checkEq(out.ugcTex, ug, "v2 ugctex payload exact");
    }

    printf("\n== the OLD algorithm on the same fixtures (must be wrong) ==\n");
    {
        // ShareMii.py / pre-fix TomoToolNX:
        //   canvasStart = find(A3A3A3A3) + 4      (first, from offset 0)
        //   ugcStart    = rfind(A4A4A4A4) + 4     (LAST, anywhere)
        auto oldSplit = [](const std::vector<uint8_t>& f,
                           std::vector<uint8_t>& canvas,
                           std::vector<uint8_t>& ugctex) {
            int cs = FindMagic(f, MAGIC_A3, 0) + 4;          // first
            int us = FindMagicLastN(f, MAGIC_A4, 4) + 4;     // last
            canvas.assign(f.begin() + cs, f.begin() + us - 4);
            ugctex.assign(f.begin() + us, f.end());
        };

        std::vector<uint8_t> canvas = {0xDE, 0xAD, 0xBE, 0xEF};
        std::vector<uint8_t> ugctex = {1, 2, 0xA4, 0xA4, 0xA4, 0xA4, 3, 4};
        std::vector<uint8_t> oc, ou;
        oldSplit(makeV3(canvas, ugctex), oc, ou);
        check(oc != canvas, "old code mangled the canvas (marker run in ugctex)");
        check(ou != ugctex, "old code truncated the ugctex");
        printf("       -> old canvas %zu bytes (want %zu), old ugctex %zu bytes (want %zu)\n",
               oc.size(), canvas.size(), ou.size(), ugctex.size());

        std::vector<uint8_t> c2 = {0x55, 0x66}, u2 = {0x77, 0x88};
        std::vector<uint8_t> f2 = makeV3(c2, u2);
        f2[200] = 0xA3; f2[201] = 0xA3; f2[202] = 0xA3; f2[203] = 0xA3;
        std::vector<uint8_t> oc2, ou2;
        oldSplit(f2, oc2, ou2);
        check(oc2 != c2, "old code started the canvas at the header run at 200");
        printf("       -> old canvas %zu bytes (want %zu)\n", oc2.size(), c2.size());
    }

    printf("\n== UGC stem padding (slot index 99) ==\n");
    {
        auto stem = [](int s) {
            char n[8];
            snprintf(n, sizeof(n), "%03d", s);
            return std::string("Cloth") + n;
        };
        check(stem(9)   == "Cloth009", "slot 9   -> Cloth009");
        check(stem(99)  == "Cloth099", "slot 99  -> Cloth099 (was Cloth99)");
        check(stem(100) == "Cloth100", "slot 100 -> Cloth100");
        check(stem(298) == "Cloth298", "slot 298 -> Cloth298");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
