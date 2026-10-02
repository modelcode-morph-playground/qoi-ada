/* Single translation unit that instantiates the vendored reference qoi.h.
 * Compiled with all warnings disabled (see tests/differential_tests.cmake). */
#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "qoi_reference_wrapper.h"

size_t qoi_ref_encode(const unsigned char* pixels, const qoi_ref_desc* desc,
                      unsigned char* out, size_t out_cap) {
    qoi_desc d;
    int len = 0;
    void* enc;
    size_t result;

    d.width = desc->width;
    d.height = desc->height;
    d.channels = (unsigned char)desc->channels;
    d.colorspace = (unsigned char)desc->colorspace;
    if (desc->channels > 255u || desc->colorspace > 255u) {
        return 0;
    }

    enc = qoi_encode(pixels, &d, &len);
    if (enc == NULL || len <= 0) {
        return 0;
    }
    result = (size_t)len;
    if (result > out_cap) {
        free(enc);
        return 0;
    }
    memcpy(out, enc, result);
    free(enc);
    return result;
}

size_t qoi_ref_decode(const unsigned char* data, size_t size, int forced_channels,
                      qoi_ref_desc* desc, unsigned char* out, size_t out_cap) {
    qoi_desc d;
    void* dec;
    size_t channels;
    size_t n;

    memset(&d, 0, sizeof d);
    desc->width = desc->height = desc->channels = desc->colorspace = 0;

    if (size > (size_t)INT_MAX) {
        return 0;
    }

    dec = qoi_decode(data, (int)size, &d, forced_channels);

    /* qoi_decode writes d only after its size check passed; the zeroed d is
     * what we report when it returned earlier. */
    desc->width = d.width;
    desc->height = d.height;
    desc->channels = d.channels;
    desc->colorspace = d.colorspace;

    if (dec == NULL) {
        return 0;
    }
    channels = forced_channels != 0 ? (size_t)forced_channels : (size_t)d.channels;
    n = (size_t)d.width * (size_t)d.height * channels;
    if (n > out_cap) {
        free(dec);
        return QOI_REF_BUFFER_TOO_SMALL;
    }
    memcpy(out, dec, n);
    free(dec);
    return n;
}
