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

/* Append the EMF header using a placeable WMF header when available. */
static int wmf2emf_append_header(wmf2emfOutput *output,
                                 const U_WMRPLACEABLE *placeable) {
    U_RECTL bounds;
    U_RECTL frame;
    U_SIZEL device;
    U_SIZEL millimeters;
    uint16_t inch;

    if (placeable != NULL && placeable->Key == 0x9AC6CDD7) {
        inch = placeable->Inch == 0 ? 1440 : placeable->Inch;
        bounds = wmf2emf_rect(placeable->Dst);
        frame.left = 0;
        frame.top = 0;
        frame.right = (int32_t)(((int64_t)(placeable->Dst.right - placeable->Dst.left) * 2540) / inch);
        frame.bottom = (int32_t)(((int64_t)(placeable->Dst.bottom - placeable->Dst.top) * 2540) / inch);
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
                                    wmf2emfContext *ctx,
                                    bool *stop) {
    uint8_t type;
    uint16_t mode;
    uint16_t object;
    int16_t dc;
    U_COLORREF color;
    U_POINT16 point;
    U_POINT16 point2;
    U_RECT16 rect;
    uint16_t count16;
    const char *data = NULL;
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
        return U_WMRSETWINDOWEXT_get(record, &point) &&
               wmf2emf_append_record(output, U_EMRSETWINDOWEXTEX_set(wmf2emf_size(point)));
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
    if (!wmf2emf_output_init(&output)) {
        wmf2emf_log(&ctx, "failed to allocate EMF output stream");
        goto done;
    }
    memset(&handle_map, 0, sizeof(handle_map));
    if (!wmf2emf_append_header(&output, &placeable)) {
        wmf2emf_log(&ctx, "failed to append EMF header");
        goto done_output;
    }
    while (off < length && !stop) {
        size_t rec_size = U_WMRRECSAFE_get(work + off, blimit);
        if (rec_size == 0) {
            wmf2emf_log(&ctx, "invalid WMF record at offset %zu", off);
            goto done_output;
        }
        if (!wmf2emf_translate_record(&output, &handle_map, work + off, &ctx,
                                      &stop)) {
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
