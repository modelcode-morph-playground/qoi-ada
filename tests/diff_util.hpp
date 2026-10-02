// Helpers shared by the differential tests (qoi_diff_ref, qoi_ada_diff): image
// and stream generators, thin wrappers around qoi::encode/qoi::decode, the
// crafted known-clean / known-wrapping inputs, and the independent wrap-around
// delta classifier.
//
// Everything here is deterministic: generators take a caller-owned
// std::mt19937 and draw bytes with `rng() & 0xFF` (not a distribution object),
// so the same seed yields the same inputs with every standard library.
#ifndef QOI_TESTS_DIFF_UTIL_HPP
#define QOI_TESTS_DIFF_UTIL_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "qoi/qoi.hpp"

namespace difftest {

using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Random helpers
// ---------------------------------------------------------------------------
inline std::uint8_t next_byte(std::mt19937& rng) {
    return static_cast<std::uint8_t>(rng() & 0xFFU);
}

// Value in [0, n). The modulo bias is irrelevant here; n must be > 0.
inline std::uint32_t below(std::mt19937& rng, std::uint32_t n) {
    return static_cast<std::uint32_t>(rng() % n);
}

inline bool chance(std::mt19937& rng, std::uint32_t one_in) { return below(rng, one_in) == 0; }

// ---------------------------------------------------------------------------
// Wrappers around the library under test
// ---------------------------------------------------------------------------

// Encodes with qoi::encode into a right-sized vector. Returns an empty vector if
// the library reports failure.
inline Bytes ours_encode(const Bytes& pix, std::uint64_t width, std::uint64_t height,
                         std::uint64_t channels, qoi::Colorspace cs) {
    const qoi::Desc desc{width, height, channels, cs};
    Bytes out(static_cast<std::size_t>(qoi::encode_worst_case(desc)));
    const std::size_t n = qoi::encode(pix, desc, out);
    out.resize(n);
    return out;
}

// Upper limit on decoded sizes that the differential helpers will allocate.
inline constexpr std::uint64_t kDecodeCapBytes = 1ULL << 24;

struct Decoded {
    bool ok = false;       // the decoder accepted the stream
    bool skipped = false;  // header declares more than kDecodeCapBytes: not run
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t channels = 0;
    unsigned colorspace = 0;
    Bytes pixels;
};

// Decodes with qoi::get_desc + qoi::decode, sizing the output from the header.
inline Decoded ours_decode(const Bytes& data) {
    Decoded r;
    qoi::Desc desc;
    qoi::get_desc(data, desc);
    r.width = desc.width;
    r.height = desc.height;
    r.channels = desc.channels;
    r.colorspace = static_cast<unsigned>(desc.colorspace);

    const bool dims_ok =
        desc.width != 0 && desc.height != 0 && desc.channels >= 3 && desc.channels <= 4;
    if (!dims_ok) {
        // Invalid header: decode must fail whatever the buffer is.
        std::array<std::uint8_t, 4> tiny{};
        qoi::Desc d2;
        r.ok = qoi::decode(data, d2, tiny) != 0;
        return r;
    }
    if (desc.width > kDecodeCapBytes / desc.height / desc.channels) {
        r.skipped = true;  // too big for the helper
        return r;
    }

    const std::size_t need = static_cast<std::size_t>(desc.width * desc.height * desc.channels);
    r.pixels.assign(need, 0);
    qoi::Desc d2;
    const std::size_t n = qoi::decode(data, d2, r.pixels);
    r.ok = n != 0;
    if (r.ok) {
        r.pixels.resize(n);
    } else {
        r.pixels.clear();
    }
    return r;
}

// Index of the first byte at which a and b differ, or min(size) if one is a
// prefix of the other (a.size() == b.size() and no difference: returns size).
inline std::size_t first_diff(const Bytes& a, const Bytes& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return n;
}

inline std::string hex_at(const Bytes& b, std::size_t pos, std::size_t count = 6) {
    static const char* const digits = "0123456789abcdef";
    std::string s;
    for (std::size_t i = pos; i < b.size() && i < pos + count; ++i) {
        s += digits[b[i] >> 4];
        s += digits[b[i] & 0x0F];
        s += ' ';
    }
    return s.empty() ? std::string("<end>") : s;
}

// ---------------------------------------------------------------------------
// Independent wrap-around delta classifier
//
// The two encoders differ in exactly one decision: for a pixel that is neither
// a run repeat nor an INDEX hit and whose alpha equals the previous pixel's,
// they choose between DIFF, LUMA and RGB from the per-channel deltas.
//
//   Ada (src/qoi.adb)       deltas are UNBOUNDED integers: 255 -> 0 is -255.
//   reference (qoi.h)       deltas are `signed char`, which wraps modulo 256:
//                           255 -> 0 is +1; vg_r and vg_b wrap again.
//
// has_wraparound_delta() replays the encoders' state machine without emitting
// bytes. Only the state that affects which pixels reach the delta decision is
// needed: the previous pixel (a repeat is a run) and the 64-entry index table
// (an INDEX hit never uses deltas). That state evolves identically for both
// encoders, because the table update and the previous pixel do not depend on
// which delta opcode was chosen. At every pixel that reaches the delta
// decision, both rules are evaluated, and the input "has a wrap-around delta"
// iff they select different opcode classes anywhere.
//
// Because the opcode class decides the chunk bytes, this gives an exact
// predicate: the two encoders emit identical bytes iff the classifier says
// there is no wrap-around delta. The classifier shares no code with either
// encoder (it never builds a stream).
// ---------------------------------------------------------------------------
enum class DeltaOp { Diff, Luma, Rgb };

// Wraps v to the signed 8-bit range (what a `signed char` conversion does).
inline int wrap_to_int8(int v) {
    const int m = v & 0xFF;
    return m >= 128 ? m - 256 : m;
}

inline DeltaOp classify_unbounded(int vr, int vg, int vb) {
    const int vg_r = vr - vg;
    const int vg_b = vb - vg;
    if (vr >= -2 && vr <= 1 && vg >= -2 && vg <= 1 && vb >= -2 && vb <= 1) {
        return DeltaOp::Diff;
    }
    if (vg_r >= -8 && vg_r <= 7 && vg >= -32 && vg <= 31 && vg_b >= -8 && vg_b <= 7) {
        return DeltaOp::Luma;
    }
    return DeltaOp::Rgb;
}

// The decision as written in qoi.h: `signed char vr = px.r - px_prev.r;` etc.
inline DeltaOp classify_signed_char(int dr, int dg, int db) {
    const int vr = wrap_to_int8(dr);
    const int vg = wrap_to_int8(dg);
    const int vb = wrap_to_int8(db);
    const int vg_r = wrap_to_int8(vr - vg);
    const int vg_b = wrap_to_int8(vb - vg);
    if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
        return DeltaOp::Diff;
    }
    if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8) {
        return DeltaOp::Luma;
    }
    return DeltaOp::Rgb;
}

struct WrapInfo {
    bool wraps = false;           // any pixel selects different opcode classes
    std::size_t first_pixel = 0;  // pixel index of the first such pixel
    std::size_t count = 0;        // number of such pixels
    DeltaOp ada = DeltaOp::Rgb;   // classes at the first such pixel
    DeltaOp reference = DeltaOp::Rgb;
};

inline int pixel_hash(int r, int g, int b, int a) { return (r * 3 + g * 5 + b * 7 + a * 11) & 63; }

inline WrapInfo find_wraparound_delta(const Bytes& pixels, unsigned channels) {
    WrapInfo info;
    struct P {
        int r, g, b, a;
        bool operator==(const P& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    };
    std::array<P, 64> table{};  // all (0,0,0,0)
    P prev{0, 0, 0, 255};

    const std::size_t npx = pixels.size() / channels;
    for (std::size_t i = 0; i < npx; ++i) {
        const std::uint8_t* s = pixels.data() + i * channels;
        const P px{s[0], s[1], s[2], channels == 4 ? s[3] : 255};

        if (px == prev) {
            continue;  // part of a run
        }
        const int pos = pixel_hash(px.r, px.g, px.b, px.a);
        if (table[static_cast<std::size_t>(pos)] == px) {
            prev = px;
            continue;  // INDEX hit: no deltas involved
        }
        table[static_cast<std::size_t>(pos)] = px;

        if (px.a == prev.a) {
            const int dr = px.r - prev.r;
            const int dg = px.g - prev.g;
            const int db = px.b - prev.b;
            const DeltaOp ada = classify_unbounded(dr, dg, db);
            const DeltaOp ref = classify_signed_char(dr, dg, db);
            if (ada != ref) {
                if (!info.wraps) {
                    info.wraps = true;
                    info.first_pixel = i;
                    info.ada = ada;
                    info.reference = ref;
                }
                ++info.count;
            }
        }
        prev = px;
    }
    return info;
}

inline bool has_wraparound_delta(const Bytes& pixels, unsigned channels) {
    return find_wraparound_delta(pixels, channels).wraps;
}

// ---------------------------------------------------------------------------
// Crafted inputs
// ---------------------------------------------------------------------------
struct Crafted {
    std::string name;
    unsigned width = 0;
    unsigned height = 0;
    unsigned channels = 3;
    Bytes pixels;
    bool expect_wrap = false;
    // For wrapping cases: hand-derived offset of the first differing byte
    // between qoi::encode and qoi.h's qoi_encode (see differential_ref_test.cpp).
    std::size_t expect_first_diff = 0;
};

namespace detail {

struct Builder {
    explicit Builder(unsigned c) : channels(c) {}
    unsigned channels;
    Bytes px;
    Builder& rgb(int r, int g, int b) {
        px.push_back(static_cast<std::uint8_t>(r));
        px.push_back(static_cast<std::uint8_t>(g));
        px.push_back(static_cast<std::uint8_t>(b));
        return *this;
    }
    Builder& rgba(int r, int g, int b, int a) {
        rgb(r, g, b);
        px.push_back(static_cast<std::uint8_t>(a));
        return *this;
    }
    Builder& rgb_n(int r, int g, int b, int n) {
        for (int i = 0; i < n; ++i) {
            rgb(r, g, b);
        }
        return *this;
    }
    Builder& rgba_n(int r, int g, int b, int a, int n) {
        for (int i = 0; i < n; ++i) {
            rgba(r, g, b, a);
        }
        return *this;
    }
    std::size_t count() const { return px.size() / channels; }
};

inline Crafted make(const std::string& name, const Builder& b, bool wrap, std::size_t diff_at = 0,
                    unsigned width = 0) {
    Crafted c;
    c.name = name;
    c.channels = b.channels;
    const unsigned n = static_cast<unsigned>(b.count());
    // Lay the pixels out in one row unless a width was requested.
    c.width = width == 0 ? n : width;
    c.height = n / c.width;
    c.pixels = b.px;
    c.expect_wrap = wrap;
    c.expect_first_diff = diff_at;
    return c;
}

}  // namespace detail

// Inputs without any wrap-around delta: both encoders must emit identical bytes.
inline std::vector<Crafted> crafted_clean_cases() {
    using detail::Builder;
    using detail::make;
    std::vector<Crafted> v;

    v.push_back(make("SingleBlackPixel", Builder{3}.rgb(0, 0, 0), false));
    v.push_back(make("SingleOpaqueBlackPixelRgba", Builder{4}.rgba(0, 0, 0, 255), false));
    v.push_back(make("SingleMidPixel", Builder{3}.rgb(10, 20, 30), false));
    // From (200,200,200): +55 is out of every delta range, as is -155; both
    // rules pick RGB (wrapped: -101, still out of range).
    v.push_back(make(
        "LargeJumpsStayRgb",
        Builder{3}.rgb(200, 200, 200).rgb(255, 255, 255).rgb(100, 100, 100).rgb(0, 0, 0), false));
    // 128 apart: wrapped -128, out of range.
    v.push_back(
        make("Jump128", Builder{3}.rgb(128, 128, 128).rgb(0, 0, 0).rgb(128, 128, 128), false));
    // DIFF boundaries -2 and +1 per channel, then LUMA boundaries.
    v.push_back(make("DiffBoundaries",
                     Builder{3}
                         .rgb(100, 100, 100)
                         .rgb(98, 98, 98)
                         .rgb(99, 99, 99)
                         .rgb(97, 100, 98)
                         .rgb(98, 101, 99),
                     false));
    // LUMA: dg = +31 with dr-dg = +7, db-dg = -8; then dg = -32 with dr-dg = -8, db-dg = +7.
    v.push_back(make("LumaBoundaries",
                     Builder{3}.rgb(100, 100, 100).rgb(138, 131, 123).rgb(113, 99, 130), false));
    v.push_back(make(
        "AscendingGrayRamp",
        [] {
            Builder b{3};
            for (int i = 0; i < 256; ++i) {
                b.rgb(i, i, i);
            }
            return b;
        }(),
        false, 0, 16));
    v.push_back(make(
        "RunsOf62And63And124",
        Builder{3}.rgb(5, 6, 7).rgb_n(40, 50, 60, 62).rgb_n(1, 2, 3, 63).rgb_n(200, 100, 50, 124),
        false));
    v.push_back(make("RunAtLastPixel", Builder{3}.rgb(9, 9, 9).rgb_n(77, 78, 79, 70), false));
    v.push_back(make("RgbaAlphaChanges",
                     Builder{4}
                         .rgba(10, 20, 30, 255)
                         .rgba(11, 21, 31, 255)
                         .rgba(11, 21, 31, 128)
                         .rgba(12, 22, 32, 128)
                         .rgba(12, 22, 32, 0)
                         .rgba(250, 5, 3, 0),
                     false));
    // The alpha change forces RGBA even though the colour delta alone would wrap.
    v.push_back(make("AlphaChangeSuppressesDeltas",
                     Builder{4}.rgba(200, 200, 200, 255).rgba(0, 0, 0, 10), false));
    // Palette: later occurrences are INDEX hits.
    v.push_back(make("PaletteIndexHits",
                     Builder{3}
                         .rgb(10, 200, 30)
                         .rgb(90, 20, 60)
                         .rgb(10, 200, 30)
                         .rgb(90, 20, 60)
                         .rgb(200, 10, 10)
                         .rgb(10, 200, 30),
                     false));
    // (255,255,255) is reached cleanly from (200,200,200); the last pixel is an
    // INDEX hit for it, although the delta from (0,0,0) would wrap to +1.
    v.push_back(make("IndexHitSuppressesWrapAround",
                     Builder{3}
                         .rgb(200, 200, 200)
                         .rgb(255, 255, 255)
                         .rgb(100, 100, 100)
                         .rgb(0, 0, 0)
                         .rgb(255, 255, 255),
                     false));
    v.push_back(make("RgbaPaletteWithZeroPixel",
                     Builder{4}.rgba(0, 0, 0, 0).rgba(0, 0, 0, 255).rgba(0, 0, 0, 0), false));
    return v;
}

// Inputs with a wrap-around delta (255<->0 etc.): both encoders must decode to
// the same pixels, but their bytes differ at expect_first_diff.
//
// The offsets were derived by hand from the encoding rules. Header = 14 bytes.
// RGB chunk = 4 bytes, DIFF = 1, LUMA = 2, RUN = 1, qoi.h's wrapped DIFF byte
// for (-1,-1,-1) is 0x55 and for (+1,+1,+1) is 0x7f.
inline std::vector<Crafted> crafted_wrapping_cases() {
    using detail::Builder;
    using detail::make;
    std::vector<Crafted> v;

    // Start state is (0,0,0,255): white is +255 per channel = -1 wrapped.
    v.push_back(make("WrapAroundFirstPixelWhite", Builder{3}.rgb(255, 255, 255), true, 14));
    // 255 -> 0: RGB(200), RGB(255), then (0,0,0): d = -255 -> +1 (DIFF).
    v.push_back(make("WrapAround255To0Diff",
                     Builder{3}.rgb(200, 200, 200).rgb(255, 255, 255).rgb(0, 0, 0), true, 22));
    // 0 -> 255: RGB(100), RGB(0), then 255: d = +255 -> -1 (DIFF).
    v.push_back(make("WrapAround0To255Diff",
                     Builder{3}.rgb(100, 100, 100).rgb(0, 0, 0).rgb(255, 255, 255), true, 22));
    // 254 -> 1: d = -253 -> +3: outside DIFF, inside LUMA (dg=3, dr-dg=0).
    v.push_back(make("WrapAround254To1Luma",
                     Builder{3}.rgb(200, 200, 200).rgb(254, 254, 254).rgb(1, 1, 1), true, 22));
    // 10 -> 246: d = +236 -> -20 (LUMA); the previous pixel is a legitimate LUMA (2 bytes).
    v.push_back(make("WrapAroundLumaGreenMinus20", Builder{3}.rgb(10, 10, 10).rgb(246, 246, 246),
                     true, 16));
    // Only red wraps; green and blue move by +1.
    v.push_back(
        make("WrapAroundSingleChannel", Builder{3}.rgb(0, 100, 100).rgb(255, 101, 101), true, 18));
    // 1 -> 255: d = +254 -> -2, the lower DIFF bound. (1,1,1) is a legitimate DIFF (1 byte).
    v.push_back(
        make("WrapAroundDiffLowerBound", Builder{3}.rgb(1, 1, 1).rgb(255, 255, 255), true, 15));
    // 0 -> 225: d = +225 -> -31, inside LUMA (dg=-31, dr-dg=0).
    v.push_back(make("WrapAroundLumaGreenMinus31", Builder{3}.rgb(225, 225, 225), true, 14));
    // Same as 255->0 with four channels and unchanged alpha, then an alpha change.
    v.push_back(make("WrapAround255To0Rgba",
                     Builder{4}
                         .rgba(200, 200, 200, 255)
                         .rgba(255, 255, 255, 255)
                         .rgba(0, 0, 0, 255)
                         .rgba(10, 20, 30, 128),
                     true, 22));
    // A run in front of the wrapping pixel: RGB(4) RGB(4) RUN62(1) RUN7(1), then the pixel.
    v.push_back(
        make("WrapAroundAfterRuns",
             Builder{3}.rgb(200, 200, 200).rgb(255, 255, 255).rgb_n(255, 255, 255, 69).rgb(0, 0, 0),
             true, 24));
    return v;
}

// ---------------------------------------------------------------------------
// Random images
// ---------------------------------------------------------------------------
enum class Kind {
    Noise,        // uniform random bytes
    Flat,         // one colour
    ClampedWalk,  // small random steps, clamped to 0..255 (no wrap-around)
    ModularWalk,  // small random steps modulo 256 (wraps at the ends)
    ExtremeWalk,  // small steps with frequent jumps between the 0 and 255 ends
    Palette,      // few colours, many near the ends, drawn at random with repeats
    Runs,         // runs of 1..130 equal pixels
    AlphaNoise,   // walking colour with randomly changing alpha
};
inline constexpr std::array<Kind, 8> kAllKinds = {
    Kind::Noise,       Kind::Flat,    Kind::ClampedWalk, Kind::ModularWalk,
    Kind::ExtremeWalk, Kind::Palette, Kind::Runs,        Kind::AlphaNoise};

inline Bytes make_image(std::mt19937& rng, Kind kind, std::size_t npx, unsigned channels) {
    Bytes img;
    img.reserve(npx * channels);
    std::array<int, 4> cur{};
    for (int& c : cur) {
        c = next_byte(rng);
    }
    cur[3] = channels == 4 ? cur[3] : 255;
    auto emit = [&]() {
        for (unsigned c = 0; c < channels; ++c) {
            img.push_back(static_cast<std::uint8_t>(cur[c]));
        }
    };
    auto extreme = [&]() {
        // 0..5 or 250..255
        const int e = static_cast<int>(below(rng, 6));
        return chance(rng, 2) ? e : 255 - e;
    };

    switch (kind) {
    case Kind::Noise:
        for (std::size_t i = 0; i < npx * channels; ++i) {
            img.push_back(next_byte(rng));
        }
        break;

    case Kind::Flat:
        for (std::size_t i = 0; i < npx; ++i) {
            emit();
        }
        break;

    case Kind::ClampedWalk:
    case Kind::ModularWalk:
        if (kind == Kind::ModularWalk) {
            for (std::size_t c = 0; c < 3; ++c) {
                if (chance(rng, 2)) {
                    cur[c] = extreme();
                }
            }
        }
        for (std::size_t i = 0; i < npx; ++i) {
            const int span = kind == Kind::ClampedWalk ? 81 : 9;
            const int half = span / 2;
            for (std::size_t c = 0; c < 3; ++c) {
                const int step =
                    static_cast<int>(below(rng, static_cast<std::uint32_t>(span))) - half;
                if (kind == Kind::ClampedWalk) {
                    cur[c] = std::max(0, std::min(255, cur[c] + step));
                } else {
                    cur[c] = (cur[c] + step) & 0xFF;
                }
            }
            if (channels == 4 && chance(rng, 40)) {
                cur[3] = next_byte(rng);
            }
            emit();
        }
        break;

    case Kind::ExtremeWalk:
        for (std::size_t i = 0; i < npx; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                if (chance(rng, 6)) {
                    cur[c] = extreme();
                } else {
                    cur[c] = (cur[c] + static_cast<int>(below(rng, 5)) - 2) & 0xFF;
                }
            }
            if (chance(rng, 5)) {  // all three channels jump together
                const int e = extreme();
                cur[0] = cur[1] = cur[2] = e;
            }
            emit();
        }
        break;

    case Kind::Palette: {
        const std::size_t k = 2 + below(rng, 15);
        std::vector<std::array<int, 4>> palette(k);
        for (auto& color : palette) {
            for (std::size_t c = 0; c < 4; ++c) {
                color[c] = chance(rng, 2) ? extreme() : next_byte(rng);
            }
            if (channels == 3 || chance(rng, 2)) {
                color[3] = 255;
            }
        }
        std::size_t pick = 0;
        for (std::size_t i = 0; i < npx; ++i) {
            if (!chance(rng, 3)) {
                pick = below(rng, static_cast<std::uint32_t>(k));
            }
            cur = palette[pick];
            emit();
        }
        break;
    }

    case Kind::Runs: {
        std::size_t i = 0;
        while (i < npx) {
            const std::size_t len = std::min<std::size_t>(npx - i, 1 + below(rng, 130));
            if (chance(rng, 2)) {
                for (std::size_t c = 0; c < 3; ++c) {
                    cur[c] = (cur[c] + static_cast<int>(below(rng, 7)) - 3) & 0xFF;
                }
            } else {
                for (std::size_t c = 0; c < 4; ++c) {
                    cur[c] = next_byte(rng);
                }
                cur[3] = channels == 4 ? cur[3] : 255;
            }
            for (std::size_t j = 0; j < len; ++j) {
                emit();
            }
            i += len;
        }
        break;
    }

    case Kind::AlphaNoise:
        for (std::size_t i = 0; i < npx; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                cur[c] = (cur[c] + static_cast<int>(below(rng, 11)) - 5) & 0xFF;
            }
            if (channels == 4 && chance(rng, 3)) {
                static const std::array<int, 3> alphas = {255, 128, 0};
                cur[3] = chance(rng, 2) ? alphas[below(rng, 3)] : next_byte(rng);
            }
            emit();
        }
        break;
    }
    return img;
}

struct Size {
    unsigned w;
    unsigned h;
};

// Sizes: degenerate rows/columns, run-length boundaries (61..64) and a few
// blocks.
inline const std::array<Size, 18>& image_sizes() {
    static const std::array<Size, 18> sizes = {{{1, 1},
                                                {1, 2},
                                                {2, 1},
                                                {1, 61},
                                                {1, 62},
                                                {63, 1},
                                                {64, 1},
                                                {3, 3},
                                                {7, 5},
                                                {16, 16},
                                                {33, 17},
                                                {64, 64},
                                                {100, 3},
                                                {255, 2},
                                                {1, 200},
                                                {200, 1},
                                                {5, 31},
                                                {47, 29}}};
    return sizes;
}

// ---------------------------------------------------------------------------
// Random / structured QOI streams for decoder differentials
// ---------------------------------------------------------------------------
inline Bytes make_header(std::uint32_t w, std::uint32_t h, unsigned channels, unsigned colorspace) {
    Bytes b = {'q', 'o', 'i', 'f'};
    for (std::uint32_t v : {w, h}) {
        b.push_back(static_cast<std::uint8_t>(v >> 24));
        b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
        b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
        b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    }
    b.push_back(static_cast<std::uint8_t>(channels));
    b.push_back(static_cast<std::uint8_t>(colorspace));
    return b;
}

inline void append_padding(Bytes& b) {
    for (const std::uint8_t p : qoi::QOI_PADDING) {
        b.push_back(p);
    }
}

// A QOI stream with valid header fields and arbitrary chunk bytes, padding that
// is valid, garbage or absent, and optionally cut short. `structured` chooses
// between uniformly random chunk bytes and opcode-aware ones.
inline Bytes make_random_stream(std::mt19937& rng, std::uint32_t w, std::uint32_t h,
                                unsigned channels, unsigned colorspace) {
    Bytes b = make_header(w, h, channels, colorspace);
    const std::size_t npx = static_cast<std::size_t>(w) * h;
    const std::size_t target = below(rng, static_cast<std::uint32_t>(npx * 3 + 24));

    if (chance(rng, 3)) {
        for (std::size_t i = 0; i < target; ++i) {
            b.push_back(next_byte(rng));
        }
    } else {
        while (b.size() < 14 + target) {
            switch (below(rng, 8)) {
            case 0:
                b.push_back(0xFE);
                for (int i = 0; i < 3; ++i)
                    b.push_back(next_byte(rng));
                break;
            case 1:
                b.push_back(0xFF);
                for (int i = 0; i < 4; ++i)
                    b.push_back(next_byte(rng));
                break;
            case 2:
                b.push_back(static_cast<std::uint8_t>(0x80 | (next_byte(rng) & 0x3F)));
                b.push_back(next_byte(rng));
                break;
            case 3:
                b.push_back(static_cast<std::uint8_t>(0xC0 | below(rng, 62)));
                break;
            case 4:
                b.push_back(static_cast<std::uint8_t>(next_byte(rng) & 0x3F));  // INDEX
                break;
            default:
                b.push_back(static_cast<std::uint8_t>(0x40 | (next_byte(rng) & 0x3F)));  // DIFF
                break;
            }
        }
    }

    switch (below(rng, 4)) {
    case 0:
    case 1:
        append_padding(b);
        break;
    case 2:
        for (std::size_t i = below(rng, 12); i > 0; --i) {
            b.push_back(next_byte(rng));
        }
        break;
    default:
        break;  // no padding at all
    }

    if (chance(rng, 4)) {
        b.resize(below(rng, static_cast<std::uint32_t>(b.size() + 1)));
    }
    return b;
}

}  // namespace difftest

#endif  // QOI_TESTS_DIFF_UTIL_HPP
