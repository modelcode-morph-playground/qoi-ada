// Minimal consumer of the installed qoi package: encodes one RGB pixel and
// checks the result. Returns 0 on success, nonzero on any failure.
#include <qoi/qoi.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    const qoi::Desc desc{1, 1, 3, qoi::Colorspace::SRGB};
    const std::vector<std::uint8_t> pixel = {10, 20, 30};

    std::vector<std::uint8_t> out(qoi::encode_worst_case(desc));
    const std::size_t size = qoi::encode(pixel, desc, out);

    // 14-byte header + one RGB chunk (4 bytes) + 8-byte padding.
    if (size != qoi::QOI_HEADER_SIZE + 4 + qoi::QOI_PADDING.size()) {
        std::fprintf(stderr, "unexpected encoded size %zu\n", size);
        return 1;
    }
    if (out[0] != 'q' || out[1] != 'o' || out[2] != 'i' || out[3] != 'f') {
        std::fprintf(stderr, "bad magic\n");
        return 2;
    }

    qoi::Desc decoded_desc;
    std::vector<std::uint8_t> decoded(3);
    const std::size_t decoded_size =
        qoi::decode(qoi::Span<const std::uint8_t>(out.data(), size), decoded_desc, decoded);
    if (decoded_size != 3 || decoded != pixel || decoded_desc.width != 1 ||
        decoded_desc.height != 1 || decoded_desc.channels != 3) {
        std::fprintf(stderr, "round trip failed\n");
        return 3;
    }
    return 0;
}
