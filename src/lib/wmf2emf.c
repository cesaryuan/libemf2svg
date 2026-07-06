#ifdef __cplusplus
extern "C" {
#endif

#ifndef DARWIN
#define _POSIX_C_SOURCE 200809L
#endif

#include "emf2svg.h"
#include "internal-fmem.h"
#include "uemf.h"
#include "uwmf.h"
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WMF2EMF_INITIAL_SIZE 65536U
#define WMF2EMF_CHUNK_SIZE 32768U
#define WMF2EMF_HANDLE_LIMIT 65536U

typedef struct {
    uint32_t values[WMF2EMF_HANDLE_LIMIT];
    uint8_t used[WMF2EMF_HANDLE_LIMIT];
} wmf2emfHandleMap;

typedef struct {
    bool verbose;
    unsigned int unsupported;
} wmf2emfContext;

typedef struct {
    EMFTRACK track;
    EMFHANDLES *handles;
    fmem memory;
} wmf2emfOutput;

typedef struct {
    bool has_window_ext;
    bool has_viewport_ext;
    bool has_viewport_org;
    bool has_default_viewport_ext;
    U_POINT16 window_ext;
    U_SIZEL default_viewport_ext;
} wmf2emfMetrics;

/* Log converter diagnostics only when verbose output is requested. */
static void wmf2emf_log(wmf2emfContext *ctx, const char *fmt, ...) {
    va_list args;

    if (ctx == NULL || !ctx->verbose) {
        return;
    }
    fprintf(stderr, "wmf2emf: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* Return an options struct even when the caller passes NULL. */
static wmf2emfOptions wmf2emf_options_or_default(wmf2emfOptions *options) {
    wmf2emfOptions defaults;

    defaults.verbose = false;
    if (options != NULL) {
        defaults = *options;
    }
    return defaults;
}

/* Convert a WMF point to an EMF point without changing the logical units. */
static U_POINTL wmf2emf_point(U_POINT16 point) {
    return pointl_set(point.x, point.y);
}

/* Convert a WMF extent to an EMF extent without changing the logical units. */
static U_SIZEL wmf2emf_size(U_POINT16 point) {
    return sizel_set(point.x, point.y);
}

/* Convert a WMF rectangle to an EMF rectangle without changing the logical units. */
static U_RECTL wmf2emf_rect(U_RECT16 rect) {
    return (U_RECTL){rect.left, rect.top, rect.right, rect.bottom};
}

/* Build a normalized rectangle from a WMF destination point and extent. */
static U_RECTL wmf2emf_rect_from_dest_size(U_POINT16 dest, U_POINT16 size) {
    int32_t left = dest.x;
    int32_t top = dest.y;
    int32_t right = (int32_t)dest.x + (int32_t)size.x;
    int32_t bottom = (int32_t)dest.y + (int32_t)size.y;
    U_RECTL rect;

    rect.left = left < right ? left : right;
    rect.right = left < right ? right : left;
    rect.top = top < bottom ? top : bottom;
    rect.bottom = top < bottom ? bottom : top;
    return rect;
}

/* Return a positive logical span while preserving zero as an invalid size. */
static int32_t wmf2emf_abs_span(int32_t first, int32_t second) {
    int64_t span = (int64_t)second - (int64_t)first;

    if (span < 0) {
        span = -span;
    }
    if (span > INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)span;
}

/* Prefer a real span, but keep malformed or missing dimensions drawable. */
static int32_t wmf2emf_span_or_default(int32_t span, int32_t fallback) {
    return span > 0 ? span : fallback;
}

/* Convert an extent to a positive viewport extent for default WMF mapping. */
static U_SIZEL wmf2emf_positive_size(U_POINT16 point) {
    return sizel_set(wmf2emf_span_or_default(wmf2emf_abs_span(0, point.x), 1),
                     wmf2emf_span_or_default(wmf2emf_abs_span(0, point.y), 1));
}

/* Derive the default viewport from placeable bounds when they exist. */
static void wmf2emf_set_default_viewport(wmf2emfMetrics *metrics,
                                         const U_WMRPLACEABLE *placeable) {
    int32_t width;
    int32_t height;

    if (metrics == NULL) {
        return;
    }
    if (placeable != NULL && placeable->Key == 0x9AC6CDD7) {
        width = wmf2emf_span_or_default(
            wmf2emf_abs_span(placeable->Dst.left, placeable->Dst.right), 1000);
        height = wmf2emf_span_or_default(
            wmf2emf_abs_span(placeable->Dst.top, placeable->Dst.bottom), 1000);
        metrics->default_viewport_ext = sizel_set(width, height);
        metrics->has_default_viewport_ext = true;
    } else if (metrics->has_window_ext) {
        metrics->default_viewport_ext = wmf2emf_positive_size(metrics->window_ext);
        metrics->has_default_viewport_ext = true;
    }
}

/* Build a conservative EMF bounds rectangle for point-based records. */
static U_RECTL wmf2emf_bounds_from_points(const U_POINT16 *points,
                                          uint32_t count) {
    U_RECTL bounds;
    uint32_t i;

    if (points == NULL || count == 0) {
        return (U_RECTL){0, 0, 0, 0};
    }
    bounds.left = points[0].x;
    bounds.right = points[0].x;
    bounds.top = points[0].y;
    bounds.bottom = points[0].y;
    for (i = 1; i < count; i++) {
        if (points[i].x < bounds.left) {
            bounds.left = points[i].x;
        }
        if (points[i].x > bounds.right) {
            bounds.right = points[i].x;
        }
        if (points[i].y < bounds.top) {
            bounds.top = points[i].y;
        }
        if (points[i].y > bounds.bottom) {
            bounds.bottom = points[i].y;
        }
    }
    return bounds;
}

/* Read a validated WMF record's byte size from its 16-bit word count header. */
static size_t wmf2emf_record_size(const char *record) {
    uint32_t words = 0;

    if (record == NULL) {
        return 0;
    }
    memcpy(&words, record + offsetof(U_METARECORD, Size16_4), sizeof(words));
    return (size_t)words * 2;
}

/* Scan records for document-level metrics that must be known before EMFHEADER. */
static int wmf2emf_scan_metrics(const char *records, size_t offset,
                                size_t length, char *blimit,
                                wmf2emfMetrics *metrics,
                                wmf2emfContext *ctx) {
    const char *record;
    size_t rec_size;
    U_POINT16 point;

    if (records == NULL || blimit == NULL || metrics == NULL) {
        return 0;
    }
    memset(metrics, 0, sizeof(*metrics));
    while (offset < length) {
        record = records + offset;
        rec_size = U_WMRRECSAFE_get(record, blimit);
        if (rec_size == 0) {
            wmf2emf_log(ctx, "invalid WMF record while scanning at offset %zu", offset);
            return 0;
        }
        if (U_WMRTYPE(record) == U_WMR_SETWINDOWEXT &&
            U_WMRSETWINDOWEXT_get(record, &point)) {
            metrics->has_window_ext = true;
            metrics->window_ext = point;
        }
        if (U_WMRTYPE(record) == U_WMR_SETVIEWPORTEXT) {
            metrics->has_viewport_ext = true;
        }
        if (U_WMRTYPE(record) == U_WMR_SETVIEWPORTORG) {
            metrics->has_viewport_org = true;
        }
        if (U_WMRTYPE(record) == U_WMR_EOF) {
            break;
        }
        offset += rec_size;
    }
    return 1;
}

/* Keep only flags that are valid for EMR_EXTTEXTOUTA text records. */
static uint32_t wmf2emf_text_options(uint16_t options, bool has_dx) {
    uint32_t valid = U_ETO_GRAYED | U_ETO_OPAQUE | U_ETO_CLIPPED |
                     U_ETO_GLYPH_INDEX | U_ETO_RTLREADING |
                     U_ETO_NUMERICSLOCAL | U_ETO_NUMERICSLATIN |
                     U_ETO_IGNORELANGUAGE | U_ETO_REVERSE_INDEX_MAP;

    if (has_dx) {
        valid |= U_ETO_PDY;
    }
    return ((uint32_t)options & valid) | U_ETO_NO_RECT;
}

/* Allocate a small EMF writer backed by fmem so the public API stays in memory. */
static int wmf2emf_output_init(wmf2emfOutput *output) {
    FILE *stream;

    if (output == NULL) {
        return 0;
    }
    memset(output, 0, sizeof(*output));
    output->track.buf = (char *)malloc(WMF2EMF_INITIAL_SIZE);
    if (output->track.buf == NULL) {
        return 0;
    }
    fmem_init(&output->memory);
    stream = fmem_open(&output->memory, "w");
    if (stream == NULL) {
        free(output->track.buf);
        output->track.buf = NULL;
        fmem_term(&output->memory);
        return 0;
    }
    output->track.fp = stream;
    output->track.allocated = WMF2EMF_INITIAL_SIZE;
    output->track.used = 0;
    output->track.records = 0;
    output->track.PalEntries = 0;
    output->track.chunk = WMF2EMF_CHUNK_SIZE;
    if (emf_htable_create(16, 16, &output->handles) != 0) {
        fclose(stream);
        free(output->track.buf);
        output->track.buf = NULL;
        fmem_term(&output->memory);
        return 0;
    }
    return 1;
}

/* Release all temporary EMF writer resources. */
static void wmf2emf_output_free(wmf2emfOutput *output) {
    if (output == NULL) {
        return;
    }
    if (output->track.fp != NULL) {
        fclose(output->track.fp);
        output->track.fp = NULL;
    }
    free(output->track.buf);
    output->track.buf = NULL;
    (void)emf_htable_free(&output->handles);
    fmem_term(&output->memory);
}

/* Finish the EMF stream and copy fmem's buffer to caller-owned memory. */
static int wmf2emf_output_finish(wmf2emfOutput *output, char **out,
                                 size_t *out_length) {
    void *mem = NULL;
    size_t size = 0;

    if (output == NULL || out == NULL || out_length == NULL) {
        return 0;
    }
    if (emf_finish(&output->track, output->handles) != 0) {
        return 0;
    }
    output->track.fp = NULL;
    fmem_mem(&output->memory, &mem, &size);
    *out = NULL;
    *out_length = 0;
    if (size == 0) {
        return 0;
    }
    *out = (char *)malloc(size);
    if (*out == NULL) {
        return 0;
    }
    memcpy(*out, mem, size);
    *out_length = size;
    return 1;
}

/* Append an allocated EMF record and let libuemf free it after appending. */
static int wmf2emf_append_record(wmf2emfOutput *output, char *record) {
    if (output == NULL || record == NULL) {
        free(record);
        return 0;
    }
    return emf_append((PU_ENHMETARECORD)record, &output->track, U_REC_FREE) == 0;
}

/* Convert WMF handle allocation order to explicit EMF handles. */
static int wmf2emf_handle_create(wmf2emfHandleMap *map, uint32_t emf_handle) {
    uint32_t i;

    if (map == NULL || emf_handle == 0) {
        return 0;
    }
    for (i = 0; i < WMF2EMF_HANDLE_LIMIT; i++) {
        if (!map->used[i]) {
            map->used[i] = 1;
            map->values[i] = emf_handle;
            return 1;
        }
    }
    return 0;
}

/* Resolve a WMF object handle to the EMF handle emitted for that object. */
static int wmf2emf_handle_lookup(wmf2emfHandleMap *map, uint16_t wmf_handle,
                                 uint32_t *emf_handle) {
    if (map == NULL || emf_handle == NULL || !map->used[wmf_handle]) {
        return 0;
    }
    *emf_handle = map->values[wmf_handle];
    return 1;
}

/* Drop a WMF handle mapping after translating DELETEOBJECT. */
static void wmf2emf_handle_delete(wmf2emfHandleMap *map, uint16_t wmf_handle) {
    if (map != NULL) {
        map->used[wmf_handle] = 0;
        map->values[wmf_handle] = 0;
    }
}

/* Append the EMF header using normalized device bounds for stable SVG output. */
static int wmf2emf_append_header(wmf2emfOutput *output,
                                 const U_WMRPLACEABLE *placeable,
                                 const wmf2emfMetrics *metrics) {
    U_RECTL bounds;
    U_RECTL frame;
    U_SIZEL device;
    U_SIZEL millimeters;
    uint16_t inch;
    int32_t width;
    int32_t height;

    if (placeable != NULL && placeable->Key == 0x9AC6CDD7) {
        inch = placeable->Inch == 0 ? 1440 : placeable->Inch;
        width = wmf2emf_span_or_default(
            wmf2emf_abs_span(placeable->Dst.left, placeable->Dst.right), 1000);
        height = wmf2emf_span_or_default(
            wmf2emf_abs_span(placeable->Dst.top, placeable->Dst.bottom), 1000);
        bounds = (U_RECTL){0, 0, width, height};
        frame.left = 0;
        frame.top = 0;
        frame.right = (int32_t)(((int64_t)width * 2540) / inch);
        frame.bottom = (int32_t)(((int64_t)height * 2540) / inch);
    } else if (metrics != NULL && metrics->has_window_ext) {
        width = wmf2emf_span_or_default(wmf2emf_abs_span(0, metrics->window_ext.x), 1000);
        height = wmf2emf_span_or_default(wmf2emf_abs_span(0, metrics->window_ext.y), 1000);
        bounds = (U_RECTL){0, 0, width, height};
        frame = (U_RECTL){0, 0, width * 2540 / 1000, height * 2540 / 1000};
    } else {
        bounds = (U_RECTL){0, 0, 1000, 1000};
        frame = (U_RECTL){0, 0, 2540, 2540};
    }
    device = sizel_set(bounds.right - bounds.left + 1,
                       bounds.bottom - bounds.top + 1);
    millimeters = sizel_set((frame.right - frame.left) / 100,
                            (frame.bottom - frame.top) / 100);
    return wmf2emf_append_record(
        output, U_EMRHEADER_set(bounds, frame, NULL, 0, NULL, device,
                                millimeters, 0));
}

/* Enable window/viewport mapping for placeable-style WMF logical extents. */
static int wmf2emf_append_initial_mapping(wmf2emfOutput *output,
                                          const wmf2emfMetrics *metrics) {
    if (metrics == NULL || !metrics->has_window_ext) {
        return 1;
    }
    return wmf2emf_append_record(output, U_EMRSETMAPMODE_set(U_MM_ANISOTROPIC));
}

/* Read an unaligned signed 16-bit field from a WMF font object. */
static int16_t wmf2emf_font_i16(const char *font, size_t offset) {
    int16_t value = 0;

    memcpy(&value, font + offset, sizeof(value));
    return value;
}

/* Read an unaligned byte field from a WMF font object. */
static uint8_t wmf2emf_font_u8(const char *font, size_t offset) {
    return (uint8_t)*(const unsigned char *)(font + offset);
}

/* Copy the WMF ANSI face name into a small UTF-8-compatible buffer. */
static char *wmf2emf_font_face_copy(const char *font) {
    const char *face = font + offsetof(U_FONT, FaceName);
    size_t max_len = 31;
    size_t len = 0;
    char *copy;

    while (len < max_len && face[len] != '\0') {
        len++;
    }
    if (len == 0) {
        face = "Arial";
        len = strlen(face);
    }
    copy = (char *)calloc(len + 1, sizeof(char));
    if (copy != NULL) {
        memcpy(copy, face, len);
    }
    return copy;
}

/* Convert a WMF ANSI face name to UTF-16LE, falling back when bytes are invalid. */
static uint16_t *wmf2emf_font_face_utf16(const char *face) {
    uint16_t *wide = U_Utf8ToUtf16le(face, 0, NULL);

    if (wide == NULL) {
        wide = U_Utf8ToUtf16le("Arial", 0, NULL);
    }
    return wide;
}

/* Convert a WMF pen record into an EMF pen record and remember its handle. */
static int wmf2emf_create_pen(wmf2emfOutput *output, wmf2emfHandleMap *map,
                              const char *record) {
    U_PEN pen;
    U_LOGPEN logpen;
    uint32_t emf_handle = 0;
    uint32_t width = 0;

    if (!U_WMRCREATEPENINDIRECT_get(record, &pen)) {
        return 0;
    }
    memcpy(&width, pen.Widthw, sizeof(pen.Widthw));
    logpen = logpen_set(pen.Style, pointl_set((int32_t)width, 0), pen.Color);
    if (emf_htable_insert(&emf_handle, output->handles) != 0 ||
        !wmf2emf_handle_create(map, emf_handle)) {
        return 0;
    }
    return wmf2emf_append_record(output, U_EMRCREATEPEN_set(emf_handle, logpen));
}

/* Convert a WMF brush record into an EMF brush record and remember its handle. */
static int wmf2emf_create_brush(wmf2emfOutput *output, wmf2emfHandleMap *map,
                                const char *record) {
    const char *brush_data = NULL;
    U_WLOGBRUSH brush;
    U_LOGBRUSH logbrush;
    uint32_t emf_handle = 0;

    if (!U_WMRCREATEBRUSHINDIRECT_get(record, &brush_data) ||
        brush_data == NULL) {
        return 0;
    }
    memcpy(&brush, brush_data, sizeof(brush));
    logbrush = logbrush_set(brush.Style, brush.Color, brush.Hatch);
    if (emf_htable_insert(&emf_handle, output->handles) != 0 ||
        !wmf2emf_handle_create(map, emf_handle)) {
        return 0;
    }
    return wmf2emf_append_record(
        output, U_EMRCREATEBRUSHINDIRECT_set(emf_handle, logbrush));
}

/* Convert a WMF font object so text records select a nonzero EMF font. */
static int wmf2emf_create_font(wmf2emfOutput *output, wmf2emfHandleMap *map,
                               const char *record, wmf2emfContext *ctx) {
    const char *font_data = NULL;
    char *face = NULL;
    uint16_t *face_wide = NULL;
    U_LOGFONT logfont;
    uint32_t emf_handle = 0;
    char *emf_record;

    if (!U_WMRCREATEFONTINDIRECT_get(record, &font_data) || font_data == NULL) {
        return 0;
    }
    face = wmf2emf_font_face_copy(font_data);
    if (face == NULL) {
        return 0;
    }
    face_wide = wmf2emf_font_face_utf16(face);
    if (face_wide == NULL) {
        wmf2emf_log(ctx, "failed to convert WMF font face '%s'", face);
        free(face);
        return 0;
    }
    logfont = logfont_set(
        wmf2emf_font_i16(font_data, offsetof(U_FONT, Height)),
        wmf2emf_font_i16(font_data, offsetof(U_FONT, Width)),
        wmf2emf_font_i16(font_data, offsetof(U_FONT, Escapement)),
        wmf2emf_font_i16(font_data, offsetof(U_FONT, Orientation)),
        wmf2emf_font_i16(font_data, offsetof(U_FONT, Weight)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, Italic)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, Underline)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, StrikeOut)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, CharSet)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, OutPrecision)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, ClipPrecision)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, Quality)),
        wmf2emf_font_u8(font_data, offsetof(U_FONT, PitchAndFamily)),
        face_wide);
    free(face);
    free(face_wide);
    if (emf_htable_insert(&emf_handle, output->handles) != 0 ||
        !wmf2emf_handle_create(map, emf_handle)) {
        return 0;
    }
    emf_record = U_EMREXTCREATEFONTINDIRECTW_set(emf_handle,
                                                 (const char *)&logfont, NULL);
    return wmf2emf_append_record(output, emf_record);
}

/* Convert packed WMF DIB blit records to EMF STRETCHDIBITS image records. */
static int wmf2emf_append_stretch_dibits(wmf2emfOutput *output,
                                         const char *record,
                                         U_POINT16 dst, U_POINT16 dst_size,
                                         U_POINT16 src, U_POINT16 src_size,
                                         uint32_t usage, uint32_t rop,
                                         const char *dib,
                                         wmf2emfContext *ctx) {
    const char *px = NULL;
    const U_RGBQUAD *ct = NULL;
    uint32_t num_ct = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t color_type = 0;
    int32_t invert = 0;
    uint32_t dib_header_size = 0;
    size_t record_size;
    size_t px_offset;
    uint32_t cb_px;

    if (record == NULL || dib == NULL) {
        wmf2emf_log(ctx, "DIB blit without embedded bitmap skipped");
        return 1;
    }
    memcpy(&dib_header_size, dib, sizeof(dib_header_size));
    if (dib_header_size != U_SIZE_BITMAPINFOHEADER) {
        // EMF STRETCHDIBITS and the downstream SVG writer expect BITMAPINFOHEADER.
        wmf2emf_log(ctx, "unsupported DIB header size %u skipped", dib_header_size);
        return 1;
    }
    (void)wget_DIB_params(dib, &px, &ct, &num_ct, &width, &height, &color_type, &invert);
    (void)ct;
    (void)num_ct;
    (void)width;
    (void)height;
    (void)color_type;
    (void)invert;
    record_size = wmf2emf_record_size(record);
    if (px == NULL || px < record || (size_t)(px - record) >= record_size) {
        wmf2emf_log(ctx, "invalid DIB pixel payload skipped");
        return 1;
    }
    px_offset = (size_t)(px - record);
    cb_px = (uint32_t)(record_size - px_offset);
    return wmf2emf_append_record(
        output,
        U_EMRSTRETCHDIBITS_set(
            wmf2emf_rect_from_dest_size(dst, dst_size),
            wmf2emf_point(dst), wmf2emf_point(dst_size),
            wmf2emf_point(src), wmf2emf_point(src_size),
            usage, rop, (PU_BITMAPINFO)dib, cb_px, (char *)px));
}

/* Convert WMF polygon point storage to the 32-bit EMF point storage. */
static U_POINTL *wmf2emf_points16_to_points32(const U_POINT16 *points,
                                              uint32_t count) {
    U_POINTL *out;
    uint32_t i;

    if (points == NULL || count == 0) {
        return NULL;
    }
    out = (U_POINTL *)calloc(count, sizeof(U_POINTL));
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        out[i] = wmf2emf_point(points[i]);
    }
    return out;
}

/* Convert a WMF text record into an ANSI EMF text record. */
static int wmf2emf_append_text(wmf2emfOutput *output, U_POINT16 dst,
                               int16_t length, uint16_t options,
                               const char *string, const int16_t *dx,
                               U_RECT16 rect, wmf2emfContext *ctx) {
    char *emrtext;
    char *record;
    char *text_copy;
    uint32_t *dx32;
    uint32_t text_options;
    bool has_dx = dx != NULL;
    int i;
    U_RECTL bounds;

    if (length <= 0) {
        // Some WMF files contain empty ExtTextOut records; skipping them keeps
        // the surrounding drawing records convertible.
        wmf2emf_log(ctx, "empty text payload skipped");
        return 1;
    }
    if (string == NULL) {
        wmf2emf_log(ctx, "invalid text payload length=%d", length);
        return 0;
    }
    text_copy = (char *)calloc((size_t)length, sizeof(char));
    if (text_copy == NULL) {
        wmf2emf_log(ctx, "failed to allocate text copy length=%d", length);
        return 0;
    }
    memcpy(text_copy, string, (size_t)length);
    dx32 = (uint32_t *)calloc((size_t)length, sizeof(uint32_t));
    if (dx32 == NULL) {
        free(text_copy);
        wmf2emf_log(ctx, "failed to allocate text dx array length=%d", length);
        return 0;
    }
    for (i = 0; i < length; i++) {
        dx32[i] = has_dx ? (uint32_t)dx[i] : 0U;
    }
    text_options = wmf2emf_text_options(options, has_dx);
    bounds = (rect.right >= rect.left && rect.bottom >= rect.top)
                 ? wmf2emf_rect(rect)
                 : (U_RECTL){dst.x, dst.y, dst.x, dst.y};
    emrtext = emrtext_set(wmf2emf_point(dst), (uint32_t)length, 1,
                          (void *)text_copy, text_options, bounds,
                          dx32);
    free(text_copy);
    free(dx32);
    if (emrtext == NULL) {
        wmf2emf_log(ctx, "failed to build EMRTEXT length=%d options=0x%04X sanitized=0x%08X", length, options, text_options);
        return 0;
    }
    record = U_EMREXTTEXTOUTA_set(bounds, U_GM_COMPATIBLE, 1.0, 1.0,
                                  (PU_EMRTEXT)emrtext);
    free(emrtext);
    if (record == NULL) {
        wmf2emf_log(ctx, "failed to build EMR_EXTTEXTOUTA length=%d options=0x%04X sanitized=0x%08X", length, options, text_options);
        return 0;
    }
    return wmf2emf_append_record(output, record);
}

/* Return the WMF dx array only when it is fully present in the current record. */
static const int16_t *wmf2emf_exttext_dx_if_present(const char *record,
                                                    int16_t length,
                                                    uint16_t options) {
    size_t offset = U_SIZE_WMREXTTEXTOUT;
    size_t record_size;
    size_t text_size;
    size_t dx_size;

    if (record == NULL || length <= 0) {
        return NULL;
    }
    record_size = wmf2emf_record_size(record);
    if (options & (U_ETO_OPAQUE | U_ETO_CLIPPED)) {
        offset += U_SIZE_RECT16;
    }
    text_size = 2U * (size_t)((length + 1) / 2);
    dx_size = sizeof(int16_t) * (size_t)length;
    if (record_size < offset + text_size + dx_size) {
        return NULL;
    }
    return (const int16_t *)(record + offset + text_size);
}

/* Translate one WMF record to one or more EMF records. */
static int wmf2emf_translate_record(wmf2emfOutput *output,
                                    wmf2emfHandleMap *map,
                                    const char *record,
                                    const wmf2emfMetrics *metrics,
                                    wmf2emfContext *ctx,
                                    bool *stop) {
    uint8_t type;
    uint16_t mode;
    uint16_t object;
    int16_t dc;
    U_COLORREF color;
    U_POINT16 point;
    U_POINT16 point2;
    U_POINT16 src;
    U_POINT16 src_size;
    U_RECT16 rect;
    uint16_t count16;
    uint32_t rop;
    const char *data = NULL;
    const char *dib = NULL;
    const int16_t *dx = NULL;
    U_POINTL *points = NULL;
    uint32_t emf_handle = 0;
    int16_t text_len;

    if (stop != NULL) {
        *stop = false;
    }
    type = U_WMRTYPE(record);
    switch (type) {
    case U_WMR_EOF:
        if (stop != NULL) {
            *stop = true;
        }
        return wmf2emf_append_record(output, U_EMREOF_set(0, NULL, &output->track));
    case U_WMR_SETBKCOLOR:
        return U_WMRSETBKCOLOR_get(record, &color) &&
               wmf2emf_append_record(output, U_EMRSETBKCOLOR_set(color));
    case U_WMR_SETBKMODE:
        return U_WMRSETBKMODE_get(record, &mode) &&
               wmf2emf_append_record(output, U_EMRSETBKMODE_set(mode));
    case U_WMR_SETMAPMODE:
        return U_WMRSETMAPMODE_get(record, &mode) &&
               wmf2emf_append_record(output, U_EMRSETMAPMODE_set(mode));
    case U_WMR_SETROP2:
        return U_WMRSETROP2_get(record, &mode) &&
               wmf2emf_append_record(output, U_EMRSETROP2_set(mode));
    case U_WMR_SETPOLYFILLMODE:
        return U_WMRSETPOLYFILLMODE_get(record, &mode) &&
               wmf2emf_append_record(output, U_EMRSETPOLYFILLMODE_set(mode));
    case U_WMR_SETSTRETCHBLTMODE:
        return U_WMRSETSTRETCHBLTMODE_get(record, &mode) &&
               wmf2emf_append_record(output, U_EMRSETSTRETCHBLTMODE_set(mode));
    case U_WMR_SETTEXTCOLOR:
        return U_WMRSETTEXTCOLOR_get(record, &color) &&
               wmf2emf_append_record(output, U_EMRSETTEXTCOLOR_set(color));
    case U_WMR_SETWINDOWORG:
        return U_WMRSETWINDOWORG_get(record, &point) &&
               wmf2emf_append_record(output, U_EMRSETWINDOWORGEX_set(wmf2emf_point(point)));
    case U_WMR_SETWINDOWEXT:
        if (!U_WMRSETWINDOWEXT_get(record, &point) ||
            !wmf2emf_append_record(output, U_EMRSETWINDOWEXTEX_set(wmf2emf_size(point)))) {
            return 0;
        }
        // WMF files often rely on the default viewport.  Writing it explicitly
        // lets EMF readers apply negative window extents instead of clipping.
        if (metrics != NULL && !metrics->has_viewport_org &&
            !wmf2emf_append_record(output, U_EMRSETVIEWPORTORGEX_set(pointl_set(0, 0)))) {
            return 0;
        }
        if (metrics != NULL && !metrics->has_viewport_ext &&
            !wmf2emf_append_record(
                output,
                U_EMRSETVIEWPORTEXTEX_set(
                    metrics->has_default_viewport_ext
                        ? metrics->default_viewport_ext
                        : wmf2emf_positive_size(point)))) {
            return 0;
        }
        return 1;
    case U_WMR_SETVIEWPORTORG:
        return U_WMRSETVIEWPORTORG_get(record, &point) &&
               wmf2emf_append_record(output, U_EMRSETVIEWPORTORGEX_set(wmf2emf_point(point)));
    case U_WMR_SETVIEWPORTEXT:
        return U_WMRSETVIEWPORTEXT_get(record, &point) &&
               wmf2emf_append_record(output, U_EMRSETVIEWPORTEXTEX_set(wmf2emf_size(point)));
    case U_WMR_SCALEWINDOWEXT:
        return U_WMRSCALEWINDOWEXT_get(record, &point, &point2) &&
               wmf2emf_append_record(output, U_EMRSCALEWINDOWEXTEX_set(point2.x, point.x, point2.y, point.y));
    case U_WMR_SCALEVIEWPORTEXT:
        return U_WMRSCALEVIEWPORTEXT_get(record, &point, &point2) &&
               wmf2emf_append_record(output, U_EMRSCALEVIEWPORTEXTEX_set(point2.x, point.x, point2.y, point.y));
    case U_WMR_LINETO:
        return U_WMRLINETO_get(record, &point) &&
               wmf2emf_append_record(output, U_EMRLINETO_set(wmf2emf_point(point)));
    case U_WMR_MOVETO:
        return U_WMRMOVETO_get(record, &point) &&
               wmf2emf_append_record(output, U_EMRMOVETOEX_set(wmf2emf_point(point)));
    case U_WMR_EXCLUDECLIPRECT:
        return U_WMREXCLUDECLIPRECT_get(record, &rect) &&
               wmf2emf_append_record(output, U_EMREXCLUDECLIPRECT_set(wmf2emf_rect(rect)));
    case U_WMR_INTERSECTCLIPRECT:
        return U_WMRINTERSECTCLIPRECT_get(record, &rect) &&
               wmf2emf_append_record(output, U_EMRINTERSECTCLIPRECT_set(wmf2emf_rect(rect)));
    case U_WMR_ARC:
        return U_WMRARC_get(record, &point, &point2, &rect) &&
               wmf2emf_append_record(output, U_EMRARC_set(wmf2emf_rect(rect), wmf2emf_point(point), wmf2emf_point(point2)));
    case U_WMR_ELLIPSE:
        return U_WMRELLIPSE_get(record, &rect) &&
               wmf2emf_append_record(output, U_EMRELLIPSE_set(wmf2emf_rect(rect)));
    case U_WMR_PIE:
        return U_WMRPIE_get(record, &point, &point2, &rect) &&
               wmf2emf_append_record(output, U_EMRPIE_set(wmf2emf_rect(rect), wmf2emf_point(point), wmf2emf_point(point2)));
    case U_WMR_RECTANGLE:
        return U_WMRRECTANGLE_get(record, &rect) &&
               wmf2emf_append_record(output, U_EMRRECTANGLE_set(wmf2emf_rect(rect)));
    case U_WMR_ROUNDRECT:
        return U_WMRROUNDRECT_get(record, &point.x, &point.y, &rect) &&
               wmf2emf_append_record(output, U_EMRROUNDRECT_set(wmf2emf_rect(rect), wmf2emf_size(point)));
    case U_WMR_SAVEDC:
        return U_WMRSAVEDC_get(record) &&
               wmf2emf_append_record(output, U_EMRSAVEDC_set());
    case U_WMR_SETPIXEL:
        return U_WMRSETPIXEL_get(record, &color, &point) &&
               wmf2emf_append_record(output, U_EMRSETPIXELV_set(wmf2emf_point(point), color));
    case U_WMR_OFFSETCLIPRGN:
        return U_WMROFFSETCLIPRGN_get(record, &point) &&
               wmf2emf_append_record(output, U_EMROFFSETCLIPRGN_set(wmf2emf_point(point)));
    case U_WMR_POLYGON:
        if (!U_WMRPOLYGON_get(record, &count16, &data)) {
            return 0;
        }
        points = wmf2emf_points16_to_points32((const U_POINT16 *)data, count16);
        if (points == NULL) {
            return 0;
        }
        rect = (U_RECT16){0, 0, 0, 0};
        {
            U_RECTL bounds = wmf2emf_bounds_from_points((const U_POINT16 *)data, count16);
            int ok = wmf2emf_append_record(output, U_EMRPOLYGON_set(bounds, count16, points));
            free(points);
            return ok;
        }
    case U_WMR_POLYLINE:
        if (!U_WMRPOLYLINE_get(record, &count16, &data)) {
            return 0;
        }
        points = wmf2emf_points16_to_points32((const U_POINT16 *)data, count16);
        if (points == NULL) {
            return 0;
        }
        {
            U_RECTL bounds = wmf2emf_bounds_from_points((const U_POINT16 *)data, count16);
            int ok = wmf2emf_append_record(output, U_EMRPOLYLINE_set(bounds, count16, points));
            free(points);
            return ok;
        }
    case U_WMR_RESTOREDC:
        return U_WMRRESTOREDC_get(record, &dc) &&
               wmf2emf_append_record(output, U_EMRRESTOREDC_set(dc));
    case U_WMR_SELECTOBJECT:
        if (!U_WMRSELECTOBJECT_get(record, &object)) {
            return 0;
        }
        if (!wmf2emf_handle_lookup(map, object, &emf_handle)) {
            ctx->unsupported++;
            wmf2emf_log(ctx, "unmapped WMF object %u selected, skipped", object);
            return 1;
        }
        return wmf2emf_append_record(output, U_EMRSELECTOBJECT_set(emf_handle));
    case U_WMR_SETTEXTALIGN:
        return U_WMRSETTEXTALIGN_get(record, &mode) &&
               wmf2emf_append_record(output, U_EMRSETTEXTALIGN_set(mode));
    case U_WMR_CHORD:
        return U_WMRCHORD_get(record, &point, &point2, &rect) &&
               wmf2emf_append_record(output, U_EMRCHORD_set(wmf2emf_rect(rect), wmf2emf_point(point), wmf2emf_point(point2)));
    case U_WMR_EXTTEXTOUT:
        if (!U_WMREXTTEXTOUT_get(record, &point, &text_len, &mode, &data, &dx, &rect)) {
            return 0;
        }
        dx = wmf2emf_exttext_dx_if_present(record, text_len, mode);
        return wmf2emf_append_text(output, point, text_len, mode, data, dx, rect, ctx);
    case U_WMR_TEXTOUT:
        return U_WMRTEXTOUT_get(record, &point, &text_len, &data) &&
               wmf2emf_append_text(output, point, text_len, U_ETO_NONE, data, NULL, U_RCL16_DEF, ctx);
    case U_WMR_POLYPOLYGON:
        if (!U_WMRPOLYPOLYGON_get(record, &count16, (const uint16_t **)&dx, &data)) {
            return 0;
        }
        {
            uint32_t total = 0;
            uint32_t *counts = NULL;
            const uint16_t *counts16 = (const uint16_t *)dx;
            uint32_t i;
            int ok;
            for (i = 0; i < count16; i++) {
                total += counts16[i];
            }
            counts = (uint32_t *)calloc(count16, sizeof(uint32_t));
            points = wmf2emf_points16_to_points32((const U_POINT16 *)data, total);
            if (counts == NULL || points == NULL) {
                free(counts);
                free(points);
                return 0;
            }
            for (i = 0; i < count16; i++) {
                counts[i] = counts16[i];
            }
            ok = wmf2emf_append_record(
                output,
                U_EMRPOLYPOLYGON_set(
                    wmf2emf_bounds_from_points((const U_POINT16 *)data, total),
                    count16, counts, total, points));
            free(counts);
            free(points);
            return ok;
        }
    case U_WMR_EXTFLOODFILL:
        return U_WMREXTFLOODFILL_get(record, &mode, &color, &point) &&
               wmf2emf_append_record(output, U_EMREXTFLOODFILL_set(wmf2emf_point(point), color, mode));
    case U_WMR_DIBBITBLT:
        if (!U_WMRDIBBITBLT_get(record, &point, &point2, &src, &rop, &dib)) {
            return 0;
        }
        return wmf2emf_append_stretch_dibits(output, record, point, point2,
                                             src, point2,
                                             U_DIB_RGB_COLORS, rop, dib, ctx);
    case U_WMR_DIBSTRETCHBLT:
        if (!U_WMRDIBSTRETCHBLT_get(record, &point, &point2,
                                    &src, &src_size,
                                    &rop, &dib)) {
            return 0;
        }
        return wmf2emf_append_stretch_dibits(output, record, point, point2,
                                             src, src_size,
                                             U_DIB_RGB_COLORS, rop, dib, ctx);
    case U_WMR_STRETCHDIB:
        if (!U_WMRSTRETCHDIB_get(record, &point, &point2,
                                 &src, &src_size,
                                 &mode, &rop, &dib)) {
            return 0;
        }
        return wmf2emf_append_stretch_dibits(output, record, point, point2,
                                             src, src_size,
                                             mode, rop, dib, ctx);
    case U_WMR_DELETEOBJECT:
        if (!U_WMRDELETEOBJECT_get(record, &object)) {
            return 0;
        }
        if (!wmf2emf_handle_lookup(map, object, &emf_handle)) {
            ctx->unsupported++;
            wmf2emf_log(ctx, "unmapped WMF object %u deleted, skipped", object);
            return 1;
        }
        wmf2emf_handle_delete(map, object);
        return wmf2emf_append_record(output, U_EMRDELETEOBJECT_set(emf_handle));
    case U_WMR_CREATEPENINDIRECT:
        return wmf2emf_create_pen(output, map, record);
    case U_WMR_CREATEBRUSHINDIRECT:
        return wmf2emf_create_brush(output, map, record);
    case U_WMR_CREATEFONTINDIRECT:
        return wmf2emf_create_font(output, map, record, ctx);
    default:
        ctx->unsupported++;
        wmf2emf_log(ctx, "unsupported WMF record %s (type=0x%02X, xb=0x%02X), skipped",
                    U_wmr_names(type), type, U_WMRXB(record));
        return 1;
    }
}

/* Convert WMF bytes to EMF bytes. Returns 1 on success and 0 on failure. */
int wmf2emf(char *contents, size_t length, char **out, size_t *out_length,
            wmf2emfOptions *options) {
    wmf2emfOptions opts = wmf2emf_options_or_default(options);
    wmf2emfContext ctx = {opts.verbose, 0};
    wmf2emfOutput output;
    wmf2emfHandleMap handle_map;
    U_WMRPLACEABLE placeable;
    U_WMRHEADER header;
    wmf2emfMetrics metrics;
    char *work = NULL;
    char *blimit;
    size_t off;
    int ok = 0;
    bool stop = false;

    if (out == NULL || out_length == NULL || contents == NULL || length == 0) {
        return 0;
    }
    *out = NULL;
    *out_length = 0;
    work = (char *)malloc(length);
    if (work == NULL) {
        return 0;
    }
    memcpy(work, contents, length);
#if U_BYTE_SWAP
    // libuemf reads native-endian records, so swap the caller buffer copy on BE.
    U_wmf_endian(work, length, 0, 0);
#endif
    blimit = work + length;
    off = wmfheader_get(work, blimit, &placeable, &header);
    if (off == 0) {
        wmf2emf_log(&ctx, "invalid WMF header");
        goto done;
    }
    if (!wmf2emf_scan_metrics(work, off, length, blimit, &metrics, &ctx)) {
        goto done;
    }
    wmf2emf_set_default_viewport(&metrics, &placeable);
    if (!wmf2emf_output_init(&output)) {
        wmf2emf_log(&ctx, "failed to allocate EMF output stream");
        goto done;
    }
    memset(&handle_map, 0, sizeof(handle_map));
    if (!wmf2emf_append_header(&output, &placeable, &metrics)) {
        wmf2emf_log(&ctx, "failed to append EMF header");
        goto done_output;
    }
    if (!wmf2emf_append_initial_mapping(&output, &metrics)) {
        wmf2emf_log(&ctx, "failed to append initial EMF mapping");
        goto done_output;
    }
    while (off < length && !stop) {
        size_t rec_size = U_WMRRECSAFE_get(work + off, blimit);
        if (rec_size == 0) {
            wmf2emf_log(&ctx, "invalid WMF record at offset %zu", off);
            goto done_output;
        }
        if (!wmf2emf_translate_record(&output, &handle_map, work + off, &metrics,
                                      &ctx, &stop)) {
            wmf2emf_log(&ctx, "failed to translate WMF record at offset %zu", off);
            goto done_output;
        }
        off += rec_size;
    }
    if (!stop) {
        if (!wmf2emf_append_record(&output, U_EMREOF_set(0, NULL, &output.track))) {
            goto done_output;
        }
    }
    if (ctx.unsupported != 0) {
        wmf2emf_log(&ctx, "%u unsupported WMF records were skipped", ctx.unsupported);
    }
    ok = wmf2emf_output_finish(&output, out, out_length);

done_output:
    wmf2emf_output_free(&output);
done:
    free(work);
    if (!ok) {
        free(*out);
        *out = NULL;
        *out_length = 0;
    }
    return ok;
}

#ifdef __cplusplus
}
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
