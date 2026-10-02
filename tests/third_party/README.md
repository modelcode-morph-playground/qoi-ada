# Third-party test code

Everything in this directory is used by the test suite only. None of it is part
of the `qoi` library, none of it is installed, and none of it is linked into
library consumers.

## qoi.h (reference QOI implementation)

| Item       | Value                                                                 |
|------------|-----------------------------------------------------------------------|
| Project    | QOI - The "Quite OK Image" format, https://github.com/phoboslab/qoi    |
| Author     | Dominic Szablewski                                                    |
| File       | `qoi.h` (single-header C implementation), vendored unmodified         |
| Source URL | https://raw.githubusercontent.com/phoboslab/qoi/4ab68bbd20618a663255625160c40875713f5485/qoi.h |
| Commit     | `4ab68bbd20618a663255625160c40875713f5485` (2025-04-28, "Make encoding 1.4% faster by replacing modulo with mask"), the last commit that touched `qoi.h` as of 2026-10-02 (upstream HEAD `ffb2d2cb74a1de60819b21b939f7209aa53e91c1` has an identical `qoi.h`) |
| SHA-256    | `7de6fca1a285b1c20d38f2723dec8b774eb9f144edb9710800a95feeea09375a`    |
| Licence    | MIT (SPDX header kept in `qoi.h`; full text in `qoi.LICENSE`)         |

`qoi.h` must not be reformatted or edited. To update it, replace the file with
a newer upstream revision and update the commit and checksum above.

Used by the differential test `tests/differential_ref_test.cpp`
(executable `qoi_diff_ref`, CTest label `differential`).

## qoi.LICENSE

Verbatim copy of the upstream `LICENSE` file at the pinned commit (MIT,
Copyright (c) 2022 Dominic Szablewski).

## qoi_reference_wrapper.h / qoi_reference_wrapper.c

First-party glue (not third-party code). The wrapper translation unit defines
`QOI_IMPLEMENTATION` and includes `qoi.h` exactly once, and exposes plain
buffer-based functions so that test code never touches `qoi.h` directly and
never has to `free()` memory returned by it. It is built as the static library
`qoi_reference` (see `tests/differential_tests.cmake`), with all compiler
warnings switched off for that one target only, so that the vendored header
does not interact with the project's strict `-Wconversion -Werror` profile. The
wrapper is written in the common subset of C and C++ and is compiled as C when
a C compiler is available.
