# qoi-ada

A C++17 codec for the "Quite OK Image" (QOI) format, based on the
[QOI](https://qoiformat.org/) specification V1.

The library is a port of the Ada/SPARK implementation of the codec (a source
mirror of [Fabien-Chouteau/qoi-spark](https://github.com/Fabien-Chouteau/qoi-spark),
MIT). The port is byte-exact with respect to the Ada encoder and decoder. The
original Ada sources (`qoi.ads`, `qoi.adb`, `qoi.gpr`, `alire.toml`) are kept
under `legacy/ada/` as the behavioural reference for the C++ code.

## Features

- Encoder and decoder for 3-channel (RGB) and 4-channel (RGBA) images, sRGB and
  sRGB-with-linear-alpha colourspaces.
- Header parsing without decoding (`get_desc`) to size output buffers.
- No dependencies beyond the C++17 standard library, no exceptions, no
  allocation inside the library, no bit-fields and no endianness assumptions.
- Failures are reported through return values only: every function is
  `noexcept`, and invalid arguments are rejected instead of being read or
  written out of bounds.
- Warning-clean (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, `/W4` on
  MSVC) and checked with ASan and UBSan.
- CMake target `qoi::qoi`, usable through `add_subdirectory`, `FetchContent`
  or an installed `find_package(qoi CONFIG REQUIRED)` package.

## Requirements

- A C++17 compiler: GCC >= 11, Clang >= 14 or MSVC 2022 (language extensions
  are switched off).
- CMake >= 3.20. The presets use the Ninja generator.
- GoogleTest 1.17.x for the tests, only when `QOI_BUILD_TESTS` is on. It is
  taken from `find_package(GTest)` when installed, and otherwise downloaded with
  `FetchContent` on first configure (network access needed).
- Optional: GNAT >= 11.2 and GPRbuild (or Alire) to run the Ada differential
  test, clang-format and clang-tidy for development.

## Building and testing

The repository ships three presets in `CMakePresets.json`:

| Preset | Build type | Notes |
|---|---|---|
| `dev` | Debug (`-g -Og`, assertions on) | warnings as errors |
| `release` | Release (`-O3 -DNDEBUG`) | warnings as errors |
| `asan-ubsan` | Debug | AddressSanitizer and UBSan, warnings as errors |

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Replace `dev` with `release` or `asan-ubsan` for the other configurations. Build
trees are created under `build/<preset>`. On Windows run the commands from an
"x64 Native Tools Command Prompt for VS 2022" (or after `vcvars64.bat`) so that
Ninja and `cl` are on the path. To choose a compiler, set `CC` and `CXX` before
configuring.

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `QOI_WARNINGS_AS_ERRORS` | `OFF` (`ON` in all presets) | Treat compiler warnings as errors (`-Werror`, `/WX`). |
| `QOI_SANITIZE` | `OFF` | Build the library and tests with AddressSanitizer and UBSan (`/fsanitize=address` on MSVC). |
| `QOI_BUILD_TESTS` | `ON` for a top-level build, `OFF` as a subproject | Build the test suite. |
| `QOI_INSTALL` | `ON` for a top-level build, `OFF` as a subproject | Generate install and export rules. |
| `QOI_CLANG_TIDY` | `OFF` | Run clang-tidy on the library sources when it is available. |
| `BUILD_SHARED_LIBS` | `OFF` | Build `qoi` as a shared library instead of a static one. |

Without a preset, a top-level build defaults to `Release`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build build
```

## Consuming the library

All of the following expose the target `qoi::qoi`, which carries the public
include directory and the `cxx_std_17` compile feature.

As a subdirectory of your project (tests are not built in this mode):

```cmake
add_subdirectory(external/qoi-ada)
target_link_libraries(app PRIVATE qoi::qoi)
```

With `FetchContent` (replace the repository URL and tag with your own):

```cmake
include(FetchContent)
FetchContent_Declare(qoi
    GIT_REPOSITORY https://example.com/your-org/qoi-ada.git
    GIT_TAG        main)
FetchContent_MakeAvailable(qoi)
target_link_libraries(app PRIVATE qoi::qoi)
```

As an installed package:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /opt/qoi
```

```cmake
find_package(qoi CONFIG REQUIRED)
target_link_libraries(app PRIVATE qoi::qoi)
```

Configure your project with `-DCMAKE_PREFIX_PATH=/opt/qoi` when the prefix is
not a default search location. The installed package is checked in CI by
building `tests/package_consumer/` against it.

## API overview

Everything lives in namespace `qoi`, declared in `<qoi/qoi.hpp>`.

| Ada (`qoi.ads`) | C++17 (`qoi.hpp`) |
|---|---|
| `Storage_Element` | `std::uint8_t` |
| `Storage_Count` | `std::uint64_t` |
| `Storage_Array` | `Span<std::uint8_t>` / `Span<const std::uint8_t>`, implicitly built from `std::vector`, `std::array` or a C array |
| `Colorspace_Kind` (`SRGB`, `SRGB_Linear_Alpha`) | `enum class Colorspace : std::uint8_t` (`SRGB`, `SRGB_Linear_Alpha`) |
| `QOI_Desc` | `struct Desc { std::uint64_t width, height, channels; Colorspace colorspace; }` |
| `QOI_HEADER_SIZE` | `QOI_HEADER_SIZE` (14) |
| `QOI_PADDING` | `QOI_PADDING` (`{0, 0, 0, 0, 0, 0, 0, 1}`) |
| `Valid_Size` | `bool valid_size(const Desc&) noexcept` |
| `Encode_Worst_Case` | `std::uint64_t encode_worst_case(const Desc&) noexcept` |
| `Encode (Pix, Desc, Output, Output_Size)` | `std::size_t encode(Span<const std::uint8_t> pix, const Desc&, Span<std::uint8_t> out) noexcept`, returns `Output_Size` |
| `Get_Desc (Data, Desc)` | `void get_desc(Span<const std::uint8_t> data, Desc&) noexcept` |
| `Decode (Data, Desc, Output, Output_Size)` | `std::size_t decode(Span<const std::uint8_t> data, Desc&, Span<std::uint8_t> out) noexcept`, returns `Output_Size` |

`Span<T>` is a minimal pointer-plus-size view, because C++17 has no `std::span`.

## Usage

```cpp
#include <qoi/qoi.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// Encode tightly packed RGB (channels == 3) or RGBA (channels == 4) pixels.
// Returns an empty vector on invalid arguments.
std::vector<std::uint8_t> encode_image(const std::vector<std::uint8_t>& pixels,
                                       std::uint64_t width, std::uint64_t height,
                                       std::uint64_t channels) {
    const qoi::Desc desc{width, height, channels, qoi::Colorspace::SRGB};
    if (!qoi::valid_size(desc)) {
        return {};
    }

    // The output buffer must hold at least the worst-case encoded size.
    std::vector<std::uint8_t> out(qoi::encode_worst_case(desc));
    const std::size_t size = qoi::encode(pixels, desc, out);  // 0 on failure
    out.resize(size);
    return out;
}

// Decode a QOI stream. Returns false if the data is not a decodable QOI image.
bool decode_image(const std::vector<std::uint8_t>& data, qoi::Desc& desc,
                  std::vector<std::uint8_t>& pixels) {
    // Read the header first: it gives the exact size of the decoded image.
    qoi::get_desc(data, desc);
    if (!qoi::valid_size(desc)) {
        return false;
    }

    pixels.resize(desc.width * desc.height * desc.channels);
    return qoi::decode(data, desc, pixels) != 0;
}

int main() {
    const std::vector<std::uint8_t> rgb = {255, 0, 0, 0, 255, 0};  // 2x1 image
    const std::vector<std::uint8_t> encoded = encode_image(rgb, 2, 1, 3);

    qoi::Desc desc;
    std::vector<std::uint8_t> decoded;
    if (encoded.empty() || !decode_image(encoded, desc, decoded)) {
        return 1;
    }
    return decoded == rgb ? 0 : 1;
}
```

Raw pointers work too: `qoi::Span<const std::uint8_t>(ptr, count)`.

### Buffer sizing

To call `encode` or `decode` you have to provide a large enough output buffer.
If the provided output buffer is not large enough, the function returns zero.
For `encode` the minimum size for the output buffer is given by the
`encode_worst_case` function, based on the dimensions of the image and the
number of channels. For `decode` you should use `get_desc` to get the image
specification, and then the exact output size is
`desc.width * desc.height * desc.channels`.

### Error protocol

- `encode` and `decode` return `0` on failure and otherwise the number of bytes
  written. `encode` fails when `valid_size(desc)` is false, when
  `pix.size() != width * height * channels`, or when the output is smaller than
  `encode_worst_case(desc)`. `decode` fails when the data is shorter than 22
  bytes (header plus padding), when the header or dimensions are invalid, or
  when the output buffer is too small.
- `get_desc` sets the descriptor to the empty descriptor
  `{0, 0, 0, Colorspace::SRGB}` when the data is shorter than 14 bytes, the
  magic is not `qoif` or the colorspace byte is not 0 or 1. It does not range
  check channels, width or height, so call `valid_size` on the result before
  allocating.
- After a failed `decode`, `desc` holds the result of `get_desc` on the input
  (as in the Ada code). The trailing padding is not verified.
- Every function is `noexcept` and nothing throws. The functions that return a
  value are `[[nodiscard]]`.
- Where the Ada code has SPARK preconditions (valid size, pixel count, minimum
  data length), the C++ code checks the condition at run time and returns `0`.

## Testing

The test suite is built with the library (`QOI_BUILD_TESTS`) and driven by
CTest. It contains:

- Golden vectors: hand-built tiny images with expected bytes cross-checked
  against the Ada implementation, covering the header, INDEX, DIFF, LUMA, RUN,
  RGB, RGBA and the padding at their boundaries, plus `valid_size` and
  `encode_worst_case` boundaries.
- Round trips: `decode(encode(x)) == x` on fixed-seed `std::mt19937` images, for
  3 and 4 channels and both colorspaces.
- Contract and robustness tests: too-small outputs, bad magic, truncated data,
  invalid channels, dimensions or colorspace, pixel-count mismatches,
  streams that end early (the previous pixel repeats), and inputs shorter than
  22 bytes.
- Differential tests against the vendored reference `qoi.h` (MIT, in
  `tests/third_party/`).
- An optional differential test against the Ada build, run only when GNAT and
  GPRbuild are found. It is labelled `ada` and reports as skipped otherwise:
  `ctest --preset release -L ada`.
- A decode fuzz target (libFuzzer), built only when the compiler supports it.
- The whole suite under AddressSanitizer and UBSan: `ctest --preset asan-ubsan`.

`tests/TRACEABILITY.md` maps every Ada precondition, postcondition and failure
path to the tests that cover it.

## Known difference from the reference `qoi.h`: wrap-around deltas

The encoder is a statement-by-statement port of the Ada encoder, and it is
Ada-identical by default. The Ada code computes the per-channel differences
between consecutive pixels as unbounded integers and range-tests them, whereas
the reference C encoder stores them in a `signed char`, which wraps modulo 256.
For example, a channel going from 255 to 0 is a delta of -255 for the Ada
encoder (outside every DIFF and LUMA range) but +1 for the reference encoder.

As a result, the encoded bytes can differ from the reference `qoi.h` only for
pixels where a channel delta wraps modulo 256: the reference emits a DIFF or
LUMA chunk where this encoder emits an RGB chunk. Both streams are valid QOI and
decode to identical pixels, and the decoder behaves identically in both cases.
On inputs without wrapping deltas the output is byte-identical to the
reference.

The delta computation lives in a single internal helper, `compute_deltas` in
`src/qoi.cpp`. Making the encoder match the reference `qoi.h` byte for byte
means changing only that helper, and then updating the golden vectors that
exercise wrap-around deltas.

Open question: whether "matches the reference specification" should mean
Ada-identical or `qoi.h`-identical encoder output has not been confirmed yet.
The Ada-identical behaviour is the current default.

## Repository layout

| Path | Contents |
|---|---|
| `include/qoi/qoi.hpp` | Public header. |
| `src/qoi.cpp` | Implementation. |
| `tests/` | GoogleTest suite, traceability table, package consumer, vendored reference `qoi.h`. |
| `cmake/` | Template for the installed `qoi-config.cmake`. |
| `legacy/ada/` | Original Ada/SPARK sources and project files (behavioural reference). |
| `.github/workflows/ci.yml` | CI: GCC, Clang and MSVC in Debug and Release, ASan/UBSan, install check, clang-format, clang-tidy, optional Ada differential test. |

## Credits and licence

- The QOI format is by Dominic Szablewski, see <https://qoiformat.org/> (spec
  V1).
- The Ada implementation this port is derived from is
  [Fabien-Chouteau/qoi-spark](https://github.com/Fabien-Chouteau/qoi-spark)
  (MIT).
- Licensed under the MIT licence, see `LICENSE`. GoogleTest (BSD-3-Clause) and
  the reference `qoi.h` (MIT) are used by the tests only.
