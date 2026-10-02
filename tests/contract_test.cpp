// Contract and robustness tests.
//
// The Ada specification (src/qoi.ads) states preconditions and postconditions that
// SPARK proves. The C++ port turns every violated precondition into a graceful `0`
// return, so these tests pin down, per function, exactly what happens at and around
// each boundary of those contracts, and what the decoder does with hostile input.
//
// Conventions:
//   * Every input stream is handed to the decoder as an exact-size heap copy
//     (qoi_test::ExactBytes), so an out-of-bounds read is an AddressSanitizer
//     error in the sanitizer CI job (spec decision 5). The decoder is also run on
//     every truncation of valid streams and on seeded random garbage.
//   * Output buffers are sentinel-filled; a failed call must leave them untouched.
//   * No test allocates anything large. Huge dimensions are only ever passed in
//     descriptors or headers together with tiny buffers.
//   * Expected decoder outputs for crafted streams are derived by hand from
//     src/qoi.adb (lines 392-470); the derivation is in the comment of each test.
//
// Decoder facts used in the derivations (src/qoi.adb, Decode):
//   - Last_Chunk = Data'Last - 8, so with the stream length S a chunk is read only
//     while its first byte is at offset p <= S - 9. A chunk that starts at S-9 may
//     run into the padding bytes (they are not verified). From offset S-8 on no
//     chunk is read and the previous pixel repeats.
//   - The pixel state starts at (0,0,0,255); the index table starts all (0,0,0,0)
//     and is refreshed with the current pixel after EVERY chunk read, including
//     INDEX and RUN chunks (but not for repeated pixels inside a run).
//   - A RUN chunk with field k yields k+1 pixels (the chunk's own pixel and k
//     repeats). Fields 62 and 63 never occur because 0xFE/0xFF are RGB/RGBA.
//   - DIFF, LUMA and RGB/RGBA channel arithmetic is modulo 256, and alpha is only
//     changed by RGBA chunks (or loaded by INDEX).
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
using qoi_test::ExactBytes;
using qoi_test::make_desc;

constexpr std::uint8_t kSentinel = 0x9D;
constexpr std::uint64_t kStorageLast = 0x7FFFFFFFFFFFFFFFULL;
constexpr std::uint64_t kMaxDim = 0x7FFFFFFFULL;

using U64List = std::initializer_list<std::uint64_t>;

// A valid 1x1 RGB image (10,20,30): header, RGB chunk, padding. 26 bytes.
Bytes valid_stream_rgb() {
    return qoi_test::stream(1, 1, 3, 0, {0xFE, 0x0A, 0x14, 0x1E});
}

// Header for arbitrary raw field values, followed by `tail_chunks` and the padding.
Bytes raw_stream(std::uint32_t w, std::uint32_t h, std::uint8_t channels, std::uint8_t cs,
                 const Bytes& chunks) {
    return qoi_test::stream(w, h, channels, cs, chunks);
}

// Header, chunks, then an arbitrary 8-byte tail instead of the real padding.
Bytes stream_with_tail(std::uint32_t w, std::uint32_t h, std::uint8_t channels, const Bytes& chunks,
                       const Bytes& tail) {
    return qoi_test::concat({qoi_test::header(w, h, channels, 0), chunks, tail});
}

Bytes pixel_bytes(std::initializer_list<std::initializer_list<int>> pixels) {
    Bytes result;
    for (const auto& px : pixels) {
        for (const int v : px) {
            result.push_back(qoi_test::u8(v));
        }
    }
    return result;
}

// Decodes `data` (exact-size copy) into an exact-size `expected_out.size()` buffer and
// compares the pixels, the return value and the descriptor.
void expect_decodes_to(const Bytes& data, const qoi::Desc& expected_desc, const Bytes& expected_out,
                       const std::string& what) {
    SCOPED_TRACE(what + ": stream " + qoi_test::hex(data));
    const ExactBytes exact(data);
    Bytes out(expected_out.size() + 8, kSentinel);
    qoi::Desc desc = qoi_test::poisoned_desc();
    const std::size_t n =
        qoi::decode(exact.span(), desc, qoi::Span<std::uint8_t>(out.data(), expected_out.size()));
    EXPECT_EQ(n, expected_out.size());
    EXPECT_TRUE(DescEq(expected_desc, desc));
    EXPECT_TRUE(BytesEqual(expected_out, out, expected_out.size()));
    EXPECT_TRUE(qoi_test::all_equal(out, expected_out.size(), out.size(), kSentinel));
}

// A failing decode returns 0, leaves the output buffer untouched and leaves in desc
// the result of get_desc (the Ada behaviour pinned by design decision 4).
void expect_decode_fails(const Bytes& data, std::size_t out_size, const qoi::Desc& expected_desc,
                         const std::string& what) {
    SCOPED_TRACE(what + ": stream " + qoi_test::hex(data));
    const ExactBytes exact(data);
    Bytes out(out_size, kSentinel);
    qoi::Desc desc = qoi_test::poisoned_desc();
    const std::size_t n = qoi::decode(exact.span(), desc, out);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(DescEq(expected_desc, desc));
    EXPECT_TRUE(qoi_test::all_equal(out, 0, out.size(), kSentinel)) << "output was modified";
}

// A failing encode returns 0 and leaves the output buffer untouched.
void expect_encode_fails(const Bytes& pix, const qoi::Desc& desc, std::size_t out_size,
                         const std::string& what) {
    SCOPED_TRACE(what);
    Bytes out(out_size, kSentinel);
    const std::size_t n = qoi::encode(pix, desc, out);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(qoi_test::all_equal(out, 0, out.size(), kSentinel)) << "output was modified";
}

qoi::Desc parsed_desc(const Bytes& data) {
    qoi::Desc desc = qoi_test::poisoned_desc();
    qoi::get_desc(data, desc);
    return desc;
}

}  // namespace

// ===========================================================================
// get_desc
// ===========================================================================

// Post: for a valid header the descriptor holds the header fields.
TEST(ContractGetDesc, ParsesAllHeaderFields) {
    const Bytes data = qoi_test::header(0x01020304U, 0x0A0B0C0DU, 4, 1);
    qoi::Desc desc = qoi_test::poisoned_desc();
    qoi::get_desc(data, desc);
    EXPECT_TRUE(DescEq(make_desc(0x01020304U, 0x0A0B0C0DU, 4, qoi::Colorspace::SRGB_Linear_Alpha), desc));

    qoi::Desc other = qoi_test::poisoned_desc();
    qoi::get_desc(qoi_test::header(7, 9, 3, 0), other);
    EXPECT_TRUE(DescEq(make_desc(7, 9, 3, qoi::Colorspace::SRGB), other));
}

// Exactly 14 bytes are enough; anything after the header is ignored.
TEST(ContractGetDesc, HeaderOnlyAndTrailingBytes) {
    const Bytes header = qoi_test::header(5, 6, 3, 0);
    EXPECT_TRUE(DescEq(make_desc(5, 6, 3), parsed_desc(header)));

    Bytes longer = header;
    longer.insert(longer.end(), 100, 0xEE);
    EXPECT_TRUE(DescEq(make_desc(5, 6, 3), parsed_desc(longer)));
}

// Data shorter than 14 bytes: the empty descriptor, whatever the bytes are.
TEST(ContractGetDesc, ShortDataGivesEmptyDescriptor) {
    const Bytes header = qoi_test::header(5, 6, 3, 0);
    for (std::size_t size = 0; size < 14; ++size) {
        const Bytes data(header.begin(), header.begin() + static_cast<std::ptrdiff_t>(size));
        const ExactBytes exact(data);
        qoi::Desc desc = qoi_test::poisoned_desc();
        qoi::get_desc(exact.span(), desc);
        EXPECT_TRUE(DescEq(qoi_test::empty_desc(), desc)) << "size " << size;
    }
    // A null view of length 0.
    qoi::Desc desc = qoi_test::poisoned_desc();
    qoi::get_desc(qoi::Span<const std::uint8_t>(), desc);
    EXPECT_TRUE(DescEq(qoi_test::empty_desc(), desc));
}

// Each of the four magic bytes is checked, and the comparison is case sensitive.
TEST(ContractGetDesc, BadMagicGivesEmptyDescriptor) {
    for (std::size_t i = 0; i < 4; ++i) {
        Bytes data = qoi_test::header(5, 6, 3, 0);
        data[i] = qoi_test::u8(data[i] ^ 0x01U);
        EXPECT_TRUE(DescEq(qoi_test::empty_desc(), parsed_desc(data))) << "magic byte " << i;
    }
    Bytes upper = qoi_test::header(5, 6, 3, 0);
    upper[0] = 'Q';
    upper[1] = 'O';
    upper[2] = 'I';
    upper[3] = 'F';
    EXPECT_TRUE(DescEq(qoi_test::empty_desc(), parsed_desc(upper)));
    EXPECT_TRUE(DescEq(qoi_test::empty_desc(), parsed_desc(Bytes(14, 0))));
    EXPECT_TRUE(DescEq(qoi_test::empty_desc(), parsed_desc(Bytes(14, 0xFF))));
}

// Colorspace byte: 0 and 1 are valid, everything else yields the empty descriptor.
TEST(ContractGetDesc, ColorspaceByteMustBeZeroOrOne) {
    for (unsigned cs = 0; cs < 256; ++cs) {
        const Bytes data = qoi_test::header(5, 6, 3, qoi_test::u8(cs));
        const qoi::Desc desc = parsed_desc(data);
        if (cs <= 1) {
            EXPECT_TRUE(DescEq(make_desc(5, 6, 3, qoi_test::colorspace_of(cs)), desc)) << cs;
        } else {
            EXPECT_TRUE(DescEq(qoi_test::empty_desc(), desc)) << cs;
        }
    }
}

// get_desc does not validate channels or dimensions (Ada: Get_Desc only checks magic and
// colorspace); decode and valid_size are responsible for that.
TEST(ContractGetDesc, ChannelsAndDimensionsAreNotRangeChecked) {
    for (const unsigned channels : {0U, 1U, 2U, 5U, 255U}) {
        const qoi::Desc desc = parsed_desc(qoi_test::header(5, 6, qoi_test::u8(channels), 0));
        EXPECT_TRUE(DescEq(make_desc(5, 6, channels), desc)) << channels;
    }
    EXPECT_TRUE(DescEq(make_desc(0, 0, 3), parsed_desc(qoi_test::header(0, 0, 3, 0))));
    EXPECT_TRUE(DescEq(make_desc(0xFFFFFFFFULL, 0xFFFFFFFFULL, 4),
                       parsed_desc(qoi_test::header(0xFFFFFFFFU, 0xFFFFFFFFU, 4, 0))));
    EXPECT_TRUE(DescEq(make_desc(0x80000000ULL, 1, 3), parsed_desc(qoi_test::header(0x80000000U, 1, 3, 0))));
}

// The returned descriptor is the complete result, the previous contents never leak
// through (also for the failure case).
TEST(ContractGetDesc, OverwritesEveryFieldOfTheOutput) {
    qoi::Desc ok = qoi_test::poisoned_desc();
    qoi::get_desc(qoi_test::header(1, 1, 3, 0), ok);
    EXPECT_TRUE(DescEq(make_desc(1, 1, 3, qoi::Colorspace::SRGB), ok));

    qoi::Desc bad = qoi_test::poisoned_desc();
    qoi::get_desc(Bytes(20, 0x55), bad);
    EXPECT_EQ(bad.width, 0u);
    EXPECT_EQ(bad.height, 0u);
    EXPECT_EQ(bad.channels, 0u);
    EXPECT_EQ(bad.colorspace, qoi::Colorspace::SRGB);
}

// ===========================================================================
// valid_size / encode_worst_case (details of the numeric boundaries are in golden_test.cpp)
// ===========================================================================

// valid_size is the guard of encode and of encode_worst_case: they agree on every
// descriptor of a grid around the boundaries.
TEST(ContractValidSize, EncodeWorstCaseIsZeroExactlyWhenInvalid) {
    const U64List dims = {0, 1, 2, 3, 1000, kMaxDim - 1, kMaxDim, kMaxDim + 1, 0xFFFFFFFFULL,
                          1ULL << 32, 1ULL << 62, kStorageLast, 0xFFFFFFFFFFFFFFFFULL};
    const U64List channels = {0, 1, 2, 3, 4, 5, 255, 0xFFFFFFFFFFFFFFFFULL};
    for (const std::uint64_t w : dims) {
        for (const std::uint64_t h : dims) {
            for (const std::uint64_t c : channels) {
                const qoi::Desc desc = make_desc(w, h, c);
                const std::uint64_t worst = qoi::encode_worst_case(desc);
                if (qoi::valid_size(desc)) {
                    EXPECT_GE(worst, 22u) << w << "x" << h << "x" << c;
                    EXPECT_LE(worst, kStorageLast);
                    EXPECT_EQ(worst, w * h * (c + 1) + 22);
                } else {
                    EXPECT_EQ(worst, 0u) << w << "x" << h << "x" << c;
                }
            }
        }
    }
}

// valid_size is independent of the colorspace.
TEST(ContractValidSize, ColorspaceIsIrrelevant) {
    for (const std::uint64_t c : U64List{0, 3, 4, 5}) {
        EXPECT_EQ(qoi::valid_size(make_desc(10, 10, c, qoi::Colorspace::SRGB)),
                  qoi::valid_size(make_desc(10, 10, c, qoi::Colorspace::SRGB_Linear_Alpha)));
    }
}

// ===========================================================================
// encode: precondition violations and failure paths
// ===========================================================================

// Pre: Valid_Size (Desc). An invalid descriptor returns 0 before the pixel or the output
// view is looked at (both are empty here, so reading them would be undefined).
TEST(ContractEncode, InvalidDescriptorReturnsZero) {
    const std::vector<qoi::Desc> invalid = {
        make_desc(1, 1, 0),        make_desc(1, 1, 1),         make_desc(1, 1, 2),
        make_desc(1, 1, 5),        make_desc(1, 1, 255),       make_desc(0, 1, 3),
        make_desc(1, 0, 3),        make_desc(0, 0, 4),         make_desc(kMaxDim + 1, 1, 3),
        make_desc(1, kMaxDim + 1, 4), make_desc(0xFFFFFFFFULL, 1, 3), make_desc(kMaxDim, kMaxDim, 3),
        make_desc(kMaxDim, kMaxDim, 4), make_desc(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 4),
        make_desc(1, 1, 0xFFFFFFFFFFFFFFFFULL),
    };
    for (const qoi::Desc& desc : invalid) {
        SCOPED_TRACE(std::to_string(desc.width) + "x" + std::to_string(desc.height) + "x" +
                     std::to_string(desc.channels));
        EXPECT_FALSE(qoi::valid_size(desc));
        EXPECT_EQ(qoi::encode(qoi::Span<const std::uint8_t>(), desc, qoi::Span<std::uint8_t>()), 0u);
    }
    // And with real buffers that would be big enough for a 1x1 image.
    expect_encode_fails(Bytes(3, 1), make_desc(1, 1, 2), 64, "channels 2");
    expect_encode_fails(Bytes(0), make_desc(0, 1, 3), 64, "width 0");
    expect_encode_fails(Bytes(0), make_desc(1, 0, 4), 64, "height 0");
}

// Pre: Pix'Length = Width * Height * Channels. Off by one in either direction, an empty
// view, and the pixel data of the other channel count are all rejected.
TEST(ContractEncode, PixelLengthMustMatchDescriptor) {
    for (const std::size_t channels : {3U, 4U}) {
        const qoi::Desc desc = make_desc(3, 2, channels);
        const std::size_t good = 6 * channels;
        const std::size_t worst = static_cast<std::size_t>(qoi::encode_worst_case(desc));
        expect_encode_fails(Bytes(good - 1, 7), desc, worst, "one byte short");
        expect_encode_fails(Bytes(good + 1, 7), desc, worst, "one byte too many");
        expect_encode_fails(Bytes(0), desc, worst, "empty pixel data");
        expect_encode_fails(Bytes(good * 2, 7), desc, worst, "twice the data");
        expect_encode_fails(Bytes(6 * (channels == 3 ? 4U : 3U), 7), desc, worst,
                            "pixel data of the other channel count");
        // Sanity: the right length is accepted.
        Bytes out(worst, kSentinel);
        EXPECT_GT(qoi::encode(Bytes(good, 7), desc, out), 0u);
    }
}

// A descriptor that is valid but huge cannot match a small pixel view: the size mismatch
// is detected from the arithmetic alone, without touching any memory.
TEST(ContractEncode, HugeValidDescriptorWithSmallBuffersReturnsZero) {
    const Bytes tiny(16, 1);
    const std::vector<qoi::Desc> huge = {
        make_desc(1ULL << 30, 1ULL << 30, 3),
        make_desc(1ULL << 30, 1ULL << 30, 4),
        make_desc(2120185131ULL, 1087566824ULL, 3),  // exact fit below Storage_Count'Last
        make_desc(2147483641ULL, 858993462ULL, 4),   // exact fit below Storage_Count'Last
        make_desc(kMaxDim, 1, 4),
    };
    for (const qoi::Desc& desc : huge) {
        ASSERT_TRUE(qoi::valid_size(desc));
        expect_encode_fails(tiny, desc, 64, "huge descriptor, tiny pixel and output buffers");
    }
}

// Pre (Ada: Output'Length >= worst case via the guard in Encode): the output view must hold
// Encode_Worst_Case (Desc) bytes, even if the actual stream would be shorter.
TEST(ContractEncode, OutputSmallerThanWorstCaseReturnsZeroAndIsUntouched) {
    const qoi::Desc desc = make_desc(1, 1, 3);
    const Bytes pix = {0, 0, 0};  // encodes to 23 bytes (header, C0, padding)
    ASSERT_EQ(qoi::encode_worst_case(desc), 26u);
    for (const std::size_t out_size : {0U, 1U, 13U, 14U, 22U, 23U, 24U, 25U}) {
        expect_encode_fails(pix, desc, out_size, "output of " + std::to_string(out_size));
    }
    // Exactly the worst case: success, and 23 bytes are written.
    Bytes out(26, kSentinel);
    EXPECT_EQ(qoi::encode(pix, desc, out), 23u);
    EXPECT_TRUE(qoi_test::all_equal(out, 23, 26, kSentinel));

    // A larger buffer is fine too and nothing beyond the stream is written.
    Bytes large(26 + 100, kSentinel);
    EXPECT_EQ(qoi::encode(pix, desc, large), 23u);
    EXPECT_TRUE(qoi_test::all_equal(large, 23, large.size(), kSentinel));

    // An empty (null) output view.
    EXPECT_EQ(qoi::encode(pix, desc, qoi::Span<std::uint8_t>()), 0u);

    // Same boundary with 4 channels: worst case = 1*5 + 22 = 27.
    const qoi::Desc rgba = make_desc(1, 1, 4);
    ASSERT_EQ(qoi::encode_worst_case(rgba), 27u);
    expect_encode_fails(Bytes{0, 0, 0, 255}, rgba, 26, "RGBA one byte below worst case");
    Bytes rgba_out(27, kSentinel);
    EXPECT_EQ(qoi::encode(Bytes{0, 0, 0, 255}, rgba, rgba_out), 23u);
}

// The same boundary on a larger image: worst case - 1 is rejected, worst case accepted.
TEST(ContractEncode, WorstCaseMinusOneIsRejectedForLargerImages) {
    for (const std::size_t channels : {3U, 4U}) {
        const qoi::Desc desc = make_desc(17, 13, channels);
        const Bytes pix = qoi_test::noise_image(17, 13, channels, 123);
        const std::size_t worst = static_cast<std::size_t>(qoi::encode_worst_case(desc));
        expect_encode_fails(pix, desc, worst - 1, "worst-1");
        Bytes out(worst, kSentinel);
        const std::size_t n = qoi::encode(pix, desc, out);
        EXPECT_GE(n, 22u);
        EXPECT_LE(n, worst);
    }
}

// Check order: an invalid descriptor wins over a bad pixel length over a short output; all
// give 0 and none writes.
TEST(ContractEncode, MultipleViolationsAllReturnZero) {
    expect_encode_fails(Bytes(1), make_desc(0, 0, 3), 0, "everything wrong");
    expect_encode_fails(Bytes(1), make_desc(2, 2, 3), 3, "wrong pixels and short output");
}

// ===========================================================================
// decode: precondition and failure paths
// ===========================================================================

// Pre: Data'Length >= 22 (header + padding). Shorter data returns 0, output untouched.
// desc is whatever get_desc returns for the bytes present: the parsed header from 14
// bytes on, the empty descriptor below that.
TEST(ContractDecode, DataShorterThan22BytesReturnsZero) {
    const Bytes full = valid_stream_rgb();  // 26 bytes, header valid
    for (std::size_t size = 0; size < 22; ++size) {
        const Bytes data(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(size));
        const qoi::Desc expected = size >= 14 ? make_desc(1, 1, 3) : qoi_test::empty_desc();
        expect_decode_fails(data, 64, expected, "truncated to " + std::to_string(size));
    }
    // Null data view.
    qoi::Desc desc = qoi_test::poisoned_desc();
    Bytes out(8, kSentinel);
    EXPECT_EQ(qoi::decode(qoi::Span<const std::uint8_t>(), desc, out), 0u);
    EXPECT_TRUE(DescEq(qoi_test::empty_desc(), desc));
    EXPECT_TRUE(qoi_test::all_equal(out, 0, out.size(), kSentinel));
}

// Exactly 22 bytes (header + padding, no chunks) is accepted: the whole image is the
// initial pixel (0,0,0,255) because no chunk is ever read (p = 14 > Last_Chunk = 13).
TEST(ContractDecode, TwentyTwoByteStreamDecodesToInitialPixel) {
    expect_decodes_to(raw_stream(2, 2, 3, 0, {}), make_desc(2, 2, 3), Bytes(12, 0), "RGB, no chunks");
    expect_decodes_to(raw_stream(2, 2, 4, 1, {}), make_desc(2, 2, 4, qoi::Colorspace::SRGB_Linear_Alpha),
                      pixel_bytes({{0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}}),
                      "RGBA, no chunks");
    // The padding is never verified: even junk that looks like chunks is not read.
    expect_decodes_to(stream_with_tail(1, 1, 3, {}, Bytes(8, 0xFE)), make_desc(1, 1, 3), Bytes(3, 0),
                      "junk padding, 1x1");
    expect_decodes_to(stream_with_tail(3, 1, 4, {}, Bytes(8, 0x7F)), make_desc(3, 1, 4),
                      pixel_bytes({{0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}}), "DIFF-looking padding");
}

// Invalid header: the call fails and desc is the empty descriptor.
TEST(ContractDecode, BadMagicOrColorspaceReturnsZeroWithEmptyDescriptor) {
    Bytes bad_magic = valid_stream_rgb();
    bad_magic[0] = 'x';
    expect_decode_fails(bad_magic, 16, qoi_test::empty_desc(), "bad magic");

    Bytes bad_cs = valid_stream_rgb();
    bad_cs[13] = 2;
    expect_decode_fails(bad_cs, 16, qoi_test::empty_desc(), "colorspace 2");
    bad_cs[13] = 0xFF;
    expect_decode_fails(bad_cs, 16, qoi_test::empty_desc(), "colorspace 255");
}

// Channels other than 3 and 4 and zero dimensions fail, and desc keeps the parsed header
// (design decision 4: follow the Ada order and leave Desc as get_desc produced it).
TEST(ContractDecode, InvalidChannelsOrDimensionsKeepParsedDescriptor) {
    for (const unsigned channels : {0U, 1U, 2U, 5U, 6U, 255U}) {
        expect_decode_fails(raw_stream(1, 1, qoi_test::u8(channels), 0, {0xC0}), 64,
                            make_desc(1, 1, channels), "channels " + std::to_string(channels));
    }
    expect_decode_fails(raw_stream(0, 1, 3, 0, {0xC0}), 64, make_desc(0, 1, 3), "width 0");
    expect_decode_fails(raw_stream(1, 0, 3, 1, {0xC0}), 64,
                        make_desc(1, 0, 3, qoi::Colorspace::SRGB_Linear_Alpha), "height 0");
    expect_decode_fails(raw_stream(0, 0, 4, 0, {0xC0}), 64, make_desc(0, 0, 4), "both zero");
}

// Dimensions whose product overflows or exceeds any real buffer: the call fails by
// arithmetic alone (the divisions in the checks never overflow) with a small output.
TEST(ContractDecode, HugeDimensionsReturnZeroWithoutAllocating) {
    const std::vector<std::array<std::uint32_t, 3>> cases = {
        {0xFFFFFFFFU, 0xFFFFFFFFU, 4}, {0xFFFFFFFFU, 0xFFFFFFFFU, 3}, {0x7FFFFFFFU, 0x7FFFFFFFU, 4},
        {0x7FFFFFFFU, 0x7FFFFFFFU, 3}, {0xFFFFFFFFU, 1, 4},           {1, 0xFFFFFFFFU, 3},
        {0x80000000U, 2, 3},           {0x10000U, 0x10000U, 3},       {0xFFFFU, 0xFFFFU, 4},
    };
    for (const auto& c : cases) {
        const qoi::Desc expected = make_desc(c[0], c[1], c[2]);
        expect_decode_fails(raw_stream(c[0], c[1], qoi_test::u8(c[2]), 0, {0xC0}), 64, expected,
                            "huge " + std::to_string(c[0]) + "x" + std::to_string(c[1]));
    }
}

// Post/Pre: Output'Length >= Width * Height * Channels (the size get_desc reports). One byte
// too few fails, exactly enough succeeds, more than enough succeeds and returns the image size.
TEST(ContractDecode, OutputBufferSizeBoundary) {
    for (const std::size_t channels : {3U, 4U}) {
        const Bytes pix = qoi_test::noise_image(5, 4, channels, 321);
        const qoi::Desc desc = make_desc(5, 4, channels);
        Bytes stream(qoi::encode_worst_case(desc));
        stream.resize(qoi::encode(pix, desc, stream));
        const std::size_t need = pix.size();

        for (const std::size_t too_small : {std::size_t{0}, std::size_t{1}, need / 2, need - 1}) {
            expect_decode_fails(stream, too_small, desc, "output of " + std::to_string(too_small));
        }
        expect_decodes_to(stream, desc, pix, "exact output");

        const ExactBytes exact(stream);
        Bytes large(need + 64, kSentinel);
        qoi::Desc out_desc = qoi_test::poisoned_desc();
        EXPECT_EQ(qoi::decode(exact.span(), out_desc, large), need);
        EXPECT_TRUE(BytesEqual(pix, large, need));
        EXPECT_TRUE(qoi_test::all_equal(large, need, large.size(), kSentinel));
    }
    // A null output view.
    qoi::Desc desc = qoi_test::poisoned_desc();
    EXPECT_EQ(qoi::decode(valid_stream_rgb(), desc, qoi::Span<std::uint8_t>()), 0u);
    EXPECT_TRUE(DescEq(make_desc(1, 1, 3), desc));
}

// The decoder ignores the descriptor it is given on entry, whatever it holds.
TEST(ContractDecode, InputDescriptorIsIgnored) {
    const Bytes stream = valid_stream_rgb();
    for (const qoi::Desc& initial : {qoi_test::empty_desc(), qoi_test::poisoned_desc(), make_desc(9, 9, 4)}) {
        qoi::Desc desc = initial;
        Bytes out(3);
        EXPECT_EQ(qoi::decode(stream, desc, out), 3u);
        EXPECT_TRUE(DescEq(make_desc(1, 1, 3), desc));
        EXPECT_TRUE(BytesEqual(Bytes{10, 20, 30}, out));
    }
}

// ===========================================================================
// decode: chunk semantics on hand-crafted streams
// ===========================================================================

// Post: Output_Size = Width * Height * Channels.
TEST(ContractDecode, ReturnsImageSizeNotStreamSize) {
    const Bytes stream = qoi_test::stream(10, 10, 4, 0, {0xFD, 0xFC});  // two runs: 62 + 61 pixels
    Bytes out(400);
    qoi::Desc desc;
    EXPECT_EQ(qoi::decode(stream, desc, out), 400u);
}

// The index is refreshed after a RUN chunk too (Ada line 460: the store follows every chunk).
// Stream (4 channels): [C0] is a RUN chunk with field 0 -> a single pixel (0,0,0,255) and the
// store index[hash(0,0,0,255) = 53] := (0,0,0,255). [35] is INDEX 53 = 0x35 and so yields
// (0,0,0,255). Without the refresh the slot would still be (0,0,0,0) and the alpha would be 0.
TEST(ContractDecode, IndexIsRefreshedAfterRunChunk) {
    expect_decodes_to(raw_stream(2, 1, 4, 0, {0xC0, 0x35}), make_desc(2, 1, 4),
                      pixel_bytes({{0, 0, 0, 255}, {0, 0, 0, 255}}), "RUN then INDEX 53");
}

// INDEX into a never-written slot gives (0,0,0,0), alpha 0 included.
// 4 channels, 2x1: [FE 0A 14 1E] -> (10,20,30,255) stored in slot 9; [01] = INDEX 1, slot 1 is
// empty -> (0,0,0,0).
TEST(ContractDecode, IndexOfEmptySlotYieldsTransparentBlack) {
    expect_decodes_to(raw_stream(2, 1, 4, 0, {0xFE, 0x0A, 0x14, 0x1E, 0x01}), make_desc(2, 1, 4),
                      pixel_bytes({{10, 20, 30, 255}, {0, 0, 0, 0}}), "INDEX 1 on empty slot");
    // INDEX 0 as the very first chunk, 1x1: slot 0 is (0,0,0,0).
    expect_decodes_to(raw_stream(1, 1, 4, 0, {0x00}), make_desc(1, 1, 4), pixel_bytes({{0, 0, 0, 0}}),
                      "INDEX 0 first");
    // With 3 channels the colour is (0,0,0) (alpha is not output).
    expect_decodes_to(raw_stream(1, 1, 3, 0, {0x00}), make_desc(1, 1, 3), Bytes{0, 0, 0}, "INDEX 0 first, RGB");
}

// INDEX hit after a coded pixel returns that pixel. [FE 0A 14 1E] -> (10,20,30), slot 9;
// [FE 28 32 3C] -> (40,50,60), slot hash(40,50,60,255) = 11; [09] -> slot 9 = (10,20,30).
TEST(ContractDecode, IndexHitRestoresEarlierPixel) {
    expect_decodes_to(raw_stream(3, 1, 3, 0, {0xFE, 0x0A, 0x14, 0x1E, 0xFE, 0x28, 0x32, 0x3C, 0x09}),
                      make_desc(3, 1, 3), Bytes{10, 20, 30, 40, 50, 60, 10, 20, 30}, "INDEX 9");
}

// RGBA chunk in a 3-channel stream: five bytes are consumed, alpha is not output but it does
// feed the hash (decode is driven by the stream, not by the channel count).
// [FF 0A 14 1E 80] -> pixel (10,20,30,128), slot hash = (30+100+210+1408) mod 64 = 20.
// [14] = INDEX 20 -> the same pixel again. With the 3-channel hash (alpha 255) it would sit in
// slot 9 and INDEX 20 would be empty, giving (0,0,0).
TEST(ContractDecode, RgbaChunkInThreeChannelStreamStillUpdatesAlphaState) {
    expect_decodes_to(raw_stream(2, 1, 3, 0, {0xFF, 0x0A, 0x14, 0x1E, 0x80, 0x14}), make_desc(2, 1, 3),
                      Bytes{10, 20, 30, 10, 20, 30}, "RGBA chunk, RGB output");
}

// RGB, DIFF and LUMA chunks keep the current alpha. 4 channels, 3x1:
// [FF 01 02 03 40] -> (1,2,3,64); [FE 09 09 09] -> (9,9,9,64) (alpha kept);
// [6A] = DIFF 0,0,0 (01 10 10 10) -> (9,9,9,64).
TEST(ContractDecode, RgbDiffLumaKeepAlpha) {
    expect_decodes_to(raw_stream(3, 1, 4, 0, {0xFF, 0x01, 0x02, 0x03, 0x40, 0xFE, 0x09, 0x09, 0x09, 0x6A}),
                      make_desc(3, 1, 4), pixel_bytes({{1, 2, 3, 64}, {9, 9, 9, 64}, {9, 9, 9, 64}}),
                      "alpha kept");
    // LUMA with dg = 0 and both relative nibbles 8 (no change): [A0 88].
    expect_decodes_to(raw_stream(2, 1, 4, 0, {0xFF, 0x01, 0x02, 0x03, 0x40, 0xA0, 0x88}), make_desc(2, 1, 4),
                      pixel_bytes({{1, 2, 3, 64}, {1, 2, 3, 64}}), "LUMA no-op keeps alpha");
}

// DIFF wraps modulo 256 in both directions.
// [4A] = 01 00 10 10 = (-2,0,0) from (0,0,0) -> (254,0,0).
// [FE FF FF FF][7F]: (255,255,255) then (+1,+1,+1) -> (0,0,0).
TEST(ContractDecode, DiffWrapsModulo256) {
    expect_decodes_to(raw_stream(1, 1, 3, 0, {0x4A}), make_desc(1, 1, 3), Bytes{254, 0, 0}, "DIFF below 0");
    expect_decodes_to(raw_stream(2, 1, 3, 0, {0xFE, 0xFF, 0xFF, 0xFF, 0x7F}), make_desc(2, 1, 3),
                      Bytes{255, 255, 255, 0, 0, 0}, "DIFF above 255");
    // The extreme DIFF values from (0,0,0): 0x40 = (-2,-2,-2) -> (254,254,254).
    expect_decodes_to(raw_stream(1, 1, 3, 0, {0x40}), make_desc(1, 1, 3), Bytes{254, 254, 254}, "DIFF -2,-2,-2");
}

// LUMA wraps modulo 256.
// [FE FA 64 FA] -> (250,100,250); [A0 FF] = dg 0, dr-dg +7, db-dg +7 -> (257,100,257) = (1,100,1).
// [FE 05 05 05] -> (5,5,5); [80 88] = dg -32, relative 0 -> (-27,-27,-27) = (229,229,229).
TEST(ContractDecode, LumaWrapsModulo256) {
    expect_decodes_to(raw_stream(2, 1, 3, 0, {0xFE, 0xFA, 0x64, 0xFA, 0xA0, 0xFF}), make_desc(2, 1, 3),
                      Bytes{250, 100, 250, 1, 100, 1}, "LUMA above 255");
    expect_decodes_to(raw_stream(2, 1, 3, 0, {0xFE, 0x05, 0x05, 0x05, 0x80, 0x88}), make_desc(2, 1, 3),
                      Bytes{5, 5, 5, 229, 229, 229}, "LUMA below 0");
}

// RUN chunk lengths: field k gives k+1 pixels; 62 pixels is the maximum (0xFD).
TEST(ContractDecode, RunChunkLengths) {
    for (const unsigned field : {0U, 1U, 2U, 30U, 60U, 61U}) {
        const std::size_t length = field + 1;
        const Bytes chunk = {qoi_test::u8(0xC0U | field)};
        // Exactly the run, then a different pixel; 4 channels.
        Bytes chunks = chunk;
        chunks.insert(chunks.end(), {0xFE, 0x0A, 0x14, 0x1E});
        Bytes expected;
        for (std::size_t i = 0; i < length; ++i) {
            expected.insert(expected.end(), {0, 0, 0, 255});
        }
        expected.insert(expected.end(), {10, 20, 30, 255});
        expect_decodes_to(raw_stream(static_cast<std::uint32_t>(length + 1), 1, 4, 0, chunks),
                          make_desc(length + 1, 1, 4), expected, "run field " + std::to_string(field));
    }
    // 0xFE and 0xFF are not runs of length 63 and 64: they are RGB and RGBA.
    expect_decodes_to(raw_stream(1, 1, 3, 0, {0xFE, 1, 2, 3}), make_desc(1, 1, 3), Bytes{1, 2, 3}, "FE is RGB");
    expect_decodes_to(raw_stream(1, 1, 4, 0, {0xFF, 1, 2, 3, 4}), make_desc(1, 1, 4), Bytes{1, 2, 3, 4}, "FF is RGBA");
}

// A run longer than the image just ends with the image. 1x2 image, [FD] (62 pixels).
TEST(ContractDecode, RunLongerThanImageIsTruncated) {
    expect_decodes_to(raw_stream(1, 2, 3, 0, {0xFD}), make_desc(1, 2, 3), Bytes(6, 0), "run 62 in 2 pixels");
}

// A run carries across and is consumed before the next chunk is read: [C1 FE 01 02 03] with
// 4 pixels is (0,0,0) x2 then (1,2,3) then, no chunk left (S-9 reached), (1,2,3) repeats.
TEST(ContractDecode, RunThenChunkThenRepeatAfterLastChunk) {
    expect_decodes_to(raw_stream(4, 1, 3, 0, {0xC1, 0xFE, 0x01, 0x02, 0x03}), make_desc(4, 1, 3),
                      Bytes{0, 0, 0, 0, 0, 0, 1, 2, 3, 1, 2, 3}, "run, RGB, repeat");
}

// Pixels beyond the last chunk repeat the previous pixel (Ada: no chunk is read when
// P > Last_Chunk). A 3x1 image with a single RGB chunk (S = 26, Last_Chunk offset 17):
// pixel 1 would read at offset 18 > 17, so it repeats (10,20,30) even though the tail bytes
// are 7F (a DIFF +1,+1,+1 if they were read).
TEST(ContractDecode, PixelsBeyondLastChunkRepeatPreviousPixel) {
    expect_decodes_to(stream_with_tail(3, 1, 3, {0xFE, 0x0A, 0x14, 0x1E}, Bytes(8, 0x7F)), make_desc(3, 1, 3),
                      Bytes{10, 20, 30, 10, 20, 30, 10, 20, 30}, "repeat after chunks");
    // A bigger image than the stream describes: 1000x1 pixels from one chunk, 3 MB-free: 3000 bytes.
    Bytes expected;
    for (std::size_t i = 0; i < 1000; ++i) {
        expected.insert(expected.end(), {10, 20, 30});
    }
    expect_decodes_to(raw_stream(1000, 1, 3, 0, {0xFE, 0x0A, 0x14, 0x1E}), make_desc(1000, 1, 3), expected,
                      "1000 pixels from one chunk");
}

// Boundary of Last_Chunk: a chunk whose first byte is at S-9 is read, one at S-8 is not.
// 2x1 3-channel image with the chunk area [7F] (S = 23, S-9 = 14):
//   pixel 0 reads 7F at offset 14 -> (1,1,1);
//   pixel 1 would read at offset 15 = S-8 (first padding byte) -> not read, (1,1,1) repeats.
// The padding is 8 bytes of 7F so that a read would be visible as (2,2,2).
TEST(ContractDecode, ChunkAtLastChunkOffsetIsReadNextOneIsNot) {
    expect_decodes_to(stream_with_tail(2, 1, 3, {0x7F}, Bytes(8, 0x7F)), make_desc(2, 1, 3),
                      Bytes{1, 1, 1, 1, 1, 1}, "boundary S-9 / S-8");
}

// A multi-byte chunk that starts at S-9 reads into the padding (the padding is not
// verified, it is simply the source of the operands). Tails are chosen so the result differs
// from the regular padding.
TEST(ContractDecode, ChunkStartingAtLastChunkOffsetMayConsumePadding) {
    // RGB opcode as the only chunk byte: the operands are the first three tail bytes.
    expect_decodes_to(stream_with_tail(1, 1, 3, {0xFE}, Bytes{0x0A, 0x14, 0x1E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
                      make_desc(1, 1, 3), Bytes{10, 20, 30}, "RGB straddling the padding");
    // With the regular padding the operands are zero.
    expect_decodes_to(raw_stream(1, 1, 3, 0, {0xFE}), make_desc(1, 1, 3), Bytes{0, 0, 0},
                      "RGB straddling the regular padding");
    // RGBA: four operands from the tail.
    expect_decodes_to(stream_with_tail(1, 1, 4, {0xFF}, Bytes{1, 2, 3, 4, 9, 9, 9, 9}), make_desc(1, 1, 4),
                      Bytes{1, 2, 3, 4}, "RGBA straddling the padding");
    // LUMA: the second byte comes from the tail. dg = -32 ([80]) and tail byte 00 (relative -8, -8):
    // r = 0 - 32 - 8 = -40 = 216, g = -32 = 224, b = 216.
    expect_decodes_to(raw_stream(1, 1, 3, 0, {0x80}), make_desc(1, 1, 3), Bytes{216, 224, 216},
                      "LUMA straddling the padding");
    // Same chunk with a tail byte of 0x88 (relative 0, 0): (-32,-32,-32) = (224,224,224).
    expect_decodes_to(stream_with_tail(1, 1, 3, {0x80}, Bytes{0x88, 0, 0, 0, 0, 0, 0, 1}), make_desc(1, 1, 3),
                      Bytes{224, 224, 224}, "LUMA straddling a custom tail");
}

// A 1-pixel-per-chunk stream whose chunk bytes end exactly at the padding decodes cleanly
// (the regular case, as a control for the straddling tests).
TEST(ContractDecode, RegularStreamsNeverNeedThePaddingAsOperands) {
    const Bytes pix = qoi_test::noise_image(9, 9, 4, 55);
    const qoi::Desc desc = make_desc(9, 9, 4);
    Bytes stream(qoi::encode_worst_case(desc));
    stream.resize(qoi::encode(pix, desc, stream));
    // Replace the padding with garbage: the result must not change.
    for (std::size_t i = stream.size() - 8; i < stream.size(); ++i) {
        stream[i] = 0xFF;
    }
    expect_decodes_to(stream, desc, pix, "garbage padding after a valid body");
}

// ===========================================================================
// decode: robustness (spec decision 5: no crash, no hang, no out-of-bounds access on any input)
// ===========================================================================

// Every truncation of a valid stream, from empty to full length, with an exact-size heap copy
// of the data so AddressSanitizer sees any read past the end.
TEST(RobustnessDecode, EveryTruncationOfValidStreams) {
    std::uint32_t seed = 600;
    for (const std::size_t channels : {3U, 4U}) {
        const std::size_t w = 6;
        const std::size_t h = 5;
        const qoi::Desc desc = make_desc(w, h, channels);
        for (const Bytes& pix : {qoi_test::noise_image(w, h, channels, ++seed),
                                 qoi_test::runs_and_noise_image(w, h, channels, ++seed),
                                 qoi_test::near_equal_image(w, h, channels, ++seed, 3, 128)}) {
            Bytes stream(qoi::encode_worst_case(desc));
            stream.resize(qoi::encode(pix, desc, stream));
            for (std::size_t size = 0; size <= stream.size(); ++size) {
                SCOPED_TRACE("truncated to " + std::to_string(size) + " of " + std::to_string(stream.size()));
                const Bytes data(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(size));
                const ExactBytes exact(data);
                Bytes out(pix.size() + 8, kSentinel);
                qoi::Desc decoded = qoi_test::poisoned_desc();
                const std::size_t n =
                    qoi::decode(exact.span(), decoded, qoi::Span<std::uint8_t>(out.data(), pix.size()));
                if (size < 22) {
                    EXPECT_EQ(n, 0u);
                    EXPECT_TRUE(qoi_test::all_equal(out, 0, out.size(), kSentinel));
                } else {
                    EXPECT_EQ(n, pix.size());
                    EXPECT_TRUE(DescEq(desc, decoded));
                    EXPECT_TRUE(qoi_test::all_equal(out, pix.size(), out.size(), kSentinel));
                }
            }
            // The full stream restores the pixels (control).
            expect_decodes_to(stream, desc, pix, "full stream");
        }
    }
}

// The padding content is irrelevant for encoder-produced streams: every chunk ends at or
// before offset S-8 (the encoder's last chunk never needs operand bytes from the padding), so
// any filler value in the last 8 bytes leaves the decoded image unchanged.
TEST(RobustnessDecode, StreamWithFullPaddingMatchesAfterPaddingContentChange) {
    const Bytes pix = qoi_test::runs_and_noise_image(11, 7, 3, 77);
    const qoi::Desc desc = make_desc(11, 7, 3);
    Bytes stream(qoi::encode_worst_case(desc));
    stream.resize(qoi::encode(pix, desc, stream));
    for (const std::uint8_t filler : std::array<std::uint8_t, 6>{0x00, 0x01, 0x7F, 0xC0, 0xFE, 0xFF}) {
        Bytes changed = stream;
        for (std::size_t i = changed.size() - 8; i < changed.size(); ++i) {
            changed[i] = filler;
        }
        expect_decodes_to(changed, desc, pix, "padding filled with " + std::to_string(filler));
    }
}

// A valid stream followed by trailing garbage: the garbage is treated as part of the
// "padding region" (the last 8 bytes) plus unread chunk area, never causing an overread.
TEST(RobustnessDecode, TrailingGarbageAfterValidStream) {
    const Bytes pix = qoi_test::noise_image(4, 4, 3, 91);
    const qoi::Desc desc = make_desc(4, 4, 3);
    Bytes stream(qoi::encode_worst_case(desc));
    stream.resize(qoi::encode(pix, desc, stream));
    qoi_test::Rng rng(92);
    for (std::size_t extra = 1; extra < 40; ++extra) {
        Bytes data = stream;
        for (std::size_t i = 0; i < extra; ++i) {
            data.push_back(rng.byte());
        }
        const ExactBytes exact(data);
        Bytes out(pix.size() + 8, kSentinel);
        qoi::Desc decoded;
        const std::size_t n = qoi::decode(exact.span(), decoded, qoi::Span<std::uint8_t>(out.data(), pix.size()));
        EXPECT_EQ(n, pix.size());
        EXPECT_TRUE(BytesEqual(pix, out, pix.size()));
        EXPECT_TRUE(qoi_test::all_equal(out, pix.size(), out.size(), kSentinel));
    }
}

// Seeded random chunk areas behind a valid header: whatever the bytes are, decode succeeds,
// returns the image size, and writes exactly that many bytes.
TEST(RobustnessDecode, RandomChunkBytesBehindValidHeader) {
    qoi_test::Rng rng(20240601);
    for (int iteration = 0; iteration < 600; ++iteration) {
        const std::uint32_t w = 1U + rng.below(20U);
        const std::uint32_t h = 1U + rng.below(20U);
        const std::uint8_t channels = qoi_test::u8(3U + rng.below(2U));
        const std::size_t chunk_bytes = rng.below(200U);
        Bytes chunks(chunk_bytes);
        for (std::uint8_t& b : chunks) {
            b = rng.byte();
        }
        const Bytes data = raw_stream(w, h, channels, qoi_test::u8(rng.below(2U)), chunks);
        const ExactBytes exact(data);
        const std::size_t need = static_cast<std::size_t>(w) * h * channels;
        Bytes out(need + 16, kSentinel);
        qoi::Desc desc = qoi_test::poisoned_desc();
        const std::size_t n = qoi::decode(exact.span(), desc, qoi::Span<std::uint8_t>(out.data(), need));
        ASSERT_EQ(n, need) << "iteration " << iteration;
        ASSERT_EQ(desc.width, w);
        ASSERT_EQ(desc.height, h);
        ASSERT_EQ(desc.channels, channels);
        ASSERT_TRUE(qoi_test::all_equal(out, need, out.size(), kSentinel)) << "iteration " << iteration;
    }
}

// Seeded fully random input (random header too) with a small output buffer: never crashes,
// returns 0 or a size that fits the buffer, and never touches the guard bytes.
TEST(RobustnessDecode, FullyRandomInput) {
    qoi_test::Rng rng(987654321);
    for (int iteration = 0; iteration < 1500; ++iteration) {
        const std::size_t size = rng.below(120U);
        Bytes data(size);
        for (std::uint8_t& b : data) {
            b = rng.byte();
        }
        // Half of the inputs get a valid magic, and a quarter also a small valid header (width
        // and height in 1..8, channels 3 or 4, colorspace 0 or 1), so that the decoding loop
        // is reached with random chunk bytes.
        const unsigned kind = rng.below(4U);
        if (size >= 4 && kind >= 2U) {
            data[0] = 0x71;
            data[1] = 0x6F;
            data[2] = 0x69;
            data[3] = 0x66;
        }
        if (size >= 22 && kind == 3U) {
            data[4] = data[5] = data[6] = 0;
            data[7] = qoi_test::u8(1U + rng.below(8U));
            data[8] = data[9] = data[10] = 0;
            data[11] = qoi_test::u8(1U + rng.below(8U));
            data[12] = qoi_test::u8(3U + rng.below(2U));
            data[13] = qoi_test::u8(rng.below(2U));
        }
        const ExactBytes exact(data);
        const std::size_t out_size = 256;
        Bytes out(out_size + 16, kSentinel);
        qoi::Desc desc = qoi_test::poisoned_desc();
        const std::size_t n = qoi::decode(exact.span(), desc, qoi::Span<std::uint8_t>(out.data(), out_size));
        ASSERT_LE(n, out_size) << "iteration " << iteration;
        ASSERT_TRUE(qoi_test::all_equal(out, out_size, out.size(), kSentinel)) << "iteration " << iteration;
        if (n != 0) {
            ASSERT_EQ(n, desc.width * desc.height * desc.channels);
        }
    }
}

// Single-bit flips of every bit of the chunk area of valid streams: decode must still succeed
// with the right size and stay inside the output.
TEST(RobustnessDecode, EverySingleBitFlipInChunkArea) {
    for (const std::size_t channels : {3U, 4U}) {
        const Bytes pix = qoi_test::runs_and_noise_image(7, 6, channels, 4711);
        const qoi::Desc desc = make_desc(7, 6, channels);
        Bytes stream(qoi::encode_worst_case(desc));
        stream.resize(qoi::encode(pix, desc, stream));
        for (std::size_t byte = 14; byte < stream.size() - 8; ++byte) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                Bytes data = stream;
                data[byte] = qoi_test::u8(data[byte] ^ (1U << bit));
                const ExactBytes exact(data);
                Bytes out(pix.size() + 8, kSentinel);
                qoi::Desc decoded;
                const std::size_t n =
                    qoi::decode(exact.span(), decoded, qoi::Span<std::uint8_t>(out.data(), pix.size()));
                ASSERT_EQ(n, pix.size()) << "byte " << byte << " bit " << bit;
                ASSERT_TRUE(qoi_test::all_equal(out, pix.size(), out.size(), kSentinel));
            }
        }
    }
}

// Every possible first chunk byte, with every possible following byte, as a 1-chunk stream
// decoded as 2x1 images with 3 and 4 channels: the opcode decoding covers all 256 values
// without a crash and without writing beyond the output.
TEST(RobustnessDecode, EveryOpcodeByteAsFirstChunk) {
    for (const std::uint8_t channels : {std::uint8_t{3}, std::uint8_t{4}}) {
        for (unsigned op = 0; op < 256; ++op) {
            for (const unsigned operand : {0U, 0x88U, 0xFFU}) {
                const Bytes data = raw_stream(2, 1, channels, 0, {qoi_test::u8(op), qoi_test::u8(operand), 1, 2, 3});
                const ExactBytes exact(data);
                const std::size_t need = 2U * channels;
                Bytes out(need + 8, kSentinel);
                qoi::Desc desc;
                const std::size_t n = qoi::decode(exact.span(), desc, qoi::Span<std::uint8_t>(out.data(), need));
                ASSERT_EQ(n, need) << "op " << op;
                ASSERT_TRUE(qoi_test::all_equal(out, need, out.size(), kSentinel));
            }
        }
    }
}

// Large declared image, tiny stream: the whole image is the repeated initial pixel and the
// work is linear in the image size (no hang). 1000x1000 RGBA = 4 MB output.
TEST(RobustnessDecode, LargeImageFromHeaderOnlyStream) {
    const std::size_t w = 1000;
    const std::size_t h = 1000;
    const Bytes data = raw_stream(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 4, 0, {});
    const ExactBytes exact(data);
    Bytes out(w * h * 4);
    qoi::Desc desc;
    ASSERT_EQ(qoi::decode(exact.span(), desc, out), out.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < out.size(); i += 4) {
        if (out[i] != 0 || out[i + 1] != 0 || out[i + 2] != 0 || out[i + 3] != 255) {
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// ===========================================================================
// Span construction (the public view type used by every call above)
// ===========================================================================

TEST(ContractSpan, ConstructsFromVectorArrayAndCArray) {
    std::vector<std::uint8_t> vec = {1, 2, 3};
    const std::vector<std::uint8_t> cvec = {4, 5};
    std::array<std::uint8_t, 4> arr = {1, 2, 3, 4};
    const std::array<std::uint8_t, 2> carr = {9, 8};
    std::uint8_t raw[5] = {0, 0, 0, 0, 0};

    const qoi::Span<std::uint8_t> a(vec);
    const qoi::Span<const std::uint8_t> b(cvec);
    const qoi::Span<std::uint8_t> c(arr);
    const qoi::Span<const std::uint8_t> d(carr);
    const qoi::Span<std::uint8_t> e(raw);
    const qoi::Span<const std::uint8_t> f(vec);       // mutable vector to const view
    const qoi::Span<const std::uint8_t> g(a);         // Span<T> -> Span<const T>
    const qoi::Span<const std::uint8_t> empty;

    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(c.size(), 4u);
    EXPECT_EQ(d.size(), 2u);
    EXPECT_EQ(e.size(), 5u);
    EXPECT_EQ(f.size(), 3u);
    EXPECT_EQ(g.size(), 3u);
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(a[1], 2u);
    EXPECT_EQ(b[0], 4u);
    EXPECT_EQ(g.data(), vec.data());
}
