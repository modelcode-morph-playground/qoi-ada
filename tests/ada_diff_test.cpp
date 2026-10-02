// C++ half of the optional Ada differential test (CTest test qoi_ada_diff).
//
// The test compares qoi::encode / qoi::decode with the ORIGINAL Ada package
// (src/qoi.adb). Ada and C++ cannot be linked together, so the two halves talk
// through a directory of files:
//
//   qoi_ada_diff gen <dir>      write test inputs and the C++ results
//   tests/ada/qoi_driver        (Ada) read the inputs, write the Ada results
//   qoi_ada_diff check <dir>    compare the Ada results with the C++ ones
//
// tests/ada/run_ada_diff.cmake orchestrates the three steps and SKIPS the test
// when no Ada toolchain is available. The same executable also offers
//
//   qoi_ada_diff emulate <dir>  act as the Ada driver using qoi:: itself
//   qoi_ada_diff selftest <dir> gen + emulate + check, then check that a
//                               tampered result is detected
//
// which let the file protocol and the comparison logic be tested without Ada
// (CTest test qoi_ada_diff_selftest). `emulate` proves nothing about Ada.
//
// Manifest (<dir>/manifest.txt), one case per line, fields separated by spaces:
//
//   E <id> <width> <height> <channels> <colorspace>
//       <id>.raw      pixels                         (written by gen)
//       <id>.cpp.qoi  qoi::encode of the pixels      (written by gen)
//       <id>.ada.qoi  Ada Encode of the pixels       (written by the driver;
//                     an empty file means Encode reported failure)
//   D <id> [<raw-id>]
//       <id>.in       QOI stream to decode           (written by gen)
//       <id>.ada.res  line 1: accept | reject | skip
//                     line 2: "<width> <height> <channels> <colorspace>"
//       <id>.ada.dec  decoded pixels (accept only)
//       If <raw-id> is given the decoded pixels must equal <raw-id>.raw.
//
// Both sides apply the same rules before calling Decode: a stream shorter than
// 22 bytes is a reject (the Ada precondition forbids calling Decode on it), an
// invalid header is decoded into a 4-byte buffer and must be rejected, and an
// image of more than 2^24 bytes is a skip.
//
// Assertions:
//   * for EVERY input, including the wrap-around ones, the Ada and C++
//     encoders emit identical bytes (the C++ port keeps Ada's unbounded delta
//     arithmetic; only qoi.h differs, see differential_ref_test.cpp);
//   * Ada decodes the C++ encoder's output to the original pixels;
//   * for random, truncated and corrupted streams Ada and C++ agree on
//     accept/reject, header and pixels.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "diff_util.hpp"

namespace fs = std::filesystem;
using difftest::Bytes;

namespace {

constexpr std::size_t kMaxReported = 20;

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
std::string path_of(const std::string& dir, const std::string& name) { return dir + "/" + name; }

bool read_file(const std::string& path, Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& path, const Bytes& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cannot write " << path << "\n";
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool write_text(const std::string& path, const std::string& text) {
    return write_file(path, Bytes(text.begin(), text.end()));
}

std::string id_of(char prefix, std::size_t n, int width) {
    std::string digits = std::to_string(n);
    while (digits.size() < static_cast<std::size_t>(width)) {
        digits.insert(digits.begin(), '0');
    }
    return std::string(1, prefix) + digits;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
struct Entry {
    char kind = 'E';
    std::string id;
    unsigned width = 0;
    unsigned height = 0;
    unsigned channels = 0;
    unsigned colorspace = 0;
    std::string raw_id;  // D entries: expected pixels
};

bool read_manifest(const std::string& dir, std::vector<Entry>& entries) {
    std::ifstream in(path_of(dir, "manifest.txt"));
    if (!in) {
        std::cerr << "cannot read " << path_of(dir, "manifest.txt") << "\n";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream tokens(line);
        Entry e;
        std::string kind;
        if (!(tokens >> kind)) {
            continue;
        }
        e.kind = kind[0];
        if (!(tokens >> e.id)) {
            return false;
        }
        if (e.kind == 'E') {
            if (!(tokens >> e.width >> e.height >> e.channels >> e.colorspace)) {
                return false;
            }
        } else {
            tokens >> e.raw_id;
        }
        entries.push_back(e);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Decode result as exchanged with the Ada driver
// ---------------------------------------------------------------------------
struct DecodeResult {
    std::string status;  // accept | reject | skip
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t channels = 0;
    unsigned colorspace = 0;
    Bytes pixels;
};

// The shared decoding rules, implemented with qoi:: (used for the C++ side and
// by `emulate`).
DecodeResult cpp_decode(const Bytes& stream) {
    DecodeResult r;
    if (stream.size() < qoi::QOI_HEADER_SIZE + qoi::QOI_PADDING.size()) {
        r.status = "reject";
        return r;
    }
    const difftest::Decoded d = difftest::ours_decode(stream);
    r.width = d.width;
    r.height = d.height;
    r.channels = d.channels;
    r.colorspace = d.colorspace;
    r.status = d.skipped ? "skip" : (d.ok ? "accept" : "reject");
    r.pixels = d.pixels;
    return r;
}

bool read_ada_result(const std::string& dir, const std::string& id, DecodeResult& r) {
    Bytes text;
    if (!read_file(path_of(dir, id + ".ada.res"), text)) {
        return false;
    }
    std::istringstream in(std::string(text.begin(), text.end()));
    if (!(in >> r.status >> r.width >> r.height >> r.channels >> r.colorspace)) {
        return false;
    }
    if (r.status == "accept") {
        return read_file(path_of(dir, id + ".ada.dec"), r.pixels);
    }
    return true;
}

// ---------------------------------------------------------------------------
// gen
// ---------------------------------------------------------------------------
class Generator {
public:
    explicit Generator(std::string dir) : dir_(std::move(dir)) {}

    bool run() {
        fs::create_directories(dir_);
        add_crafted();
        add_random_images();
        add_random_streams();
        add_truncations();
        add_corrupted_headers();
        if (!ok_) {
            return false;
        }
        std::cout << "gen: " << encodes_ << " encode cases, " << decodes_ << " decode cases in "
                  << dir_ << "\n";
        return write_text(path_of(dir_, "manifest.txt"), manifest_);
    }

private:
    void add_encode(const std::string& name, const Bytes& pix, unsigned w, unsigned h,
                    unsigned channels, unsigned colorspace) {
        const std::string id = id_of('e', encodes_++, 4);
        const Bytes stream = difftest::ours_encode(
            pix, w, h, channels,
            colorspace == 0 ? qoi::Colorspace::SRGB : qoi::Colorspace::SRGB_Linear_Alpha);
        if (stream.empty()) {
            std::cerr << "qoi::encode failed for " << name << "\n";
            ok_ = false;
            return;
        }
        ok_ = write_file(path_of(dir_, id + ".raw"), pix) &&
              write_file(path_of(dir_, id + ".cpp.qoi"), stream) && ok_;
        manifest_ += "E " + id + " " + std::to_string(w) + " " + std::to_string(h) + " " +
                     std::to_string(channels) + " " + std::to_string(colorspace) + "\n";
        // Ada must decode the C++ stream back to the original pixels.
        const std::string did = id_of('d', decodes_++, 5);
        ok_ = write_file(path_of(dir_, did + ".in"), stream) && ok_;
        manifest_ += "D " + did + " " + id + "\n";
    }

    void add_decode(const Bytes& stream) {
        const std::string id = id_of('d', decodes_++, 5);
        ok_ = write_file(path_of(dir_, id + ".in"), stream) && ok_;
        manifest_ += "D " + id + "\n";
    }

    void add_crafted() {
        for (const difftest::Crafted& c : difftest::crafted_clean_cases()) {
            add_encode(c.name, c.pixels, c.width, c.height, c.channels, 0);
            add_encode(c.name, c.pixels, c.width, c.height, c.channels, 1);
        }
        for (const difftest::Crafted& c : difftest::crafted_wrapping_cases()) {
            add_encode(c.name, c.pixels, c.width, c.height, c.channels, 0);
            add_encode(c.name, c.pixels, c.width, c.height, c.channels, 1);
        }
    }

    void add_random_images() {
        std::mt19937 rng(0xADA0D1FFU);
        for (const difftest::Kind kind : difftest::kAllKinds) {
            for (const unsigned channels : {3U, 4U}) {
                for (const difftest::Size size : difftest::image_sizes()) {
                    const Bytes pix =
                        difftest::make_image(rng, kind, std::size_t{size.w} * size.h, channels);
                    add_encode("random", pix, size.w, size.h, channels,
                               static_cast<unsigned>(rng() & 1U));
                }
            }
        }
    }

    void add_random_streams() {
        std::mt19937 rng(0xADA0DEC1U);
        for (int i = 0; i < 1500; ++i) {
            const std::uint32_t w = 1 + difftest::below(rng, 24);
            const std::uint32_t h = 1 + difftest::below(rng, 24);
            add_decode(difftest::make_random_stream(rng, w, h,
                                                    3 + static_cast<unsigned>(rng() & 1U),
                                                    static_cast<unsigned>(rng() & 1U)));
        }
    }

    void add_truncations() {
        std::mt19937 rng(0xADA0DEC2U);
        for (const difftest::Kind kind : {difftest::Kind::Palette, difftest::Kind::ModularWalk,
                                          difftest::Kind::Runs, difftest::Kind::AlphaNoise}) {
            for (const unsigned channels : {3U, 4U}) {
                const Bytes pix = difftest::make_image(rng, kind, 12 * 9, channels);
                const Bytes stream =
                    difftest::ours_encode(pix, 12, 9, channels, qoi::Colorspace::SRGB);
                for (std::size_t len = 0; len <= stream.size(); ++len) {
                    add_decode(
                        Bytes(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(len)));
                }
            }
        }
    }

    void add_corrupted_headers() {
        std::mt19937 rng(0xADA0DEC3U);
        for (int i = 0; i < 600; ++i) {
            const Bytes pix = difftest::make_image(rng, difftest::Kind::Palette, 6 * 5, 4);
            Bytes stream = difftest::ours_encode(pix, 6, 5, 4, qoi::Colorspace::SRGB);
            const std::size_t edits = 1 + difftest::below(rng, 3);
            for (std::size_t k = 0; k < edits; ++k) {
                const std::size_t pos = difftest::below(rng, 14);
                const bool high_dim_byte = (pos >= 4 && pos <= 6) || (pos >= 8 && pos <= 10);
                stream[pos] = high_dim_byte ? std::uint8_t{0} : difftest::next_byte(rng);
            }
            add_decode(stream);
        }
        // Dimensions beyond any buffer: both sides must skip or reject, never crash.
        for (const std::uint32_t w : {0xFFFFFFFFU, 0x80000000U, 20000U}) {
            Bytes s = difftest::make_header(w, w == 20000U ? 20000U : 0xFFFFFFFFU, 4, 0);
            s.insert(s.end(), {0xFE, 1, 2, 3});
            difftest::append_padding(s);
            add_decode(s);
        }
    }

    std::string dir_;
    std::string manifest_;
    std::size_t encodes_ = 0;
    std::size_t decodes_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// emulate: the Ada driver's job, done with qoi:: (protocol self-test only)
// ---------------------------------------------------------------------------
int emulate(const std::string& dir) {
    std::vector<Entry> entries;
    if (!read_manifest(dir, entries)) {
        return 1;
    }
    for (const Entry& e : entries) {
        if (e.kind == 'E') {
            Bytes pix;
            if (!read_file(path_of(dir, e.id + ".raw"), pix)) {
                return 1;
            }
            const Bytes stream = difftest::ours_encode(
                pix, e.width, e.height, e.channels,
                e.colorspace == 0 ? qoi::Colorspace::SRGB : qoi::Colorspace::SRGB_Linear_Alpha);
            if (!write_file(path_of(dir, e.id + ".ada.qoi"), stream)) {
                return 1;
            }
        } else {
            Bytes in;
            if (!read_file(path_of(dir, e.id + ".in"), in)) {
                return 1;
            }
            const DecodeResult r = cpp_decode(in);
            std::ostringstream res;
            res << r.status << "\n"
                << r.width << " " << r.height << " " << r.channels << " " << r.colorspace << "\n";
            if (!write_text(path_of(dir, e.id + ".ada.res"), res.str())) {
                return 1;
            }
            if (r.status == "accept" && !write_file(path_of(dir, e.id + ".ada.dec"), r.pixels)) {
                return 1;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------
class Checker {
public:
    // stop_at_first: used by the selftest, which only needs to know that a
    // deliberate defect is noticed.
    explicit Checker(bool stop_at_first = false) : stop_at_first_(stop_at_first) {}

    int run(const std::string& dir) {
        std::vector<Entry> entries;
        if (!read_manifest(dir, entries) || entries.empty()) {
            std::cerr << "empty or unreadable manifest\n";
            return 1;
        }
        for (const Entry& e : entries) {
            if (e.kind == 'E') {
                check_encode(dir, e);
            } else {
                check_decode(dir, e);
            }
            if (stop_at_first_ && mismatches_ > 0) {
                break;
            }
        }
        std::cout << "QOI_ADA_DIFF_CHECKED encode=" << encodes_ << " decode=" << decodes_
                  << " (accepted " << accepted_ << ", rejected " << rejected_ << ", skipped "
                  << skipped_ << ") wrap-around inputs=" << wrapping_
                  << " mismatches=" << mismatches_ << std::endl;
        return mismatches_ == 0 ? 0 : 1;
    }

private:
    void mismatch(const std::string& what) {
        if (++mismatches_ <= kMaxReported) {
            std::cerr << "MISMATCH " << what << "\n";
        }
    }

    void check_encode(const std::string& dir, const Entry& e) {
        ++encodes_;
        Bytes pix;
        Bytes cpp;
        Bytes ada;
        if (!read_file(path_of(dir, e.id + ".raw"), pix) ||
            !read_file(path_of(dir, e.id + ".cpp.qoi"), cpp)) {
            mismatch(e.id + ": missing generated files");
            return;
        }
        if (!read_file(path_of(dir, e.id + ".ada.qoi"), ada)) {
            mismatch(e.id + ": the Ada driver wrote no encoder output");
            return;
        }
        if (difftest::has_wraparound_delta(pix, e.channels)) {
            ++wrapping_;
        }
        if (ada != cpp) {
            const std::size_t at = difftest::first_diff(ada, cpp);
            mismatch(e.id + " (" + std::to_string(e.width) + "x" + std::to_string(e.height) + "x" +
                     std::to_string(e.channels) + ", wrap-around=" +
                     (difftest::has_wraparound_delta(pix, e.channels) ? "yes" : "no") +
                     "): encoded bytes differ at offset " + std::to_string(at) + " (ada " +
                     difftest::hex_at(ada, at) + "| c++ " + difftest::hex_at(cpp, at) +
                     "), sizes " + std::to_string(ada.size()) + "/" + std::to_string(cpp.size()));
        }
    }

    void check_decode(const std::string& dir, const Entry& e) {
        ++decodes_;
        Bytes in;
        if (!read_file(path_of(dir, e.id + ".in"), in)) {
            mismatch(e.id + ": missing generated input");
            return;
        }
        DecodeResult ada;
        if (!read_ada_result(dir, e.id, ada)) {
            mismatch(e.id + ": the Ada driver wrote no (or a malformed) decoder result");
            return;
        }
        const DecodeResult cpp = cpp_decode(in);
        if (ada.status == "accept") {
            ++accepted_;
        } else if (ada.status == "reject") {
            ++rejected_;
        } else {
            ++skipped_;
        }

        const std::string where = e.id + " (" + std::to_string(in.size()) + " bytes, head " +
                                  difftest::hex_at(in, 0, 14) + ")";
        if (ada.status != cpp.status) {
            mismatch(where + ": Ada says " + ada.status + ", C++ says " + cpp.status);
            return;
        }
        if (ada.status == "accept") {
            if (ada.width != cpp.width || ada.height != cpp.height ||
                ada.channels != cpp.channels || ada.colorspace != cpp.colorspace) {
                mismatch(where + ": header fields differ");
            }
            if (ada.pixels != cpp.pixels) {
                mismatch(where + ": decoded pixels differ at byte " +
                         std::to_string(difftest::first_diff(ada.pixels, cpp.pixels)));
            }
        }
        if (!e.raw_id.empty()) {
            Bytes raw;
            if (!read_file(path_of(dir, e.raw_id + ".raw"), raw)) {
                mismatch(where + ": missing raw pixels " + e.raw_id);
            } else if (ada.status != "accept" || ada.pixels != raw) {
                mismatch(where + ": Ada did not decode the C++ encoder output (" + e.raw_id +
                         ") to the original pixels");
            }
        }
    }

    bool stop_at_first_;
    std::size_t encodes_ = 0;
    std::size_t decodes_ = 0;
    std::size_t accepted_ = 0;
    std::size_t rejected_ = 0;
    std::size_t skipped_ = 0;
    std::size_t wrapping_ = 0;
    std::size_t mismatches_ = 0;
};

// ---------------------------------------------------------------------------
// selftest
// ---------------------------------------------------------------------------
int selftest(const std::string& dir) {
    fs::remove_all(dir);
    if (!Generator(dir).run()) {
        return 1;
    }
    if (emulate(dir) != 0) {
        std::cerr << "emulate failed\n";
        return 1;
    }
    if (Checker().run(dir) != 0) {
        std::cerr << "selftest: the emulated run must pass\n";
        return 1;
    }

    // The check must notice a corrupted Ada encoder result ...
    Bytes ada;
    if (!read_file(path_of(dir, "e0000.ada.qoi"), ada) || ada.size() < 15) {
        return 1;
    }
    Bytes saved = ada;
    ada[14] = static_cast<std::uint8_t>(ada[14] ^ 0x01U);
    if (!write_file(path_of(dir, "e0000.ada.qoi"), ada)) {
        return 1;
    }
    if (Checker(true).run(dir) == 0) {
        std::cerr << "selftest: a corrupted encoder result went unnoticed\n";
        return 1;
    }
    write_file(path_of(dir, "e0000.ada.qoi"), saved);

    // ... a flipped accept/reject decision ...
    Bytes saved_res;
    if (!read_file(path_of(dir, "d00000.ada.res"), saved_res)) {
        return 1;
    }
    if (!write_text(path_of(dir, "d00000.ada.res"), "reject\n0 0 0 0\n")) {
        return 1;
    }
    if (Checker(true).run(dir) == 0) {
        std::cerr << "selftest: a wrong decoder verdict went unnoticed\n";
        return 1;
    }
    write_file(path_of(dir, "d00000.ada.res"), saved_res);

    // ... a corrupted decoded pixel ...
    Bytes dec;
    if (!read_file(path_of(dir, "d00000.ada.dec"), dec) || dec.empty()) {
        return 1;
    }
    const Bytes saved_dec = dec;
    dec[0] = static_cast<std::uint8_t>(dec[0] ^ 0x80U);
    if (!write_file(path_of(dir, "d00000.ada.dec"), dec)) {
        return 1;
    }
    if (Checker(true).run(dir) == 0) {
        std::cerr << "selftest: a corrupted decoded pixel went unnoticed\n";
        return 1;
    }
    write_file(path_of(dir, "d00000.ada.dec"), saved_dec);

    // ... and a missing driver output.
    Bytes saved_res1;
    if (!read_file(path_of(dir, "d00001.ada.res"), saved_res1)) {
        return 1;
    }
    fs::remove(path_of(dir, "d00001.ada.res"));
    if (Checker(true).run(dir) == 0) {
        std::cerr << "selftest: a missing decoder result went unnoticed\n";
        return 1;
    }
    write_file(path_of(dir, "d00001.ada.res"), saved_res1);
    if (Checker().run(dir) != 0) {
        std::cerr << "selftest: the restored results must pass again\n";
        return 1;
    }
    std::cout << "QOI_ADA_DIFF_SELFTEST_OK" << std::endl;
    return 0;
}

int usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " gen|check|emulate|selftest <directory>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return usage(argv[0]);
    }
    const std::string mode = argv[1];
    const std::string dir = argv[2];
    if (mode == "gen") {
        return Generator(dir).run() ? 0 : 1;
    }
    if (mode == "check") {
        return Checker().run(dir);
    }
    if (mode == "emulate") {
        return emulate(dir);
    }
    if (mode == "selftest") {
        return selftest(dir);
    }
    return usage(argv[0]);
}
