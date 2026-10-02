// Smoke tests: the first GoogleTest cases, used as the build healthcheck.
//
// Each case encodes a single pixel and checks the complete byte stream
// (14-byte header, one chunk, 8-byte padding) against hand-computed values.
#include <qoi/qoi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "test_util.hpp"

namespace {

using qoi_test::Bytes;

}  // namespace

// Pixel (10,20,30) against the initial previous pixel (0,0,0,255):
//   hash  = (10*3 + 20*5 + 30*7 + 255*11) mod 64 = 3145 mod 64 = 9 -> slot 9 is
//           still the zero pixel, so no INDEX op
//   alpha equal, dr=10 dg=20 db=30 -> outside DIFF (-2..1)
//   dg=20 in -32..31 but dr-dg=-10 is outside -8..7 -> outside LUMA
//   => QOI_OP_RGB: FE 0A 14 1E
TEST(Smoke, EncodeSingleRgbPixelProducesHeaderRgbChunkAndPadding) {
    const std::array<std::uint8_t, 3> pix = {10, 20, 30};
    const qoi::Desc desc{1, 1, 3, qoi::Colorspace::SRGB};

    const std::uint64_t worst = qoi::encode_worst_case(desc);
    ASSERT_EQ(worst, 1u * 1u * (3u + 1u) + qoi::QOI_HEADER_SIZE + qoi::QOI_PADDING.size());
    ASSERT_EQ(worst, 26u);

    Bytes out(static_cast<std::size_t>(worst), 0xAA);
    const std::size_t n = qoi::encode(pix, desc, out);

    // 14 header + 4 (RGB op) + 8 padding
    ASSERT_EQ(n, 26u);
    ASSERT_LE(n, out.size());

    const Bytes expected_header = qoi_test::header(1, 1, 3, 0);
    ASSERT_EQ(expected_header.size(), qoi::QOI_HEADER_SIZE);
    EXPECT_TRUE(qoi_test::BytesEqual(expected_header, out, expected_header.size()));

    const Bytes expected_chunk = {0xFE, 0x0A, 0x14, 0x1E};
    EXPECT_TRUE(qoi_test::BytesEqual(expected_chunk.data(), expected_chunk.size(),
                                     out.data() + qoi::QOI_HEADER_SIZE, expected_chunk.size()));

    const Bytes expected_padding = qoi_test::padding();
    ASSERT_EQ(expected_padding.size(), qoi::QOI_PADDING.size());
    EXPECT_TRUE(qoi_test::BytesEqual(expected_padding.data(), expected_padding.size(),
                                     out.data() + n - expected_padding.size(),
                                     expected_padding.size()));
    EXPECT_TRUE(qoi_test::BytesEqual(qoi::QOI_PADDING.data(), qoi::QOI_PADDING.size(),
                                     out.data() + n - qoi::QOI_PADDING.size(),
                                     qoi::QOI_PADDING.size()));
}

// An RGBA pixel equal to the initial previous pixel (0,0,0,255) is a run of
// length 1: QOI_OP_RUN with bias -1 -> 0xC0 | 0 = C0.
TEST(Smoke, EncodeSingleRgbaPixelEqualToInitialPreviousIsRunOfOne) {
    const std::array<std::uint8_t, 4> pix = {0, 0, 0, 255};
    const qoi::Desc desc{1, 1, 4, qoi::Colorspace::SRGB};

    const std::uint64_t worst = qoi::encode_worst_case(desc);
    ASSERT_EQ(worst, 1u * 1u * (4u + 1u) + qoi::QOI_HEADER_SIZE + qoi::QOI_PADDING.size());
    ASSERT_EQ(worst, 27u);

    Bytes out(static_cast<std::size_t>(worst), 0xAA);
    const std::size_t n = qoi::encode(pix, desc, out);

    // 14 header + 1 (RUN op) + 8 padding
    ASSERT_EQ(n, 23u);
    ASSERT_LE(n, out.size());

    const Bytes expected_header = qoi_test::header(1, 1, 4, 0);
    EXPECT_TRUE(qoi_test::BytesEqual(expected_header, out, expected_header.size()));

    EXPECT_EQ(out[qoi::QOI_HEADER_SIZE], 0xC0);

    const Bytes expected_padding = qoi_test::padding();
    EXPECT_TRUE(qoi_test::BytesEqual(expected_padding.data(), expected_padding.size(),
                                     out.data() + n - expected_padding.size(),
                                     expected_padding.size()));
}
