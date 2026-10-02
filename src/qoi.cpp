// QOI (Quite OK Image) codec, C++17 port of the Ada/SPARK body legacy/ada/src/qoi.adb.
//
// The port mirrors the Ada body statement by statement. The Unchecked_Conversion
// bit-field records of legacy/ada/src/qoi.ads (Index_Tag, Diff_Tag, LUMA_Tag_A, LUMA_Tag_B,
// Run_Tag) are replaced by explicit shift/mask code and named opcode constants.
#include "qoi/qoi.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace qoi {

namespace {

// ---------------------------------------------------------------------------
// Opcode constants (QOI specification; Tag_Op, QOI_OP_RGB, QOI_OP_RGBA and
// QOI_MAGIC in legacy/ada/src/qoi.ads).
// ---------------------------------------------------------------------------
constexpr std::uint8_t QOI_OP_INDEX = 0x00;
constexpr std::uint8_t QOI_OP_DIFF = 0x40;
constexpr std::uint8_t QOI_OP_LUMA = 0x80;
constexpr std::uint8_t QOI_OP_RUN = 0xC0;
constexpr std::uint8_t QOI_MASK_2 = 0xC0;
constexpr std::uint8_t QOI_OP_RGB = 0xFE;
constexpr std::uint8_t QOI_OP_RGBA = 0xFF;
constexpr std::uint32_t QOI_MAGIC = 0x716F6966U;  // "qoif"

// Maximum run length held in one RUN chunk before it is flushed (Run_Range'Last
// in the Ada body).
constexpr std::uint32_t QOI_MAX_RUN = 62;

// Largest value of Ada's Storage_Count ('Last = 2**63 - 1).
constexpr std::uint64_t STORAGE_COUNT_LAST = 0x7FFFFFFFFFFFFFFFULL;

// Largest dimension accepted by valid_size (Integer_32'Last).
constexpr std::uint64_t MAX_DIMENSION = 0x7FFFFFFFULL;

struct Color {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;

    constexpr bool operator==(const Color& other) const noexcept {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

// Hash of a pixel (Ada: Hash). Computed in 8-bit modular arithmetic, as with the
// Ada Storage_Element operators, and reduced mod 64 by the caller's index size.
std::uint8_t hash(const Color& c) noexcept {
    const std::uint8_t h = static_cast<std::uint8_t>(c.r * 3 + c.g * 5 + c.b * 7 + c.a * 11);
    return static_cast<std::uint8_t>(h % 64);
}

// Big-endian store of a 32-bit value (Ada: Push (Unsigned_32)).
void push32(std::uint8_t* buf, std::size_t& p, std::uint32_t value) noexcept {
    buf[p] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
    buf[p + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    buf[p + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    buf[p + 3] = static_cast<std::uint8_t>(value & 0xFFU);
    p += 4;
}

// Big-endian load of a 32-bit value (Ada: Pop32). The caller guarantees that
// four bytes are readable at buf[p].
std::uint32_t pop32(const std::uint8_t* buf, std::size_t& p) noexcept {
    const std::uint32_t a = buf[p];
    const std::uint32_t b = buf[p + 1];
    const std::uint32_t c = buf[p + 2];
    const std::uint32_t d = buf[p + 3];
    p += 4;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// Per-channel differences between a pixel and its predecessor, plus the LUMA
// differences relative to the green delta.
struct Deltas {
    int vr;
    int vg;
    int vb;
    int vg_r;  // vr - vg
    int vg_b;  // vb - vg
};

// The single place where the encoder's delta computation lives.
//
// Default behaviour is Ada-identical: the deltas are UNBOUNDED integer
// differences which are then range-tested by the caller (so 255 -> 0 is -255,
// which selects RGB). The reference qoi.h instead stores the deltas in
// `signed char`, which wraps (255 -> 0 is +1, selecting DIFF/LUMA). Switching to
// that behaviour means changing ONLY this function: wrap all five values (vr, vg,
// vb, vg_r and vg_b) to the signed 8-bit range, e.g. static_cast<signed char>(...)
// of each difference. In qoi.h vg_r and vg_b are themselves signed char, so they
// wrap a second time rather than being derived from the wrapped vr/vg/vb.
Deltas compute_deltas(const Color& px, const Color& prev) noexcept {
    Deltas d{};
    d.vr = static_cast<int>(px.r) - static_cast<int>(prev.r);
    d.vg = static_cast<int>(px.g) - static_cast<int>(prev.g);
    d.vb = static_cast<int>(px.b) - static_cast<int>(prev.b);
    d.vg_r = d.vr - d.vg;
    d.vg_b = d.vb - d.vg;
    return d;
}

// Emit a RUN chunk for `run` repeated pixels (1..62), stored as run-1.
void push_run(std::uint8_t* buf, std::size_t& p, std::uint32_t run) noexcept {
    assert(run >= 1 && run <= QOI_MAX_RUN);
    buf[p++] = static_cast<std::uint8_t>(QOI_OP_RUN | ((run - 1U) & 0x3FU));
}

constexpr bool in_range(int v, int lo, int hi) noexcept { return v >= lo && v <= hi; }

}  // namespace

// ---------------------------------------------------------------------------
// Valid_Size / Encode_Worst_Case
// ---------------------------------------------------------------------------

bool valid_size(const Desc& desc) noexcept {
    // Mirrors the Ada expression function; every product is guarded by a
    // division so that nothing overflows std::uint64_t (nor the 2**63-1 bound).
    if (desc.width < 1 || desc.width > MAX_DIMENSION) {
        return false;
    }
    if (desc.height < 1 || desc.height > MAX_DIMENSION) {
        return false;
    }
    if (desc.channels < 3 || desc.channels > 4) {
        return false;
    }
    if (desc.width > STORAGE_COUNT_LAST / desc.height) {
        return false;
    }
    const std::uint64_t pixels = desc.width * desc.height;
    if (desc.channels + 1 > STORAGE_COUNT_LAST / pixels) {
        return false;
    }
    const std::uint64_t body = pixels * (desc.channels + 1);
    return QOI_HEADER_SIZE + QOI_PADDING.size() <= STORAGE_COUNT_LAST - body;
}

std::uint64_t encode_worst_case(const Desc& desc) noexcept {
    if (!valid_size(desc)) {
        return 0;
    }
    return desc.width * desc.height * (desc.channels + 1) + QOI_HEADER_SIZE + QOI_PADDING.size();
}

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------

std::size_t encode(Span<const std::uint8_t> pix, const Desc& desc,
                   Span<std::uint8_t> out) noexcept {
    // SPARK preconditions turned into checked conditions.
    if (!valid_size(desc)) {
        return 0;
    }
    const std::uint64_t number_of_pixels = desc.width * desc.height;
    if (static_cast<std::uint64_t>(pix.size()) != number_of_pixels * desc.channels) {
        return 0;
    }
    // Ada: if Output'Length < Encode_Worst_Case (Desc) then Output_Size := 0.
    if (static_cast<std::uint64_t>(out.size()) < encode_worst_case(desc)) {
        return 0;
    }

    std::uint8_t* const buf = out.data();
    const std::uint8_t* const src = pix.data();
    const std::size_t channels = static_cast<std::size_t>(desc.channels);
    std::size_t p = 0;

    // Header: magic, width, height (big-endian u32), channels, colorspace.
    push32(buf, p, QOI_MAGIC);
    push32(buf, p, static_cast<std::uint32_t>(desc.width));
    push32(buf, p, static_cast<std::uint32_t>(desc.height));
    buf[p++] = static_cast<std::uint8_t>(desc.channels);
    buf[p++] = static_cast<std::uint8_t>(desc.colorspace);
    assert(p == QOI_HEADER_SIZE);

    std::array<Color, 64> seen{};  // zero-initialised: (0, 0, 0, 0)
    Color px_prev{0, 0, 0, 255};
    std::uint32_t run = 0;

    for (std::uint64_t px_index = 0; px_index < number_of_pixels; ++px_index) {
        const std::uint8_t* const s = src + static_cast<std::size_t>(px_index) * channels;
        Color px{s[0], s[1], s[2], 255};
        if (channels == 4) {
            px.a = s[3];
        }

        if (px == px_prev) {
            ++run;
            if (run == QOI_MAX_RUN || px_index == number_of_pixels - 1) {
                push_run(buf, p, run);
                run = 0;
            }
        } else {
            if (run > 0) {
                push_run(buf, p, run);
                run = 0;
            }

            const std::uint8_t index_pos = hash(px);  // already mod 64
            if (seen[index_pos] == px) {
                // INDEX hit: the table already holds px, so it is not updated.
                buf[p++] = static_cast<std::uint8_t>(QOI_OP_INDEX | index_pos);
            } else {
                seen[index_pos] = px;

                if (px.a == px_prev.a) {
                    const Deltas d = compute_deltas(px, px_prev);

                    if (in_range(d.vr, -2, 1) && in_range(d.vg, -2, 1) && in_range(d.vb, -2, 1)) {
                        buf[p++] = static_cast<std::uint8_t>(QOI_OP_DIFF | ((d.vr + 2) << 4) |
                                                             ((d.vg + 2) << 2) | (d.vb + 2));
                    } else if (in_range(d.vg_r, -8, 7) && in_range(d.vg, -32, 31) &&
                               in_range(d.vg_b, -8, 7)) {
                        buf[p++] = static_cast<std::uint8_t>(QOI_OP_LUMA | (d.vg + 32));
                        buf[p++] = static_cast<std::uint8_t>(((d.vg_r + 8) << 4) | (d.vg_b + 8));
                    } else {
                        buf[p++] = QOI_OP_RGB;
                        buf[p++] = px.r;
                        buf[p++] = px.g;
                        buf[p++] = px.b;
                    }
                } else {
                    buf[p++] = QOI_OP_RGBA;
                    buf[p++] = px.r;
                    buf[p++] = px.g;
                    buf[p++] = px.b;
                    buf[p++] = px.a;
                }
            }
        }

        px_prev = px;
    }

    // Padding: 00 00 00 00 00 00 00 01.
    for (const std::uint8_t pad : QOI_PADDING) {
        buf[p++] = pad;
    }

    assert(p <= out.size());
    return p;
}

// ---------------------------------------------------------------------------
// Get_Desc
// ---------------------------------------------------------------------------

void get_desc(Span<const std::uint8_t> data, Desc& desc) noexcept {
    if (data.size() < QOI_HEADER_SIZE) {
        desc = Desc{0, 0, 0, Colorspace::SRGB};
        return;
    }

    const std::uint8_t* const buf = data.data();
    std::size_t p = 0;

    const std::uint32_t magic = pop32(buf, p);
    if (magic != QOI_MAGIC) {
        desc = Desc{0, 0, 0, Colorspace::SRGB};
        return;
    }

    const std::uint32_t width = pop32(buf, p);
    const std::uint32_t height = pop32(buf, p);
    const std::uint8_t channels = buf[p++];
    const std::uint8_t colorspace = buf[p++];
    assert(p == QOI_HEADER_SIZE);

    // Only the two defined colorspace bytes are accepted. No other validation
    // (channels, dimensions) happens here, as in the Ada body.
    if (colorspace != static_cast<std::uint8_t>(Colorspace::SRGB) &&
        colorspace != static_cast<std::uint8_t>(Colorspace::SRGB_Linear_Alpha)) {
        desc = Desc{0, 0, 0, Colorspace::SRGB};
        return;
    }

    desc.width = width;
    desc.height = height;
    desc.channels = channels;
    desc.colorspace = static_cast<Colorspace>(colorspace);
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

std::size_t decode(Span<const std::uint8_t> data, Desc& desc, Span<std::uint8_t> out) noexcept {
    // desc keeps get_desc's result on any later failure (Ada behaviour).
    get_desc(data, desc);

    // SPARK precondition Data'Length >= QOI_HEADER_SIZE + QOI_PADDING'Length,
    // checked after get_desc so that a short but header-valid stream still
    // reports its parsed descriptor.
    if (data.size() < QOI_HEADER_SIZE + QOI_PADDING.size()) {
        return 0;
    }

    // Ada's checks, in Ada's order. Short-circuiting guarantees that the
    // divisions below never see a zero divisor and that the final product
    // cannot overflow.
    if (desc.width == 0 || desc.height == 0 || desc.channels < 3 || desc.channels > 4 ||
        desc.height > STORAGE_COUNT_LAST / desc.width ||
        desc.channels > STORAGE_COUNT_LAST / (desc.width * desc.height) ||
        static_cast<std::uint64_t>(out.size()) < desc.width * desc.height * desc.channels) {
        return 0;
    }

    const std::uint8_t* const src = data.data();
    std::uint8_t* const dst = out.data();
    const bool has_alpha = desc.channels == 4;
    const std::uint64_t number_of_pixels = desc.width * desc.height;

    std::size_t p = QOI_HEADER_SIZE;
    // Ada: Last_Chunk := Data'Last - QOI_PADDING'Length. With data.size() >= 22
    // this does not underflow. `p <= last_chunk` iff p + 9 <= data.size(), so
    // the longest chunk (RGBA, 5 bytes) always stays inside the data.
    const std::size_t last_chunk = (data.size() - 1) - QOI_PADDING.size();
    std::size_t out_index = 0;

    std::array<Color, 64> seen{};  // zero-initialised: (0, 0, 0, 0)
    Color px{0, 0, 0, 255};
    std::uint32_t run = 0;

    for (std::uint64_t px_index = 0; px_index < number_of_pixels; ++px_index) {
        if (run > 0) {
            --run;
        } else if (p <= last_chunk) {
            const std::uint8_t b1 = src[p++];

            if (b1 == QOI_OP_RGB) {
                px.r = src[p++];
                px.g = src[p++];
                px.b = src[p++];
            } else if (b1 == QOI_OP_RGBA) {
                px.r = src[p++];
                px.g = src[p++];
                px.b = src[p++];
                px.a = src[p++];
            } else {
                switch (static_cast<std::uint8_t>(b1 & QOI_MASK_2)) {
                case QOI_OP_INDEX:
                    px = seen[b1 & 0x3FU];
                    break;

                case QOI_OP_DIFF:
                    // All additions are modulo 256.
                    px.r = static_cast<std::uint8_t>(px.r + ((b1 >> 4) & 3) - 2);
                    px.g = static_cast<std::uint8_t>(px.g + ((b1 >> 2) & 3) - 2);
                    px.b = static_cast<std::uint8_t>(px.b + (b1 & 3) - 2);
                    break;

                case QOI_OP_LUMA: {
                    const std::uint8_t b2 = src[p++];
                    const int vg = (b1 & 0x3F) - 32;
                    px.r = static_cast<std::uint8_t>(px.r + vg + (b2 >> 4) - 8);
                    px.g = static_cast<std::uint8_t>(px.g + vg);
                    px.b = static_cast<std::uint8_t>(px.b + vg + (b2 & 0x0F) - 8);
                    break;
                }

                case QOI_OP_RUN:
                    run = static_cast<std::uint32_t>((b1 & 0x3F) % 63);
                    break;

                default:
                    // Unreachable: b1 & QOI_MASK_2 has only four values.
                    assert(false);
                    break;
                }
            }

            // The index is refreshed after every chunk (including INDEX and
            // RUN chunks), but not for repeated or past-the-end pixels.
            seen[hash(px)] = px;
        }
        // else: past the last chunk, the previous pixel repeats.

        dst[out_index++] = px.r;
        dst[out_index++] = px.g;
        dst[out_index++] = px.b;
        if (has_alpha) {
            dst[out_index++] = px.a;
        }
    }

    return out_index;
}

}  // namespace qoi
