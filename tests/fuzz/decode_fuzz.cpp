// Decode fuzz target for qoi::decode (and qoi::get_desc).
//
// Built twice from this one file (see tests/differential_tests.cmake):
//   * qoi_decode_fuzz  - libFuzzer target, only when the toolchain supports
//                        -fsanitize=fuzzer (clang, option QOI_BUILD_FUZZ);
//   * qoi_fuzz_replay  - always built; replay_main.cpp feeds it the files of
//                        the checked-in seed corpus (CTest test qoi_fuzz_corpus).
//
// Properties checked for every input (a violation aborts, which both libFuzzer
// and CTest report as a failure):
//   1. get_desc and decode never read or write out of bounds (no crash, and
//      AddressSanitizer / UBSan clean under QOI_SANITIZE);
//   2. decode into a buffer one byte too small returns 0 and nothing else;
//   3. decode into an exactly sized buffer returns width*height*channels
//      exactly when the header is acceptable, and 0 otherwise;
//   4. a successfully decoded image re-encodes, and decoding that encoding
//      gives back the same pixels (encode/decode round trip).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "qoi/qoi.hpp"

namespace {

// Largest decoded image the target allocates; bigger headers are only used to
// exercise the "output buffer too small" path.
constexpr std::uint64_t kMaxPixelBytes = 1ULL << 24;

[[noreturn]] void fail() { std::abort(); }

void check(bool condition) {
    if (!condition) {
        fail();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // std::vector::data() of an empty vector may be null, and so may `data`.
    const std::vector<std::uint8_t> input(data, data + size);

    qoi::Desc desc;
    qoi::get_desc(input, desc);

    const bool header_ok =
        desc.width != 0 && desc.height != 0 && desc.channels >= 3 && desc.channels <= 4;

    // Decoding into no buffer at all must fail without touching memory.
    {
        qoi::Desc d;
        std::vector<std::uint8_t> none;
        check(qoi::decode(input, d, none) == 0);
    }

    if (!header_ok || desc.width > kMaxPixelBytes / desc.height / desc.channels) {
        // Invalid header, or an image far larger than this 64-byte buffer:
        // decode must fail without touching the output.
        std::vector<std::uint8_t> small(64, 0xA5);
        qoi::Desc d;
        check(qoi::decode(input, d, small) == 0);
        for (const std::uint8_t b : small) {
            check(b == 0xA5);
        }
        return 0;
    }

    const std::size_t need = static_cast<std::size_t>(desc.width * desc.height * desc.channels);

    // One byte too small: must fail.
    {
        std::vector<std::uint8_t> small(need - 1);
        qoi::Desc d;
        check(qoi::decode(input, d, small) == 0);
    }

    std::vector<std::uint8_t> pixels(need);
    qoi::Desc decoded_desc;
    const std::size_t n = qoi::decode(input, decoded_desc, pixels);
    if (size < qoi::QOI_HEADER_SIZE + qoi::QOI_PADDING.size()) {
        check(n == 0);
        return 0;
    }
    check(n == need);
    check(decoded_desc.width == desc.width && decoded_desc.height == desc.height &&
          decoded_desc.channels == desc.channels && decoded_desc.colorspace == desc.colorspace);

    // Round trip.
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(qoi::encode_worst_case(desc)));
    check(!encoded.empty());
    const std::size_t m = qoi::encode(pixels, desc, encoded);
    check(m != 0 && m <= encoded.size());
    encoded.resize(m);

    std::vector<std::uint8_t> again(need);
    qoi::Desc again_desc;
    check(qoi::decode(encoded, again_desc, again) == need);
    check(again == pixels);
    return 0;
}
