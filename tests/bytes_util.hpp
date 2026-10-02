// Byte-level helpers shared by every test header: the Bytes alias, QOI stream
// builders written from the format specification, hex formatting, the
// first-difference search and the deterministic random source.
//
// This header has no GoogleTest dependency, so tests/diff_util.hpp (also used by
// the non-gtest qoi_ada_diff and fuzz targets) can include it as well as
// tests/test_util.hpp.
//
// Nothing here calls the codec to build an expectation: the header and padding
// builders are written out byte by byte so that golden vectors stay independent
// of the code under test.
#ifndef QOI_TESTS_BYTES_UTIL_HPP
#define QOI_TESTS_BYTES_UTIL_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// Comparison helpers
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

// Index of the first byte at which the two sequences differ, or the length of
// the shorter one if it is a prefix of the other.
inline std::size_t first_mismatch(const std::uint8_t* a, std::size_t a_size, const std::uint8_t* b,
                                  std::size_t b_size) {
    const std::size_t common = std::min(a_size, b_size);
    for (std::size_t i = 0; i < common; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return common;
}

// ---------------------------------------------------------------------------
// Deterministic random source
//
// std::mt19937 output is fully specified by the standard, so sequences are
// identical on every platform. std::uniform_int_distribution is NOT used, its
// results are implementation-defined; bytes and bounded values are derived with
// masks and the modulo operator instead.
// ---------------------------------------------------------------------------

inline std::uint8_t rng_byte(std::mt19937& engine) {
    return static_cast<std::uint8_t>(engine() & 0xFFU);
}

// Value in [0, bound), bound > 0. The modulo bias is irrelevant for test data.
inline std::uint32_t rng_below(std::mt19937& engine, std::uint32_t bound) {
    return static_cast<std::uint32_t>(engine() % bound);
}

class Rng {
public:
    explicit Rng(std::uint32_t seed) : engine_(seed) {}

    std::uint32_t next() { return static_cast<std::uint32_t>(engine_()); }
    std::uint8_t byte() { return rng_byte(engine_); }
    std::uint32_t below(std::uint32_t bound) { return rng_below(engine_, bound); }

private:
    std::mt19937 engine_;
};

}  // namespace qoi_test

#endif  // QOI_TESTS_BYTES_UTIL_HPP
