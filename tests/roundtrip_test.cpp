// Round-trip tests: encode an image, check the stream invariants, decode it again
// and require the original pixels back.
//
// All random data comes from a fixed-seed std::mt19937 through qoi_test::Rng
// (bytes are `rng() & 0xFF`, no std::uniform_int_distribution), so every run on
// every platform sees exactly the same images.
//
// What every round trip checks (see check_round_trip):
//   * encode_worst_case(desc) > 0 and the encoder returns n with 22 <= n <= worst
//   * the stream starts with the header for desc, built independently of the codec
//   * the last 8 bytes of the stream are the padding 00*7 01
//   * the encoder writes nothing at or beyond offset n (sentinel-filled buffer)
//   * get_desc on the stream returns desc
//   * decode into an exact-size buffer returns w*h*c, sets desc, restores the pixels
//     and does not touch guard bytes after the output
//   * decode into an oversized buffer behaves identically and leaves the surplus alone
//   * decode from an exact-size heap copy of the stream (no spare capacity to hide a
//     read past the end)
//   * encoding is deterministic and re-encoding the decoded pixels gives the same stream
#include <qoi/qoi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_util.hpp"

namespace {

using qoi_test::Bytes;
using qoi_test::BytesEqual;
using qoi_test::DescEq;
using qoi_test::make_desc;

constexpr std::uint8_t kEncodeSentinel = 0xA5;
constexpr std::uint8_t kGuard = 0xC3;
constexpr std::uint8_t kSurplus = 0x3C;
constexpr std::size_t kGuardSize = 16;

std::string describe(const qoi::Desc& desc, const std::string& what) {
    return what + " " + std::to_string(desc.width) + "x" + std::to_string(desc.height) + "x" +
           std::to_string(desc.channels) + " cs=" +
           std::to_string(static_cast<unsigned>(desc.colorspace));
}

// Returns the encoded stream (for callers that want to inspect it further).
Bytes check_round_trip(const qoi::Desc& desc, const Bytes& pix, const std::string& what) {
    SCOPED_TRACE(describe(desc, what));
    Bytes stream;

    if (pix.size() != qoi_test::byte_count(desc)) {
        ADD_FAILURE() << "malformed test image: " << pix.size() << " bytes";
        return stream;
    }

    // ---- encode ------------------------------------------------------------
    const std::size_t worst = static_cast<std::size_t>(qoi::encode_worst_case(desc));
    EXPECT_GT(worst, 22u);
    Bytes buffer(worst + kGuardSize, kEncodeSentinel);
    const std::size_t n = qoi::encode(pix, desc, qoi::Span<std::uint8_t>(buffer.data(), worst));
    EXPECT_GE(n, 22u) << "stream shorter than header plus padding";
    EXPECT_LE(n, worst) << "encoder exceeded encode_worst_case";
    if (n < 22u || n > worst) {
        return stream;
    }
    EXPECT_TRUE(qoi_test::all_equal(buffer, n, buffer.size(), kEncodeSentinel))
        << "encoder wrote beyond the reported size";

    stream.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));

    const Bytes expected_header = qoi_test::header(
        static_cast<std::uint32_t>(desc.width), static_cast<std::uint32_t>(desc.height),
        static_cast<std::uint8_t>(desc.channels), static_cast<std::uint8_t>(desc.colorspace));
    EXPECT_TRUE(BytesEqual(expected_header, Bytes(stream.begin(), stream.begin() + 14)));
    EXPECT_TRUE(BytesEqual(qoi_test::padding(), Bytes(stream.end() - 8, stream.end())));

    // ---- deterministic ------------------------------------------------------
    {
        Bytes again(worst, 0x11);
        const std::size_t m = qoi::encode(pix, desc, again);
        EXPECT_EQ(m, n);
        EXPECT_TRUE(BytesEqual(stream, again, m));
    }

    // ---- get_desc ------------------------------------------------------------
    {
        qoi::Desc parsed = qoi_test::poisoned_desc();
        qoi::get_desc(stream, parsed);
        EXPECT_TRUE(DescEq(desc, parsed));
    }

    // ---- decode into an exact-size output buffer with guard bytes ------------
    const qoi_test::ExactBytes exact_stream(stream);
    {
        Bytes out(pix.size() + kGuardSize, kGuard);
        qoi::Desc decoded = qoi_test::poisoned_desc();
        const std::size_t size = qoi::decode(
            exact_stream.span(), decoded, qoi::Span<std::uint8_t>(out.data(), pix.size()));
        EXPECT_EQ(size, pix.size());
        EXPECT_TRUE(DescEq(desc, decoded));
        EXPECT_TRUE(BytesEqual(pix, out, pix.size()));
        EXPECT_TRUE(qoi_test::all_equal(out, pix.size(), out.size(), kGuard))
            << "decoder wrote beyond the pixel data";
    }

    // ---- decode into an oversized output buffer -------------------------------
    {
        const std::size_t extra = 37;
        Bytes out(pix.size() + extra, kSurplus);
        qoi::Desc decoded = qoi_test::poisoned_desc();
        const std::size_t size = qoi::decode(stream, decoded, out);
        EXPECT_EQ(size, pix.size()) << "the return value is the image size, not the buffer size";
        EXPECT_TRUE(DescEq(desc, decoded));
        EXPECT_TRUE(BytesEqual(pix, out, pix.size()));
        EXPECT_TRUE(qoi_test::all_equal(out, pix.size(), out.size(), kSurplus))
            << "decoder touched the surplus part of an oversized buffer";

        // Re-encoding the decoded pixels reproduces the stream.
        const Bytes decoded_pixels(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size));
        Bytes again(worst, 0x22);
        const std::size_t m = qoi::encode(decoded_pixels, decoded, again);
        EXPECT_EQ(m, n);
        EXPECT_TRUE(BytesEqual(stream, again, m));
    }
    return stream;
}

// Cheaper variant for the exhaustive sweeps: encode, decode, compare.
void quick_round_trip(const qoi::Desc& desc, const Bytes& pix) {
    Bytes buffer(static_cast<std::size_t>(qoi::encode_worst_case(desc)));
    const std::size_t n = qoi::encode(pix, desc, buffer);
    ASSERT_GE(n, 22u);
    ASSERT_LE(n, buffer.size());
    Bytes out(pix.size());
    qoi::Desc decoded = qoi_test::poisoned_desc();
    const std::size_t size = qoi::decode(qoi::Span<const std::uint8_t>(buffer.data(), n), decoded, out);
    ASSERT_EQ(size, pix.size()) << qoi_test::hex(pix);
    ASSERT_TRUE(DescEq(desc, decoded));
    ASSERT_TRUE(BytesEqual(pix, out)) << "stream: " << qoi_test::hex(buffer.data(), n);
}

struct Shape {
    std::size_t width;
    std::size_t height;
};

const std::vector<Shape>& shapes() {
    static const std::vector<Shape> all = {
        {1, 1},  {1, 2},  {2, 1},  {2, 2},  {3, 1},  {1, 3},  {3, 3},   {4, 5},  {5, 4},
        {7, 9},  {9, 7},  {1, 61}, {61, 1}, {1, 62}, {62, 1}, {1, 63},  {63, 1}, {1, 64},
        {64, 1}, {8, 8},  {16, 16}, {31, 17}, {17, 31}, {1, 100}, {100, 1}, {64, 63}, {33, 65},
    };
    return all;
}

}  // namespace

// ---------------------------------------------------------------------------
// Image families over many shapes, both channel counts and both colorspaces
// ---------------------------------------------------------------------------

TEST(RoundTrip, NoiseAllShapesChannelsColorspaces) {
    std::uint32_t seed = 1000;
    for (const Shape& s : shapes()) {
        for (const std::size_t channels : {3U, 4U}) {
            for (std::size_t cs = 0; cs < 2; ++cs) {
                const qoi::Desc desc = make_desc(s.width, s.height, channels, qoi_test::colorspace_of(cs));
                check_round_trip(desc, qoi_test::noise_image(s.width, s.height, channels, ++seed),
                                 "noise");
            }
        }
    }
}

TEST(RoundTrip, FlatImages) {
    const std::vector<std::array<std::uint8_t, 4>> colours = {
        {0, 0, 0, 255},     {0, 0, 0, 0},       {255, 255, 255, 255}, {255, 255, 255, 0},
        {1, 2, 3, 4},       {128, 128, 128, 128}, {255, 0, 0, 255},   {0, 255, 0, 1},
        {17, 200, 99, 254},
    };
    for (const Shape& s : shapes()) {
        for (const auto& colour : colours) {
            for (const std::size_t channels : {3U, 4U}) {
                const qoi::Desc desc = make_desc(s.width, s.height, channels);
                check_round_trip(desc, qoi_test::flat_image(s.width, s.height, channels, colour),
                                 "flat");
            }
        }
    }
}

TEST(RoundTrip, GradientsHorizontalAndVertical) {
    for (const Shape& s : shapes()) {
        for (const std::size_t channels : {3U, 4U}) {
            const qoi::Desc desc = make_desc(s.width, s.height, channels, qoi::Colorspace::SRGB_Linear_Alpha);
            check_round_trip(desc, qoi_test::horizontal_gradient_image(s.width, s.height, channels),
                             "horizontal gradient");
            check_round_trip(desc, qoi_test::vertical_gradient_image(s.width, s.height, channels),
                             "vertical gradient");
        }
    }
}

TEST(RoundTrip, SmallPalettesExerciseIndex) {
    std::uint32_t seed = 5000;
    for (const std::uint32_t palette : {1U, 2U, 3U, 8U, 16U, 63U, 64U, 65U, 200U}) {
        for (const std::size_t channels : {3U, 4U}) {
            const qoi::Desc desc = make_desc(48, 33, channels);
            check_round_trip(desc, qoi_test::palette_image(48, 33, channels, ++seed, palette),
                             "palette " + std::to_string(palette));
        }
    }
}

TEST(RoundTrip, RunsInterleavedWithNoise) {
    std::uint32_t seed = 7000;
    for (const Shape& s : {Shape{200, 50}, Shape{1, 3000}, Shape{3000, 1}, Shape{97, 101}}) {
        for (const std::size_t channels : {3U, 4U}) {
            const qoi::Desc desc = make_desc(s.width, s.height, channels);
            check_round_trip(desc, qoi_test::runs_and_noise_image(s.width, s.height, channels, ++seed),
                             "runs and noise");
        }
    }
}

// Small per-channel steps computed modulo 256 starting near 0 and near 255, so the
// colours cross the byte boundary (the case where unbounded deltas matter: see the
// WrapAround golden tests) and still must round-trip.
TEST(RoundTrip, NearEqualWalksAcrossByteBoundary) {
    std::uint32_t seed = 9000;
    for (const std::uint32_t max_step : {1U, 2U, 3U, 8U, 20U, 40U}) {
        for (const std::uint8_t start : std::array<std::uint8_t, 5>{0, 1, 127, 250, 255}) {
            for (const std::size_t channels : {3U, 4U}) {
                const qoi::Desc desc = make_desc(40, 25, channels);
                check_round_trip(
                    desc, qoi_test::near_equal_image(40, 25, channels, ++seed, max_step, start),
                    "near-equal walk step " + std::to_string(max_step));
            }
        }
    }
}

// Every flat run length from 1 to 400 pixels, 3 and 4 channels, for the colour that
// equals the initial previous pixel (run from the first pixel) and for another colour
// (run after a coded pixel). Crosses the 62-pixel flush limit six times.
TEST(RoundTrip, EveryRunLengthUpTo400) {
    for (std::size_t length = 1; length <= 400; ++length) {
        for (const std::size_t channels : {3U, 4U}) {
            const qoi::Desc desc = make_desc(length, 1, channels);
            check_round_trip(desc, qoi_test::flat_image(length, 1, channels, {0, 0, 0, 255}),
                             "black run");
            check_round_trip(desc, qoi_test::flat_image(length, 1, channels, {9, 8, 7, 6}),
                             "colour run");
        }
    }
}

// A run of length k followed by one different pixel and another run, for k around the
// flush boundary (the pending run must be flushed before the next chunk).
TEST(RoundTrip, RunsBetweenDifferentPixels) {
    for (const std::size_t channels : {3U, 4U}) {
        for (const std::size_t k : {1U, 2U, 60U, 61U, 62U, 63U, 64U, 123U, 124U, 125U, 126U}) {
            Bytes pix;
            const auto add = [&](std::size_t count, std::uint8_t v) {
                for (std::size_t i = 0; i < count; ++i) {
                    pix.push_back(v);
                    pix.push_back(qoi_test::u8(v + 1U));
                    pix.push_back(qoi_test::u8(v + 2U));
                    if (channels == 4) {
                        pix.push_back(255);
                    }
                }
            };
            add(k, 50);
            add(1, 200);
            add(k, 50);
            add(k, 51);
            const std::size_t pixels = 3 * k + 1;
            check_round_trip(make_desc(pixels, 1, channels), pix, "run of " + std::to_string(k));
        }
    }
}

TEST(RoundTrip, AlphaVariations) {
    // Alpha alone changes from pixel to pixel while the colour stays fixed.
    Bytes pix;
    for (std::size_t i = 0; i < 300; ++i) {
        pix.insert(pix.end(), {10, 20, 30, qoi_test::u8(i * 7U)});
    }
    check_round_trip(make_desc(300, 1, 4), pix, "alpha ramp");

    // Alpha toggles between two values.
    Bytes toggle;
    for (std::size_t i = 0; i < 200; ++i) {
        toggle.insert(toggle.end(), {1, 2, 3, static_cast<std::uint8_t>(i % 2U == 0U ? 0 : 255)});
    }
    check_round_trip(make_desc(20, 10, 4), toggle, "alpha toggle");

    // Fully transparent pixels with arbitrary colour: colour must be preserved.
    Bytes transparent = qoi_test::noise_image(30, 30, 4, 4242);
    for (std::size_t i = 3; i < transparent.size(); i += 4) {
        transparent[i] = 0;
    }
    check_round_trip(make_desc(30, 30, 4), transparent, "transparent noise");
}

TEST(RoundTrip, LargeImages) {
    check_round_trip(make_desc(512, 384, 3), qoi_test::noise_image(512, 384, 3, 777), "large noise");
    check_round_trip(make_desc(512, 384, 4), qoi_test::noise_image(512, 384, 4, 778), "large noise");
    check_round_trip(make_desc(640, 480, 3), qoi_test::horizontal_gradient_image(640, 480, 3),
                     "large gradient");
    check_round_trip(make_desc(640, 480, 4), qoi_test::flat_image(640, 480, 4, {0, 0, 0, 255}),
                     "large flat");
    check_round_trip(make_desc(4000, 1, 3), qoi_test::runs_and_noise_image(4000, 1, 3, 779),
                     "wide strip");
    check_round_trip(make_desc(1, 4000, 4), qoi_test::runs_and_noise_image(1, 4000, 4, 780),
                     "tall strip");
}

// The incompressible case: every pixel is a fresh RGB or RGBA chunk; the encoded size
// stays within the worst case and the image still round-trips.
TEST(RoundTrip, WorstCaseImageStaysWithinBound) {
    for (const std::size_t channels : {3U, 4U}) {
        // Alternate between two far-apart colours with a unique alpha each time so that
        // neither INDEX nor DIFF/LUMA nor runs apply (a new alpha forces RGBA; for 3
        // channels the colour step is larger than the LUMA range).
        Bytes pix;
        const std::size_t count = 64;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t base = (i % 2U == 0U) ? 10 : 200;
            pix.push_back(qoi_test::u8(base + i));
            pix.push_back(qoi_test::u8(base * 3U + i * 5U));
            pix.push_back(qoi_test::u8(base * 7U + i * 11U));
            if (channels == 4) {
                pix.push_back(qoi_test::u8(i * 3U));
            }
        }
        const qoi::Desc desc = make_desc(count, 1, channels);
        const Bytes stream = check_round_trip(desc, pix, "worst-case candidate");
        EXPECT_LE(stream.size(), static_cast<std::size_t>(qoi::encode_worst_case(desc)));
    }
}

// ---------------------------------------------------------------------------
// Exhaustive and randomised sweeps
// ---------------------------------------------------------------------------

// Two-pixel images covering every value pair (a, b) for one channel while the other
// channels are fixed at a "middle" value. Covers every delta from -255 to +255 in each
// channel, so every DIFF/LUMA/RGB decision boundary (and the wrap-around of the decoder)
// is exercised in both directions.
TEST(RoundTrip, EveryTwoPixelValuePairPerChannel) {
    for (std::size_t channels : {3U, 4U}) {
        for (std::size_t varying = 0; varying < channels; ++varying) {
            for (unsigned a = 0; a < 256; ++a) {
                for (unsigned b = 0; b < 256; ++b) {
                    Bytes pix = {100, 150, 50, 255, 100, 150, 50, 255};
                    if (channels == 3) {
                        pix = {100, 150, 50, 100, 150, 50};
                    }
                    pix[varying] = qoi_test::u8(a);
                    pix[channels + varying] = qoi_test::u8(b);
                    quick_round_trip(make_desc(2, 1, channels), pix);
                    if (::testing::Test::HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

// Pairs of full pixels drawn from boundary values of the DIFF/LUMA windows and of the
// byte range, for every channel at once.
TEST(RoundTrip, BoundaryValuePixelPairs) {
    const std::array<unsigned, 8> values = {0, 1, 2, 126, 128, 253, 254, 255};
    for (const unsigned r0 : values) {
        for (const unsigned g0 : values) {
            for (const unsigned b0 : values) {
                for (const unsigned r1 : values) {
                    for (const unsigned g1 : values) {
                        for (const unsigned b1 : values) {
                            const Bytes pix = {qoi_test::u8(r0), qoi_test::u8(g0), qoi_test::u8(b0),
                                               qoi_test::u8(r1), qoi_test::u8(g1), qoi_test::u8(b1)};
                            quick_round_trip(make_desc(2, 1, 3), pix);
                            if (::testing::Test::HasFatalFailure()) {
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
}

// Four-channel pairs: colour and alpha from boundary values.
TEST(RoundTrip, BoundaryValueRgbaPairs) {
    const std::array<unsigned, 5> values = {0, 1, 127, 254, 255};
    for (const unsigned c0 : values) {
        for (const unsigned a0 : values) {
            for (const unsigned c1 : values) {
                for (const unsigned a1 : values) {
                    const Bytes pix = {qoi_test::u8(c0), qoi_test::u8(c0 / 2U), qoi_test::u8(c0), qoi_test::u8(a0),
                                       qoi_test::u8(c1), qoi_test::u8(c1 / 2U), qoi_test::u8(c1), qoi_test::u8(a1)};
                    quick_round_trip(make_desc(2, 1, 4), pix);
                    if (::testing::Test::HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

// Many seeds, random shape, channel count, colorspace and image family.
TEST(RoundTrip, RandomisedFixedSeeds) {
    for (std::uint32_t seed = 1; seed <= 300; ++seed) {
        qoi_test::Rng rng(seed * 2654435761U);
        const std::size_t width = 1U + rng.below(48U);
        const std::size_t height = 1U + rng.below(48U);
        const std::size_t channels = 3U + rng.below(2U);
        const qoi::Desc desc = make_desc(width, height, channels, qoi_test::colorspace_of(rng.below(2U)));
        Bytes pix;
        switch (rng.below(6U)) {
            case 0:
                pix = qoi_test::noise_image(width, height, channels, seed);
                break;
            case 1:
                pix = qoi_test::palette_image(width, height, channels, seed, 1U + rng.below(70U));
                break;
            case 2:
                pix = qoi_test::runs_and_noise_image(width, height, channels, seed);
                break;
            case 3:
                pix = qoi_test::near_equal_image(width, height, channels, seed, 1U + rng.below(12U),
                                                 rng.byte());
                break;
            case 4:
                pix = qoi_test::horizontal_gradient_image(width, height, channels);
                break;
            default:
                pix = qoi_test::vertical_gradient_image(width, height, channels);
                break;
        }
        check_round_trip(desc, pix, "random seed " + std::to_string(seed));
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

// The reported size is exactly what the decoder needs: decoding into a buffer of exactly
// that size works (checked above) and the stream length never depends on the colorspace.
TEST(RoundTrip, ColorspaceDoesNotChangeTheChunkStream) {
    const Bytes pix = qoi_test::noise_image(20, 20, 4, 31337);
    Bytes a(qoi::encode_worst_case(make_desc(20, 20, 4)));
    Bytes b(a.size());
    const std::size_t na = qoi::encode(pix, make_desc(20, 20, 4, qoi::Colorspace::SRGB), a);
    const std::size_t nb = qoi::encode(pix, make_desc(20, 20, 4, qoi::Colorspace::SRGB_Linear_Alpha), b);
    ASSERT_EQ(na, nb);
    EXPECT_EQ(static_cast<unsigned>(a[13]), 0u);
    EXPECT_EQ(static_cast<unsigned>(b[13]), 1u);
    a[13] = 1;
    a.resize(na);
    b.resize(nb);
    EXPECT_TRUE(BytesEqual(a, b));
}

// Dimensions are stored per axis: a 6x4 and a 4x6 image with the same pixel data have
// identical chunks but different headers, and each decodes to its own descriptor.
TEST(RoundTrip, TransposedDimensionsKeepTheirOwnDescriptor) {
    const Bytes pix = qoi_test::noise_image(6, 4, 3, 99);
    const Bytes wide = check_round_trip(make_desc(6, 4, 3), pix, "6x4");
    const Bytes tall = check_round_trip(make_desc(4, 6, 3), pix, "4x6");
    ASSERT_EQ(wide.size(), tall.size());
    EXPECT_TRUE(BytesEqual(Bytes(wide.begin() + 14, wide.end()), Bytes(tall.begin() + 14, tall.end())));
    EXPECT_FALSE(BytesEqual(Bytes(wide.begin(), wide.begin() + 14), Bytes(tall.begin(), tall.begin() + 14)));
}
