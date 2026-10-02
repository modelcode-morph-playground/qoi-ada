// Standalone driver for LLVMFuzzerTestOneInput: runs the fuzz target once for
// every file named on the command line, or found (non-recursively, in sorted
// order) in every directory named. Used by the CTest test qoi_fuzz_corpus to
// replay the checked-in seed corpus with any compiler, with or without libFuzzer.
//
// Exit status: 0 if every input was processed (a property violation aborts the
// process), 1 on usage errors, unreadable files or an empty corpus.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace fs = std::filesystem;

namespace {

bool run_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path.string() << "\n";
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    std::cout << "replay " << path.filename().string() << " (" << bytes.size() << " bytes)"
              << std::endl;
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <file-or-directory>...\n";
        return 1;
    }

    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        const fs::path arg(argv[i]);
        std::error_code ec;
        if (fs::is_directory(arg, ec)) {
            std::vector<fs::path> found;
            for (const fs::directory_entry& entry : fs::directory_iterator(arg, ec)) {
                if (entry.is_regular_file()) {
                    found.push_back(entry.path());
                }
            }
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else if (fs::is_regular_file(arg, ec)) {
            files.push_back(arg);
        } else {
            std::cerr << "not a file or directory: " << arg.string() << "\n";
            return 1;
        }
    }

    if (files.empty()) {
        std::cerr << "no input files found\n";
        return 1;
    }
    for (const fs::path& file : files) {
        if (!run_file(file)) {
            return 1;
        }
    }
    std::cout << "QOI_FUZZ_REPLAY_OK " << files.size() << " inputs" << std::endl;
    return 0;
}
