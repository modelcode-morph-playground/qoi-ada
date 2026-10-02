/* Thin buffer-based wrapper around the vendored reference qoi.h.
 *
 * Test-only glue (see README.md in this directory). The functions copy results
 * into caller-provided buffers and release everything the reference
 * implementation allocated, so callers never see a pointer from malloc().
 *
 * Written in the common subset of C and C++.
 */
#ifndef QOI_REFERENCE_WRAPPER_H
#define QOI_REFERENCE_WRAPPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors qoi_desc of qoi.h with plain unsigned ints. */
typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int channels;
    unsigned int colorspace;
} qoi_ref_desc;

/* Returned by qoi_ref_decode when the caller's buffer is smaller than the
 * decoded image (a bug in the caller; the reference itself did not fail). */
#define QOI_REF_BUFFER_TOO_SMALL ((size_t)-1)

/* Encode pixels with qoi_encode. Returns the encoded size, or 0 if the
 * reference rejected the arguments, or out_cap is too small for the result. */
size_t qoi_ref_encode(const unsigned char* pixels, const qoi_ref_desc* desc,
                      unsigned char* out, size_t out_cap);

/* Decode with qoi_decode. forced_channels is 0 (use the header's channels), 3
 * or 4. Returns the number of pixel bytes written to out (> 0), 0 if the
 * reference rejected the stream, or QOI_REF_BUFFER_TOO_SMALL. desc receives the
 * header the reference parsed (qoi_decode fills it before validating, so it
 * is meaningful even on rejection); it is zeroed if the reference returned
 * before reading the header. */
size_t qoi_ref_decode(const unsigned char* data, size_t size, int forced_channels,
                      qoi_ref_desc* desc, unsigned char* out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* QOI_REFERENCE_WRAPPER_H */
