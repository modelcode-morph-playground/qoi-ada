// Differential tests: qoi::encode / qoi::decode against the reference qoi.h
// (vendored under tests/third_party, reached through qoi_reference_wrapper.h).
//
// The two encoders agree except for one documented divergence (project spec,
// decision 6): src/qoi.adb range-tests UNBOUNDED integer channel deltas, while
// qoi.h stores them in `signed char`, which wraps. 255 -> 0 is -255 for the
// Ada-identical C++ port (an RGB chunk) and +1 for qoi.h (a one-byte DIFF
// chunk). Both streams are valid and decode to the same pixels.
//
// Consequently the tests assert:
//   * known-clean inputs (and random inputs the classifier calls clean):
//       identical encoded bytes, and identical decoded pixels from both decoders;
//   * known-wrapping inputs (and random inputs the classifier calls wrapping):
//       identical decoded pixels, but DIFFERENT bytes, with the first differing
//       offset reported;
//   * the classifier agrees with the observed byte equality on every input.
//
// The decoders are specified identically, so decoder differentials are asserted
// on every stream, including malformed ones.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "diff_util.hpp"
#include "third_party/qoi_reference_wrapper.h"

namespace {

using difftest::Bytes;

// ---------------------------------------------------------------------------
// Reference wrappers
// ---------------------------------------------------------------------------
Bytes ref_encode(const Bytes& pix, unsigned w, unsigned h, unsigned channels, unsigned colorspace) {
    const qoi_ref_desc desc{w, h, channels, colorspace};
    Bytes out(static_cast<std::size_t>(w) * h * (channels + 1) + 14 + 8);
    const std::size_t n = qoi_ref_encode(pix.data(), &desc, out.data(), out.size());
    out.resize(n);
    return out;
}

struct RefDecoded {
    bool ok = false;
    bool skipped = false;
    qoi_ref_desc desc{};
    Bytes pixels;
};

// Parses width/height/channels from the header by hand (big-endian), without
// using the library under test, to size the output buffer.
bool parse_dims(const Bytes& d, std::uint64_t& w, std::uint64_t& h, std::uint64_t& c) {
    if (d.size() < 14) {
        return false;
    }
    w = (std::uint64_t{d[4]} << 24) | (std::uint64_t{d[5]} << 16) | (std::uint64_t{d[6]} << 8) | d[7];
    h = (std::uint64_t{d[8]} << 24) | (std::uint64_t{d[9]} << 16) | (std::uint64_t{d[10]} << 8) | d[11];
    c = d[12];
    return true;
}

RefDecoded ref_decode(const Bytes& data) {
    RefDecoded r;
    std::uint64_t w = 0;
    std::uint64_t h = 0;
    std::uint64_t c = 0;
    std::size_t cap = 0;
    if (parse_dims(data, w, h, c) && w != 0 && h != 0 && c >= 3 && c <= 4) {
        if (w > difftest::kDecodeCapBytes / h / c) {
            r.skipped = true;
            return r;
        }
        cap = static_cast<std::size_t>(w * h * c);
    }
    r.pixels.assign(cap, 0);
    const std::size_t n = qoi_ref_decode(data.data(), data.size(), 0, &r.desc, r.pixels.data(), cap);
    EXPECT_NE(n, QOI_REF_BUFFER_TOO_SMALL);
    r.ok = n != 0 && n != QOI_REF_BUFFER_TOO_SMALL;
    if (r.ok) {
        r.pixels.resize(n);
    } else {
        r.pixels.clear();
    }
    return r;
}

qoi::Colorspace cs_of(unsigned v) {
    return v == 0 ? qoi::Colorspace::SRGB : qoi::Colorspace::SRGB_Linear_Alpha;
}

// Both decoders must accept `stream` and reproduce `original` exactly, with
// header fields matching.
void expect_decodes_to(const Bytes& stream, const Bytes& original, unsigned w, unsigned h,
                       unsigned channels, unsigned colorspace, const char* what) {
    SCOPED_TRACE(what);
    const difftest::Decoded ours = difftest::ours_decode(stream);
    ASSERT_TRUE(ours.ok) << "qoi::decode rejected the stream";
    EXPECT_EQ(ours.width, w);
    EXPECT_EQ(ours.height, h);
    EXPECT_EQ(ours.channels, channels);
    EXPECT_EQ(ours.colorspace, colorspace);
    EXPECT_TRUE(ours.pixels == original) << "qoi::decode pixels differ from the original";

    const RefDecoded ref = ref_decode(stream);
    ASSERT_TRUE(ref.ok) << "qoi_decode rejected the stream";
    EXPECT_EQ(ref.desc.width, w);
    EXPECT_EQ(ref.desc.height, h);
    EXPECT_EQ(ref.desc.channels, channels);
    EXPECT_EQ(ref.desc.colorspace, colorspace);
    EXPECT_TRUE(ref.pixels == original) << "qoi_decode pixels differ from the original";
}

struct Encoded {
    Bytes ours;
    Bytes ref;
};

Encoded encode_both(const Bytes& pix, unsigned w, unsigned h, unsigned channels, unsigned colorspace) {
    Encoded e;
    e.ours = difftest::ours_encode(pix, w, h, channels, cs_of(colorspace));
    e.ref = ref_encode(pix, w, h, channels, colorspace);
    return e;
}

// ---------------------------------------------------------------------------
// Crafted inputs
// ---------------------------------------------------------------------------
TEST(DifferentialRef, CraftedCleanCasesAreByteIdentical) {
    const std::vector<difftest::Crafted> cases = difftest::crafted_clean_cases();
    ASSERT_FALSE(cases.empty());
    for (const difftest::Crafted& c : cases) {
        for (unsigned colorspace = 0; colorspace < 2; ++colorspace) {
            SCOPED_TRACE(c.name + " colorspace=" + std::to_string(colorspace));
            ASSERT_EQ(c.pixels.size(), std::size_t{c.width} * c.height * c.channels);

            // The classifier must call a known-clean case clean.
            EXPECT_FALSE(difftest::has_wraparound_delta(c.pixels, c.channels));

            const Encoded e = encode_both(c.pixels, c.width, c.height, c.channels, colorspace);
            ASSERT_FALSE(e.ours.empty());
            ASSERT_FALSE(e.ref.empty());
            EXPECT_TRUE(e.ours == e.ref)
                << "streams differ at offset " << difftest::first_diff(e.ours, e.ref)
                << " (ours " << difftest::hex_at(e.ours, difftest::first_diff(e.ours, e.ref))
                << "reference " << difftest::hex_at(e.ref, difftest::first_diff(e.ours, e.ref)) << ")";

            // Each decoder reads each encoder's stream.
            expect_decodes_to(e.ours, c.pixels, c.width, c.height, c.channels, colorspace, "ours stream");
            expect_decodes_to(e.ref, c.pixels, c.width, c.height, c.channels, colorspace, "reference stream");
        }
    }
}

TEST(DifferentialRef, WrapAroundCraftedCasesDecodeIdenticallyButEncodeDifferently) {
    const std::vector<difftest::Crafted> cases = difftest::crafted_wrapping_cases();
    ASSERT_FALSE(cases.empty());
    for (const difftest::Crafted& c : cases) {
        for (unsigned colorspace = 0; colorspace < 2; ++colorspace) {
            SCOPED_TRACE(c.name + " colorspace=" + std::to_string(colorspace));
            ASSERT_EQ(c.pixels.size(), std::size_t{c.width} * c.height * c.channels);

            // The classifier must call a known-wrapping case wrapping.
            const difftest::WrapInfo info = difftest::find_wraparound_delta(c.pixels, c.channels);
            EXPECT_TRUE(info.wraps);
            // At such a pixel the reference picks a delta chunk and the Ada
            // rule picks RGB (see diff_util.hpp for the argument).
            EXPECT_EQ(info.ada, difftest::DeltaOp::Rgb);
            EXPECT_NE(info.reference, difftest::DeltaOp::Rgb);

            const Encoded e = encode_both(c.pixels, c.width, c.height, c.channels, colorspace);
            ASSERT_FALSE(e.ours.empty());
            ASSERT_FALSE(e.ref.empty());

            // Documented divergence: the byte streams differ.
            EXPECT_FALSE(e.ours == e.ref);
            const std::size_t at = difftest::first_diff(e.ours, e.ref);
            EXPECT_EQ(at, c.expect_first_diff) << "hand-derived offset of the first difference";
            // The reference never needs more bytes: it replaces 4-5 byte RGB
            // chunks by 1-2 byte DIFF/LUMA chunks and nothing else changes.
            EXPECT_GT(e.ours.size(), e.ref.size());

            if (colorspace == 0) {
                std::cout << "[wrap-around] " << c.name << ": first differing offset " << at << " (ours "
                          << difftest::hex_at(e.ours, at) << "| reference " << difftest::hex_at(e.ref, at)
                          << "), sizes ours/reference " << e.ours.size() << "/" << e.ref.size() << "\n";
                ::testing::Test::RecordProperty("first_diff_" + c.name, std::to_string(at));
            }

            // Decoded pixels are identical, in all four encoder/decoder pairings.
            expect_decodes_to(e.ours, c.pixels, c.width, c.height, c.channels, colorspace, "ours stream");
            expect_decodes_to(e.ref, c.pixels, c.width, c.height, c.channels, colorspace, "reference stream");
        }
    }
}

TEST(DifferentialRef, WrapAroundFirstPixelWhiteExactBytes) {
    // A fully worked example: 1x1 RGB (255,255,255) from the start state
    // (0,0,0,255). qoi.h sees signed-char deltas (-1,-1,-1): DIFF 0x55. The
    // Ada-identical port sees (+255,+255,+255): an RGB chunk.
    const Bytes px = {255, 255, 255};
    const Encoded e = encode_both(px, 1, 1, 3, 0);
    const Bytes header = {'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0};
    const Bytes padding = {0, 0, 0, 0, 0, 0, 0, 1};

    Bytes want_ours = header;
    want_ours.insert(want_ours.end(), {0xFE, 0xFF, 0xFF, 0xFF});
    want_ours.insert(want_ours.end(), padding.begin(), padding.end());
    Bytes want_ref = header;
    want_ref.push_back(0x55);
    want_ref.insert(want_ref.end(), padding.begin(), padding.end());

    EXPECT_TRUE(e.ours == want_ours);
    EXPECT_TRUE(e.ref == want_ref);
}

// ---------------------------------------------------------------------------
// Random images
// ---------------------------------------------------------------------------
TEST(DifferentialRef, RandomImagesFixedSeed) {
    std::mt19937 rng(0x51A0'D1FFU);
    std::size_t clean = 0;
    std::size_t wrapping = 0;
    std::size_t images = 0;

    for (const difftest::Kind kind : difftest::kAllKinds) {
        for (const unsigned channels : {3U, 4U}) {
            for (const difftest::Size size : difftest::image_sizes()) {
                for (int rep = 0; rep < 3; ++rep) {
                    const unsigned colorspace = static_cast<unsigned>(rng() & 1U);
                    const Bytes pix = difftest::make_image(rng, kind, std::size_t{size.w} * size.h, channels);
                    SCOPED_TRACE("kind=" + std::to_string(static_cast<int>(kind)) + " " +
                                 std::to_string(size.w) + "x" + std::to_string(size.h) + "x" +
                                 std::to_string(channels) + " rep=" + std::to_string(rep));
                    ASSERT_EQ(pix.size(), std::size_t{size.w} * size.h * channels);
                    ++images;

                    const difftest::WrapInfo info = difftest::find_wraparound_delta(pix, channels);
                    const Encoded e = encode_both(pix, size.w, size.h, channels, colorspace);
                    ASSERT_FALSE(e.ours.empty());
                    ASSERT_FALSE(e.ref.empty());

                    if (info.wraps) {
                        ++wrapping;
                        // The classifier predicts divergence: bytes must differ,
                        // at the position of the first wrapping pixel or earlier
                        // never (earlier chunks are clean), and the reference
                        // stream must be smaller.
                        EXPECT_FALSE(e.ours == e.ref) << "classifier says wrap-around, bytes are identical";
                        EXPECT_GT(e.ours.size(), e.ref.size());
                        EXPECT_EQ(info.ada, difftest::DeltaOp::Rgb);
                    } else {
                        ++clean;
                        EXPECT_TRUE(e.ours == e.ref)
                            << "classifier says clean, but the streams differ at offset "
                            << difftest::first_diff(e.ours, e.ref);
                    }

                    // Always: identical decoded pixels from both decoders, for
                    // both encoders' streams.
                    expect_decodes_to(e.ours, pix, size.w, size.h, channels, colorspace, "ours stream");
                    expect_decodes_to(e.ref, pix, size.w, size.h, channels, colorspace, "reference stream");
                }
            }
        }
    }

    std::cout << "[random] images=" << images << " clean=" << clean << " wrapping=" << wrapping << "\n";
    ::testing::Test::RecordProperty("images", std::to_string(images));
    ::testing::Test::RecordProperty("clean", std::to_string(clean));
    ::testing::Test::RecordProperty("wrapping", std::to_string(wrapping));
    // The corpus must exercise both classes, or the test would be vacuous.
    EXPECT_GE(clean, images / 5);
    EXPECT_GE(wrapping, images / 10);
}

// ---------------------------------------------------------------------------
// Decoder differentials
// ---------------------------------------------------------------------------

// Runs both decoders on `data` and asserts they agree: same accept/reject
// decision, same header, same pixels. Returns +1 if both accepted, 0 if both
// rejected, -1 if skipped as too large (helpers cap allocations).
int expect_decoders_agree(const Bytes& data) {
    const difftest::Decoded ours = difftest::ours_decode(data);
    const RefDecoded ref = ref_decode(data);
    if (ours.skipped || ref.skipped) {
        return -1;
    }
    EXPECT_EQ(ours.ok, ref.ok) << "accept/reject disagreement (ours " << ours.ok << ", reference " << ref.ok
                               << "), size " << data.size() << ", head " << difftest::hex_at(data, 0, 14);
    if (ours.ok && ref.ok) {
        EXPECT_EQ(ours.width, ref.desc.width);
        EXPECT_EQ(ours.height, ref.desc.height);
        EXPECT_EQ(ours.channels, ref.desc.channels);
        EXPECT_EQ(ours.colorspace, ref.desc.colorspace);
        EXPECT_TRUE(ours.pixels == ref.pixels)
            << "pixels differ at byte " << difftest::first_diff(ours.pixels, ref.pixels) << " of "
            << ours.pixels.size();
        return 1;
    }
    return 0;
}

TEST(DifferentialRef, DecoderRandomChunkStreams) {
    // Valid header fields, arbitrary chunk bytes, valid / garbage / missing
    // padding, sometimes truncated: both decoders treat the last 8 bytes as
    // padding without checking it, repeat the last pixel when the chunks run
    // out, and read the same bytes for every opcode.
    std::mt19937 rng(0xDEC0DE01U);
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (int i = 0; i < 4000; ++i) {
        const std::uint32_t w = 1 + difftest::below(rng, 24);
        const std::uint32_t h = 1 + difftest::below(rng, 24);
        const unsigned channels = 3 + static_cast<unsigned>(rng() & 1U);
        const unsigned colorspace = static_cast<unsigned>(rng() & 1U);
        const Bytes stream = difftest::make_random_stream(rng, w, h, channels, colorspace);
        SCOPED_TRACE("case " + std::to_string(i));
        const int r = expect_decoders_agree(stream);
        accepted += r > 0 ? 1U : 0U;
        rejected += r == 0 ? 1U : 0U;
    }
    std::cout << "[decoder] random streams accepted=" << accepted << " rejected=" << rejected << "\n";
    EXPECT_GT(accepted, 1000U);
    EXPECT_GT(rejected, 50U);  // streams shorter than 22 bytes
}

TEST(DifferentialRef, DecoderTruncatedEncoderOutput) {
    // Every prefix of a valid stream from either encoder, including prefixes
    // shorter than the 22-byte minimum.
    std::mt19937 rng(0xDEC0DE02U);
    for (const difftest::Kind kind : {difftest::Kind::Palette, difftest::Kind::ModularWalk,
                                      difftest::Kind::Runs, difftest::Kind::AlphaNoise}) {
        for (const unsigned channels : {3U, 4U}) {
            const Bytes pix = difftest::make_image(rng, kind, 12 * 9, channels);
            const Encoded e = encode_both(pix, 12, 9, channels, 0);
            for (const Bytes* stream : {&e.ours, &e.ref}) {
                for (std::size_t len = 0; len <= stream->size(); ++len) {
                    const Bytes cut(stream->begin(), stream->begin() + static_cast<std::ptrdiff_t>(len));
                    SCOPED_TRACE("kind=" + std::to_string(static_cast<int>(kind)) + " channels=" +
                                 std::to_string(channels) + " prefix=" + std::to_string(len));
                    expect_decoders_agree(cut);
                }
            }
        }
    }
}

TEST(DifferentialRef, DecoderCorruptedHeaders) {
    // Overwrite random header bytes of valid streams: bad magic, odd channel
    // counts, colorspace bytes other than 0/1, zero or changed dimensions.
    std::mt19937 rng(0xDEC0DE03U);
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (int i = 0; i < 3000; ++i) {
        const Bytes pix = difftest::make_image(rng, difftest::Kind::Palette, 6 * 5, 4);
        Bytes stream = ref_encode(pix, 6, 5, 4, 0);
        const std::size_t edits = 1 + difftest::below(rng, 3);
        for (std::size_t k = 0; k < edits; ++k) {
            const std::size_t pos = difftest::below(rng, 14);
            // Keep the dimension bytes small so that the helpers' allocation
            // cap is not hit constantly: the three high bytes of each
            // dimension are only ever zeroed, the low bytes change freely.
            const bool high_dim_byte = (pos >= 4 && pos <= 6) || (pos >= 8 && pos <= 10);
            stream[pos] = high_dim_byte ? std::uint8_t{0} : difftest::next_byte(rng);
        }
        SCOPED_TRACE("case " + std::to_string(i));
        const int r = expect_decoders_agree(stream);
        accepted += r > 0 ? 1U : 0U;
        rejected += r == 0 ? 1U : 0U;
    }
    std::cout << "[decoder] corrupted headers accepted=" << accepted << " rejected=" << rejected << "\n";
    EXPECT_GT(accepted, 100U);
    EXPECT_GT(rejected, 100U);
}

TEST(DifferentialRef, DecoderRejectsInvalidHeadersLikeReference) {
    const Bytes pix(3, 128);  // 1x1 RGB
    const Bytes valid = ref_encode(pix, 1, 1, 3, 0);
    ASSERT_EQ(valid.size(), 14U + 4U + 8U);
    ASSERT_EQ(expect_decoders_agree(valid), 1);

    struct Edit {
        const char* name;
        std::size_t pos;
        std::uint8_t value;
    };
    const Edit edits[] = {
        {"magic0", 0, 'x'},      {"magic3", 3, 'g'},    {"width-zero", 7, 0},  {"height-zero", 11, 0},
        {"channels-0", 12, 0},   {"channels-1", 12, 1}, {"channels-2", 12, 2}, {"channels-5", 12, 5},
        {"channels-255", 12, 255}, {"colorspace-2", 13, 2}, {"colorspace-255", 13, 255},
    };
    for (const Edit& edit : edits) {
        SCOPED_TRACE(edit.name);
        Bytes s = valid;
        s[edit.pos] = edit.value;
        EXPECT_EQ(expect_decoders_agree(s), 0);
    }
    // 3 and 4 channel headers over the same body are fine for both.
    Bytes four = valid;
    four[12] = 4;
    EXPECT_EQ(expect_decoders_agree(four), 1);
}

TEST(DifferentialRef, DecoderRejectsHugeDimensionsLikeReference) {
    // Dimensions for which qoi.h refuses before allocating (its 400 million
    // pixel cap) and for which the C++ decoder cannot be given a large enough
    // output buffer by a caller. Both must reject. (Between the two limits the
    // decoders legitimately differ: qoi.h rejects images of 400M pixels or
    // more, qoi::decode accepts whatever fits the caller's buffer.)
    struct Dims {
        std::uint32_t w;
        std::uint32_t h;
    };
    const Dims dims[] = {{0xFFFFFFFFU, 0xFFFFFFFFU}, {0x7FFFFFFFU, 2}, {2, 0x7FFFFFFFU},
                         {20000, 20000},             {1, 400000000U},  {400000000U, 1}};
    for (const Dims& d : dims) {
        for (const unsigned channels : {3U, 4U}) {
            SCOPED_TRACE(std::to_string(d.w) + "x" + std::to_string(d.h) + "x" + std::to_string(channels));
            Bytes s = difftest::make_header(d.w, d.h, channels, 0);
            s.insert(s.end(), {0xFE, 1, 2, 3});
            difftest::append_padding(s);

            // qoi.h: rejects without allocating. qoi::decode: no valid output
            // buffer of this size exists in the test; hand it a tiny one.
            std::uint8_t tiny[16] = {};
            qoi_ref_desc rd{};
            EXPECT_EQ(qoi_ref_decode(s.data(), s.size(), 0, &rd, tiny, sizeof tiny), 0U);
            qoi::Desc od;
            EXPECT_EQ(qoi::decode(s, od, tiny), 0U);
        }
    }
}

TEST(DifferentialRef, DecoderForcedChannelsMatchesDroppingAlpha) {
    // qoi.h can force 3 or 4 output channels; qoi::decode always uses the
    // header's. For a 4-channel stream, forcing 3 is the same as dropping the
    // alpha byte of our output.
    std::mt19937 rng(0xDEC0DE04U);
    for (int i = 0; i < 200; ++i) {
        const Bytes pix = difftest::make_image(rng, difftest::Kind::AlphaNoise, 15 * 4, 4);
        const Bytes stream = difftest::ours_encode(pix, 15, 4, 4, qoi::Colorspace::SRGB);
        ASSERT_FALSE(stream.empty());
        const difftest::Decoded ours = difftest::ours_decode(stream);
        ASSERT_TRUE(ours.ok);

        Bytes rgb(15 * 4 * 3, 0);
        qoi_ref_desc rd{};
        ASSERT_EQ(qoi_ref_decode(stream.data(), stream.size(), 3, &rd, rgb.data(), rgb.size()), rgb.size());
        Bytes dropped;
        for (std::size_t p = 0; p < 15 * 4; ++p) {
            dropped.insert(dropped.end(), ours.pixels.begin() + static_cast<std::ptrdiff_t>(p * 4),
                           ours.pixels.begin() + static_cast<std::ptrdiff_t>(p * 4 + 3));
        }
        EXPECT_TRUE(rgb == dropped);
    }
}

}  // namespace
