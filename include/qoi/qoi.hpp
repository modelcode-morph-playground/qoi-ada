// QOI (Quite OK Image) codec, C++17 port of the Ada/SPARK package QOI.
//
// Error protocol: no exceptions. encode() and decode() return 0 on any invalid
// argument or too-small output buffer; get_desc() yields the empty descriptor
// on invalid data.
#ifndef QOI_QOI_HPP
#define QOI_QOI_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace qoi {

enum class Colorspace : std::uint8_t {
    SRGB = 0,
    SRGB_Linear_Alpha = 1,
};

struct Desc {
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t channels = 0;
    Colorspace colorspace = Colorspace::SRGB;
};

inline constexpr std::size_t QOI_HEADER_SIZE = 14;
inline constexpr std::array<std::uint8_t, 8> QOI_PADDING = {0, 0, 0, 0, 0, 0, 0, 1};

// Minimal pointer-plus-size view (C++17 has no std::span).
template <typename T>
class Span {
public:
    using element_type = T;

    constexpr Span() noexcept = default;
    constexpr Span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <std::size_t N>
    constexpr Span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}

    template <typename U, std::size_t N,
              typename = std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>>>
    constexpr Span(std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N) {}

    template <typename U, std::size_t N,
              typename = std::enable_if_t<std::is_convertible_v<const U (*)[], T (*)[]>>>
    constexpr Span(const std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N) {}

    template <typename U, typename A,
              typename = std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>>>
    Span(std::vector<U, A>& vec) noexcept : data_(vec.data()), size_(vec.size()) {}

    template <typename U, typename A,
              typename = std::enable_if_t<std::is_convertible_v<const U (*)[], T (*)[]>>>
    Span(const std::vector<U, A>& vec) noexcept : data_(vec.data()), size_(vec.size()) {}

    template <typename U,
              typename = std::enable_if_t<!std::is_same_v<U, T> &&
                                          std::is_convertible_v<U (*)[], T (*)[]>>>
    constexpr Span(Span<U> other) noexcept : data_(other.data()), size_(other.size()) {}

    [[nodiscard]] constexpr T* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr T& operator[](std::size_t i) const noexcept { return data_[i]; }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

// True if desc describes an image that can be encoded: width and height in
// 1..2^31-1, channels 3 or 4, and w*h*(channels+1)+22 does not overflow the
// signed 64-bit size range.
[[nodiscard]] bool valid_size(const Desc& desc) noexcept;

// Worst-case encoded size: w*h*(channels+1) + QOI_HEADER_SIZE + 8. Returns 0
// if !valid_size(desc).
[[nodiscard]] std::uint64_t encode_worst_case(const Desc& desc) noexcept;

// Encode RGB (3 channels) or RGBA (4 channels) pixels. Returns the number of
// bytes written, or 0 if desc is invalid, pix.size() != w*h*channels, or
// out.size() < encode_worst_case(desc).
[[nodiscard]] std::size_t encode(Span<const std::uint8_t> pix, const Desc& desc,
                                 Span<std::uint8_t> out) noexcept;

// Parse the 14-byte header. Sets desc to the empty descriptor {0,0,0,SRGB} if
// data is shorter than 14 bytes, the magic is wrong or the colorspace byte is
// not 0 or 1. Channels, width and height are not range-checked here.
void get_desc(Span<const std::uint8_t> data, Desc& desc) noexcept;

// Decode a QOI stream. Returns width*height*channels, or 0 on failure (data
// shorter than 22 bytes, invalid header or dimensions, or out too small). desc
// receives the result of get_desc even when decoding fails afterwards. The
// trailing padding is not verified.
[[nodiscard]] std::size_t decode(Span<const std::uint8_t> data, Desc& desc,
                                 Span<std::uint8_t> out) noexcept;

}  // namespace qoi

#endif  // QOI_QOI_HPP
