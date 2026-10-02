# Traceability: Ada contract to C++ implementation to tests

Every precondition, postcondition and failure path in `src/qoi.ads` and `src/qoi.adb` is listed
here with the place where `src/qoi.cpp` implements it and the tests that exercise it.

Conventions:

- Test names are `Suite.Name` as printed by `ctest`. `Golden.*` live in `golden_test.cpp`,
  `RoundTrip.*` in `roundtrip_test.cpp`, `Contract*.*` and `RobustnessDecode.*` in
  `contract_test.cpp`, `DifferentialRef.*` in `differential_ref_test.cpp`.
- "see differential/fuzz" marks a row that is additionally covered by the differential tests
  (reference `qoi.h`, the Ada driver `qoi_ada_diff`) or by the decode fuzz target
  (`tests/fuzz/decode_fuzz.cpp`, CTest `qoi_fuzz_corpus`). Those files are owned by the
  differential/fuzz work and are only referenced here.
- "Spec decision N" refers to the numbered items of the project spec (`PROJECT.md`); "design
  decision N" refers to the milestone design decisions.
- C++ locations are function names in `src/qoi.cpp` (line numbers drift, so they are not
  listed).
- Ada preconditions about index ranges (`Output'First >= 0`, `Output'Last < Storage_Count'Last`,
  `Data'First >= 0`, `Pix'First >= 1`) have no C++ counterpart: `Span<T>` is zero-based and its
  size is a `std::size_t`, so they hold by construction. They are not tested.

## 1. `Valid_Size` (`qoi.ads`, private part) and `Encode_Worst_Case`

| Ada obligation | C++ implementation | Tests |
|---|---|---|
| `Width in 1 .. Integer_32'Last` | `valid_size`, width checks | `Golden.ValidSizeZeroDimensions`, `Golden.ValidSizeDimensionLimits`, `ContractValidSize.EncodeWorstCaseIsZeroExactlyWhenInvalid`, `ContractEncode.InvalidDescriptorReturnsZero` |
| `Height in 1 .. Integer_32'Last` | `valid_size`, height checks | same as above |
| `Channels in 3 .. 4` | `valid_size`, channel checks | `Golden.ValidSizeChannelBoundaries`, `Golden.ValidSizeVerdictDependsOnChannels`, `ContractEncode.InvalidDescriptorReturnsZero` |
| `Width <= Storage_Count'Last / Height` | `valid_size`, first overflow guard | `Golden.ValidSizeMaximumSquareOverflows`, `Golden.ValidSizeTwoToThe30Square`, `Golden.ValidSizeDimensionLimits` |
| `Channels + 1 <= Storage_Count'Last / (Width * Height)` | `valid_size`, second overflow guard | `Golden.ValidSizeVerdictDependsOnChannels`, `Golden.ValidSizeExactFitAtStorageCountLimit`, `ContractValidSize.EncodeWorstCaseIsZeroExactlyWhenInvalid` |
| `22 <= Storage_Count'Last - Width * Height * (Channels + 1)` | `valid_size`, last return expression | `Golden.ValidSizeExactFitAtStorageCountLimit` (largest passing sizes), `Golden.ValidSizeJustPastLimitFailsOnlyOnHeaderAndPadding` (fails only on this conjunct), `ContractValidSize.EncodeWorstCaseIsZeroExactlyWhenInvalid` |
| `Valid_Size` ignores `Colorspace` | `valid_size` | `ContractValidSize.ColorspaceIsIrrelevant` |
| `Encode_Worst_Case` formula `W*H*(C+1) + 14 + 8` | `encode_worst_case` | `Golden.EncodeWorstCaseSmallImages`, `Golden.EncodedSizeEqualsWorstCaseWhenNothingCompresses`, `RoundTrip.WorstCaseImageStaysWithinBound` |
| Post: `Encode_Worst_Case'Result >= 22` | `encode_worst_case` | `Golden.EncodeWorstCaseZeroForInvalidAndAtLeast22ForValid` |
| Pre of `Encode_Worst_Case`: `Valid_Size (Desc)`; C++ returns 0 when violated (no Ada counterpart, error protocol of the port) | `encode_worst_case`, first statement | `Golden.EncodeWorstCaseZeroForInvalidAndAtLeast22ForValid`, `ContractValidSize.EncodeWorstCaseIsZeroExactlyWhenInvalid` |

## 2. `Encode`

| Ada obligation / path | C++ implementation | Tests |
|---|---|---|
| Pre: `Valid_Size (Desc)`, violated gives 0, output untouched | `encode`, first check | `ContractEncode.InvalidDescriptorReturnsZero`, `ContractEncode.MultipleViolationsAllReturnZero` |
| Pre: `Pix'Length = Width * Height * Channels`, violated gives 0, output untouched | `encode`, second check | `ContractEncode.PixelLengthMustMatchDescriptor`, `ContractEncode.MultipleViolationsAllReturnZero` |
| Pre: `Output'Length >= Encode_Worst_Case (Desc)`. The Ada body (`qoi.adb` line 143) instead returns `Output_Size = 0` for a short buffer | `encode`, third check | `ContractEncode.OutputSmallerThanWorstCaseReturnsZeroAndIsUntouched`, `ContractEncode.WorstCaseMinusOneIsRejectedForLargerImages`, `ContractEncode.HugeValidDescriptorWithSmallBuffersReturnsZero` |
| Post: `Output (First .. First - 1 + Output_Size)'Initialized` (all returned bytes are written) | whole `encode` | `RoundTrip.*` (via `check_round_trip`: every returned byte is compared against the decoded and re-encoded stream, and the sentinel past `Output_Size` is untouched), all `Golden.*` encode checks (`check_golden` compares the returned prefix byte for byte and checks the sentinel after it) |
| Header: magic, big-endian width and height, channels, colorspace byte | `encode`, header block | `Golden.HeaderRgbSrgbFullStream`, `Golden.HeaderRgbaLinearFullStream`, `Golden.HeaderChannelsAndColorspaceBytes`, `Golden.HeaderWidthIsBigEndian258`, `Golden.HeaderHeightIsBigEndian258`, `Golden.HeaderWidthThirdByte65536`, `Golden.HeaderAndPaddingConstants`, `RoundTrip.ColorspaceDoesNotChangeTheChunkStream`, `RoundTrip.TransposedDimensionsKeepTheirOwnDescriptor` |
| Padding `00 00 00 00 00 00 00 01` ends every stream | `encode`, last loop | `Golden.PaddingEndsEveryStream`, `Golden.HeaderAndPaddingConstants`, `RoundTrip.*` |
| First pixel compared with initial previous pixel (0,0,0,255) | `encode`, `px_prev` initialisation | `Golden.FirstPixelEqualsInitialPreviousRgb`, `Golden.FirstPixelEqualsInitialPreviousRgba`, `Golden.FirstPixelZeroRgbaYieldsIndexZero`, `Golden.RgbOpForFirstPixel`, `Golden.RgbaOpForFirstPixelWithAlphaChange` |
| RGB chunk | `encode`, last branch of the delta test | `Golden.RgbOpForFirstPixel`, `Golden.JustOutsideLumaFallsThroughToRgb`, `Golden.ThreeChannelImageNeverEmitsRgba` |
| RGBA chunk (alpha differs) | `encode`, `px.a != px_prev.a` branch | `Golden.RgbaOpForFirstPixelWithAlphaChange`, `Golden.AlphaChangeEmitsRgbaAndIndexStillWins`, `Golden.SingleAlphaStepEmitsRgba`, `Golden.ZeroPixelThenOpaqueBlackEmitsIndexZeroThenRgba`, `RoundTrip.AlphaVariations`, `RoundTrip.BoundaryValueRgbaPairs` |
| INDEX chunk, checked before the alpha test, index not updated on a hit | `encode`, `seen[index_pos] == px` | `Golden.IndexHitAfterRgbAndLuma`, `Golden.IndexTakesPrecedenceOverDiffAndDiffPixelsAreStored`, `Golden.IndexHitAfterRgba`, `Golden.AlphaChangeEmitsRgbaAndIndexStillWins`, `Golden.RunAfterIndexHit`, `RoundTrip.SmallPalettesExerciseIndex` |
| INDEX slot overwritten by a colliding pixel | `encode`, `seen[index_pos] = px` | `Golden.IndexSlotOverwrittenByCollidingPixel`, `Golden.IndexHitOnOverwritingPixel` |
| DIFF chunk, boundaries -2 and 1 per channel | `encode`, first `in_range` test | `Golden.DiffBoundariesPerChannel`, `Golden.DiffAndLumaWithFourChannelsAndConstantAlpha`, `Golden.JustOutsideDiffFallsThroughToLuma`, `Golden.NoWrapAroundControlStepsAreDiff` |
| LUMA chunk, boundaries dg -32 and 31, dr-dg and db-dg -8 and 7 | `encode`, second `in_range` test | `Golden.LumaBoundaries`, `Golden.DiffAndLumaWithFourChannelsAndConstantAlpha`, `Golden.JustOutsideLumaFallsThroughToRgb`, `Golden.IndexHitAfterRgbAndLuma` |
| RUN chunk, lengths 1, 2, 61, 62, 63, 124, 125 | `encode`, `px == px_prev` branch, `push_run` | `Golden.RunLength1`, `Golden.RunLength2`, `Golden.RunLength61`, `Golden.RunLength62`, `Golden.RunLength63`, `Golden.RunLength124`, `Golden.RunLength125`, `Golden.RunLengthsFourChannels`, `RoundTrip.EveryRunLengthUpTo400` |
| Run flushed at 62 (cannot be 63 or 64, those are RGB/RGBA bytes) | `encode`, `run == QOI_MAX_RUN` | `Golden.RunFlushAt62ThenRemainderRunThenNewPixel`, `Golden.RunLength62`, `Golden.RunLength63` |
| Run flushed before a different pixel | `encode`, `run > 0` in the else branch | `Golden.RunFlushedBeforeNewPixel`, `Golden.RunFlushedThenDiff`, `Golden.RunAfterCodedPixel`, `RoundTrip.RunsBetweenDifferentPixels`, `RoundTrip.RunsInterleavedWithNoise` |
| Run flushed at the last pixel | `encode`, `px_index == number_of_pixels - 1` | `Golden.RunEndingAtFinalPixel2D`, `Golden.ZeroRgbaPixelsIndexThenRun`, `Golden.RunLength1` |
| Wrap-around deltas encode as RGB, not as `qoi.h`'s DIFF/LUMA (spec decision 6, Ada-identical encoder) | `compute_deltas` (single helper) | `Golden.WrapAroundDeltaRed255To0EmitsRgbLikeAda`, `Golden.WrapAroundDeltaRed0To255EmitsRgbLikeAda`, `Golden.WrapAroundDeltaRed254To1EmitsRgbLikeAda`, `Golden.WrapAroundDeltaGreen255To0EmitsRgbLikeAda`, `Golden.WrapAroundDeltaBlue0To255EmitsRgbLikeAda`, `Golden.WrapAroundDeltaAllChannels255To0EmitsRgbLikeAda`, `Golden.WrapAroundDeltaAllChannels0To255EmitsRgbLikeAda`, `Golden.WrapAroundDeltaRedBlue250To1EmitsRgbLikeAda`, `Golden.WrapAroundDeltaFourChannelsEmitsRgbLikeAda`, control `Golden.NoWrapAroundControlStepsAreDiff`. See differential/fuzz: `DifferentialRef.WrapAroundCraftedCasesDecodeIdenticallyButEncodeDifferently`, `DifferentialRef.WrapAroundFirstPixelWhiteExactBytes`, `qoi_ada_diff` |
| Worst case size is reached and never exceeded | `encode`, whole | `Golden.EncodedSizeEqualsWorstCaseWhenNothingCompresses`, `RoundTrip.WorstCaseImageStaysWithinBound`, `RoundTrip.NoiseAllShapesChannelsColorspaces` |
| Determinism, no state kept between calls | `encode` has no static state | `RoundTrip.*` (`check_round_trip` encodes twice and compares) |
| Byte-for-byte equality with the original Ada | whole `encode` | see differential/fuzz: `qoi_ada_diff` (when GNAT is available), `DifferentialRef.CraftedCleanCasesAreByteIdentical`, `DifferentialRef.RandomImagesFixedSeed` |

## 3. `Get_Desc`

| Ada obligation / path | C++ implementation | Tests |
|---|---|---|
| Pre (index ranges of `Data`) | holds by construction, see conventions | not applicable |
| `Data'Length < 14` gives the empty descriptor (`qoi.adb` line 308) | `get_desc`, size check | `ContractGetDesc.ShortDataGivesEmptyDescriptor` (every length 0 to 13 on exact-size heap copies, plus a null span) |
| Bad magic gives the empty descriptor (line 315) | `get_desc`, magic check | `ContractGetDesc.BadMagicGivesEmptyDescriptor`, `ContractDecode.BadMagicOrColorspaceReturnsZeroWithEmptyDescriptor` |
| Colorspace byte not 0 or 1 gives the empty descriptor (line 328) | `get_desc`, colorspace check | `ContractGetDesc.ColorspaceByteMustBeZeroOrOne` (all 256 values), `ContractDecode.BadMagicOrColorspaceReturnsZeroWithEmptyDescriptor` |
| Valid header: all fields parsed (big-endian), exactly 14 bytes needed, trailing bytes ignored | `get_desc`, `pop32` | `ContractGetDesc.ParsesAllHeaderFields`, `ContractGetDesc.HeaderOnlyAndTrailingBytes`, the `Golden.Header*` tests |
| Channels and dimensions are not range-checked by `Get_Desc` | `get_desc` (no checks) | `ContractGetDesc.ChannelsAndDimensionsAreNotRangeChecked` |
| Output descriptor is fully overwritten (no stale field survives) | `get_desc`, every path assigns all fields | `ContractGetDesc.OverwritesEveryFieldOfTheOutput` |
| Used to size the output of `Decode` | `get_desc` | `RoundTrip.*` (`get_desc` of an encoded stream equals the input descriptor), `Golden.*` (`check_golden`) |

## 4. `Decode`

| Ada obligation / path | C++ implementation | Tests |
|---|---|---|
| Pre: `Data'Length >= 22`; violated gives 0 (the Ada contract makes this the caller's duty, the port returns 0) | `decode`, size check after `get_desc` | `ContractDecode.DataShorterThan22BytesReturnsZero`, `ContractDecode.TwentyTwoByteStreamDecodesToInitialPixel` (the exact boundary), `RobustnessDecode.EveryTruncationOfValidStreams` |
| `Get_Desc` is called first; its result stays in `Desc` on every later failure (design decision 4) | `decode`, first statement | `ContractDecode.BadMagicOrColorspaceReturnsZeroWithEmptyDescriptor`, `ContractDecode.InvalidChannelsOrDimensionsKeepParsedDescriptor`, `ContractDecode.InputDescriptorIsIgnored`, `ContractDecode.DataShorterThan22BytesReturnsZero` |
| Failure: `Desc.Width = 0` or `Desc.Height = 0` (line 391 region) | `decode`, combined check | `ContractDecode.InvalidChannelsOrDimensionsKeepParsedDescriptor` |
| Failure: `Desc.Channels not in 3 .. 4` | `decode`, combined check | `ContractDecode.InvalidChannelsOrDimensionsKeepParsedDescriptor` |
| Failure: `Height > Storage_Count'Last / Width` and `Channels > Storage_Count'Last / (Width * Height)` (overflow guards, no allocation) | `decode`, combined check | `ContractDecode.HugeDimensionsReturnZeroWithoutAllocating`, `RobustnessDecode.LargeImageFromHeaderOnlyStream`. See differential/fuzz: `DifferentialRef.DecoderRejectsHugeDimensionsLikeReference` |
| Failure: `Output'Length < W * H * C`, output untouched | `decode`, combined check | `ContractDecode.OutputBufferSizeBoundary` (one byte short fails, exact and larger succeed, surplus untouched), `ContractDecode.HugeDimensionsReturnZeroWithoutAllocating` |
| Post: `Output_Size = W * H * C` (not the stream size) | `decode`, return value | `ContractDecode.ReturnsImageSizeNotStreamSize`, `ContractDecode.OutputBufferSizeBoundary`, `RoundTrip.*` |
| Post: `Height <= Last / Width` and `Channels <= Last / (Width * Height)` when `Output_Size /= 0` | implied by the combined check | `ContractDecode.HugeDimensionsReturnZeroWithoutAllocating` (the same inputs return 0), `RoundTrip.LargeImages` |
| Post: `Output (First .. First - 1 + Output_Size)'Initialized` | `decode`, loop writes every pixel | `RoundTrip.*` (decode into an exact buffer with guard bytes, compare every byte), `ContractDecode.PixelsBeyondLastChunkRepeatPreviousPixel` |
| Failed decode leaves the output unmodified | `decode`, early returns | `expect_decode_fails` in every failure test of `ContractDecode.*` and `RobustnessDecode.*` |
| Chunks read only while `P <= Last_Chunk` (`Data'Last - 8`) | `decode`, `p <= last_chunk` | `ContractDecode.ChunkAtLastChunkOffsetIsReadNextOneIsNot`, `ContractDecode.ChunkStartingAtLastChunkOffsetMayConsumePadding`, `ContractDecode.RegularStreamsNeverNeedThePaddingAsOperands` |
| Stream ends before all pixels: previous pixel repeats (repeat-last-pixel) | `decode`, final `else` comment | `ContractDecode.PixelsBeyondLastChunkRepeatPreviousPixel`, `ContractDecode.RunThenChunkThenRepeatAfterLastChunk`, `ContractDecode.TwentyTwoByteStreamDecodesToInitialPixel`, `RobustnessDecode.EveryTruncationOfValidStreams` |
| Padding content is never interpreted for encoder-made streams | `decode` | `RobustnessDecode.StreamWithFullPaddingMatchesAfterPaddingContentChange`, `RobustnessDecode.TrailingGarbageAfterValidStream` |
| RGB chunk | `decode`, `QOI_OP_RGB` | `ContractDecode.RgbDiffLumaKeepAlpha`, `Golden.RgbOpForFirstPixel` (round trip of the encoder bytes) |
| RGBA chunk, also in a 3-channel stream (alpha state and hash still updated) | `decode`, `QOI_OP_RGBA` | `ContractDecode.RgbaChunkInThreeChannelStreamStillUpdatesAlphaState`, `Golden.RgbaOpForFirstPixelWithAlphaChange` |
| INDEX chunk, empty slot yields (0,0,0,0), hit restores an earlier pixel | `decode`, `QOI_OP_INDEX` | `ContractDecode.IndexOfEmptySlotYieldsTransparentBlack`, `ContractDecode.IndexHitRestoresEarlierPixel`, `Golden.IndexHitAfterRgbAndLuma` |
| DIFF chunk, modulo 256 arithmetic, alpha kept | `decode`, `QOI_OP_DIFF` | `ContractDecode.DiffWrapsModulo256`, `ContractDecode.RgbDiffLumaKeepAlpha` |
| LUMA chunk, modulo 256 arithmetic, alpha kept | `decode`, `QOI_OP_LUMA` | `ContractDecode.LumaWrapsModulo256`, `ContractDecode.RgbDiffLumaKeepAlpha` |
| RUN chunk, field k gives k+1 pixels, truncated at the image end | `decode`, `QOI_OP_RUN`, `run` counter | `ContractDecode.RunChunkLengths`, `ContractDecode.RunLongerThanImageIsTruncated`, `ContractDecode.RunThenChunkThenRepeatAfterLastChunk` |
| Index refreshed after every chunk including RUN and INDEX, not for repeated pixels | `decode`, `seen[hash(px)] = px` | `ContractDecode.IndexIsRefreshedAfterRunChunk`, `ContractDecode.IndexOfEmptySlotYieldsTransparentBlack` |
| Decoder of encoder output restores the input | whole `decode` | `RoundTrip.*` (all 17 tests), see differential/fuzz: `DifferentialRef.RandomImagesFixedSeed`, `qoi_ada_diff` |
| Decoder agrees with the reference and the Ada on arbitrary chunk bytes | whole `decode` | see differential/fuzz: `DifferentialRef.DecoderRandomChunkStreams`, `DifferentialRef.DecoderTruncatedEncoderOutput`, `DifferentialRef.DecoderCorruptedHeaders`, `DifferentialRef.DecoderRejectsInvalidHeadersLikeReference`, `qoi_ada_diff` |

## 5. Design decisions and cross-cutting items

| Item | C++ implementation | Tests |
|---|---|---|
| Design decision 4: `Desc` after a failed `decode` is the `get_desc` result (not reset) | `decode`, first statement | `ContractDecode.InvalidChannelsOrDimensionsKeepParsedDescriptor`, `ContractDecode.BadMagicOrColorspaceReturnsZeroWithEmptyDescriptor`, `ContractDecode.DataShorterThan22BytesReturnsZero`, `ContractDecode.InputDescriptorIsIgnored` |
| Spec decision 5: decode never crashes, hangs or reads out of bounds on arbitrary input; inputs shorter than 22 bytes never read out of bounds | `decode`, `get_desc` (all reads guarded by the size checks and `last_chunk`) | `RobustnessDecode.EveryTruncationOfValidStreams`, `RobustnessDecode.RandomChunkBytesBehindValidHeader`, `RobustnessDecode.FullyRandomInput`, `RobustnessDecode.EverySingleBitFlipInChunkArea`, `RobustnessDecode.EveryOpcodeByteAsFirstChunk`, `RobustnessDecode.TrailingGarbageAfterValidStream`, `ContractGetDesc.ShortDataGivesEmptyDescriptor`. Every input is passed as an exact-size heap copy (`qoi_test::ExactBytes`) so an overread is an AddressSanitizer error. The whole suite runs under ASan and UBSan in the `asan-ubsan` CI job (`.github/workflows/ci.yml`). See differential/fuzz: `qoi_fuzz_corpus` (and the libFuzzer target `qoi_decode_fuzz` where the toolchain supports it) |
| Spec decision 5: UBSan integer checks on the modular arithmetic casts | `decode` (explicit `static_cast<std::uint8_t>`), `hash`, `compute_deltas` | `ContractDecode.DiffWrapsModulo256`, `ContractDecode.LumaWrapsModulo256`, `RoundTrip.BoundaryValuePixelPairs`, `RoundTrip.EveryTwoPixelValuePairPerChannel`, `RoundTrip.NearEqualWalksAcrossByteBoundary` (run in the `asan-ubsan` CI job, which adds `-fsanitize=implicit-conversion` on clang) |
| Spec decision 6 (milestone open point): encoder deltas are Ada-identical, differing from `qoi.h` only on wrap-around deltas | `compute_deltas`, the single helper | `Golden.WrapAround*` (comment in `golden_test.cpp` states which vectors to change if the stakeholder chooses `qoi.h`-identical output), see differential/fuzz: `DifferentialRef.WrapAroundCraftedCasesDecodeIdenticallyButEncodeDifferently` |
| Decoding is unaffected by decision 6 (wraps modulo 256 in both implementations) | `decode` | `ContractDecode.DiffWrapsModulo256`, `ContractDecode.LumaWrapsModulo256`, `DifferentialRef.WrapAroundCraftedCasesDecodeIdenticallyButEncodeDifferently` |
| Span helper used by the whole API | `include/qoi/qoi.hpp` | `ContractSpan.ConstructsFromVectorArrayAndCArray` |

## Not covered by these tests

- Sanitizers do not run in the local MinGW build, only in the Linux `asan-ubsan` CI job. The
  local run still checks the same assertions, but an overread would not be detected locally.
- Valid descriptors near the `Storage_Count` limit (`Golden.ValidSizeExactFitAtStorageCountLimit`,
  `ContractEncode.HugeValidDescriptorWithSmallBuffersReturnsZero`) are exercised through
  descriptors and tiny buffers only. A multi-gigabyte image is never allocated.
- The Ada range preconditions on `Output'First`, `Output'Last`, `Data'First`, `Data'Last` and
  `Pix'First` have no C++ counterpart (see conventions).
