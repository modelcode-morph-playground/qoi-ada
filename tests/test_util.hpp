// Shared helpers for the codec verification suite (golden, round trip and
// contract tests): byte-vector builders, a deterministic random source, image
// generators and comparison helpers with readable failure output.
//
// Nothing here calls the codec to build an expectation: the header and padding
// builders are written out byte by byte from the QOI format specification so
// that the golden vectors stay independent of the code under test.
#ifndef QOI_TESTS_TEST_UTIL_HPP
#define QOI_TESTS_TEST_UTIL_HPP

#include <qoi/qoi.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace qoi_test {

using Bytes = std::vector<std::uint8_t>;

// Truncating conversion to one byte (modulo 256), usable with int and unsigned
// arguments without tripping -Wconversion.
template <typename T> constexpr std::uint8_t u8(T value) noexcept {
    return static_cast<std::uint8_t>(static_cast<unsigned>(value) & 0xFFU);
}

// ---------------------------------------------------------------------------
// Byte stream builders (written from the QOI specification)
// ---------------------------------------------------------------------------

// Big-endian encoding of a 32-bit value.
inline Bytes be32(std::uint32_t value) {
    return {u8(value >> 24), u8(value >> 16), u8(value >> 8), u8(value)};
}

inline Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const Bytes& part : parts) {
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

// The 14-byte QOI header: magic "qoif", width BE, height BE, channels,
// colorspace byte.
inline Bytes header(std::uint32_t width, std::uint32_t height, std::uint8_t channels,
                    std::uint8_t colorspace) {
    return concat(
        {Bytes{0x71, 0x6F, 0x69, 0x66}, be32(width), be32(height), Bytes{channels, colorspace}});
}

// The 8-byte end marker: seven zero bytes and a one.
inline Bytes padding() { return Bytes{0, 0, 0, 0, 0, 0, 0, 1}; }

// header + chunks + padding.
inline Bytes stream(std::uint32_t width, std::uint32_t height, std::uint8_t channels,
                    std::uint8_t colorspace, const Bytes& chunks) {
    return concat({header(width, height, channels, colorspace), chunks, padding()});
}

// ---------------------------------------------------------------------------
// Descriptor helpers
// ---------------------------------------------------------------------------

inline qoi::Desc make_desc(std::uint64_t width, std::uint64_t height, std::uint64_t channels,
                           qoi::Colorspace colorspace = qoi::Colorspace::SRGB) {
    qoi::Desc desc;
    desc.width = width;
    desc.height = height;
    desc.channels = channels;
    desc.colorspace = colorspace;
    return desc;
}

inline qoi::Desc empty_desc() { return make_desc(0, 0, 0, qoi::Colorspace::SRGB); }

// A descriptor that no successful or failed parse would produce, used to prove
// that a function really overwrites its descriptor argument.
inline qoi::Desc poisoned_desc() {
    return make_desc(0xDEADBEEFULL, 0xFEEDF00DULL, 99, qoi::Colorspace::SRGB_Linear_Alpha);
}

inline std::size_t byte_count(const qoi::Desc& desc) {
    return static_cast<std::size_t>(desc.width * desc.height * desc.channels);
}

inline qoi::Colorspace colorspace_of(std::size_t index) {
    return (index % 2U == 0U) ? qoi::Colorspace::SRGB : qoi::Colorspace::SRGB_Linear_Alpha;
}

// ---------------------------------------------------------------------------
// Comparison helpers with readable output
// ---------------------------------------------------------------------------

inline std::string hex(const std::uint8_t* data, std::size_t size, std::size_t max_bytes = 96) {
    static const char digits[] = "0123456789ABCDEF";
    std::string text;
    const std::size_t shown = std::min(size, max_bytes);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            text += ' ';
        }
        text += digits[(data[i] >> 4) & 0x0F];
        text += digits[data[i] & 0x0F];
    }
    if (shown < size) {
        text += " ... (" + std::to_string(size - shown) + " more)";
    }
    return text;
}

inline std::string hex(const Bytes& bytes, std::size_t max_bytes = 96) {
    return hex(bytes.data(), bytes.size(), max_bytes);
}

// Compares two byte sequences; on mismatch reports both lengths, the first
// differing offset and a hex dump window around it.
inline ::testing::AssertionResult BytesEqual(const std::uint8_t* expected,
                                             std::size_t expected_size, const std::uint8_t* actual,
                                             std::size_t actual_size) {
    const std::size_t common = std::min(expected_size, actual_size);
    std::size_t first_diff = common;
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            first_diff = i;
            break;
        }
    }
    if (first_diff == common && expected_size == actual_size) {
        return ::testing::AssertionSuccess();
    }

    const std::size_t window_begin = first_diff >= 8 ? first_diff - 8 : 0;
    const auto window = [&](const std::uint8_t* data, std::size_t size) {
        if (window_begin >= size) {
            return std::string("<none>");
        }
        return hex(data + window_begin, size - window_begin, 24);
    };

    std::ostringstream message;
    message << "byte sequences differ: expected " << expected_size << " bytes, actual "
            << actual_size << " bytes, first difference at offset " << first_diff << "\n"
            << "  expected [from " << window_begin << "]: " << window(expected, expected_size)
            << "\n"
            << "  actual   [from " << window_begin << "]: " << window(actual, actual_size);
    return ::testing::AssertionFailure() << message.str();
}

inline ::testing::AssertionResult BytesEqual(const Bytes& expected, const Bytes& actual) {
    return BytesEqual(expected.data(), expected.size(), actual.data(), actual.size());
}

// Compares the first `size` bytes of a buffer against an expected vector.
inline ::testing::AssertionResult BytesEqual(const Bytes& expected, const Bytes& buffer,
                                             std::size_t size) {
    if (size > buffer.size()) {
        return ::testing::AssertionFailure()
               << "buffer holds " << buffer.size() << " bytes, cannot compare " << size;
    }
    return BytesEqual(expected.data(), expected.size(), buffer.data(), size);
}

inline ::testing::AssertionResult DescEq(const qoi::Desc& expected, const qoi::Desc& actual) {
    if (expected.width == actual.width && expected.height == actual.height &&
        expected.channels == actual.channels && expected.colorspace == actual.colorspace) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "desc differs: expected {w=" << expected.width << " h=" << expected.height
           << " c=" << expected.channels << " cs=" << static_cast<unsigned>(expected.colorspace)
           << "} actual {w=" << actual.width << " h=" << actual.height << " c=" << actual.channels
           << " cs=" << static_cast<unsigned>(actual.colorspace) << "}";
}

// True if every byte in [begin, end) of the buffer equals `value`.
inline bool all_equal(const Bytes& buffer, std::size_t begin, std::size_t end, std::uint8_t value) {
    for (std::size_t i = begin; i < end; ++i) {
        if (buffer[i] != value) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Exact-size heap copies, so that sanitizers see every out-of-bounds access
// ---------------------------------------------------------------------------

// Owns a heap array of exactly `size` bytes (a new[] of that size, not a
// vector with spare capacity). Zero-length copies are valid and non-null.
class ExactBytes {
public:
    ExactBytes(const std::uint8_t* source, std::size_t size)
        : size_(size), data_(new std::uint8_t[size]) {
        std::copy(source, source + size, data_.get());
    }

    explicit ExactBytes(const Bytes& source) : ExactBytes(source.data(), source.size()) {}

    [[nodiscard]] qoi::Span<const std::uint8_t> span() const noexcept {
        return qoi::Span<const std::uint8_t>(data_.get(), size_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::size_t size_;
    std::unique_ptr<std::uint8_t[]> data_;
};

// ---------------------------------------------------------------------------
// Deterministic random source
//
// std::mt19937 output is fully specified by the standard, so sequences are
// identical on every platform. std::uniform_int_distribution is NOT used, its
// results are implementation-defined; bytes and bounded values are derived with
// masks and the modulo operator instead.
// ---------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(std::uint32_t seed) : engine_(seed) {}

    std::uint32_t next() { return static_cast<std::uint32_t>(engine_()); }
    std::uint8_t byte() { return u8(next() & 0xFFU); }
    // Value in [0, bound). The modulo bias is irrelevant for test data.
    std::uint32_t below(std::uint32_t bound) { return next() % bound; }

private:
    std::mt19937 engine_;
};

// ---------------------------------------------------------------------------
// Image generators. Images are tightly packed, row-major, `channels` bytes per
// pixel, as the codec expects.
// ---------------------------------------------------------------------------

inline Bytes noise_image(std::size_t width, std::size_t height, std::size_t channels,
                         std::uint32_t seed) {
    Rng rng(seed);
    Bytes pix(width * height * channels);
    for (std::uint8_t& value : pix) {
        value = rng.byte();
    }
    return pix;
}

inline Bytes flat_image(std::size_t width, std::size_t height, std::size_t channels,
                        const std::array<std::uint8_t, 4>& colour) {
    Bytes pix(width * height * channels);
    for (std::size_t i = 0; i < width * height; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            pix[i * channels + c] = colour[c];
        }
    }
    return pix;
}

// Colour varies with x only: r rises, g falls, b and a rise at half and a third
// of the rate. Gives long runs, +1 steps (DIFF) and occasional larger steps.
inline Bytes horizontal_gradient_image(std::size_t width, std::size_t height,
                                       std::size_t channels) {
    Bytes pix(width * height * channels);
    const std::size_t span = width > 1 ? width - 1 : 1;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t g = x * 255U / span;
            std::uint8_t* dst = &pix[(y * width + x) * channels];
            dst[0] = u8(g);
            dst[1] = u8(255U - g);
            dst[2] = u8(g / 2U);
            if (channels == 4) {
                dst[3] = u8(255U - g / 3U);
            }
        }
    }
    return pix;
}

// Colour varies with y only.
inline Bytes vertical_gradient_image(std::size_t width, std::size_t height, std::size_t channels) {
    Bytes pix(width * height * channels);
    const std::size_t span = height > 1 ? height - 1 : 1;
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t g = y * 255U / span;
        for (std::size_t x = 0; x < width; ++x) {
            std::uint8_t* dst = &pix[(y * width + x) * channels];
            dst[0] = u8(255U - g);
            dst[1] = u8(g);
            dst[2] = u8(g / 2U + 7U);
            if (channels == 4) {
                dst[3] = u8(g / 4U + 100U);
            }
        }
    }
    return pix;
}

// Every pixel is drawn at random from a small palette, which exercises INDEX.
inline Bytes palette_image(std::size_t width, std::size_t height, std::size_t channels,
                           std::uint32_t seed, std::uint32_t palette_size) {
    Rng rng(seed);
    std::vector<std::array<std::uint8_t, 4>> palette(palette_size);
    for (auto& colour : palette) {
        for (std::uint8_t& value : colour) {
            value = rng.byte();
        }
    }
    Bytes pix(width * height * channels);
    for (std::size_t i = 0; i < width * height; ++i) {
        const auto& colour = palette[rng.below(palette_size)];
        for (std::size_t c = 0; c < channels; ++c) {
            pix[i * channels + c] = colour[c];
        }
    }
    return pix;
}

// Long runs of one colour (1..200 pixels, so both short runs and runs past the
// 62 flush limit occur) interleaved with stretches of random pixels.
inline Bytes runs_and_noise_image(std::size_t width, std::size_t height, std::size_t channels,
                                  std::uint32_t seed) {
    Rng rng(seed);
    const std::size_t pixels = width * height;
    Bytes pix(pixels * channels);
    std::size_t i = 0;
    while (i < pixels) {
        std::array<std::uint8_t, 4> colour{};
        for (std::uint8_t& value : colour) {
            value = rng.byte();
        }
        const std::size_t run = 1U + rng.below(200U);
        for (std::size_t k = 0; k < run && i < pixels; ++k, ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                pix[i * channels + c] = colour[c];
            }
        }
        const std::size_t noisy = rng.below(12U);
        for (std::size_t k = 0; k < noisy && i < pixels; ++k, ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                pix[i * channels + c] = rng.byte();
            }
        }
    }
    return pix;
}

// A random walk in colour space: every pixel differs from its predecessor by a
// small per-channel step in [-max_step, +max_step], computed modulo 256, so the
// walk crosses 0 and 255 repeatedly when it starts near an edge. The alpha
// channel (4 channels) changes only occasionally, so DIFF/LUMA stay applicable.
inline Bytes near_equal_image(std::size_t width, std::size_t height, std::size_t channels,
                              std::uint32_t seed, std::uint32_t max_step, std::uint8_t start) {
    Rng rng(seed);
    const std::size_t pixels = width * height;
    Bytes pix(pixels * channels);
    std::array<std::uint8_t, 4> current = {start, u8(start + 1U), u8(start + 2U), 255};
    const std::uint32_t span = 2U * max_step + 1U;
    for (std::size_t i = 0; i < pixels; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const unsigned step = rng.below(span);  // 0 .. 2*max_step
            // (value + step - max_step) mod 256
            current[c] = u8(static_cast<unsigned>(current[c]) + step + 256U - max_step);
        }
        if (channels == 4 && rng.below(40U) == 0U) {
            current[3] = rng.byte();
        }
        for (std::size_t c = 0; c < channels; ++c) {
            pix[i * channels + c] = current[c];
        }
    }
    return pix;
}

}  // namespace qoi_test

#endif  // QOI_TESTS_TEST_UTIL_HPP
