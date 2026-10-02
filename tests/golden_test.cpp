// Golden byte-vector tests.
//
// Every expected byte stream below was derived BY HAND from the QOI format
// specification and by tracing legacy/ada/src/qoi.adb (Encode, Decode). None of them was
// produced by running the C++ encoder, so they are an independent oracle.
//
// Notation used in the derivations:
//   hash(r,g,b,a) = (3r + 5g + 7b + 11a) mod 64   (Ada: Hash, 8-bit modular, then
//                   "mod Index'Length" = mod 64; 256 is a multiple of 64, so the
//                   8-bit wrap does not change the result)
//   For a 3-channel image alpha is fixed at 255, and 11*255 = 2805 = 53 (mod 64),
//   so hash3(r,g,b) = (3r + 5g + 7b + 53) mod 64.
//   The encoder starts with the previous pixel (0,0,0,255) and an index table
//   whose 64 entries are all (0,0,0,0).
//   Opcodes: RUN 11xxxxxx (xxxxxx = run-1), INDEX 00xxxxxx, DIFF 01rrggbb (bias 2),
//   LUMA 10gggggg + rrrrbbbb (biases 32 and 8), RGB FE r g b, RGBA FF r g b a.
//   Encoder precedence (Ada lines 173-247): equal to previous -> run; else flush a
//   pending run, then INDEX hit, else (store in the table) alpha equal -> DIFF,
//   LUMA, RGB, alpha different -> RGBA.
//
// A "pixel that cannot hit the index" argument used repeatedly: the table only
// ever holds (0,0,0,0) and pixels that already occurred earlier in the image. A
// pixel that has never occurred and is not (0,0,0,0) therefore cannot be an
// INDEX hit. A 3-channel pixel always has alpha 255, so it is never (0,0,0,0).
//
// Each golden image is checked in both directions: the encoder must produce the
// golden bytes, and the decoder must turn the golden bytes back into the pixels.
#include <qoi/qoi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_util.hpp"

namespace {

using qoi_test::Bytes;
using qoi_test::BytesEqual;
using qoi_test::DescEq;
using qoi_test::make_desc;

constexpr std::uint64_t kStorageLast = 0x7FFFFFFFFFFFFFFFULL;  // Storage_Count'Last
constexpr std::uint64_t kMaxDim = 0x7FFFFFFFULL;               // Integer_32'Last

// Brace lists of 64-bit values; avoids deduction trouble between uint64_t and
// unsigned long long literals on platforms where they are distinct types.
using U64List = std::initializer_list<std::uint64_t>;

qoi::Desc rgb_desc(std::uint64_t width, std::uint64_t height,
                   qoi::Colorspace cs = qoi::Colorspace::SRGB) {
    return make_desc(width, height, 3, cs);
}

qoi::Desc rgba_desc(std::uint64_t width, std::uint64_t height,
                    qoi::Colorspace cs = qoi::Colorspace::SRGB) {
    return make_desc(width, height, 4, cs);
}

std::uint8_t colorspace_byte(const qoi::Desc& desc) {
    return static_cast<std::uint8_t>(desc.colorspace);
}

// Full golden check. `chunks` is the expected byte string between the 14-byte
// header and the 8-byte padding (the header and padding are built here from the
// descriptor, independently of the codec).
void check_golden(const qoi::Desc& desc, const Bytes& pix, const Bytes& chunks) {
    ASSERT_EQ(pix.size(), qoi_test::byte_count(desc)) << "malformed test vector";
    const Bytes expected = qoi_test::stream(
        static_cast<std::uint32_t>(desc.width), static_cast<std::uint32_t>(desc.height),
        static_cast<std::uint8_t>(desc.channels), colorspace_byte(desc), chunks);

    // Encode into a buffer of exactly the worst-case size, pre-filled with a
    // sentinel so that stray writes are visible.
    const std::size_t worst = static_cast<std::size_t>(qoi::encode_worst_case(desc));
    ASSERT_GE(worst, expected.size()) << "golden stream is longer than the worst case";
    Bytes out(worst, 0xAA);
    const std::size_t written = qoi::encode(pix, desc, out);
    ASSERT_EQ(written, expected.size());
    EXPECT_TRUE(BytesEqual(expected, out, written));
    EXPECT_TRUE(qoi_test::all_equal(out, written, out.size(), 0xAA))
        << "encoder wrote past the reported size";

    // The header alone is also visible through get_desc.
    qoi::Desc parsed = qoi_test::poisoned_desc();
    qoi::get_desc(expected, parsed);
    EXPECT_TRUE(DescEq(desc, parsed));

    // Decode the golden bytes into an exact-size buffer and compare the pixels.
    Bytes decoded(pix.size(), 0x55);
    qoi::Desc decoded_desc = qoi_test::poisoned_desc();
    const std::size_t decoded_size = qoi::decode(expected, decoded_desc, decoded);
    ASSERT_EQ(decoded_size, pix.size());
    EXPECT_TRUE(DescEq(desc, decoded_desc));
    EXPECT_TRUE(BytesEqual(pix, decoded));
}

// A repeated pixel value: `count` pixels of (r,g,b).
Bytes repeat_rgb(std::size_t count, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Bytes pix;
    for (std::size_t i = 0; i < count; ++i) {
        pix.push_back(r);
        pix.push_back(g);
        pix.push_back(b);
    }
    return pix;
}

// Two-pixel 3-channel image used by the DIFF/LUMA/RGB boundary tables. The first
// pixel is always B = (100,100,100):
//   hash3(B) = 300 + 500 + 700 + 53 = 1553 = 17 (mod 64); slot 17 is empty, and the
//   alpha (255) equals the previous alpha, so against the initial previous pixel
//   (0,0,0,255) the deltas are (100,100,100): no DIFF, LUMA needs dg in -32..31 ->
//   RGB: FE 64 64 64.
// The second pixel P1 never occurred before, so it cannot be an INDEX hit and its
// opcode is decided by the deltas against B alone.
struct PairCase {
    int r;
    int g;
    int b;
    Bytes chunk;  // expected bytes for the second pixel
};

void check_pair_cases(const std::vector<PairCase>& cases) {
    for (const PairCase& c : cases) {
        SCOPED_TRACE("second pixel (" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                     std::to_string(c.b) + ") expected chunk " + qoi_test::hex(c.chunk));
        const Bytes pix = {100, 100, 100, qoi_test::u8(c.r), qoi_test::u8(c.g), qoi_test::u8(c.b)};
        Bytes chunks = {0xFE, 0x64, 0x64, 0x64};
        chunks.insert(chunks.end(), c.chunk.begin(), c.chunk.end());
        check_golden(rgb_desc(2, 1), pix, chunks);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Constants and header
// ---------------------------------------------------------------------------

TEST(Golden, HeaderAndPaddingConstants) {
    EXPECT_EQ(qoi::QOI_HEADER_SIZE, 14u);
    ASSERT_EQ(qoi::QOI_PADDING.size(), 8u);
    const Bytes expected = {0, 0, 0, 0, 0, 0, 0, 1};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(qoi::QOI_PADDING[i], expected[i]) << "padding byte " << i;
    }
    EXPECT_EQ(static_cast<unsigned>(qoi::Colorspace::SRGB), 0u);
    EXPECT_EQ(static_cast<unsigned>(qoi::Colorspace::SRGB_Linear_Alpha), 1u);
}

// 1x1 RGB sRGB black pixel, written out in full:
//   magic 71 6F 69 66 ("qoif"), width 00 00 00 01, height 00 00 00 01,
//   channels 03, colorspace 00
//   pixel (0,0,0) equals the initial previous pixel (0,0,0,255): run of 1 -> C0
//   padding 00 00 00 00 00 00 00 01
TEST(Golden, HeaderRgbSrgbFullStream) {
    const Bytes expected = {0x71, 0x6F, 0x69, 0x66, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                            0x03, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    const qoi::Desc desc = rgb_desc(1, 1, qoi::Colorspace::SRGB);
    const Bytes pix = {0, 0, 0};
    Bytes out(qoi::encode_worst_case(desc), 0xAA);
    const std::size_t n = qoi::encode(pix, desc, out);
    ASSERT_EQ(n, expected.size());
    EXPECT_TRUE(BytesEqual(expected, out, n));
    check_golden(desc, pix, {0xC0});
}

// Same, 4 channels and colorspace byte 01: only bytes 12 and 13 change (04 01).
// Pixel (0,0,0,255) equals the initial previous pixel -> C0.
TEST(Golden, HeaderRgbaLinearFullStream) {
    const Bytes expected = {0x71, 0x6F, 0x69, 0x66, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                            0x04, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    const qoi::Desc desc = rgba_desc(1, 1, qoi::Colorspace::SRGB_Linear_Alpha);
    const Bytes pix = {0, 0, 0, 255};
    Bytes out(qoi::encode_worst_case(desc), 0xAA);
    const std::size_t n = qoi::encode(pix, desc, out);
    ASSERT_EQ(n, expected.size());
    EXPECT_TRUE(BytesEqual(expected, out, n));
    check_golden(desc, pix, {0xC0});
}

// All four channel/colorspace combinations of the header, each as a 1x1 black
// (initial-previous) pixel: header bytes 12 and 13 are channels and colorspace.
TEST(Golden, HeaderChannelsAndColorspaceBytes) {
    check_golden(rgb_desc(1, 1, qoi::Colorspace::SRGB), {0, 0, 0}, {0xC0});
    check_golden(rgb_desc(1, 1, qoi::Colorspace::SRGB_Linear_Alpha), {0, 0, 0}, {0xC0});
    check_golden(rgba_desc(1, 1, qoi::Colorspace::SRGB), {0, 0, 0, 255}, {0xC0});
    check_golden(rgba_desc(1, 1, qoi::Colorspace::SRGB_Linear_Alpha), {0, 0, 0, 255}, {0xC0});
}

// Width is stored big-endian. 258 = 0x0102 -> 00 00 01 02. 258 black pixels:
// four full runs of 62 (4*62 = 248 -> FD each, the run counter reaches 62), then
// the remaining 10 pixels are flushed at the last pixel as run 10 -> C0|9 = C9.
TEST(Golden, HeaderWidthIsBigEndian258) {
    const Bytes pix = repeat_rgb(258, 0, 0, 0);
    check_golden(rgb_desc(258, 1, qoi::Colorspace::SRGB_Linear_Alpha), pix,
                 {0xFD, 0xFD, 0xFD, 0xFD, 0xC9});
    EXPECT_EQ(qoi_test::header(258, 1, 3, 1), (Bytes{0x71, 0x6F, 0x69, 0x66, 0x00, 0x00, 0x01, 0x02,
                                                     0x00, 0x00, 0x00, 0x01, 0x03, 0x01}));
}

// Height is stored big-endian too: 1x258 gives height bytes 00 00 01 02, and the
// same chunk stream as above (the pixel order does not matter for a flat image).
TEST(Golden, HeaderHeightIsBigEndian258) {
    const Bytes pix = repeat_rgb(258, 0, 0, 0);
    check_golden(rgb_desc(1, 258), pix, {0xFD, 0xFD, 0xFD, 0xFD, 0xC9});
    EXPECT_EQ(qoi_test::header(1, 258, 3, 0), (Bytes{0x71, 0x6F, 0x69, 0x66, 0x00, 0x00, 0x00, 0x01,
                                                     0x00, 0x00, 0x01, 0x02, 0x03, 0x00}));
}

// Width 65536 = 0x00010000 -> 00 01 00 00 (third byte of the big-endian field).
// 65536 black pixels = 1057 * 62 + 2 (1057*62 = 65534): 1057 runs of 62 (FD), then
// the last 2 pixels flush at the final pixel as run 2 -> C1.
TEST(Golden, HeaderWidthThirdByte65536) {
    const Bytes pix = repeat_rgb(65536, 0, 0, 0);
    Bytes chunks(1057, 0xFD);
    chunks.push_back(0xC1);
    check_golden(rgb_desc(65536, 1), pix, chunks);
    EXPECT_EQ(qoi_test::header(65536, 1, 3, 0), (Bytes{0x71, 0x6F, 0x69, 0x66, 0x00, 0x01, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x00}));
}

// The last 8 bytes of every encoded stream are 00 00 00 00 00 00 00 01, also when
// the stream ends in a multi-byte chunk (RGB: 1x1 (10,20,30)).
TEST(Golden, PaddingEndsEveryStream) {
    const qoi::Desc desc = rgb_desc(1, 1);
    const Bytes pix = {10, 20, 30};
    Bytes out(qoi::encode_worst_case(desc), 0xAA);
    const std::size_t n = qoi::encode(pix, desc, out);
    ASSERT_EQ(n, 26u);  // 14 + 4 + 8
    const Bytes tail(out.begin() + 18, out.begin() + 26);
    EXPECT_TRUE(BytesEqual(Bytes{0, 0, 0, 0, 0, 0, 0, 1}, tail));
}

// ---------------------------------------------------------------------------
// RGB / RGBA opcodes
// ---------------------------------------------------------------------------

// 1x1 RGB (10,20,30): hash3 = 30 + 100 + 210 + 53 = 393 = 9 (mod 64), slot 9 holds
// the zero pixel -> no INDEX; alpha equal; deltas (10,20,30): not DIFF; dg = 20 is
// in -32..31 but dr-dg = -10 is outside -8..7 -> not LUMA -> RGB: FE 0A 14 1E.
TEST(Golden, RgbOpForFirstPixel) {
    check_golden(rgb_desc(1, 1), {10, 20, 30}, {0xFE, 0x0A, 0x14, 0x1E});
}

// 1x1 RGBA (10,20,30,40): hash = 30 + 100 + 210 + 440 = 780 = 12 (mod 64), slot empty;
// alpha 40 differs from the initial previous alpha 255 -> RGBA: FF 0A 14 1E 28.
TEST(Golden, RgbaOpForFirstPixelWithAlphaChange) {
    check_golden(rgba_desc(1, 1), {10, 20, 30, 40}, {0xFF, 0x0A, 0x14, 0x1E, 0x28});
}

// Alpha handling over three RGBA pixels:
//   P0 (10,20,30,255): alpha equals the initial previous alpha 255, so the colour is
//      coded like a 3-channel pixel: RGB FE 0A 14 1E. hash = 30+100+210+2805 = 3145 =
//      9 (mod 64) -> slot 9 := P0.
//   P1 (10,20,30,128): hash = 340 + 1408 = 1748 = 20 (mod 64), slot empty; alpha 128 !=
//      255 -> RGBA FF 0A 14 1E 80 (the colour is unchanged but alpha forces RGBA).
//   P2 (10,20,30,255): hash 9, slot 9 == P2 -> INDEX 9 = 09, even though the alpha
//      differs from the previous pixel (INDEX is tested before the alpha rule).
TEST(Golden, AlphaChangeEmitsRgbaAndIndexStillWins) {
    check_golden(rgba_desc(3, 1), {10, 20, 30, 255, 10, 20, 30, 128, 10, 20, 30, 255},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xFF, 0x0A, 0x14, 0x1E, 0x80, 0x09});
}

// P0 (0,0,0,255) equals the initial previous pixel: run of 1 -> C0. P1 (0,0,0,254):
// hash = 11*254 = 2794 = 42 (mod 64), slot empty; alpha differs by one ->
// RGBA FF 00 00 00 FE. The pending run is flushed first.
TEST(Golden, SingleAlphaStepEmitsRgba) {
    check_golden(rgba_desc(2, 1), {0, 0, 0, 255, 0, 0, 0, 254},
                 {0xC0, 0xFF, 0x00, 0x00, 0x00, 0xFE});
}

// P0 (0,0,0,0): hash 0, slot 0 holds the zero-initialised (0,0,0,0) -> INDEX 0 = 00
// (and the pixel differs from the initial previous pixel (0,0,0,255) in alpha).
// P1 (0,0,0,255): hash = 2805 = 53, slot empty; alpha 255 != previous alpha 0 ->
// RGBA FF 00 00 00 FF.
TEST(Golden, ZeroPixelThenOpaqueBlackEmitsIndexZeroThenRgba) {
    check_golden(rgba_desc(2, 1), {0, 0, 0, 0, 0, 0, 0, 255}, {0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF});
}

// A 3-channel image never produces an RGBA chunk (alpha is fixed at 255). Four
// pixels that all differ widely from their predecessors, each pixel new (no INDEX):
//   (10,20,30)  vs (0,0,0):    deltas (10,20,30), dr-dg = -10 -> RGB FE 0A 14 1E
//   (200,10,90) vs previous:   deltas (190,-10,60)            -> RGB FE C8 0A 5A
//   (5,250,5)   vs previous:   deltas (-195,240,-85)          -> RGB FE 05 FA 05
//   (0,0,0)     vs previous:   deltas (-5,-250,-5), vg out    -> RGB FE 00 00 00
// (hash3(0,0,0) = 53. Slot 53 holds (200,10,90,255), which also hashes to 53
// (600+50+630+53 = 1333 = 53 mod 64), and that is != (0,0,0,255): no INDEX hit.)
TEST(Golden, ThreeChannelImageNeverEmitsRgba) {
    check_golden(rgb_desc(4, 1), {10, 20, 30, 200, 10, 90, 5, 250, 5, 0, 0, 0},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xFE, 0xC8, 0x0A, 0x5A, 0xFE, 0x05, 0xFA, 0x05, 0xFE,
                  0x00, 0x00, 0x00});
}

// ---------------------------------------------------------------------------
// First pixel versus the initial previous pixel (0,0,0,255) and the zero table
// ---------------------------------------------------------------------------

// 3 channels: first pixel (0,0,0) is the initial previous pixel (alpha 255) -> run of 1.
TEST(Golden, FirstPixelEqualsInitialPreviousRgb) {
    check_golden(rgb_desc(1, 1), {0, 0, 0}, {0xC0});
}

// 4 channels: first pixel (0,0,0,255) is the initial previous pixel -> run of 1.
TEST(Golden, FirstPixelEqualsInitialPreviousRgba) {
    check_golden(rgba_desc(1, 1), {0, 0, 0, 255}, {0xC0});
}

// 4 channels: first pixel (0,0,0,0) is NOT the initial previous pixel (alpha 0 vs 255),
// so no run starts. hash = 0 and slot 0 is the zero-initialised (0,0,0,0) -> INDEX 0 = 00.
TEST(Golden, FirstPixelZeroRgbaYieldsIndexZero) {
    check_golden(rgba_desc(1, 1), {0, 0, 0, 0}, {0x00});
}

// Two (0,0,0,0) pixels: INDEX 00, then the second pixel equals the previous one
// (previous := (0,0,0,0) after the first) -> run of 1 flushed at the last pixel: C0.
TEST(Golden, ZeroRgbaPixelsIndexThenRun) {
    check_golden(rgba_desc(2, 1), {0, 0, 0, 0, 0, 0, 0, 0}, {0x00, 0xC0});
    // Three pixels: run of 2 -> C1.
    check_golden(rgba_desc(3, 1), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0x00, 0xC1});
}

// (0,0,0) then (1,1,1): the first is a pending run of 1, flushed (C0) when the second
// pixel differs; (1,1,1) is new, deltas (1,1,1) -> DIFF 01 11 11 11 = 0x7F.
TEST(Golden, RunFlushedBeforeNewPixel) {
    check_golden(rgb_desc(2, 1), {0, 0, 0, 1, 1, 1}, {0xC0, 0x7F});
}

// ---------------------------------------------------------------------------
// INDEX
// ---------------------------------------------------------------------------

// A=(10,20,30) hash3 = 9. B=(40,50,60): hash3 = 120+250+420+53 = 843 = 11 (mod 64).
//   A: slot 9 empty -> RGB FE 0A 14 1E, slot 9 := A.
//   B: slot 11 empty; deltas (30,30,30): not DIFF; vg=30 in range, dr-dg = 0, db-dg = 0
//      -> LUMA: byte1 = 10 000000 | (30+32 = 62 = 0x3E) = 0xBE, byte2 = (0+8)<<4 | (0+8)
//      = 0x88. slot 11 := B.
//   A: slot 9 == A -> INDEX 9 = 09.
TEST(Golden, IndexHitAfterRgbAndLuma) {
    check_golden(rgb_desc(3, 1), {10, 20, 30, 40, 50, 60, 10, 20, 30},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xBE, 0x88, 0x09});
}

// INDEX beats DIFF. B0=(100,100,100): hash3 = 1553 = 17 -> RGB FE 64 64 64. B1=
// (101,101,101): hash3 = 15*101 + 53 = 1568 = 32 -> DIFF (+1,+1,+1) = 0x7F. B0 again:
// slot 17 == B0 -> INDEX 17 = 0x11, although the delta (-1,-1,-1) would also fit DIFF.
// B1 again: slot 32 == B1 (stored when it was coded as DIFF: the table is updated for
// every non-INDEX pixel) -> INDEX 32 = 0x20.
TEST(Golden, IndexTakesPrecedenceOverDiffAndDiffPixelsAreStored) {
    check_golden(rgb_desc(4, 1), {100, 100, 100, 101, 101, 101, 100, 100, 100, 101, 101, 101},
                 {0xFE, 0x64, 0x64, 0x64, 0x7F, 0x11, 0x20});
}

// RGBA pixels are stored in the table as well. Q0=(10,20,30,40): hash = 780 = 12 ->
// RGBA FF 0A 14 1E 28. Q1=(1,2,3,4): hash = 3+10+21+44 = 78 = 14, alpha 4 != 40 ->
// RGBA FF 01 02 03 04. Q0 again: slot 12 == Q0 -> INDEX 12 = 0C (the alpha differs
// from the previous pixel, but INDEX is tested first).
TEST(Golden, IndexHitAfterRgba) {
    check_golden(rgba_desc(3, 1), {10, 20, 30, 40, 1, 2, 3, 4, 10, 20, 30, 40},
                 {0xFF, 0x0A, 0x14, 0x1E, 0x28, 0xFF, 0x01, 0x02, 0x03, 0x04, 0x0C});
}

// A pixel that collides with an earlier one overwrites its slot and the earlier pixel
// is then no longer reachable through INDEX. A=(10,20,30) hash3 = 9. C=(74,20,30):
// 3*74 = 222, hash3 = 222+100+210+53 = 585 = 9 (mod 64): same slot as A.
//   A: RGB FE 0A 14 1E, slot 9 := A.
//   C: slot 9 holds A != C -> slot 9 := C; deltas (64,0,0): dr-dg = 64 -> RGB FE 4A 14 1E.
//   A: slot 9 holds C != A -> NO index hit; slot 9 := A; deltas (-64,0,0) -> RGB FE 0A 14 1E.
//   A: equals the previous pixel -> run of 1 at the last pixel: C0.
TEST(Golden, IndexSlotOverwrittenByCollidingPixel) {
    check_golden(rgb_desc(4, 1), {10, 20, 30, 74, 20, 30, 10, 20, 30, 10, 20, 30},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xFE, 0x4A, 0x14, 0x1E, 0xFE, 0x0A, 0x14, 0x1E, 0xC0});
}

// Same collision, but the overwriting pixel is then found through INDEX.
// A=(10,20,30), C=(74,20,30) (both slot 9), B=(75,20,30): hash3 = 225+100+210+53 = 588 =
// 12 (mod 64).
//   A: RGB FE 0A 14 1E; C: RGB FE 4A 14 1E (slot 9 := C).
//   B: slot 12 empty, deltas vs C (1,0,0) -> DIFF 01 11 10 10 = 0x7A.
//   C: slot 9 == C -> INDEX 9 = 09.
TEST(Golden, IndexHitOnOverwritingPixel) {
    check_golden(rgb_desc(4, 1), {10, 20, 30, 74, 20, 30, 75, 20, 30, 74, 20, 30},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xFE, 0x4A, 0x14, 0x1E, 0x7A, 0x09});
}

// ---------------------------------------------------------------------------
// DIFF boundaries: each channel delta -2 and +1, base pixel B = (100,100,100)
//   chunk = 01 | (dr+2)<<4 | (dg+2)<<2 | (db+2)
// ---------------------------------------------------------------------------

TEST(Golden, DiffBoundariesPerChannel) {
    check_pair_cases({
        {98, 98, 98, {0x40}},     // (-2,-2,-2): 01 00 00 00
        {101, 101, 101, {0x7F}},  // (+1,+1,+1): 01 11 11 11
        {98, 100, 100, {0x4A}},   // (-2, 0, 0): 01 00 10 10
        {101, 100, 100, {0x7A}},  // (+1, 0, 0): 01 11 10 10
        {100, 98, 100, {0x62}},   // ( 0,-2, 0): 01 10 00 10
        {100, 101, 100, {0x6E}},  // ( 0,+1, 0): 01 10 11 10
        {100, 100, 98, {0x68}},   // ( 0, 0,-2): 01 10 10 00
        {100, 100, 101, {0x6B}},  // ( 0, 0,+1): 01 10 10 11
        {98, 101, 98, {0x4C}},    // (-2,+1,-2): 01 00 11 00
        {101, 98, 101, {0x73}},   // (+1,-2,+1): 01 11 00 11
    });
}

// 4 channels, alpha constant: alpha only matters through "equal to previous".
//   P0 (100,100,100,7): hash = 300+500+700+77 = 1577 = 41; alpha 7 != 255 -> RGBA
//      FF 64 64 64 07.
//   P1 (101,99,100,7): new pixel, alpha equal, deltas (+1,-1,0) -> DIFF
//      01 11 01 10 = 0x76.
//   P2 (101,99,100,8): new pixel, alpha 8 != 7 -> RGBA FF 65 63 64 08.
//   P3 (111,109,109,8): new pixel, alpha equal, deltas (10,10,9): not DIFF; dg = 10,
//      dr-dg = 0, db-dg = -1 -> LUMA: byte1 = 0x80 | (10+32 = 42 = 0x2A) = 0xAA,
//      byte2 = (0+8)<<4 | (-1+8) = 0x87.
TEST(Golden, DiffAndLumaWithFourChannelsAndConstantAlpha) {
    check_golden(rgba_desc(4, 1),
                 {100, 100, 100, 7, 101, 99, 100, 7, 101, 99, 100, 8, 111, 109, 109, 8},
                 {0xFF, 0x64, 0x64, 0x64, 0x07, 0x76, 0xFF, 0x65, 0x63, 0x64, 0x08, 0xAA, 0x87});
}

// ---------------------------------------------------------------------------
// LUMA boundaries, base B = (100,100,100); vg = dg, vg_r = dr-dg, vg_b = db-dg
//   chunk = 10 | (dg+32) , (vg_r+8)<<4 | (vg_b+8)
// ---------------------------------------------------------------------------

TEST(Golden, LumaBoundaries) {
    check_pair_cases({
        // dg = -32 (lowest), vg_r = vg_b = 0: 10 000000, 1000 1000
        {68, 68, 68, {0x80, 0x88}},
        // dg = +31 (highest): 10 111111, 1000 1000
        {131, 131, 131, {0xBF, 0x88}},
        // dg = 0, vg_r = vg_b = -8 (lowest): 10 100000, 0000 0000
        {92, 100, 92, {0xA0, 0x00}},
        // dg = 0, vg_r = vg_b = +7 (highest): 10 100000, 1111 1111
        {107, 100, 107, {0xA0, 0xFF}},
        // dg = 5, vg_r = -8, vg_b = +7 (deltas -3,5,12): 10 100101, 0000 1111
        {97, 105, 112, {0xA5, 0x0F}},
        // dg = -5, vg_r = +7, vg_b = -8 (deltas 2,-5,-13): 10 011011, 1111 0000
        {102, 95, 87, {0x9B, 0xF0}},
        // dg = 31 with vg_r = vg_b = 7 (deltas 38,31,38): 10 111111, 1111 1111
        {138, 131, 138, {0xBF, 0xFF}},
        // dg = -32 with vg_r = vg_b = -8 (deltas -40,-32,-40): 10 000000, 0000 0000
        {60, 68, 60, {0x80, 0x00}},
    });
}

// Deltas just outside DIFF (-3 or +2 on one channel) that still fit LUMA.
TEST(Golden, JustOutsideDiffFallsThroughToLuma) {
    check_pair_cases({
        // dr = -3: vg_r = -3 -> 5<<4 | 8
        {97, 100, 100, {0xA0, 0x58}},
        // dr = +2: vg_r = 2 -> 10<<4 | 8
        {102, 100, 100, {0xA0, 0xA8}},
        // dg = -3: vg = -3 -> 29 = 0x1D, vg_r = vg_b = +3 -> 11<<4 | 11
        {100, 97, 100, {0x9D, 0xBB}},
        // dg = +2: vg = 2 -> 34 = 0x22, vg_r = vg_b = -2 -> 6<<4 | 6
        {100, 102, 100, {0xA2, 0x66}},
        // db = -3: vg_b = -3 -> 8<<4 | 5
        {100, 100, 97, {0xA0, 0x85}},
        // db = +2: vg_b = 2 -> 8<<4 | 10
        {100, 100, 102, {0xA0, 0x8A}},
    });
}

// Values just outside the LUMA ranges fall through to RGB.
TEST(Golden, JustOutsideLumaFallsThroughToRgb) {
    check_pair_cases({
        {132, 132, 132, {0xFE, 0x84, 0x84, 0x84}},  // dg = +32
        {67, 67, 67, {0xFE, 0x43, 0x43, 0x43}},     // dg = -33
        {108, 100, 100, {0xFE, 0x6C, 0x64, 0x64}},  // dr-dg = +8
        {91, 100, 100, {0xFE, 0x5B, 0x64, 0x64}},   // dr-dg = -9
        {100, 100, 108, {0xFE, 0x64, 0x64, 0x6C}},  // db-dg = +8
        {100, 100, 91, {0xFE, 0x64, 0x64, 0x5B}},   // db-dg = -9
    });
}

// ---------------------------------------------------------------------------
// RUN
// ---------------------------------------------------------------------------

// Runs of black pixels (equal to the initial previous pixel). A run is flushed when
// the counter reaches 62 (chunk C0|61 = FD) or at the final pixel (chunk C0|(n-1)).
TEST(Golden, RunLength1) { check_golden(rgb_desc(1, 1), repeat_rgb(1, 0, 0, 0), {0xC0}); }

TEST(Golden, RunLength2) { check_golden(rgb_desc(2, 1), repeat_rgb(2, 0, 0, 0), {0xC1}); }

// 61 pixels: the counter reaches 61 at the last pixel -> C0|60 = FC.
TEST(Golden, RunLength61) { check_golden(rgb_desc(61, 1), repeat_rgb(61, 0, 0, 0), {0xFC}); }

// 62 pixels: the counter reaches 62 (also the last pixel; one push only) -> FD.
TEST(Golden, RunLength62) { check_golden(rgb_desc(62, 1), repeat_rgb(62, 0, 0, 0), {0xFD}); }

// 63 pixels: flush at 62 -> FD, the 63rd pixel starts a new run of 1, flushed at the
// last pixel -> C0. Shaped 7x9 to exercise a non-trivial width/height as well.
TEST(Golden, RunLength63) {
    check_golden(rgb_desc(63, 1), repeat_rgb(63, 0, 0, 0), {0xFD, 0xC0});
    check_golden(rgb_desc(7, 9), repeat_rgb(63, 0, 0, 0), {0xFD, 0xC0});
}

// 124 = 2 * 62 pixels: two flushes at 62, the second coincides with the last pixel.
TEST(Golden, RunLength124) {
    check_golden(rgb_desc(124, 1), repeat_rgb(124, 0, 0, 0), {0xFD, 0xFD});
}

// 125 pixels: FD, FD, then a run of 1 at the last pixel.
TEST(Golden, RunLength125) {
    check_golden(rgb_desc(125, 1), repeat_rgb(125, 0, 0, 0), {0xFD, 0xFD, 0xC0});
}

// The same lengths with 4 channels (pixel (0,0,0,255) equals the initial previous one).
TEST(Golden, RunLengthsFourChannels) {
    const auto black_rgba = [](std::size_t count) {
        Bytes pix;
        for (std::size_t i = 0; i < count; ++i) {
            pix.insert(pix.end(), {0, 0, 0, 255});
        }
        return pix;
    };
    check_golden(rgba_desc(1, 1), black_rgba(1), {0xC0});
    check_golden(rgba_desc(62, 1), black_rgba(62), {0xFD});
    check_golden(rgba_desc(63, 1), black_rgba(63), {0xFD, 0xC0});
}

// A run after a coded pixel. X=(10,20,30) -> RGB FE 0A 14 1E (as above), then the
// repeats of X are runs.
TEST(Golden, RunAfterCodedPixel) {
    // X, X -> run of 1 at the last pixel.
    check_golden(rgb_desc(2, 1), {10, 20, 30, 10, 20, 30}, {0xFE, 0x0A, 0x14, 0x1E, 0xC0});
    // X, X, X -> run of 2.
    check_golden(rgb_desc(3, 1), {10, 20, 30, 10, 20, 30, 10, 20, 30},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xC1});
    // X followed by 62 repeats: the counter reaches 62 at the last pixel -> FD.
    check_golden(rgb_desc(63, 1), repeat_rgb(63, 10, 20, 30), {0xFE, 0x0A, 0x14, 0x1E, 0xFD});
}

// Flush at 62 with a remainder run, then a different pixel. 1 + 63 copies of
// X=(10,20,30), then Y=(11,21,31):
//   X: RGB FE 0A 14 1E. The first 62 repeats fill a run -> FD (flush at 62). The 63rd
//   repeat starts a new run of 1, flushed when Y differs -> C0. Y is new, deltas
//   (1,1,1) -> DIFF 0x7F.
TEST(Golden, RunFlushAt62ThenRemainderRunThenNewPixel) {
    Bytes pix = repeat_rgb(64, 10, 20, 30);
    pix.insert(pix.end(), {11, 21, 31});
    check_golden(rgb_desc(65, 1), pix, {0xFE, 0x0A, 0x14, 0x1E, 0xFD, 0xC0, 0x7F});
}

// X, X, Y: the run of 1 is flushed (C0) before Y is coded as DIFF.
TEST(Golden, RunFlushedThenDiff) {
    check_golden(rgb_desc(3, 1), {10, 20, 30, 10, 20, 30, 11, 21, 31},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xC0, 0x7F});
}

// A run following an INDEX hit. A=(10,20,30), B=(40,50,60), A, A, A:
//   FE 0A 14 1E (A), BE 88 (B as LUMA), 09 (A via INDEX 9), then two repeats of A form
//   a run of 2 flushed at the last pixel -> C1.
TEST(Golden, RunAfterIndexHit) {
    check_golden(rgb_desc(5, 1), {10, 20, 30, 40, 50, 60, 10, 20, 30, 10, 20, 30, 10, 20, 30},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xBE, 0x88, 0x09, 0xC1});
}

// Run at the final pixel of a 2D image: 3x2 image, flat (0,0,0) -> one run of 6 -> C5.
TEST(Golden, RunEndingAtFinalPixel2D) {
    check_golden(rgb_desc(3, 2), repeat_rgb(6, 0, 0, 0), {0xC5});
}

// ---------------------------------------------------------------------------
// WRAP-AROUND deltas (milestone design decision 6).
//
// The Ada encoder (legacy/ada/src/qoi.adb lines 206-214) computes VR, VG and VB as UNBOUNDED
// Integer differences and range-tests them, so a step 255 -> 0 is -255 and selects
// RGB. The reference encoder qoi.h stores the deltas in `signed char`, which wraps:
// 255 -> 0 is +1 and it emits DIFF (or LUMA). Both streams decode to the same pixels.
//
// These vectors pin the ADA-IDENTICAL behaviour. The single internal helper
// `compute_deltas` in src/qoi.cpp is the only place that decides this. If it is ever
// switched to signed-char wrap-around (qoi.h-identical output), THESE are the golden
// vectors to update: each test below names, in a comment, the bytes qoi.h would emit.
// Decoding is not affected (the decoder wraps modulo 256 in both cases).
// ---------------------------------------------------------------------------

// r: 255 -> 0. P0 (255,100,100): vs (0,0,0) deltas (255,100,100): RGB FE FF 64 64.
// P1 (0,100,100): unbounded dr = -255 -> not DIFF, vg_r = -255 -> not LUMA -> RGB
// FE 00 64 64. (qoi.h: dr wraps to +1 -> DIFF 01 11 10 10 = 7A.)
TEST(Golden, WrapAroundDeltaRed255To0EmitsRgbLikeAda) {
    check_golden(rgb_desc(2, 1), {255, 100, 100, 0, 100, 100},
                 {0xFE, 0xFF, 0x64, 0x64, 0xFE, 0x00, 0x64, 0x64});
}

// r: 0 -> 255. P0 (0,100,100): deltas (0,100,100), vg = 100 out of range -> RGB
// FE 00 64 64. P1 (255,100,100): unbounded dr = +255 -> RGB FE FF 64 64.
// (qoi.h: dr wraps to -1 -> DIFF 01 01 10 10 = 5A.)
TEST(Golden, WrapAroundDeltaRed0To255EmitsRgbLikeAda) {
    check_golden(rgb_desc(2, 1), {0, 100, 100, 255, 100, 100},
                 {0xFE, 0x00, 0x64, 0x64, 0xFE, 0xFF, 0x64, 0x64});
}

// r: 254 -> 1. P0 (254,100,100) -> RGB FE FE 64 64. P1 (1,100,100): unbounded dr =
// -253 -> RGB FE 01 64 64. (qoi.h: dr wraps to +3, vg_r = +3 -> LUMA A0 B8.)
TEST(Golden, WrapAroundDeltaRed254To1EmitsRgbLikeAda) {
    check_golden(rgb_desc(2, 1), {254, 100, 100, 1, 100, 100},
                 {0xFE, 0xFE, 0x64, 0x64, 0xFE, 0x01, 0x64, 0x64});
}

// g: 255 -> 0 with r and b unchanged. P0 (100,255,100): deltas vs (0,0,0) (100,255,100)
// -> RGB FE 64 FF 64. P1 (100,0,100): unbounded dg = -255 -> RGB FE 64 00 64.
// (qoi.h: dg wraps to +1 -> DIFF 01 10 11 10 = 6E.)
TEST(Golden, WrapAroundDeltaGreen255To0EmitsRgbLikeAda) {
    check_golden(rgb_desc(2, 1), {100, 255, 100, 100, 0, 100},
                 {0xFE, 0x64, 0xFF, 0x64, 0xFE, 0x64, 0x00, 0x64});
}

// b: 0 -> 255. P0 (50,50,0): deltas (50,50,0), vg = 50 out of range -> RGB FE 32 32 00.
// P1 (50,50,255): unbounded db = +255 -> RGB FE 32 32 FF.
// (qoi.h: db wraps to -1 -> DIFF 01 10 10 01 = 69.)
TEST(Golden, WrapAroundDeltaBlue0To255EmitsRgbLikeAda) {
    check_golden(rgb_desc(2, 1), {50, 50, 0, 50, 50, 255},
                 {0xFE, 0x32, 0x32, 0x00, 0xFE, 0x32, 0x32, 0xFF});
}

// All channels 255 -> 0. P0 (255,255,255): deltas (255,255,255) -> RGB FE FF FF FF.
// P1 (0,0,0): hash3 = 53, slot 53 holds (0,0,0,0) != (0,0,0,255): no INDEX; deltas
// (-255,-255,-255) -> RGB FE 00 00 00. (qoi.h: all wrap to +1 -> DIFF 7F.) P2 (0,0,0)
// repeats -> run of 1 at the last pixel: C0.
TEST(Golden, WrapAroundDeltaAllChannels255To0EmitsRgbLikeAda) {
    check_golden(rgb_desc(3, 1), {255, 255, 255, 0, 0, 0, 0, 0, 0},
                 {0xFE, 0xFF, 0xFF, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0xC0});
}

// The reverse direction: all channels 0 -> 255 (after a non-black pixel to avoid a run).
// P0 (1,1,1): deltas (1,1,1) -> DIFF 7F. P1 (0,0,0): deltas (-1,-1,-1) -> DIFF 01 01 01 01
// = 0x55. P2 (255,255,255): unbounded (255,255,255) -> RGB FE FF FF FF.
// (qoi.h: all wrap to -1 -> DIFF 55.) hash3(255,255,255) = 15*255 + 53 = 3878 = 38; hash3
// of P0, P1 are 4 and 53 respectively, so P2 cannot be an INDEX hit.
TEST(Golden, WrapAroundDeltaAllChannels0To255EmitsRgbLikeAda) {
    check_golden(rgb_desc(3, 1), {1, 1, 1, 0, 0, 0, 255, 255, 255},
                 {0x7F, 0x55, 0xFE, 0xFF, 0xFF, 0xFF});
}

// A step that qoi.h would code as LUMA after wrapping: red and blue 250 -> 1.
// P0 (250,100,250): deltas (250,100,250) -> RGB FE FA 64 FA. P1 (1,100,1): unbounded
// dr = db = -249, vg_r = -249 -> RGB FE 01 64 01. (qoi.h: dr = db wrap to +7, dg = 0,
// so vg_r = vg_b = 7 -> LUMA A0 FF.)
TEST(Golden, WrapAroundDeltaRedBlue250To1EmitsRgbLikeAda) {
    check_golden(rgb_desc(2, 1), {250, 100, 250, 1, 100, 1},
                 {0xFE, 0xFA, 0x64, 0xFA, 0xFE, 0x01, 0x64, 0x01});
}

// Wrap-around inside a 4-channel image with constant alpha 200: red 255 -> 0.
// P0 (255,0,0,200): alpha 200 != 255 -> RGBA FF FF 00 00 C8. P1 (0,0,0,200): alpha equal,
// unbounded dr = -255 -> RGB FE 00 00 00. (qoi.h: dr wraps to +1 -> DIFF 01 11 10 10 = 7A.)
TEST(Golden, WrapAroundDeltaFourChannelsEmitsRgbLikeAda) {
    check_golden(rgba_desc(2, 1), {255, 0, 0, 200, 0, 0, 0, 200},
                 {0xFF, 0xFF, 0x00, 0x00, 0xC8, 0xFE, 0x00, 0x00, 0x00});
}

// Control for the wrap-around vectors: the same step sizes WITHOUT crossing the byte
// boundary are DIFF/LUMA in both encoders (1 -> 0 is -1 -> DIFF).
TEST(Golden, NoWrapAroundControlStepsAreDiff) {
    // P0 (1,100,100): vs (0,0,0) deltas (1,100,100): vg = 100 -> RGB FE 01 64 64.
    // P1 (0,100,100): dr = -1 -> DIFF 01 01 10 10 = 0x5A.
    check_golden(rgb_desc(2, 1), {1, 100, 100, 0, 100, 100}, {0xFE, 0x01, 0x64, 0x64, 0x5A});
}

// ---------------------------------------------------------------------------
// valid_size and encode_worst_case
// ---------------------------------------------------------------------------

// Worst case for small images: w*h*(channels+1) + 14 + 8.
TEST(Golden, EncodeWorstCaseSmallImages) {
    EXPECT_EQ(qoi::encode_worst_case(rgb_desc(1, 1)), 26u);           // 1*4 + 22
    EXPECT_EQ(qoi::encode_worst_case(rgba_desc(1, 1)), 27u);          // 1*5 + 22
    EXPECT_EQ(qoi::encode_worst_case(rgb_desc(2, 3)), 46u);           // 6*4 + 22
    EXPECT_EQ(qoi::encode_worst_case(rgba_desc(2, 3)), 52u);          // 6*5 + 22
    EXPECT_EQ(qoi::encode_worst_case(rgb_desc(3, 2)), 46u);           // symmetric in w and h
    EXPECT_EQ(qoi::encode_worst_case(rgba_desc(100, 100)), 50022u);   // 10000*5 + 22
    EXPECT_EQ(qoi::encode_worst_case(rgb_desc(640, 480)), 1228822u);  // 307200*4 + 22
}

// The worst case is reached: every pixel needs its largest chunk.
//   3 channels: (10,20,30) -> RGB (4 bytes); (200,100,50): dr = 190 -> RGB (4 bytes):
//     14 + 8 + 8 = 30 = 2*4 + 22.
//   4 channels: (10,20,30,40) -> RGBA (5 bytes); (50,60,70,80): alpha changes -> RGBA:
//     14 + 10 + 8 = 32 = 2*5 + 22.
TEST(Golden, EncodedSizeEqualsWorstCaseWhenNothingCompresses) {
    check_golden(rgb_desc(2, 1), {10, 20, 30, 200, 100, 50},
                 {0xFE, 0x0A, 0x14, 0x1E, 0xFE, 0xC8, 0x64, 0x32});
    EXPECT_EQ(qoi::encode_worst_case(rgb_desc(2, 1)), 30u);
    check_golden(rgba_desc(2, 1), {10, 20, 30, 40, 50, 60, 70, 80},
                 {0xFF, 0x0A, 0x14, 0x1E, 0x28, 0xFF, 0x32, 0x3C, 0x46, 0x50});
    EXPECT_EQ(qoi::encode_worst_case(rgba_desc(2, 1)), 32u);
}

// valid_size: only 3 and 4 channels are supported.
TEST(Golden, ValidSizeChannelBoundaries) {
    for (const std::uint64_t channels :
         U64List{0, 1, 2, 5, 6, 7, 255, 256, 0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}) {
        EXPECT_FALSE(qoi::valid_size(make_desc(1, 1, channels))) << "channels " << channels;
        EXPECT_EQ(qoi::encode_worst_case(make_desc(1, 1, channels)), 0u) << "channels " << channels;
    }
    EXPECT_TRUE(qoi::valid_size(make_desc(1, 1, 3)));
    EXPECT_TRUE(qoi::valid_size(make_desc(1, 1, 4)));
    EXPECT_TRUE(qoi::valid_size(make_desc(1, 1, 3, qoi::Colorspace::SRGB_Linear_Alpha)));
    EXPECT_TRUE(qoi::valid_size(make_desc(1, 1, 4, qoi::Colorspace::SRGB_Linear_Alpha)));
}

// valid_size: width and height must be at least 1.
TEST(Golden, ValidSizeZeroDimensions) {
    for (const std::uint64_t channels : U64List{3, 4}) {
        EXPECT_FALSE(qoi::valid_size(make_desc(0, 1, channels)));
        EXPECT_FALSE(qoi::valid_size(make_desc(1, 0, channels)));
        EXPECT_FALSE(qoi::valid_size(make_desc(0, 0, channels)));
        EXPECT_FALSE(qoi::valid_size(make_desc(0, kMaxDim, channels)));
        EXPECT_FALSE(qoi::valid_size(make_desc(kMaxDim, 0, channels)));
        EXPECT_EQ(qoi::encode_worst_case(make_desc(0, 1, channels)), 0u);
        EXPECT_EQ(qoi::encode_worst_case(make_desc(1, 0, channels)), 0u);
    }
}

// valid_size: width and height are at most Integer_32'Last = 2^31 - 1.
//   (2^31-1) x 1: w*h = 2^31-1, times 5 = 10737418235, +22 = 10737418257: valid for 4
//   channels; times 4 = 8589934588, +22 = 8589934610 for 3 channels.
TEST(Golden, ValidSizeDimensionLimits) {
    EXPECT_TRUE(qoi::valid_size(make_desc(kMaxDim, 1, 4)));
    EXPECT_TRUE(qoi::valid_size(make_desc(kMaxDim, 1, 3)));
    EXPECT_TRUE(qoi::valid_size(make_desc(1, kMaxDim, 4)));
    EXPECT_TRUE(qoi::valid_size(make_desc(1, kMaxDim, 3)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim, 1, 4)), 10737418257ULL);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim, 1, 3)), 8589934610ULL);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(1, kMaxDim, 4)), 10737418257ULL);

    // 2^31 and above are rejected, whatever the other dimension is.
    for (const std::uint64_t big : U64List{kMaxDim + 1, kMaxDim + 2, 0xFFFFFFFFULL, 0x100000000ULL,
                                           0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}) {
        for (const std::uint64_t channels : U64List{3, 4}) {
            EXPECT_FALSE(qoi::valid_size(make_desc(big, 1, channels))) << "width " << big;
            EXPECT_FALSE(qoi::valid_size(make_desc(1, big, channels))) << "height " << big;
            EXPECT_FALSE(qoi::valid_size(make_desc(big, big, channels))) << "both " << big;
            EXPECT_EQ(qoi::encode_worst_case(make_desc(big, 1, channels)), 0u);
            EXPECT_EQ(qoi::encode_worst_case(make_desc(1, big, channels)), 0u);
        }
    }
}

// valid_size: overflow of w*h*(c+1)+22 beyond Storage_Count'Last = 2^63-1.
//
// Ada's Valid_Size is the conjunction (lines 105-113 of legacy/ada/src/qoi.ads):
//   (4) w <= LAST / h
//   (5) (c+1) <= LAST / (w*h)
//   (6) 22 <= LAST - w*h*(c+1)
// For w = h = 2^31-1: w*h = 2^62 - 2^32 + 1 = 4611686014132420609, and
// LAST / (w*h) = 2 (2*(w*h) = 2^63 - 2^33 + 2 <= LAST < 3*(w*h)). Conjunct (5) needs
// c+1 <= 2, but c+1 is 4 or 5, so BOTH channel counts are invalid.
TEST(Golden, ValidSizeMaximumSquareOverflows) {
    EXPECT_FALSE(qoi::valid_size(make_desc(kMaxDim, kMaxDim, 4)));
    EXPECT_FALSE(qoi::valid_size(make_desc(kMaxDim, kMaxDim, 3)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim, kMaxDim, 4)), 0u);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim, kMaxDim, 3)), 0u);
    // (2^31-1) x (2^31-2): w*h = 2^62 - 3*2^31 + 2, LAST / (w*h) = 2 as well.
    EXPECT_FALSE(qoi::valid_size(make_desc(kMaxDim, kMaxDim - 1, 3)));
    EXPECT_FALSE(qoi::valid_size(make_desc(kMaxDim - 1, kMaxDim, 4)));
}

// 2^30 x 2^30: w*h = 2^60, LAST / 2^60 = 7, so 4 <= 7 and 5 <= 7: valid for both.
//   3 channels: 4 * 2^60 + 22 = 2^62 + 22 = 4611686018427387926.
//   4 channels: 5 * 2^60 + 22 = 5764607523034234902.
TEST(Golden, ValidSizeTwoToThe30Square) {
    const std::uint64_t dim = 1ULL << 30;
    EXPECT_TRUE(qoi::valid_size(make_desc(dim, dim, 3)));
    EXPECT_TRUE(qoi::valid_size(make_desc(dim, dim, 4)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(dim, dim, 3)), 4611686018427387926ULL);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(dim, dim, 4)), 5764607523034234902ULL);
}

// The channel count changes the verdict: w = 2^31-1, h = 2^30. w*h = 2^61 - 2^30.
//   3 channels: 4*w*h = 2^63 - 2^32 <= LAST - 22: valid, worst case = 2^63 - 2^32 + 22.
//   4 channels: LAST / (w*h) = 4 (4*w*h <= LAST < 5*w*h), so c+1 = 5 fails (5): invalid.
TEST(Golden, ValidSizeVerdictDependsOnChannels) {
    const std::uint64_t h = 1ULL << 30;
    EXPECT_TRUE(qoi::valid_size(make_desc(kMaxDim, h, 3)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim, h, 3)), (1ULL << 63) - (1ULL << 32) + 22);
    EXPECT_FALSE(qoi::valid_size(make_desc(kMaxDim, h, 4)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim, h, 4)), 0u);
}

// Exact fit: the largest size for which the 22 bytes of header and padding still fit
// below 2^63-1. The factorisations below were found by searching for products
// w*h <= (LAST-22)/(c+1) with both factors <= 2^31-1; the static_asserts verify the
// arithmetic at compile time.
//
// 3 channels: w = 2120185131, h = 1087566824, w*h = 2^61 - 8, so
//   4*w*h + 22 = 2^63 - 32 + 22 = 2^63 - 10 = LAST - 9: fits (9 bytes to spare).
// 4 channels: w = 2147483641, h = 858993462, w*h = 1844674407370955142, so
//   5*w*h + 22 = 9223372036854775732 = LAST - 75: fits.
TEST(Golden, ValidSizeExactFitAtStorageCountLimit) {
    constexpr std::uint64_t w3 = 2120185131ULL;
    constexpr std::uint64_t h3 = 1087566824ULL;
    static_assert(w3 <= kMaxDim && h3 <= kMaxDim, "dimensions in range");
    static_assert(w3 * h3 == (1ULL << 61) - 8, "w*h = 2^61 - 8");
    static_assert(4 * (w3 * h3) + 22 == kStorageLast - 9, "3-channel worst case = LAST - 9");
    EXPECT_TRUE(qoi::valid_size(make_desc(w3, h3, 3)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(w3, h3, 3)), kStorageLast - 9);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(h3, w3, 3)), kStorageLast - 9);

    constexpr std::uint64_t w4 = 2147483641ULL;
    constexpr std::uint64_t h4 = 858993462ULL;
    static_assert(w4 <= kMaxDim && h4 <= kMaxDim, "dimensions in range");
    static_assert(w4 * h4 == 1844674407370955142ULL, "w*h");
    static_assert(5 * (w4 * h4) + 22 == kStorageLast - 75, "4-channel worst case = LAST - 75");
    EXPECT_TRUE(qoi::valid_size(make_desc(w4, h4, 4)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(w4, h4, 4)), kStorageLast - 75);
    // The same pixel count is of course also valid with 3 channels (smaller factor).
    EXPECT_TRUE(qoi::valid_size(make_desc(w4, h4, 3)));
}

// One past the limit: only conjunct (6) of Valid_Size (22 <= LAST - w*h*(c+1)) fails;
// (5) c+1 <= LAST / (w*h) still holds, so these isolate the "+22" term.
// 3 channels: w = 2^31-2, h = 2^30+1, w*h = 2^61 - 2, 4*w*h = 2^63 - 8 <= LAST, but only
//   LAST - 4*w*h = 7 bytes remain, fewer than 22 -> invalid.
// 4 channels: w = 1876222553, h = 983185286, 5*w*h = LAST - 17: 17 bytes remain -> invalid.
TEST(Golden, ValidSizeJustPastLimitFailsOnlyOnHeaderAndPadding) {
    constexpr std::uint64_t w3 = (1ULL << 31) - 2;
    constexpr std::uint64_t h3 = (1ULL << 30) + 1;
    static_assert(w3 <= kMaxDim && h3 <= kMaxDim, "dimensions in range");
    static_assert(4 * (w3 * h3) == (1ULL << 63) - 8, "4*w*h = 2^63 - 8");
    static_assert(4 * (w3 * h3) <= kStorageLast, "conjunct (5) holds");
    static_assert(kStorageLast - 4 * (w3 * h3) < 22, "conjunct (6) fails");
    EXPECT_FALSE(qoi::valid_size(make_desc(w3, h3, 3)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(w3, h3, 3)), 0u);

    constexpr std::uint64_t w4 = 1876222553ULL;
    constexpr std::uint64_t h4 = 983185286ULL;
    static_assert(w4 <= kMaxDim && h4 <= kMaxDim, "dimensions in range");
    static_assert(5 * (w4 * h4) <= kStorageLast, "conjunct (5) holds");
    static_assert(kStorageLast - 5 * (w4 * h4) == 17, "17 bytes remain");
    EXPECT_FALSE(qoi::valid_size(make_desc(w4, h4, 4)));
    EXPECT_EQ(qoi::encode_worst_case(make_desc(w4, h4, 4)), 0u);
}

// encode_worst_case returns 0 for an invalid descriptor (documented in the header) and,
// for a valid one, always at least the 22 bytes of header and padding.
TEST(Golden, EncodeWorstCaseZeroForInvalidAndAtLeast22ForValid) {
    EXPECT_EQ(qoi::encode_worst_case(qoi_test::empty_desc()), 0u);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(1, 1, 2)), 0u);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(1, 1, 5)), 0u);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(0, 5, 3)), 0u);
    EXPECT_EQ(qoi::encode_worst_case(make_desc(kMaxDim + 1, 1, 3)), 0u);
    for (const std::uint64_t channels : U64List{3, 4}) {
        for (const std::uint64_t w : U64List{1, 2, 17, 4096}) {
            const std::uint64_t worst = qoi::encode_worst_case(make_desc(w, 3, channels));
            EXPECT_GE(worst, 22u);
            EXPECT_EQ(worst, w * 3 * (channels + 1) + 14 + 8);
        }
    }
}
