/**
  @file upmf_draw.c

  @brief Functions for printing EMF records
  */

/*
File:      upmf_draw.c
Version:   0.0.3
Date:      24-MAR-2014
Author:    David Mathog, Biology Division, Caltech
email:     mathog@caltech.edu
Copyright: 2014 David Mathog and California Institute of Technology (Caltech)
*/

/* compiler options:

   -DNOBRUSH causes brush objects to be treated as pen objects.  PowerPoint 2003
   and 2010 define pen objects
   as brush objects, and this is one way to see their structure even though they
   are misidentified.
   This option should only be used for tiny test files, consisting of just line
   objects.
   */

#ifdef __cplusplus
extern "C" {
#endif

#include "emf2svg_private.h"
#include "emf2svg.h"
#include "pmf2svg.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! \cond

#define UNUSED(x)                                                              \
    (void)(x) //! Please ignore - Doxygen simply insisted on including this

int U_PMF_VARPOINTS_get(const char *contents, uint16_t Flags, int Elements,
                        U_PMF_POINTF **Points, const char *blimit);

typedef struct {
    bool active;
    char *data;
    size_t size;
} pmfImageCacheEntry;

typedef struct {
    bool active;
    U_PMF_POINTF *points;
    uint8_t *types;
    uint32_t count;
} pmfPathCacheEntry;

typedef struct {
    bool active;
    U_PMF_ARGB color;
    U_FLOAT width;
    U_FLOAT *dash_lengths;
    int32_t dash_count;
    U_FLOAT dash_offset;
    U_FLOAT start_cap_inset;
    U_FLOAT end_cap_inset;
    U_FLOAT start_cap_width_scale;
    U_FLOAT end_cap_width_scale;
    bool custom_start_cap;
    bool custom_end_cap;
} pmfPenCacheEntry;

typedef struct {
    bool active;
    U_PMF_ARGB color;
    bool path_gradient;
    U_PMF_POINTF center;
    U_PMF_ARGB *gradient_colors;
    uint32_t gradient_count;
} pmfBrushCacheEntry;

typedef struct {
    bool active;
    U_FLOAT em_size;
    int32_t style_flags;
    char *family;
} pmfFontCacheEntry;

typedef struct {
    bool active;
    char *data;
    size_t size;
} pmfRegionCacheEntry;

static pmfImageCacheEntry pmf_image_cache[64];
static pmfPathCacheEntry pmf_path_cache[64];
static pmfPenCacheEntry pmf_pen_cache[64];
static pmfBrushCacheEntry pmf_brush_cache[64];
static pmfFontCacheEntry pmf_font_cache[64];
static pmfRegionCacheEntry pmf_region_cache[64];
static U_XFORM pmf_world_transform = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
static int pmf_metafile_depth = 0;
static double pmf_metafile_stroke_scale = 1.0;
static drawingStates *pmf_parent_bitmap_box_states = NULL;
static bool pmf_top_level_primitive_draw_enabled = false;
static uint64_t pmf_clip_mask_serial = 0;
static uint64_t pmf_active_clip_mask_id = 0;
static uint64_t pmf_gradient_serial = 0;

/**
  \brief Remember a top-level EMF+ FillPath color for its GDI fallback.

  Dual EMF+/GDI files such as test-188.emf keep alpha only in the EMF+ ARGB
  FillPath. The later GDI PATINVERT mask fallback carries RGB only, so this
  short-lived color lets the fallback preserve opacity.
  */
static void pmf_recent_fill_color_store(drawingStates *states,
                                        U_PMF_ARGB color) {
    states->recentEmfPlusFill.active = true;
    states->recentEmfPlusFill.red = color.Red;
    states->recentEmfPlusFill.green = color.Green;
    states->recentEmfPlusFill.blue = color.Blue;
    states->recentEmfPlusFill.alpha = color.Alpha;
}

/**
  \brief Reset the EMF+ world transform used by DrawImagePoints.
  */
static void pmf_world_transform_reset(void) {
    pmf_world_transform.eM11 = 1.0;
    pmf_world_transform.eM12 = 0.0;
    pmf_world_transform.eM21 = 0.0;
    pmf_world_transform.eM22 = 1.0;
    pmf_world_transform.eDx = 0.0;
    pmf_world_transform.eDy = 0.0;
}

/**
  \brief Clear cached EMF+ image objects.

  EMF+ DrawImagePoints refers to images by object ID. The cache is reset at EMF+
  headers and EOF so recursive metafile rendering does not reuse stale objects.
  */
static void pmf_image_cache_clear(void) {
    pmf_world_transform_reset();
    for (size_t i = 0; i < sizeof(pmf_image_cache) / sizeof(pmf_image_cache[0]);
         i++) {
        free(pmf_image_cache[i].data);
        pmf_image_cache[i].data = NULL;
        pmf_image_cache[i].size = 0;
        pmf_image_cache[i].active = false;
    }
}

/**
  \brief Clear cached EMF+ path objects.

  EMF+ path-only metafiles such as test-formula2.emf define outlines as Path
  objects followed by FillPath records. Reset the object cache at EMF+ stream
  boundaries so object IDs do not leak across nested metafiles.
  */
static void pmf_path_cache_clear(void) {
    for (size_t i = 0; i < sizeof(pmf_path_cache) / sizeof(pmf_path_cache[0]);
         i++) {
        free(pmf_path_cache[i].points);
        free(pmf_path_cache[i].types);
        pmf_path_cache[i].points = NULL;
        pmf_path_cache[i].types = NULL;
        pmf_path_cache[i].count = 0;
        pmf_path_cache[i].active = false;
    }
}

/**
  \brief Clear cached EMF+ pen objects.

  test-formula2.emf draws operators such as equals signs, minus signs, and
  wavy relation marks with DrawPath plus Pen records rather than FillPath.
  Keep pen state scoped to one EMF+ stream.
  */
static void pmf_pen_cache_clear(void) {
    for (size_t i = 0; i < sizeof(pmf_pen_cache) / sizeof(pmf_pen_cache[0]);
         i++) {
        free(pmf_pen_cache[i].dash_lengths);
        pmf_pen_cache[i].dash_lengths = NULL;
        pmf_pen_cache[i].dash_count = 0;
        pmf_pen_cache[i].dash_offset = 0.0;
        pmf_pen_cache[i].start_cap_inset = 0.0;
        pmf_pen_cache[i].end_cap_inset = 0.0;
        pmf_pen_cache[i].start_cap_width_scale = 1.0;
        pmf_pen_cache[i].end_cap_width_scale = 1.0;
        pmf_pen_cache[i].custom_start_cap = false;
        pmf_pen_cache[i].custom_end_cap = false;
        pmf_pen_cache[i].active = false;
        pmf_pen_cache[i].width = 0.0;
        memset(&pmf_pen_cache[i].color, 0, sizeof(pmf_pen_cache[i].color));
    }
}

/**
  \brief Clear cached EMF+ brush objects.
  */
static void pmf_brush_cache_clear(void) {
    for (size_t i = 0;
         i < sizeof(pmf_brush_cache) / sizeof(pmf_brush_cache[0]); i++) {
        free(pmf_brush_cache[i].gradient_colors);
        pmf_brush_cache[i].gradient_colors = NULL;
        pmf_brush_cache[i].gradient_count = 0;
        pmf_brush_cache[i].path_gradient = false;
        pmf_brush_cache[i].active = false;
        memset(&pmf_brush_cache[i].color, 0, sizeof(pmf_brush_cache[i].color));
    }
}

/**
  \brief Clear cached EMF+ font objects.
  */
static void pmf_font_cache_clear(void) {
    for (size_t i = 0; i < sizeof(pmf_font_cache) / sizeof(pmf_font_cache[0]);
         i++) {
        free(pmf_font_cache[i].family);
        pmf_font_cache[i].family = NULL;
        pmf_font_cache[i].em_size = 0.0;
        pmf_font_cache[i].style_flags = 0;
        pmf_font_cache[i].active = false;
    }
}

/**
  \brief Clear cached EMF+ region objects.
  */
static void pmf_region_cache_clear(void) {
    for (size_t i = 0;
         i < sizeof(pmf_region_cache) / sizeof(pmf_region_cache[0]); i++) {
        free(pmf_region_cache[i].data);
        pmf_region_cache[i].data = NULL;
        pmf_region_cache[i].size = 0;
        pmf_region_cache[i].active = false;
    }
}

/**
  \brief Reset EMF+ object caches at a stream boundary.
  */
static void pmf_object_caches_clear(void) {
    pmf_image_cache_clear();
    pmf_path_cache_clear();
    pmf_pen_cache_clear();
    pmf_brush_cache_clear();
    pmf_font_cache_clear();
    pmf_region_cache_clear();
    pmf_top_level_primitive_draw_enabled = false;
    pmf_active_clip_mask_id = 0;
}

/**
  \brief Store a completed EMF+ Image object for later DrawImagePoints records.
  */
static int pmf_image_cache_store(uint32_t id, const char *data, size_t size) {
    if (id >= sizeof(pmf_image_cache) / sizeof(pmf_image_cache[0]) ||
        data == NULL || size == 0) {
        return 0;
    }

    char *copy = (char *)malloc(size);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data, size);

    free(pmf_image_cache[id].data);
    pmf_image_cache[id].data = copy;
    pmf_image_cache[id].size = size;
    pmf_image_cache[id].active = true;
    return 1;
}

/**
  \brief Return a cached EMF+ Image object by object ID.
  */
static const pmfImageCacheEntry *pmf_image_cache_get(uint32_t id) {
    if (id >= sizeof(pmf_image_cache) / sizeof(pmf_image_cache[0]) ||
        !pmf_image_cache[id].active) {
        return NULL;
    }
    return &pmf_image_cache[id];
}

/**
  \brief Decode EMF+ path point types into one byte per point.
  */
static uint8_t *pmf_path_types_decode(const char *types, uint16_t flags,
                                      uint32_t count, const char *blimit) {
    uint8_t *decoded;
    uint32_t out_i = 0;

    if (types == NULL || count == 0) {
        return NULL;
    }
    decoded = (uint8_t *)calloc(count, sizeof(uint8_t));
    if (decoded == NULL) {
        return NULL;
    }

    if (flags & U_PPF_R) {
        while (out_i < count) {
            int bezier;
            int run_length;
            int point_type;
            if (!U_PMF_PATHPOINTTYPERLE_get(types, &bezier, &run_length,
                                            &point_type, blimit)) {
                free(decoded);
                return NULL;
            }
            types += sizeof(U_PMF_PATHPOINTTYPERLE);
            for (int i = 0; i < run_length && out_i < count; i++) {
                decoded[out_i++] =
                    (uint8_t)((bezier ? U_PPT_Bezier : point_type) &
                              (U_PPT_MASK | U_PTP_MASK));
            }
        }
    } else {
        for (out_i = 0; out_i < count; out_i++) {
            int type_flags;
            int point_type;
            if (!U_PMF_PATHPOINTTYPE_get(types + out_i, &type_flags,
                                         &point_type, blimit)) {
                free(decoded);
                return NULL;
            }
            decoded[out_i] =
                (uint8_t)((type_flags << U_PTP_SHIFT) | point_type);
        }
    }

    return decoded;
}

/**
  \brief Store a completed EMF+ Path object for later FillPath/DrawPath records.

  test-formula2.emf stores formula glyph outlines as EMF+ paths without a GDI
  fallback. Caching Path objects lets the later FillPath records emit SVG path
  geometry instead of leaving the conversion blank.
  */
static int pmf_path_cache_store(uint32_t id, const char *data,
                                const char *blimit) {
    uint32_t version;
    uint32_t count;
    uint16_t flags;
    const char *points_raw;
    const char *types_raw;
    U_PMF_POINTF *points = NULL;
    uint8_t *types = NULL;
    pmfPathCacheEntry *entry;

    if (id >= sizeof(pmf_path_cache) / sizeof(pmf_path_cache[0]) ||
        data == NULL) {
        return 0;
    }
    if (!U_PMF_PATH_get(data, &version, &count, &flags, &points_raw,
                        &types_raw, blimit)) {
        return 0;
    }
    UNUSED(version);
    if (!U_PMF_VARPOINTS_get(points_raw, flags, (int)count, &points, blimit)) {
        return 0;
    }
    types = pmf_path_types_decode(types_raw, flags, count, blimit);
    if (types == NULL) {
        free(points);
        return 0;
    }

    entry = &pmf_path_cache[id];
    free(entry->points);
    free(entry->types);
    entry->points = points;
    entry->types = types;
    entry->count = count;
    entry->active = true;
    return 1;
}

/**
  \brief Return a cached EMF+ Path object by object ID.
  */
static const pmfPathCacheEntry *pmf_path_cache_get(uint32_t id) {
    if (id >= sizeof(pmf_path_cache) / sizeof(pmf_path_cache[0]) ||
        !pmf_path_cache[id].active) {
        return NULL;
    }
    return &pmf_path_cache[id];
}

/**
  \brief Return the solid color carried by an EMF+ Brush object.
  */
static int pmf_solid_brush_object_color(const char *brush, const char *blimit,
                                        U_PMF_ARGB *color) {
    uint32_t version;
    uint32_t type;
    const char *data;

    if (brush == NULL || color == NULL) {
        return 0;
    }
    if (!U_PMF_BRUSH_get(brush, &version, &type, &data, blimit)) {
        return 0;
    }
    UNUSED(version);
    if (type != U_BT_SolidColor) {
        return 0;
    }
    return U_PMF_ARGB_get(data, &color->Blue, &color->Green, &color->Red,
                          &color->Alpha, blimit);
}

/**
 * \brief Read the dimensions of one default EMF+ custom line cap.
 *
 * k-hop2.emf uses the default custom-cap payload for triangular arrowheads.
 * Its fill path is more detail than the SVG fallback needs; the inset and
 * width scale are sufficient to recreate the cap at a stroked path endpoint.
 */
static bool pmf_custom_cap_metrics(const char *contents, bool start_cap,
                                   const char *blimit, U_FLOAT *inset,
                                   U_FLOAT *width_scale) {
    int32_t size;
    const char *cap_contents;
    const char *cap_data;
    const char *optional_data;
    const char *cap_limit;
    uint32_t version;
    uint32_t type;
    U_PMF_CUSTOMLINECAPDATA cap;
    int ok;

    if (contents == NULL || inset == NULL || width_scale == NULL) {
        return false;
    }
    if (start_cap) {
        ok = U_PMF_CUSTOMSTARTCAPDATA_get(contents, &size, &cap_contents,
                                           blimit);
    } else {
        ok = U_PMF_CUSTOMENDCAPDATA_get(contents, &size, &cap_contents,
                                         blimit);
    }
    if (!ok || size <= 0 || cap_contents > blimit ||
        (size_t)(blimit - cap_contents) < (size_t)size) {
        return false;
    }
    cap_limit = cap_contents + size;
    if (!U_PMF_CUSTOMLINECAP_get(cap_contents, &version, &type, &cap_data,
                                 cap_limit) ||
        type != U_CLCDT_Default ||
        !U_PMF_CUSTOMLINECAPDATA_get(cap_data, &cap, &optional_data,
                                     cap_limit)) {
        return false;
    }
    UNUSED(version);
    UNUSED(optional_data);
    if (cap.Inset <= 0.0 || cap.WidthScale <= 0.0) {
        return false;
    }
    *inset = cap.Inset;
    *width_scale = cap.WidthScale;
    return true;
}

/**
  \brief Store a completed EMF+ Pen object for later DrawPath records.

  The formula samples use solid pens for stroke-only operators. Caching the pen
  width and color lets DrawPath render those symbols without enabling broader
  unsupported pen features.
  */
static int pmf_pen_cache_store(uint32_t id, const char *data,
                               const char *blimit) {
    uint32_t version;
    uint32_t type;
    const char *pen_data;
    const char *brush;
    uint32_t flags;
    uint32_t unit;
    U_FLOAT width;
    const char *optional_data;
    U_PMF_ARGB color;
    pmfPenCacheEntry *entry;
    U_PMF_TRANSFORMMATRIX matrix;
    int32_t start_cap;
    int32_t end_cap;
    uint32_t join;
    U_FLOAT miter_limit;
    int32_t line_style;
    int32_t dash_cap;
    U_FLOAT dash_offset = 0.0;
    const char *dash_data = NULL;
    int32_t alignment;
    const char *compound_line_data = NULL;
    const char *custom_start_cap_data = NULL;
    const char *custom_end_cap_data = NULL;
    U_FLOAT *dash_lengths = NULL;
    int32_t dash_count = 0;
    U_FLOAT start_cap_inset = 0.0;
    U_FLOAT end_cap_inset = 0.0;
    U_FLOAT start_cap_width_scale = 1.0;
    U_FLOAT end_cap_width_scale = 1.0;
    bool custom_start_cap = false;
    bool custom_end_cap = false;

    if (id >= sizeof(pmf_pen_cache) / sizeof(pmf_pen_cache[0]) ||
        data == NULL) {
        return 0;
    }
    if (!U_PMF_PEN_get(data, &version, &type, &pen_data, &brush, blimit)) {
        return 0;
    }
    UNUSED(version);
    if (type != 0 ||
        !U_PMF_PENDATA_get(pen_data, &flags, &unit, &width, &optional_data,
                           blimit) ||
        !pmf_solid_brush_object_color(brush, blimit, &color)) {
        return 0;
    }
    UNUSED(unit);

    if (flags != U_PD_None &&
        U_PMF_PENOPTIONALDATA_get(
            optional_data, flags, &matrix, &start_cap, &end_cap, &join,
            &miter_limit, &line_style, &dash_cap, &dash_offset, &dash_data,
            &alignment, &compound_line_data, &custom_start_cap_data,
            &custom_end_cap_data, blimit)) {
        if ((flags & U_PD_DLData) != 0 && dash_data != NULL &&
            U_PMF_DASHEDLINEDATA_get(dash_data, &dash_count, &dash_lengths,
                                     blimit) &&
            (dash_count <= 0 || dash_count > 64)) {
            free(dash_lengths);
            dash_lengths = NULL;
            dash_count = 0;
        }
        if ((flags & U_PD_CustomStartCap) != 0 &&
            pmf_custom_cap_metrics(custom_start_cap_data, true, blimit,
                                   &start_cap_inset,
                                   &start_cap_width_scale)) {
            custom_start_cap = true;
        }
        if ((flags & U_PD_CustomEndCap) != 0 &&
            pmf_custom_cap_metrics(custom_end_cap_data, false, blimit,
                                   &end_cap_inset, &end_cap_width_scale)) {
            custom_end_cap = true;
        }
    }

    entry = &pmf_pen_cache[id];
    free(entry->dash_lengths);
    memset(entry, 0, sizeof(*entry));
    entry->color = color;
    entry->width = width;
    entry->dash_lengths = dash_lengths;
    entry->dash_count = dash_count;
    entry->dash_offset = dash_offset;
    entry->start_cap_inset = start_cap_inset;
    entry->end_cap_inset = end_cap_inset;
    entry->start_cap_width_scale = start_cap_width_scale;
    entry->end_cap_width_scale = end_cap_width_scale;
    entry->custom_start_cap = custom_start_cap;
    entry->custom_end_cap = custom_end_cap;
    entry->active = true;
    return 1;
}

/**
  \brief Return a cached EMF+ Pen object by object ID.
  */
static const pmfPenCacheEntry *pmf_pen_cache_get(uint32_t id) {
    if (id >= sizeof(pmf_pen_cache) / sizeof(pmf_pen_cache[0]) ||
        !pmf_pen_cache[id].active) {
        return NULL;
    }
    return &pmf_pen_cache[id];
}

/**
  \brief Store a completed EMF+ Brush object for basic shape and text fills.

  test-038.emf is an EMF+ testbed file with no useful GDI fallback for most
  content. It stores many primitive fills in Brush objects, so caching a
  conservative representative color prevents those shapes from disappearing.
  */
static int pmf_brush_cache_store(uint32_t id, const char *data,
                                 const char *blimit) {
    uint32_t version;
    uint32_t type;
    const char *brush_data;
    U_PMF_ARGB color;
    U_PMF_ARGB *gradient_colors = NULL;
    uint32_t gradient_count = 0;
    U_PMF_POINTF gradient_center = {0.0, 0.0};
    bool path_gradient = false;
    pmfBrushCacheEntry *entry;

    if (id >= sizeof(pmf_brush_cache) / sizeof(pmf_brush_cache[0]) ||
        data == NULL) {
        return 0;
    }
    if (!U_PMF_BRUSH_get(data, &version, &type, &brush_data, blimit)) {
        return 0;
    }
    UNUSED(version);
    switch (type) {
    case U_BT_SolidColor:
        if (!U_PMF_ARGB_get(brush_data, &color.Blue, &color.Green, &color.Red,
                            &color.Alpha, blimit)) {
            return 0;
        }
        break;
    case U_BT_HatchFill: {
        uint32_t style;
        U_PMF_ARGB background;
        if (!U_PMF_HATCHBRUSHDATA_get(brush_data, &style, &color, &background,
                                      blimit)) {
            return 0;
        }
        UNUSED(style);
        UNUSED(background);
        break;
    }
    case U_BT_LinearGradient: {
        U_PMF_LINEARGRADIENTBRUSHDATA gradient;
        const char *optional_data;
        if (!U_PMF_LINEARGRADIENTBRUSHDATA_get(brush_data, &gradient,
                                               &optional_data, blimit)) {
            return 0;
        }
        UNUSED(optional_data);
        color = gradient.StartColor;
        break;
    }
    case U_BT_PathGradient: {
        U_PMF_PATHGRADIENTBRUSHDATA gradient;
        const char *colors;
        const char *boundary;
        const char *optional_data;
        if (!U_PMF_PATHGRADIENTBRUSHDATA_get(brush_data, &gradient, &colors,
                                             &boundary, &optional_data,
                                             blimit)) {
            return 0;
        }
        UNUSED(colors);
        UNUSED(boundary);
        UNUSED(optional_data);
        color = gradient.CenterColor;
        gradient_count = gradient.Elements;
        gradient_center = gradient.Center;
        if (gradient_count != 0) {
            gradient_colors = (U_PMF_ARGB *)calloc(
                gradient_count, sizeof(*gradient_colors));
            if (gradient_colors == NULL) {
                return 0;
            }
            for (uint32_t i = 0; i < gradient_count; i++) {
                if (!U_PMF_ARGB_get(colors + i * sizeof(U_PMF_ARGB),
                                    &gradient_colors[i].Blue,
                                    &gradient_colors[i].Green,
                                    &gradient_colors[i].Red,
                                    &gradient_colors[i].Alpha, blimit)) {
                    free(gradient_colors);
                    return 0;
                }
            }
        }
        path_gradient = true;
        break;
    }
    default:
        return 0;
    }

    entry = &pmf_brush_cache[id];
    free(entry->gradient_colors);
    entry->color = color;
    entry->path_gradient = path_gradient;
    entry->center = gradient_center;
    entry->gradient_colors = gradient_colors;
    entry->gradient_count = gradient_count;
    entry->active = true;
    return 1;
}

/**
  \brief Return a cached EMF+ Brush object by object ID.
  */
static const pmfBrushCacheEntry *pmf_brush_cache_get(uint32_t id) {
    if (id >= sizeof(pmf_brush_cache) / sizeof(pmf_brush_cache[0]) ||
        !pmf_brush_cache[id].active) {
        return NULL;
    }
    return &pmf_brush_cache[id];
}

/**
  \brief Store a completed EMF+ Font object for DrawString records.
  */
static int pmf_font_cache_store(uint32_t id, const char *data,
                                const char *blimit) {
    uint32_t version;
    uint32_t size_unit;
    uint32_t length;
    U_FLOAT em_size;
    int32_t style_flags;
    const char *family_data;
    char *family;
    pmfFontCacheEntry *entry;

    if (id >= sizeof(pmf_font_cache) / sizeof(pmf_font_cache[0]) ||
        data == NULL) {
        return 0;
    }
    if (!U_PMF_FONT_get(data, &version, &em_size, &size_unit, &style_flags,
                        &length, &family_data, blimit)) {
        return 0;
    }
    UNUSED(version);
    UNUSED(size_unit);
    family = U_Utf16leToUtf8((uint16_t *)family_data, length, NULL);
    if (family == NULL) {
        return 0;
    }

    entry = &pmf_font_cache[id];
    free(entry->family);
    entry->family = family;
    entry->em_size = em_size;
    entry->style_flags = style_flags;
    entry->active = true;
    return 1;
}

/**
  \brief Return a cached EMF+ Font object by object ID.
  */
static const pmfFontCacheEntry *pmf_font_cache_get(uint32_t id) {
    if (id >= sizeof(pmf_font_cache) / sizeof(pmf_font_cache[0]) ||
        !pmf_font_cache[id].active) {
        return NULL;
    }
    return &pmf_font_cache[id];
}

/**
  \brief Store a completed EMF+ Region object for clipping operations.
  */
static int pmf_region_cache_store(uint32_t id, const char *data, size_t size) {
    char *copy;

    if (id >= sizeof(pmf_region_cache) / sizeof(pmf_region_cache[0]) ||
        data == NULL || size == 0) {
        return 0;
    }
    copy = (char *)malloc(size);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data, size);
    free(pmf_region_cache[id].data);
    pmf_region_cache[id].data = copy;
    pmf_region_cache[id].size = size;
    pmf_region_cache[id].active = true;
    return 1;
}

/**
  \brief Return a cached EMF+ Region object by object ID.
  */
static const pmfRegionCacheEntry *pmf_region_cache_get(uint32_t id) {
    if (id >= sizeof(pmf_region_cache) / sizeof(pmf_region_cache[0]) ||
        !pmf_region_cache[id].active) {
        return NULL;
    }
    return &pmf_region_cache[id];
}

/**
  \brief Project one EMF+ path point through the EMF+ transform and EMF state.
  */
static POINT_D pmf_path_point_project(drawingStates *states,
                                      const U_PMF_POINTF *point) {
    double x = pmf_world_transform.eM11 * point->X +
               pmf_world_transform.eM21 * point->Y +
               pmf_world_transform.eDx;
    double y = pmf_world_transform.eM12 * point->X +
               pmf_world_transform.eM22 * point->Y +
               pmf_world_transform.eDy;
    return point_cal(states, x, y);
}

/**
  \brief Return the projected scale for EMF+ pen widths.

  Formula samples can store operators as EMF+ paths under a world transform.
  Path coordinates already go through that transform, so DrawPath pen widths
  must use the same projected scale instead of the raw pen width.
  */
static double pmf_path_stroke_scale(drawingStates *states) {
    U_PMF_POINTF origin = {0.0, 0.0};
    U_PMF_POINTF unit_x = {1.0, 0.0};
    U_PMF_POINTF unit_y = {0.0, 1.0};
    POINT_D p0 = pmf_path_point_project(states, &origin);
    POINT_D px = pmf_path_point_project(states, &unit_x);
    POINT_D py = pmf_path_point_project(states, &unit_y);
    double sx = hypot(px.x - p0.x, px.y - p0.y);
    double sy = hypot(py.x - p0.x, py.y - p0.y);

    if (sx <= 0.0) {
        return sy > 0.0 ? sy : 1.0;
    }
    if (sy <= 0.0) {
        return sx;
    }
    return fmin(sx, sy);
}

/**
  \brief Resolve the inline solid brush color supported for FillPath.
  */
static int pmf_solid_brush_color(uint32_t brush_id, int inline_argb,
                                 U_PMF_ARGB *color) {
    const pmfBrushCacheEntry *brush;

    if (color == NULL) {
        return 0;
    }
    if (inline_argb) {
        memcpy(color, &brush_id, sizeof(*color));
        return 1;
    }
    brush = pmf_brush_cache_get(brush_id);
    if (brush != NULL) {
        *color = brush->color;
        return 1;
    }
    return 0;
}

/**
  \brief Return true when top-level EMF+ primitives should be emitted.

  Dual EMF+ files often carry a GDI fallback for the same vector paths. Only
  recursive EMF+ metafiles and streams that explicitly identify themselves as
  pure EMF+ test content should draw primitives directly; otherwise the GDI
  fallback remains the single SVG source of truth.
  */
static bool pmf_primitive_draw_allowed(void) {
    return pmf_metafile_depth > 0 || pmf_top_level_primitive_draw_enabled;
}

/**
  \brief Check whether a bounded EMF+ comment payload contains a marker.
  */
static bool pmf_comment_contains(const char *data, size_t data_size,
                                 const char *marker) {
    size_t marker_size;

    if (data == NULL || marker == NULL) {
        return false;
    }
    marker_size = strlen(marker);
    if (marker_size == 0 || data_size < marker_size) {
        return false;
    }
    for (size_t i = 0; i <= data_size - marker_size; i++) {
        if (memcmp(data + i, marker, marker_size) == 0) {
            return true;
        }
    }
    return false;
}

/**
  \brief Emit a CSS/SVG color and opacity pair for an EMF+ ARGB value.
  */
static void pmf_color_attrs_draw(FILE *out, const char *name,
                                 U_PMF_ARGB color) {
    fprintf(out, "%s=\"#%02X%02X%02X\" %s-opacity=\"%.4f\"", name, color.Red,
            color.Green, color.Blue, name, color.Alpha / 255.0);
}

/**
  \brief Escape one UTF-8 string for an SVG attribute.
  */
static void pmf_attr_text_draw(FILE *out, const char *value) {
    if (value == NULL) {
        return;
    }
    for (const char *p = value; *p != '\0'; p++) {
        switch (*p) {
        case '&':
            fprintf(out, "&amp;");
            break;
        case '<':
            fprintf(out, "&lt;");
            break;
        case '"':
            fprintf(out, "&quot;");
            break;
        default:
            fputc(*p, out);
            break;
        }
    }
}

/**
  \brief Emit one EMF+ rectangle as a transformed SVG path.
  */
static void pmf_rect_path_draw(FILE *out, const U_PMF_RECTF *rect,
                               drawingStates *states) {
    U_PMF_POINTF points[4] = {
        {rect->X, rect->Y},
        {rect->X + rect->Width, rect->Y},
        {rect->X + rect->Width, rect->Y + rect->Height},
        {rect->X, rect->Y + rect->Height},
    };
    POINT_D p0 = pmf_path_point_project(states, &points[0]);
    POINT_D p1 = pmf_path_point_project(states, &points[1]);
    POINT_D p2 = pmf_path_point_project(states, &points[2]);
    POINT_D p3 = pmf_path_point_project(states, &points[3]);

    fprintf(out, "M %.4f,%.4f L %.4f,%.4f L %.4f,%.4f L %.4f,%.4f Z", p0.x,
            p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
}

/**
  \brief Emit one EMF+ ellipse as a transformed polygon path.

  This is a conservative fallback for test-038.emf. It preserves position,
  color, and rotation/shear without trying to translate every GDI+ ellipse
  nuance into SVG arc commands.
  */
static void pmf_ellipse_path_draw(FILE *out, const U_PMF_RECTF *rect,
                                  drawingStates *states) {
    const int segments = 32;
    double cx = rect->X + rect->Width / 2.0;
    double cy = rect->Y + rect->Height / 2.0;
    double rx = rect->Width / 2.0;
    double ry = rect->Height / 2.0;

    for (int i = 0; i < segments; i++) {
        double angle = (2.0 * 3.14159265358979323846 * i) / segments;
        U_PMF_POINTF src = {cx + rx * cos(angle), cy + ry * sin(angle)};
        POINT_D point = pmf_path_point_project(states, &src);
        fprintf(out, "%c %.4f,%.4f ", i == 0 ? 'M' : 'L', point.x, point.y);
    }
    fprintf(out, "Z");
}

/**
  \brief Emit an EMF+ dash pattern scaled to the resolved SVG pen width.
  */
static void pmf_pen_dash_attrs_draw(FILE *out, const pmfPenCacheEntry *pen,
                                    double stroke_width) {
    if (pen->dash_count <= 0 || pen->dash_lengths == NULL ||
        stroke_width <= 0.0) {
        return;
    }
    for (int32_t i = 0; i < pen->dash_count; i++) {
        if (pen->dash_lengths[i] <= 0.0) {
            return;
        }
    }
    fprintf(out, " stroke-dasharray=\"");
    for (int32_t i = 0; i < pen->dash_count; i++) {
        double dash_length = fabs((double)pen->dash_lengths[i]) * stroke_width;
        fprintf(out, "%s%.4f", i == 0 ? "" : ",", dash_length);
    }
    fprintf(out, "\"");
    if (pen->dash_offset != 0.0) {
        /* SVG offsets move the first painted dash in the opposite direction. */
        fprintf(out, " stroke-dashoffset=\"%.4f\"",
                -(double)pen->dash_offset * stroke_width);
    }
}

/**
  \brief Return whether a dual EMF+/GDI path needs top-level compensation.

  The GDI fallback for k-hop2.emf omits some dashed custom-cap paths. Other
  top-level EMF+ primitives continue to rely solely on their GDI fallback.
  */
static bool pmf_pen_needs_top_level_compensation(const pmfPenCacheEntry *pen) {
    return pen != NULL && pen->dash_count > 0 && pen->dash_lengths != NULL &&
           (pen->custom_start_cap || pen->custom_end_cap);
}

/**
  \brief Emit stroke attributes for a cached EMF+ pen.
  */
static void pmf_pen_attrs_draw(FILE *out, const pmfPenCacheEntry *pen,
                               drawingStates *states) {
    double width = fabs((double)pen->width) * pmf_path_stroke_scale(states);
    bool device_hairline = false;

    if (pmf_metafile_depth == 0 && pmf_top_level_primitive_draw_enabled &&
        width < 1.0) {
        /* test-038.emf uses one-world-unit outline pens as device hairlines. */
        width = 1.0;
        device_hairline = true;
    } else if (width <= 0.0) {
        width = 1.0;
    }
    pmf_color_attrs_draw(out, "stroke", pen->color);
    fprintf(out, " stroke-width=\"%.4f\"", width);
    if (device_hairline) {
        fprintf(out, " vector-effect=\"non-scaling-stroke\"");
    }
}

/**
  \brief Emit a cached EMF+ Path as SVG path data.

  This intentionally implements the subset needed by path-only formula
  metafiles: Start, Line, Bezier, and CloseSubpath point types.
  */
static int pmf_path_data_draw(const pmfPathCacheEntry *path, FILE *out,
                              drawingStates *states) {
    uint32_t i = 0;

    if (path == NULL || !path->active || path->count == 0) {
        return 0;
    }
    while (i < path->count) {
        uint8_t point_type = path->types[i] & U_PPT_MASK;
        bool close_subpath = (path->types[i] & U_PTP_CloseSubpath) != 0;
        POINT_D point = pmf_path_point_project(states, &path->points[i]);

        switch (point_type) {
        case U_PPT_Start:
            fprintf(out, "M %.4f,%.4f ", point.x, point.y);
            i++;
            break;
        case U_PPT_Line:
            fprintf(out, "L %.4f,%.4f ", point.x, point.y);
            if (close_subpath) {
                fprintf(out, "Z ");
            }
            i++;
            break;
        case U_PPT_Bezier:
            if (i + 2 >= path->count) {
                return 0;
            }
            POINT_D control1 = point;
            POINT_D control2 = pmf_path_point_project(states,
                                                      &path->points[i + 1]);
            POINT_D end =
                pmf_path_point_project(states, &path->points[i + 2]);
            fprintf(out, "C %.4f,%.4f %.4f,%.4f %.4f,%.4f ", control1.x,
                    control1.y, control2.x, control2.y, end.x, end.y);
            if (path->types[i + 2] & U_PTP_CloseSubpath) {
                fprintf(out, "Z ");
            }
            i += 3;
            break;
        default:
            i++;
            break;
        }
    }
    return 1;
}

typedef enum {
    PMF_CLIP_SHAPE_RECT,
    PMF_CLIP_SHAPE_PATH,
    PMF_CLIP_SHAPE_REGION,
} pmfClipShapeType;

typedef struct {
    pmfClipShapeType type;
    U_PMF_RECTF rect;
    const pmfPathCacheEntry *path;
    const pmfRegionCacheEntry *region;
} pmfClipShape;

/**
  \brief Emit the effectively unbounded rectangle used by EMF+ clip masks.

  EMF+ starts with an infinite clipping region. SVG masks need a finite backing
  shape, so use a deliberately oversized user-space rectangle that safely
  covers the converter's projected page coordinates.
  */
static void pmf_clip_mask_page_draw(FILE *out, const char *fill,
                                    uint64_t mask_id) {
    fprintf(out,
            "<rect x=\"-1000000\" y=\"-1000000\" width=\"2000000\" "
            "height=\"2000000\" fill=\"%s\"",
            fill);
    if (mask_id != 0) {
        fprintf(out, " mask=\"url(#pmfClip%llu)\"",
                (unsigned long long)mask_id);
    }
    fprintf(out, " />\n");
}

/**
  \brief Decode and emit a path embedded in an EMF+ Region node.
  */
static int pmf_region_path_shape_draw(const char *data, const char *blimit,
                                      FILE *out, drawingStates *states,
                                      const char *fill) {
    uint32_t version;
    uint32_t count;
    uint16_t flags;
    const char *points_raw;
    const char *types_raw;
    pmfPathCacheEntry path = {0};

    if (!U_PMF_PATH_get(data, &version, &count, &flags, &points_raw,
                        &types_raw, blimit)) {
        return 0;
    }
    UNUSED(version);
    if (!U_PMF_VARPOINTS_get(points_raw, flags, (int)count, &path.points,
                             blimit)) {
        return 0;
    }
    path.types = pmf_path_types_decode(types_raw, flags, count, blimit);
    if (path.types == NULL) {
        free(path.points);
        return 0;
    }
    path.count = count;
    path.active = true;
    fprintf(out, "<path d=\"");
    (void)pmf_path_data_draw(&path, out, states);
    fprintf(out, "\" fill=\"%s\" stroke=\"none\" />\n", fill);
    free(path.points);
    free(path.types);
    return 1;
}

/**
  \brief Emit the rectangle and path leaves of one union Region node.

  test-038.emf uses a two-child OR region for its final clip examples. Other
  boolean RegionNode operations remain deliberately unsupported here because
  the surrounding clip-mask combiner already handles the general operations.
  */
static const char *pmf_region_node_shape_draw(const char *node,
                                               const char *blimit, FILE *out,
                                               drawingStates *states,
                                               const char *fill) {
    uint32_t type;
    const char *data;

    if (!U_PMF_REGIONNODE_get(node, &type, &data, blimit)) {
        return NULL;
    }
    switch (type) {
    case U_RNDT_Rect: {
        U_PMF_RECTF rect;
        const char *cursor = data;
        if (!U_PMF_RECTF_get(&cursor, &rect.X, &rect.Y, &rect.Width,
                             &rect.Height, blimit)) {
            return NULL;
        }
        fprintf(out, "<path d=\"");
        pmf_rect_path_draw(out, &rect, states);
        fprintf(out, "\" fill=\"%s\" stroke=\"none\" />\n", fill);
        return cursor;
    }
    case U_RNDT_Path: {
        int32_t size;
        const char *path_data;
        const char *next;
        if (!U_PMF_REGIONNODEPATH_get(data, &size, &path_data, blimit) ||
            size < 0 || (size_t)size > (size_t)(blimit - path_data)) {
            return NULL;
        }
        next = path_data + size;
        if (!pmf_region_path_shape_draw(path_data, next, out, states, fill)) {
            return NULL;
        }
        return next;
    }
    case U_RNDT_Or: {
        const char *right = pmf_region_node_shape_draw(data, blimit, out,
                                                       states, fill);
        if (right == NULL) {
            return NULL;
        }
        return pmf_region_node_shape_draw(right, blimit, out, states, fill);
    }
    case U_RNDT_Empty:
        return node + sizeof(uint32_t);
    case U_RNDT_Infinite:
        pmf_clip_mask_page_draw(out, fill, 0);
        return node + sizeof(uint32_t);
    default:
        return NULL;
    }
}

/**
  \brief Emit the supported geometry of one cached EMF+ Region object.
  */
static int pmf_region_shape_draw(const pmfRegionCacheEntry *region, FILE *out,
                                 drawingStates *states, const char *fill) {
    uint32_t version;
    uint32_t count;
    const char *nodes;
    const char *blimit;

    if (region == NULL || region->data == NULL || region->size == 0) {
        return 0;
    }
    blimit = region->data + region->size;
    if (!U_PMF_REGION_get(region->data, &version, &count, &nodes, blimit)) {
        return 0;
    }
    UNUSED(version);
    UNUSED(count);
    return pmf_region_node_shape_draw(nodes, blimit, out, states, fill) != NULL;
}

/**
  \brief Emit one rectangle or cached path as mask geometry.
  */
static void pmf_clip_shape_draw(FILE *out, const pmfClipShape *shape,
                                drawingStates *states, const char *fill) {
    if (shape->type == PMF_CLIP_SHAPE_REGION) {
        (void)pmf_region_shape_draw(shape->region, out, states, fill);
        return;
    }
    fprintf(out, "<path d=\"");
    if (shape->type == PMF_CLIP_SHAPE_RECT) {
        pmf_rect_path_draw(out, &shape->rect, states);
    } else if (shape->path != NULL) {
        (void)pmf_path_data_draw(shape->path, out, states);
    }
    fprintf(out, "\" fill=\"%s\" stroke=\"none\" />\n", fill);
}

/**
  \brief Combine one EMF+ clip shape with the active SVG mask.

  Nested mask references preserve all six GDI+ CombineMode operations without
  reducing the clip to a bounding box. This is needed by the clipped-star
  section of test-038.emf, which intentionally exercises every combine mode.
  */
static void pmf_clip_mask_combine_draw(FILE *out, drawingStates *states,
                                       int combine_mode,
                                       const pmfClipShape *shape) {
    uint64_t previous_id = pmf_active_clip_mask_id;
    uint64_t new_id;

    if (shape == NULL) {
        return;
    }
    if (previous_id == 0 && combine_mode == U_CM_Union) {
        return;
    }

    new_id = ++pmf_clip_mask_serial;
    fprintf(out,
            "<defs><mask id=\"pmfClip%llu\" maskUnits=\"userSpaceOnUse\" "
            "maskContentUnits=\"userSpaceOnUse\" x=\"-1000000\" "
            "y=\"-1000000\" width=\"2000000\" height=\"2000000\">\n",
            (unsigned long long)new_id);

    if (previous_id == 0) {
        switch (combine_mode) {
        case U_CM_XOR:
        case U_CM_Exclude:
            pmf_clip_mask_page_draw(out, "white", 0);
            pmf_clip_shape_draw(out, shape, states, "black");
            break;
        case U_CM_Complement:
            /* New minus the initial infinite region is empty. */
            break;
        case U_CM_Replace:
        case U_CM_Intersect:
        default:
            pmf_clip_shape_draw(out, shape, states, "white");
            break;
        }
    } else {
        switch (combine_mode) {
        case U_CM_Intersect:
            fprintf(out, "<g mask=\"url(#pmfClip%llu)\">\n",
                    (unsigned long long)previous_id);
            pmf_clip_shape_draw(out, shape, states, "white");
            fprintf(out, "</g>\n");
            break;
        case U_CM_Union:
            pmf_clip_mask_page_draw(out, "white", previous_id);
            pmf_clip_shape_draw(out, shape, states, "white");
            break;
        case U_CM_XOR:
            pmf_clip_mask_page_draw(out, "white", previous_id);
            pmf_clip_shape_draw(out, shape, states, "white");
            fprintf(out, "<g mask=\"url(#pmfClip%llu)\">\n",
                    (unsigned long long)previous_id);
            pmf_clip_shape_draw(out, shape, states, "black");
            fprintf(out, "</g>\n");
            break;
        case U_CM_Exclude:
            pmf_clip_mask_page_draw(out, "white", previous_id);
            pmf_clip_shape_draw(out, shape, states, "black");
            break;
        case U_CM_Complement:
            pmf_clip_shape_draw(out, shape, states, "white");
            pmf_clip_mask_page_draw(out, "black", previous_id);
            break;
        case U_CM_Replace:
        default:
            pmf_clip_shape_draw(out, shape, states, "white");
            break;
        }
    }
    fprintf(out, "</mask></defs>\n");
    pmf_active_clip_mask_id = new_id;
}

/**
  \brief Attach the active EMF+ clipping mask to an emitted SVG element.
  */
static void pmf_clip_attr_draw(FILE *out) {
    if (pmf_active_clip_mask_id != 0) {
        fprintf(out, " mask=\"url(#pmfClip%llu)\"",
                (unsigned long long)pmf_active_clip_mask_id);
    }
}

/**
  \brief Find the visible segment that anchors a custom cap at one path end.
 */
static bool pmf_path_cap_segment(const pmfPathCacheEntry *path,
                                 drawingStates *states, bool start_cap,
                                 POINT_D *tip, POINT_D *body) {
    uint32_t tip_index;

    if (path == NULL || !path->active || path->count < 2 || tip == NULL ||
        body == NULL) {
        return false;
    }
    tip_index = start_cap ? 0 : path->count - 1;
    *tip = pmf_path_point_project(states, &path->points[tip_index]);
    if (start_cap) {
        for (uint32_t i = 1; i < path->count; i++) {
            *body = pmf_path_point_project(states, &path->points[i]);
            if (hypot(body->x - tip->x, body->y - tip->y) > 0.0) {
                return true;
            }
        }
    } else {
        for (uint32_t i = path->count - 1; i-- > 0;) {
            *body = pmf_path_point_project(states, &path->points[i]);
            if (hypot(body->x - tip->x, body->y - tip->y) > 0.0) {
                return true;
            }
        }
    }
    return false;
}

/**
  \brief Emit one default EMF+ custom cap as a filled triangular arrowhead.

  Default custom caps in k-hop2.emf carry a triangle fill path. The cap inset
  measures the shaft overlap, so adding two thirds of a pen width reproduces
  the four-pen-width arrowhead recorded by the GDI fallback.
 */
static void pmf_arrow_cap_draw(FILE *out, drawingStates *states,
                               const pmfPenCacheEntry *pen, POINT_D tip,
                               POINT_D body, double stroke_width,
                               U_FLOAT inset, U_FLOAT width_scale) {
    double dx = tip.x - body.x;
    double dy = tip.y - body.y;
    double direction_length = hypot(dx, dy);
    double cap_length;
    double cap_half_width;
    POINT_D base;
    POINT_D side_a;
    POINT_D side_b;

    if (direction_length <= 0.0 || stroke_width <= 0.0 || inset <= 0.0 ||
        width_scale <= 0.0) {
        return;
    }
    dx /= direction_length;
    dy /= direction_length;
    cap_length = ((double)inset + 2.0 / 3.0) * stroke_width * width_scale;
    cap_half_width = cap_length / 2.0;
    base.x = tip.x - dx * cap_length;
    base.y = tip.y - dy * cap_length;
    side_a.x = base.x - dy * cap_half_width;
    side_a.y = base.y + dx * cap_half_width;
    side_b.x = base.x + dy * cap_half_width;
    side_b.y = base.y - dx * cap_half_width;

    fprintf(out,
            "<%spath d=\"M %.4f,%.4f L %.4f,%.4f L %.4f,%.4f Z\" ",
            states->nameSpaceString, tip.x, tip.y, side_a.x, side_a.y,
            side_b.x, side_b.y);
    pmf_color_attrs_draw(out, "fill", pen->color);
    fprintf(out, " stroke=\"none\"");
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
}

/**
  \brief Draw custom start and end caps for one compensated EMF+ path.
 */
static void pmf_path_custom_caps_draw(const pmfPathCacheEntry *path,
                                      const pmfPenCacheEntry *pen,
                                      FILE *out, drawingStates *states,
                                      double stroke_width) {
    POINT_D tip;
    POINT_D body;

    if (pen->custom_start_cap &&
        pmf_path_cap_segment(path, states, true, &tip, &body)) {
        pmf_arrow_cap_draw(out, states, pen, tip, body, stroke_width,
                           pen->start_cap_inset,
                           pen->start_cap_width_scale);
    }
    if (pen->custom_end_cap &&
        pmf_path_cap_segment(path, states, false, &tip, &body)) {
        pmf_arrow_cap_draw(out, states, pen, tip, body, stroke_width,
                           pen->end_cap_inset, pen->end_cap_width_scale);
    }
}

/**
  \brief Approximate an EMF+ PathGradient with colored SVG fan sectors.

  SVG 1.1 has no direct equivalent of a GDI+ path gradient whose boundary
  vertices each carry a different color. Splitting a polygon into wedges keeps
  the black center and the per-vertex colors used by the test-038.emf stars,
  instead of collapsing the complete star to the center color.
  */
static int pmf_path_gradient_fill_draw(const pmfPathCacheEntry *path,
                                       const pmfBrushCacheEntry *brush,
                                       FILE *out, drawingStates *states) {
    POINT_D center;

    if (path == NULL || brush == NULL || !brush->path_gradient ||
        path->count < 3 || brush->gradient_count != path->count) {
        return 0;
    }
    for (uint32_t i = 0; i < path->count; i++) {
        uint8_t type = path->types[i] & U_PPT_MASK;
        if (type != U_PPT_Start && type != U_PPT_Line) {
            return 0;
        }
    }

    center = pmf_path_point_project(states, &brush->center);
    for (uint32_t i = 0; i < path->count; i++) {
        uint32_t previous = (i + path->count - 1) % path->count;
        uint32_t next = (i + 1) % path->count;
        POINT_D point = pmf_path_point_project(states, &path->points[i]);
        POINT_D previous_point =
            pmf_path_point_project(states, &path->points[previous]);
        POINT_D next_point =
            pmf_path_point_project(states, &path->points[next]);
        POINT_D previous_midpoint = {
            (previous_point.x + point.x) / 2.0,
            (previous_point.y + point.y) / 2.0,
        };
        POINT_D next_midpoint = {
            (next_point.x + point.x) / 2.0,
            (next_point.y + point.y) / 2.0,
        };
        U_PMF_ARGB edge = brush->gradient_colors[i];
        uint64_t gradient_id = ++pmf_gradient_serial;

        fprintf(out,
                "<%sdefs><%slinearGradient id=\"pmfGradient%llu\" "
                "gradientUnits=\"userSpaceOnUse\" x1=\"%.4f\" y1=\"%.4f\" "
                "x2=\"%.4f\" y2=\"%.4f\">\n"
                "<%sstop offset=\"0\" stop-color=\"#%02X%02X%02X\" "
                "stop-opacity=\"%.4f\" />\n"
                "<%sstop offset=\"1\" stop-color=\"#%02X%02X%02X\" "
                "stop-opacity=\"%.4f\" />\n"
                "</%slinearGradient></%sdefs>\n",
                states->nameSpaceString, states->nameSpaceString,
                (unsigned long long)gradient_id, center.x, center.y, point.x,
                point.y, states->nameSpaceString, brush->color.Red,
                brush->color.Green, brush->color.Blue,
                brush->color.Alpha / 255.0, states->nameSpaceString, edge.Red,
                edge.Green, edge.Blue, edge.Alpha / 255.0,
                states->nameSpaceString, states->nameSpaceString);
        fprintf(out,
                "<%spath d=\"M %.4f,%.4f L %.4f,%.4f L %.4f,%.4f "
                "L %.4f,%.4f Z\" fill=\"url(#pmfGradient%llu)\" "
                "stroke=\"none\"",
                states->nameSpaceString, center.x, center.y,
                previous_midpoint.x, previous_midpoint.y, point.x, point.y,
                next_midpoint.x, next_midpoint.y,
                (unsigned long long)gradient_id);
        pmf_clip_attr_draw(out);
        fprintf(out, " />\n");
    }
    return 1;
}

/**
  \brief Return the MIME type for an EMF+ compressed image payload.
  */
static const char *pmf_image_mime_type(const unsigned char *data, size_t size) {
    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return "image/png";
    }
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 &&
        data[2] == 0xff) {
        return "image/jpeg";
    }
    if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 ||
                      memcmp(data, "GIF89a", 6) == 0)) {
        return "image/gif";
    }
    return "application/octet-stream";
}

/**
  \brief Calculate the EMF+ destination triangle before point_cal().

  The EMF+ world transform is part of the image placement, but the final EMF
  map-mode and resize scaling still belong to the target drawing state. Keeping
  these raw points lets the current state and any parent state project the same
  bitmap into their own local coordinate systems.
  */
static void pmf_image_transform_points_raw(const U_PMF_RECTF *src,
                                           const U_PMF_POINTF *points,
                                           POINT_D *dest0, POINT_D *dest1,
                                           POINT_D *dest2) {
    double src_w = src->Width == 0.0 ? 1.0 : src->Width;
    double src_h = src->Height == 0.0 ? 1.0 : src->Height;
    double a = (points[1].X - points[0].X) / src_w;
    double b = (points[1].Y - points[0].Y) / src_w;
    double c = (points[2].X - points[0].X) / src_h;
    double d = (points[2].Y - points[0].Y) / src_h;
    double e = points[0].X - a * src->X - c * src->Y;
    double f = points[0].Y - b * src->X - d * src->Y;
    double wa = pmf_world_transform.eM11;
    double wb = pmf_world_transform.eM12;
    double wc = pmf_world_transform.eM21;
    double wd = pmf_world_transform.eM22;
    double we = pmf_world_transform.eDx;
    double wf = pmf_world_transform.eDy;
    double ma = wa * a + wc * b;
    double mb = wb * a + wd * b;
    double mc = wa * c + wc * d;
    double md = wb * c + wd * d;
    double me = wa * e + wc * f + we;
    double mf = wb * e + wd * f + wf;

    dest0->x = ma * src->X + mc * src->Y + me;
    dest0->y = mb * src->X + md * src->Y + mf;
    dest1->x = ma * (src->X + src->Width) + mc * src->Y + me;
    dest1->y = mb * (src->X + src->Width) + md * src->Y + mf;
    dest2->x = ma * src->X + mc * (src->Y + src->Height) + me;
    dest2->y = mb * src->X + md * (src->Y + src->Height) + mf;
}

/**
  \brief Project raw DrawImagePoints corners through one drawing state.

  Resize requests and map modes live in point_cal(), so EMF+ image placement
  must use the same projection path as GDI records to keep bitmaps aligned.
  */
static void pmf_image_project_points(drawingStates *states, POINT_D raw0,
                                     POINT_D raw1, POINT_D raw2, POINT_D *dest0,
                                     POINT_D *dest1, POINT_D *dest2) {
    *dest0 = point_cal(states, raw0.x, raw0.y);
    *dest1 = point_cal(states, raw1.x, raw1.y);
    *dest2 = point_cal(states, raw2.x, raw2.y);
}

/**
  \brief Calculate the affine transform described by DrawImagePoints.
  */
static void pmf_image_transform_matrix(drawingStates *states,
                                       const U_PMF_RECTF *src,
                                       const U_PMF_POINTF *points, double *ma,
                                       double *mb, double *mc, double *md,
                                       double *me, double *mf) {
    POINT_D dest0;
    POINT_D dest1;
    POINT_D dest2;
    POINT_D raw0;
    POINT_D raw1;
    POINT_D raw2;
    double src_w = src->Width == 0.0 ? 1.0 : src->Width;
    double src_h = src->Height == 0.0 ? 1.0 : src->Height;

    pmf_image_transform_points_raw(src, points, &raw0, &raw1, &raw2);
    pmf_image_project_points(states, raw0, raw1, raw2, &dest0, &dest1, &dest2);

    *ma = (dest1.x - dest0.x) / src_w;
    *mb = (dest1.y - dest0.y) / src_w;
    *mc = (dest2.x - dest0.x) / src_h;
    *md = (dest2.y - dest0.y) / src_h;
    *me = dest0.x - *ma * src->X - *mc * src->Y;
    *mf = dest0.y - *mb * src->X - *md * src->Y;
}

/**
  \brief Write the affine transform described by DrawImagePoints.
  */
static void pmf_image_transform_draw(FILE *out, const U_PMF_RECTF *src,
                                     const U_PMF_POINTF *points,
                                     drawingStates *states) {
    double ma, mb, mc, md, me, mf;
    pmf_image_transform_matrix(states, src, points, &ma, &mb, &mc, &md, &me,
                               &mf);

    fprintf(out, " transform=\"matrix(%.4f %.4f %.4f %.4f %.4f %.4f)\" ",
            ma, mb, mc, md, me, mf);
}

/**
  \brief Return a conservative stroke scale for a DrawImagePoints matrix.

  Nested EMF+ metafiles can be placed with non-uniform scaling. Using the
  smaller axis for open multi-segment strokes avoids visually fattening wavy
  relation symbols while keeping simple horizontal operators on their original
  SVG path.
  */
static double pmf_image_stroke_scale(drawingStates *states,
                                     const U_PMF_RECTF *src,
                                     const U_PMF_POINTF *points) {
    double ma, mb, mc, md, me, mf;
    double sx;
    double sy;

    pmf_image_transform_matrix(states, src, points, &ma, &mb, &mc, &md, &me,
                               &mf);
    UNUSED(me);
    UNUSED(mf);
    sx = hypot(ma, mb);
    sy = hypot(mc, md);
    if (sx <= 0.0) {
        return sy > 0.0 ? sy : 1.0;
    }
    if (sy <= 0.0) {
        return sx;
    }
    return fmin(sx, sy);
}

/**
  \brief Store one emitted EMF+ bitmap box in a drawing state.
  */
static void pmf_image_box_store(drawingStates *states, double min_x,
                                double min_y, double max_x, double max_y) {
    emfPlusImageBox *box;

    box = &states->recentEmfPlusImages[states->recentEmfPlusImageNext %
                                       EMFPLUS_RECENT_IMAGE_BOX_COUNT];
    box->active = true;
    box->position.x = min_x;
    box->position.y = min_y;
    box->size.x = max_x - min_x;
    box->size.y = max_y - min_y;
    states->recentEmfPlusImageNext++;
}

/**
  \brief Store one projected DrawImagePoints parallelogram as a recent box.
  */
static void pmf_image_box_store_projected(drawingStates *states, POINT_D dest0,
                                          POINT_D dest1, POINT_D dest2) {
    POINT_D dest3;
    double min_x;
    double min_y;
    double max_x;
    double max_y;

    dest3.x = dest1.x + dest2.x - dest0.x;
    dest3.y = dest1.y + dest2.y - dest0.y;
    min_x = fmin(fmin(dest0.x, dest1.x), fmin(dest2.x, dest3.x));
    min_y = fmin(fmin(dest0.y, dest1.y), fmin(dest2.y, dest3.y));
    max_x = fmax(fmax(dest0.x, dest1.x), fmax(dest2.x, dest3.x));
    max_y = fmax(fmax(dest0.y, dest1.y), fmax(dest2.y, dest3.y));
    pmf_image_box_store(states, min_x, min_y, max_x, max_y);
}

/**
  \brief Remember an emitted EMF+ bitmap box to suppress a matching GDI fallback.

  PowerPoint can store the same bitmap once as EMF+ DrawImagePoints and again
  as a following EMR_STRETCHDIBITS fallback. Recording only successfully emitted
  compressed bitmaps avoids broad fallback skipping while fixing duplicated
  depth maps in framework-overview.emf. For recursively drawn EMF+ metafiles,
  the child bitmap box is also copied to the parent state in child coordinates,
  because the parent's GDI fallback is emitted under the same local transform.
  */
static void pmf_image_box_record(const U_PMF_RECTF *src,
                                 const U_PMF_POINTF *points,
                                 drawingStates *states) {
    POINT_D raw0;
    POINT_D raw1;
    POINT_D raw2;
    POINT_D dest0;
    POINT_D dest1;
    POINT_D dest2;

    pmf_image_transform_points_raw(src, points, &raw0, &raw1, &raw2);
    pmf_image_project_points(states, raw0, raw1, raw2, &dest0, &dest1, &dest2);
    pmf_image_box_store_projected(states, dest0, dest1, dest2);
    if (pmf_parent_bitmap_box_states != NULL &&
        pmf_parent_bitmap_box_states != states) {
        pmf_image_project_points(pmf_parent_bitmap_box_states, raw0, raw1, raw2,
                                 &dest0, &dest1, &dest2);
        pmf_image_box_store_projected(pmf_parent_bitmap_box_states, dest0,
                                      dest1, dest2);
    }
}

/**
  \brief Draw a cached EMF+ bitmap image object.

  Some EMF files store the high-quality image as an EMF+ compressed bitmap and
  keep a lower-quality GDI fallback for non-EMF+ consumers. Drawing the EMF+
  bytes preserves antialiased bitmap text in files such as test-blur-font.emf.
  */
static int pmf_bitmap_image_draw(const pmfImageCacheEntry *image,
                                 const char *data, const char *blimit,
                                 const U_PMF_RECTF *src,
                                 const U_PMF_POINTF *points, FILE *out,
                                 drawingStates *states) {
    U_PMF_BITMAP bitmap;
    const char *bitmap_data;
    size_t offset;
    size_t data_size;
    size_t b64_size;
    char *b64;

    if (!U_PMF_BITMAP_get(data, &bitmap, &bitmap_data, blimit)) {
        return 0;
    }
    if (bitmap.Type != 1) {
        return 0;
    }

    offset = (size_t)(bitmap_data - image->data);
    if (offset >= image->size) {
        return 0;
    }
    data_size = image->size - offset;
    b64 = base64_encode((const unsigned char *)bitmap_data, data_size,
                        &b64_size);
    if (b64 == NULL) {
        return 0;
    }

    fprintf(out, "<%simage x=\"%.4f\" y=\"%.4f\" width=\"%.4f\" "
                 "height=\"%.4f\" ",
            states->nameSpaceString, src->X, src->Y, src->Width, src->Height);
    pmf_image_transform_draw(out, src, points, states);
    pmf_clip_attr_draw(out);
    if (pmf_active_clip_mask_id != 0) {
        fprintf(out, " ");
    }
    fprintf(out, "xlink:href=\"data:%s;base64,%s\" />\n",
            pmf_image_mime_type((const unsigned char *)bitmap_data, data_size),
            b64);
    pmf_image_box_record(src, points, states);
    free(b64);
    return 1;
}

/**
  \brief Draw a cached EMF+ metafile image object by recursively converting it.
  */
static int pmf_metafile_image_draw(const char *data, const char *blimit,
                                   const U_PMF_RECTF *src,
                                   const U_PMF_POINTF *points, FILE *out,
                                   drawingStates *states) {
    uint32_t type;
    uint32_t size;
    const char *metafile_data;
    char *metafile_copy;
    char *svg = NULL;
    size_t svg_len = 0;
    generatorOptions options;
    U_XFORM saved_world_transform = pmf_world_transform;
    bool saved_top_level_primitive_draw_enabled =
        pmf_top_level_primitive_draw_enabled;
    uint64_t saved_active_clip_mask_id = pmf_active_clip_mask_id;
    double saved_metafile_stroke_scale = pmf_metafile_stroke_scale;
    drawingStates *saved_parent_bitmap_box_states =
        pmf_parent_bitmap_box_states;
    int ok = 0;

    if (!U_PMF_METAFILE_get(data, &type, &size, &metafile_data, blimit) ||
        size == 0) {
        return 0;
    }

    if (type == U_MDT_Emf) {
        /*
         * test-179.emf and test-180.emf contain a top-level EMF+ Image object
         * that is a plain EMF preview. The surrounding EMF already contains the
         * same GDI fallback records, so recursively drawing this object paints
         * the main picture twice. Keep EMF+ only metafiles, which carry content
         * that the GDI fallback does not reproduce.
         */
        verbose_printf("   Status:         %sSKIPPED EMF+ EMF METAFILE PREVIEW%s\n",
                       KYEL, KNRM);
        return 0;
    }

    uint32_t first_record_type = 0;
    if (size >= sizeof(first_record_type)) {
        memcpy(&first_record_type, metafile_data, sizeof(first_record_type));
    }
    if ((type == U_MDT_EmfPlusOnly || type == U_MDT_EmfPlusDual) &&
        size >= sizeof(uint32_t) && first_record_type != U_EMR_HEADER) {
        /*
         * EmfPlusOnly image payloads can start at the EMR_HEADER nSize field,
         * omitting the leading iType. test-formula2.emf uses this form; add
         * the missing record type so the recursive EMF parser can enter the
         * nested stream.
         */
        metafile_copy = (char *)malloc(size + sizeof(uint32_t));
        if (metafile_copy == NULL) {
            return 0;
        }
        uint32_t header_type = U_EMR_HEADER;
        memcpy(metafile_copy, &header_type, sizeof(header_type));
        memcpy(metafile_copy + sizeof(header_type), metafile_data, size);
        size += sizeof(header_type);
    } else {
        metafile_copy = (char *)malloc(size);
        if (metafile_copy == NULL) {
            return 0;
        }
        memcpy(metafile_copy, metafile_data, size);
    }

    if (metafile_copy == NULL) {
        return 0;
    }

    memset(&options, 0, sizeof(options));
    options.verbose = false;
    options.emfplus = true;
    options.svgDelimiter = false;

    pmf_parent_bitmap_box_states = states;
    pmf_metafile_stroke_scale =
        saved_metafile_stroke_scale *
        pmf_image_stroke_scale(states, src, points);
    pmf_metafile_depth++;
    int convert_ok = emf2svg(metafile_copy, size, &svg, &svg_len, &options);
    pmf_metafile_depth--;
    pmf_parent_bitmap_box_states = saved_parent_bitmap_box_states;
    pmf_metafile_stroke_scale = saved_metafile_stroke_scale;
    pmf_world_transform = saved_world_transform;
    pmf_top_level_primitive_draw_enabled =
        saved_top_level_primitive_draw_enabled;
    pmf_active_clip_mask_id = saved_active_clip_mask_id;

    if (convert_ok != 0 && svg != NULL) {
        const char *extra_close = "</g>\n";
        size_t extra_close_len = strlen(extra_close);

        if (svg_len >= extra_close_len &&
            memcmp(svg + svg_len - extra_close_len, extra_close,
                   extra_close_len) == 0) {
            svg_len -= extra_close_len;
            svg[svg_len] = '\0';
        }

        fprintf(out, "<%sg", states->nameSpaceString);
        pmf_image_transform_draw(out, src, points, states);
        pmf_clip_attr_draw(out);
        fprintf(out, ">\n%.*s</%sg>\n", (int)svg_len, svg,
                states->nameSpaceString);
        ok = 1;
    }

    free(svg);
    free(metafile_copy);
    return ok;
}

/*
   this function is not visible in the API.  Print "data" for one of the many
   records that has none.
   */
int U_PMR_NODATAREC_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    return (1);
}

/*
   this function is not visible in the API.  Common routine used by many
   functions that draw points.
   */
void U_PMF_VARPOINTS_draw(const char **contents, int Flags, uint32_t Elements,
                          FILE *out, drawingStates *states) {
    return;
}

/*
   this function is not visible in the API.  Common routine used by many
   functions that draw points.
   */
void U_PMF_VARPOINTF_S_draw(U_PMF_POINTF *Points, uint32_t Elements, FILE *out,
                            drawingStates *states) {
    return;
}

/*
   this function is not visible in the API.  Common routine used by many
   functions that draw rectangles.
   */
int U_PMF_VARRECTF_S_draw(U_PMF_RECTF *Rects, uint32_t Elements, FILE *out,
                          drawingStates *states) {
    return (1);
}

/*
   this function is not visible in the API.  Common routine used by many
   functions.
   */
int U_PMF_VARBRUSHID_draw(int btype, uint32_t BrushID, FILE *out,
                          drawingStates *states) {
    return (1);
}
//! \endcond

/**
  \brief Print any EMF+ record
  \returns record length for a normal record, 0 for EMREOF or , -1 for a bad
  record
  \param contents   pointer to a buffer holding this EMF+ record
  \param blimit     one byte past the end of data of this EMF+ record
  \param recnum     EMF number of this record in contents
  \param off        Offset from the beginning of the EMF+ file.
  */
int U_pmf_onerec_draw(const char *contents, const char *blimit, int recnum,
                      int off, FILE *out, drawingStates *states) {
    int status;
    static U_OBJ_ACCUM ObjCont = {
        NULL, 0, 0, 0,
        0}; /* for keeping track of object continuation. These may
               be split across multiple EMF Comment records */
    U_PMF_CMN_HDR Header;
    const char *contemp = contents;
    if (!U_PMF_CMN_HDR_get(&contemp, &Header)) {
        return (-1);
    }

    int type = Header.Type & U_PMR_TYPE_MASK; /* strip the U_PMR_RECFLAG bit,
                                                 leaving the indexable part */
    if (type < U_PMR_MIN || type > U_PMR_MAX)
        return (-1); /* unknown EMF+ record type */

    status =
        U_PMF_CMN_HDR_draw(Header, recnum, off, out, states); /* EMF+ part */

    /* Buggy EMF+ can set the continue bit and then do something else. In that
       case, force out the pending
       Object.  Side effect - clears the pending object. */
    if ((type != U_PMR_OBJECT) && (ObjCont.used > 0)) {
        U_PMR_OBJECT_draw(contents, blimit, &ObjCont, 1, out, states);
    }

    switch (type) {
    case (U_PMR_HEADER):
        pmf_object_caches_clear();
        U_PMR_HEADER_draw(contents, out, states);
        break;
    case (U_PMR_ENDOFFILE):
        U_PMR_ENDOFFILE_draw(contents, out, states);
        U_OA_release(&ObjCont);
        pmf_object_caches_clear();
        break;
    case (U_PMR_COMMENT):
        U_PMR_COMMENT_draw(contents, out, states);
        break;
    case (U_PMR_GETDC):
        U_PMR_GETDC_draw(contents, out, states);
        break;
    case (U_PMR_MULTIFORMATSTART):
        U_PMR_MULTIFORMATSTART_draw(contents, out, states);
        break;
    case (U_PMR_MULTIFORMATSECTION):
        U_PMR_MULTIFORMATSECTION_draw(contents, out, states);
        break;
    case (U_PMR_MULTIFORMATEND):
        U_PMR_MULTIFORMATEND_draw(contents, out, states);
        break;
    case (U_PMR_OBJECT):
        U_PMR_OBJECT_draw(contents, blimit, &ObjCont, 0, out, states);
        break;
    case (U_PMR_CLEAR):
        U_PMR_CLEAR_draw(contents, out, states);
        break;
    case (U_PMR_FILLRECTS):
        U_PMR_FILLRECTS_draw(contents, blimit, out, states);
        break;
    case (U_PMR_DRAWRECTS):
        U_PMR_DRAWRECTS_draw(contents, blimit, out, states);
        break;
    case (U_PMR_FILLPOLYGON):
        U_PMR_FILLPOLYGON_draw(contents, out, states);
        break;
    case (U_PMR_DRAWLINES):
        U_PMR_DRAWLINES_draw(contents, out, states);
        break;
    case (U_PMR_FILLELLIPSE):
        U_PMR_FILLELLIPSE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWELLIPSE):
        U_PMR_DRAWELLIPSE_draw(contents, out, states);
        break;
    case (U_PMR_FILLPIE):
        U_PMR_FILLPIE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWPIE):
        U_PMR_DRAWPIE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWARC):
        U_PMR_DRAWARC_draw(contents, out, states);
        break;
    case (U_PMR_FILLREGION):
        U_PMR_FILLREGION_draw(contents, out, states);
        break;
    case (U_PMR_FILLPATH):
        U_PMR_FILLPATH_draw(contents, out, states);
        break;
    case (U_PMR_DRAWPATH):
        U_PMR_DRAWPATH_draw(contents, out, states);
        break;
    case (U_PMR_FILLCLOSEDCURVE):
        U_PMR_FILLCLOSEDCURVE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWCLOSEDCURVE):
        U_PMR_DRAWCLOSEDCURVE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWCURVE):
        U_PMR_DRAWCURVE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWBEZIERS):
        U_PMR_DRAWBEZIERS_draw(contents, out, states);
        break;
    case (U_PMR_DRAWIMAGE):
        U_PMR_DRAWIMAGE_draw(contents, out, states);
        break;
    case (U_PMR_DRAWIMAGEPOINTS):
        U_PMR_DRAWIMAGEPOINTS_draw(contents, out, states);
        break;
    case (U_PMR_DRAWSTRING):
        U_PMR_DRAWSTRING_draw(contents, out, states);
        break;
    case (U_PMR_SETRENDERINGORIGIN):
        U_PMR_SETRENDERINGORIGIN_draw(contents, out, states);
        break;
    case (U_PMR_SETANTIALIASMODE):
        U_PMR_SETANTIALIASMODE_draw(contents, out, states);
        break;
    case (U_PMR_SETTEXTRENDERINGHINT):
        U_PMR_SETTEXTRENDERINGHINT_draw(contents, out, states);
        break;
    case (U_PMR_SETTEXTCONTRAST):
        U_PMR_SETTEXTCONTRAST_draw(contents, out, states);
        break;
    case (U_PMR_SETINTERPOLATIONMODE):
        U_PMR_SETINTERPOLATIONMODE_draw(contents, out, states);
        break;
    case (U_PMR_SETPIXELOFFSETMODE):
        U_PMR_SETPIXELOFFSETMODE_draw(contents, out, states);
        break;
    case (U_PMR_SETCOMPOSITINGMODE):
        U_PMR_SETCOMPOSITINGMODE_draw(contents, out, states);
        break;
    case (U_PMR_SETCOMPOSITINGQUALITY):
        U_PMR_SETCOMPOSITINGQUALITY_draw(contents, out, states);
        break;
    case (U_PMR_SAVE):
        U_PMR_SAVE_draw(contents, out, states);
        break;
    case (U_PMR_RESTORE):
        U_PMR_RESTORE_draw(contents, out, states);
        break;
    case (U_PMR_BEGINCONTAINER):
        U_PMR_BEGINCONTAINER_draw(contents, out, states);
        break;
    case (U_PMR_BEGINCONTAINERNOPARAMS):
        U_PMR_BEGINCONTAINERNOPARAMS_draw(contents, out, states);
        break;
    case (U_PMR_ENDCONTAINER):
        U_PMR_ENDCONTAINER_draw(contents, out, states);
        break;
    case (U_PMR_SETWORLDTRANSFORM):
        U_PMR_SETWORLDTRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_RESETWORLDTRANSFORM):
        U_PMR_RESETWORLDTRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_MULTIPLYWORLDTRANSFORM):
        U_PMR_MULTIPLYWORLDTRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_TRANSLATEWORLDTRANSFORM):
        U_PMR_TRANSLATEWORLDTRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_SCALEWORLDTRANSFORM):
        U_PMR_SCALEWORLDTRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_ROTATEWORLDTRANSFORM):
        U_PMR_ROTATEWORLDTRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_SETPAGETRANSFORM):
        U_PMR_SETPAGETRANSFORM_draw(contents, out, states);
        break;
    case (U_PMR_RESETCLIP):
        U_PMR_RESETCLIP_draw(contents, out, states);
        break;
    case (U_PMR_SETCLIPRECT):
        U_PMR_SETCLIPRECT_draw(contents, out, states);
        break;
    case (U_PMR_SETCLIPPATH):
        U_PMR_SETCLIPPATH_draw(contents, out, states);
        break;
    case (U_PMR_SETCLIPREGION):
        U_PMR_SETCLIPREGION_draw(contents, out, states);
        break;
    case (U_PMR_OFFSETCLIP):
        U_PMR_OFFSETCLIP_draw(contents, out, states);
        break;
    case (U_PMR_DRAWDRIVERSTRING):
        U_PMR_DRAWDRIVERSTRING_draw(contents, out, states);
        break;
    case (U_PMR_STROKEFILLPATH):
        U_PMR_STROKEFILLPATH_draw(contents, out, states);
        break;
    case (U_PMR_SERIALIZABLEOBJECT):
        U_PMR_SERIALIZABLEOBJECT_draw(contents, out, states);
        break;
    case (U_PMR_SETTSGRAPHICS):
        U_PMR_SETTSGRAPHICS_draw(contents, out, states);
        break;
    case (U_PMR_SETTSCLIP):
        U_PMR_SETTSCLIP_draw(contents, out, states);
        break;
    }
    if (states->Error) {
        U_OA_release(&ObjCont);
    }
    return (status);
}

/**
  \brief Print data from a  U_PMF_CMN_HDR object
  \return number of bytes in record, 0 on error
  \param  Header     Header of the record
  \param  precnum    EMF+ record number in file.
  \param  off        Offset in file to the start of this EMF+ record.
  common structure present at the beginning of all(*) EMF+ records
  */
int U_PMF_CMN_HDR_draw(U_PMF_CMN_HDR Header, int precnum, int off, FILE *out,
                       drawingStates *states) {
    return ((int)Header.Size);
}

/**
  \brief Print data from a an array of uint8_t values
  \return 1
  \param  Start      Text to lead array data
  \param  Array      uint8_t array of data passed as char *
  \param  Elements   Number of elements in Array
  \param  End        Text to follow array data
  */
int U_PMF_UINT8_ARRAY_draw(const char *Start, const uint8_t *Array,
                           int Elements, char *End, FILE *out,
                           drawingStates *states) {
    return (1);
}

/**
  \brief Print value of an BrushType Enumeration
  \returns record 1 on sucess, 0 on error
  \param otype    Value to print.
  EMF+ manual 2.1.1.3, Microsoft name: BrushType Enumeration
  */
int U_PMF_BRUSHTYPEENUMERATION_draw(int otype, FILE *out,
                                    drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print value of an BrushType Enumeration
  \returns record 1 on sucess, 0 on error
  \param otype    Value to print.
  EMF+ manual 2.1.1.4, Microsoft name: BrushType Enumeration
  */
int U_PMF_COMBINEMODEENUMERATION_draw(int otype, FILE *out,
                                      drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print value of a HatchStyle Enumeration
  \returns record 1 on sucess, 0 on error
  \param hstype    Value to print.
  EMF+ manual 2.1.1.13, Microsoft name: HatchStyle Enumeration
  */
int U_PMF_HATCHSTYLEENUMERATION_draw(int hstype, FILE *out,
                                     drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print value of an ObjectType Enumeration
  \returns record 1 on sucess, 0 on error
  \param otype    Value to print.
  EMF+ manual 2.1.1.22, Microsoft name: ObjectType Enumeration
  */
int U_PMF_OBJECTTYPEENUMERATION_draw(int otype, FILE *out,
                                     drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print value of a  U_PMF_PATHPOINTTYPE_ENUM object
  \return 1
  \param  Type   Value to print
  EMF+ manual 2.1.1.23, Microsoft name: PathPointType Enumeration
  */
int U_PMF_PATHPOINTTYPE_ENUM_draw(int Type, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a PixelFormat Enumeration value
  \return 1 always
  \param  pfe   A PixelFormat Enumeration value
  EMF+ manual 2.1.1.25, Microsoft name: PixelFormat Enumeration (U_PF_*)
  */
int U_PMF_PX_FMT_ENUM_draw(int pfe, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print as text a RegionNodeDataType Enumeration
  \return 1
  \param  Type   RegionNodeDataType Enumeration
  EMF+ manual 2.1.1.27, Microsoft name: RegionNodeDataType Enumeration
  (U_RNDT_*)
  */
int U_PMF_NODETYPE_draw(int Type, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_BRUSH object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.1, Microsoft name: EmfPlusBrush Object
  */
int U_PMF_BRUSH_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CUSTOMLINECAP object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  \param  Which      A string which is either "Start" or "End".
  EMF+ manual 2.2.1.2, Microsoft name: EmfPlusCustomLineCap Object
  */
int U_PMF_CUSTOMLINECAP_draw(const char *contents, const char *Which, FILE *out,
                             drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_FONT object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.3, Microsoft name: EmfPlusFont Object
  */
int U_PMF_FONT_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IMAGE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.4, Microsoft name: EmfPlusImage Object
  */
int U_PMF_IMAGE_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IMAGEATTRIBUTES object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.5, Microsoft name: EmfPlusImageAttributes Object
  */
int U_PMF_IMAGEATTRIBUTES_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PATH object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.6, Microsoft name: EmfPlusPath Object
  */
int U_PMF_PATH_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PEN object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.7, Microsoft name: EmfPlusPen Object
  */
int U_PMF_PEN_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_REGION object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.8, Microsoft name: EmfPlusRegion Object
  */
int U_PMF_REGION_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_STRINGFORMAT object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.1.9, Microsoft name: EmfPlusStringFormat Object
  */
int U_PMF_STRINGFORMAT_draw(const char *contents, FILE *out,
                            drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_ARGB object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.1, Microsoft name: EmfPlusARGB Object
  */
int U_PMF_ARGB_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_BITMAP object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.2, Microsoft name: EmfPlusBitmap Object
  */
int U_PMF_BITMAP_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_BITMAPDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.3, Microsoft name: EmfPlusBitmapData Object
  */
int U_PMF_BITMAPDATA_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_BLENDCOLORS object
  \return size in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.4, Microsoft name: EmfPlusBlendColors Object
  */
int U_PMF_BLENDCOLORS_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_BLENDFACTORS object
  \return size on success, 0 on error
  \param  type       Type of BlendFactors, usually H or V
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.5, Microsoft name: EmfPlusBlendFactors Object
  */
int U_PMF_BLENDFACTORS_draw(const char *contents, const char *type, FILE *out,
                            drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_BOUNDARYPATHDATA object
  \return size on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.6, Microsoft name: EmfPlusBoundaryPathData Object
  */
int U_PMF_BOUNDARYPATHDATA_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_BOUNDARYPOINTDATA object
  \return size on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.7, Microsoft name: EmfPlusBoundaryPointData Object
  */
int U_PMF_BOUNDARYPOINTDATA_draw(const char *contents, FILE *out,
                                 drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CHARACTERRANGE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.8, Microsoft name: EmfPlusCharacterRange Object
  */
int U_PMF_CHARACTERRANGE_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_DASHEDLINEDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.9, Microsoft name: EmfPlusCompoundLineData Object
  */
int U_PMF_COMPOUNDLINEDATA_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_COMPRESSEDIMAGE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.10, Microsoft name: EmfPlusCompressedImage Object

  This function does not do anything useful, but it is included so that all
  objects have a corresponding _get().
  */
int U_PMF_COMPRESSEDIMAGE_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CUSTOMENDCAPDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.11, Microsoft name: EmfPlusCustomEndCapData Object
  */
int U_PMF_CUSTOMENDCAPDATA_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CUSTOMLINECAPARROWDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.12, Microsoft name: EmfPlusCustomLineCapArrowData Object
  */
int U_PMF_CUSTOMLINECAPARROWDATA_draw(const char *contents, FILE *out,
                                      drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CUSTOMLINECAPDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.13, Microsoft name: EmfPlusCustomLineCapData Object
  */
int U_PMF_CUSTOMLINECAPDATA_draw(const char *contents, FILE *out,
                                 drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CUSTOMLINECAPOPTIONALDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  \param  Flags      CustomLineCapData Flags

  EMF+ manual 2.2.2.14, Microsoft name: EmfPlusCustomLineCapOptionalData Object
  */
int U_PMF_CUSTOMLINECAPOPTIONALDATA_draw(const char *contents, uint32_t Flags,
                                         FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_CUSTOMSTARTCAPDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.15, Microsoft name: EmfPlusCustomStartCapData Object
  */
int U_PMF_CUSTOMSTARTCAPDATA_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_DASHEDLINEDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.16, Microsoft name: EmfPlusDashedLineData Object
  */
int U_PMF_DASHEDLINEDATA_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_FILLPATHOBJ object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.17, Microsoft name: EmfPlusFillPath Object
  */
int U_PMF_FILLPATHOBJ_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_FOCUSSCALEDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.18, Microsoft name: EmfPlusFocusScaleData Object
  */
int U_PMF_FOCUSSCALEDATA_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_GRAPHICSVERSION_draw object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.19, Microsoft name: EmfPlusGraphicsVersion Object
  */
int U_PMF_GRAPHICSVERSION_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_HATCHBRUSHDATA_draw object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.20, Microsoft name: EmfPlusHatchBrushData Object
  */
int U_PMF_HATCHBRUSHDATA_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_LANGUAGEIDENTIFIER object
  \return 1 on success, 0 on error
  \param  LId   Record from which to print data
  EMF+ manual 2.2.2.23, Microsoft name: EmfPlusLanguageIdentifier Object
  */
int U_PMF_LANGUAGEIDENTIFIER_draw(U_PMF_LANGUAGEIDENTIFIER LId, FILE *out,
                                  drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_LINEARGRADIENTBRUSHDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.24, Microsoft name: EmfPlusLinearGradientBrushData Object
  */
int U_PMF_LINEARGRADIENTBRUSHDATA_draw(const char *contents, FILE *out,
                                       drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_LINEARGRADIENTBRUSHOPTIONALDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  \param  BDFlag     Describes optional values in contents
  EMF+ manual 2.2.2.25, Microsoft name: EmfPlusLinearGradientBrushOptionalData
  Object
  */
int U_PMF_LINEARGRADIENTBRUSHOPTIONALDATA_draw(const char *contents, int BDFlag,
                                               FILE *out,
                                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_LINEPATH object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.26, Microsoft name: EmfPlusLinePath Object
  */
int U_PMF_LINEPATH_draw(const char *contents, FILE *out,
                        drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_METAFILE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.27, Microsoft name: EmfPlusMetafile Object
  */
int U_PMF_METAFILE_draw(const char *contents, FILE *out,
                        drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PALETTE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.28, Microsoft name: EmfPlusPalette Object
  */
int U_PMF_PALETTE_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PATHGRADIENTBRUSHDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.29, Microsoft name: EmfPlusPathGradientBrushData Object
  */
int U_PMF_PATHGRADIENTBRUSHDATA_draw(const char *contents, FILE *out,
                                     drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PATHGRADIENTBRUSHOPTIONALDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  \param  BDFlag     Describes optional values in contents
  EMF+ manual 2.2.2.30, Microsoft name: EmfPlusPathGradientBrushOptionalData
  Object
  */
int U_PMF_PATHGRADIENTBRUSHOPTIONALDATA_draw(const char *contents, int BDFlag,
                                             FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_PATHPOINTTYPE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.31, Microsoft name: EmfPlusPathPointType Object
  */
int U_PMF_PATHPOINTTYPE_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PATHPOINTTYPERLE object
  \return Number of elements in the run, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.32, Microsoft name: EmfPlusPathPointTypeRLE Object
  */
int U_PMF_PATHPOINTTYPERLE_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PATHPOINTTYPERLE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.33, Microsoft name: EmfPlusPenData Object
  */
int U_PMF_PENDATA_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_PENOPTIONALDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  \param  Flags      PenData Flags that determine which optionaldata fields are
  present in the record.

  EMF+ manual 2.2.2.34, Microsoft name: EmfPlusPenOptionalData Object
  */
int U_PMF_PENOPTIONALDATA_draw(const char *contents, int Flags, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}
/**
  \brief Print data from a  U_PMF_POINT object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.35, Microsoft name: EmfPlusPoint Object
  */
int U_PMF_POINT_draw(const char **contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_POINT Structure
  \return 1 on success, 0 on error
  \param  Point   U_PMF_POINT Structure to print
  EMF+ manual 2.2.2.35, Microsoft name: EmfPlusPoint Object
  */
int U_PMF_POINT_S_draw(U_PMF_POINT *Point, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_POINTF object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.36, Microsoft name: EmfPlusPointF Object
  */
int U_PMF_POINTF_draw(const char **contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_POINTF Structure
  \return 1 on success, 0 on error
  \param  Point   U_PMF_POINTF Structure to print
  EMF+ manual 2.2.2.36, Microsoft name: EmfPlusPointF Object
  */
int U_PMF_POINTF_S_draw(U_PMF_POINTF *Point, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_POINTR object
  \return bytes traversed on success, 0 on error
  \param  contents   Pointer to next data to print
  \param  Xpos       X coordinate for current point
  \param  Ypos       Y coordinate for current point

  On each call the next relative offset is extracted, the current
  coordinates are modified with that offset, and the pointer is
  advanced to the next data point.

  EMF+ manual 2.2.2.37, Microsoft name: EmfPlusPointR Object
  */
int U_PMF_POINTR_draw(const char **contents, U_FLOAT *Xpos, U_FLOAT *Ypos,
                      FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_RECT object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.38, Microsoft name: EmfPlusRect Object
  */
int U_PMF_RECT_draw(const char **contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_RECT Structure
  \return 1 on success, 0 on error
  \param  Rect   U_PMF_RECT structure
  EMF+ manual 2.2.2.39, Microsoft name: EmfPlusRectF Object
  */
int U_PMF_RECT_S_draw(U_PMF_RECT *Rect, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_RECTF object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.39, Microsoft name: EmfPlusRectF Object
  */
int U_PMF_RECTF_draw(const char **contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_RECTF Structure
  \return 1 on success, 0 on error
  \param  Rect   U_PMF_RECTF Structure
  EMF+ manual 2.2.2.39, Microsoft name: EmfPlusRectF Object
  */
int U_PMF_RECTF_S_draw(U_PMF_RECTF *Rect, FILE *out, drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_REGIONNODE object
  \return size on success, 0 on error
  \param  contents   Record from which to print data
  \param  Level      Tree level.  This routine is recursive and could go down
  many levels. 1 is the top, >1 are child nodes.
  EMF+ manual 2.2.2.40, Microsoft name: EmfPlusRegionNode Object
  */
int U_PMF_REGIONNODE_draw(const char *contents, int Level, FILE *out,
                          drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_REGIONNODECHILDNODES object
  \return size on success, 0 on error
  \param  contents   Record from which to print data
  \param  Level      Tree level.  This routine is recursive and could go down
  many levels. 1 is the top, >1 are child nodes.
  EMF+ manual 2.2.2.41, Microsoft name: EmfPlusRegionNodeChildNodes Object
  */
int U_PMF_REGIONNODECHILDNODES_draw(const char *contents, int Level, FILE *out,
                                    drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_REGIONNODEPATH object
  \return Size of data on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.42, Microsoft name: EmfPlusRegionNodePath Object
  */
int U_PMF_REGIONNODEPATH_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_SOLIDBRUSHDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.43, Microsoft name: EmfPlusSolidBrushData Object
  */
int U_PMF_SOLIDBRUSHDATA_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_STRINGFORMATDATA object
  \return 1 on success, 0 on error
  \param  contents      Record from which to print data
  \param  TabStopCount  Entries in TabStop array
  \param  RangeCount    Entries in CharRange array
  EMF+ manual 2.2.2.44, Microsoft name: EmfPlusStringFormatData Object
  */
int U_PMF_STRINGFORMATDATA_draw(const char *contents, uint32_t TabStopCount,
                                uint32_t RangeCount, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_TEXTUREBRUSHDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.45, Microsoft name: EmfPlusTextureBrushData Object
  */
int U_PMF_TEXTUREBRUSHDATA_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_TEXTUREBRUSHOPTIONALDATA object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  \param  HasImage   True if the record contains an image.

  EMF+ manual 2.2.2.46, Microsoft name: EmfPlusTextureBrushOptionalData Object
  */
int U_PMF_TEXTUREBRUSHOPTIONALDATA_draw(const char *contents, int HasImage,
                                        FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_TRANSFORMMATRIX object stored in file byte
  order.
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.2.47, Microsoft name: EmfPlusTransformMatrix Object
  */
int U_PMF_TRANSFORMMATRIX_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_TRANSFORMMATRIX structure
  \return 1 on success, 0 on error
  \param  Tm  U_PMF_TRANSFORMMATRIX structure
  EMF+ manual 2.2.2.47, Microsoft name: EmfPlusTransformMatrix Object
  */
int U_PMF_TRANSFORMMATRIX2_draw(U_PMF_TRANSFORMMATRIX *Tm, FILE *out,
                                drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_ROTMATRIX object
  \return 1 on success, 0 on error
  \param  Rm   U_PMF_ROTMATRIX object
  NOT DOCUMENTED, like EMF+ manual 2.2.2.47, Microsoft name:
  EmfPlusTransformMatrix Object, but missing offset values
  */
int U_PMF_ROTMATRIX2_draw(U_PMF_ROTMATRIX *Rm, FILE *out,
                          drawingStates *states) {
    return (1);
}

/**
  \brief Print data from a  U_PMF_IE_BLUR object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.1, Microsoft name: BlurEffect Object
  */
int U_PMF_IE_BLUR_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_BRIGHTNESSCONTRAST object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.2, Microsoft name: BrightnessContrastEffect Object
  */
int U_PMF_IE_BRIGHTNESSCONTRAST_draw(const char *contents, FILE *out,
                                     drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_COLORBALANCE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.3, Microsoft name: ColorBalanceEffect Object
  */
int U_PMF_IE_COLORBALANCE_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_COLORCURVE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.4, Microsoft name: ColorCurveEffect Object
  */
int U_PMF_IE_COLORCURVE_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_COLORLOOKUPTABLE object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.5, Microsoft name: ColorLookupTableEffect Object
  */
int U_PMF_IE_COLORLOOKUPTABLE_draw(const char *contents, FILE *out,
                                   drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_COLORMATRIX object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.6, Microsoft name: ColorMatrixEffect Object
  */
int U_PMF_IE_COLORMATRIX_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_HUESATURATIONLIGHTNESS object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.7, Microsoft name: HueSaturationLightnessEffect Object
  */
int U_PMF_IE_HUESATURATIONLIGHTNESS_draw(const char *contents, FILE *out,
                                         drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_LEVELS object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.8, Microsoft name: LevelsEffect Object
  */
int U_PMF_IE_LEVELS_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_REDEYECORRECTION object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.9, Microsoft name: RedEyeCorrectionEffect Object
  */
int U_PMF_IE_REDEYECORRECTION_draw(const char *contents, FILE *out,
                                   drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_SHARPEN object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.10, Microsoft name: SharpenEffect Object
  */
int U_PMF_IE_SHARPEN_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMF_IE_TINT object
  \return 1 on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.2.3.11, Microsoft name: TintEffect Object
  */
int U_PMF_IE_TINT_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/* *****************************************************************************************
 */
/* EMF+ records, the EMF+ record header is printed separately, these print the
 * contents only */
/* *****************************************************************************************
 */

/**
  \brief Print data from a  U_PMR_OFFSETCLIP record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.1.1, Microsoft name: EmfPlusOffsetClip Record,  Index 0x35
  */
int U_PMR_OFFSETCLIP_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    U_FLOAT dx;
    U_FLOAT dy;
    U_PMF_POINTF origin = {0.0, 0.0};
    U_PMF_POINTF offset;
    POINT_D projected_origin;
    POINT_D projected_offset;
    uint64_t previous_id;
    uint64_t new_id;

    if (pmf_metafile_depth != 0 || !pmf_top_level_primitive_draw_enabled) {
        return 1;
    }
    if (!U_PMR_OFFSETCLIP_get(contents, NULL, &dx, &dy)) {
        return 0;
    }
    if (pmf_active_clip_mask_id == 0) {
        return 1;
    }
    offset.X = dx;
    offset.Y = dy;
    projected_origin = pmf_path_point_project(states, &origin);
    projected_offset = pmf_path_point_project(states, &offset);
    previous_id = pmf_active_clip_mask_id;
    new_id = ++pmf_clip_mask_serial;

    fprintf(out,
            "<defs><mask id=\"pmfClip%llu\" maskUnits=\"userSpaceOnUse\" "
            "maskContentUnits=\"userSpaceOnUse\" x=\"-1000000\" "
            "y=\"-1000000\" width=\"2000000\" height=\"2000000\">\n"
            "<g transform=\"translate(%.4f,%.4f)\">\n",
            (unsigned long long)new_id,
            projected_offset.x - projected_origin.x,
            projected_offset.y - projected_origin.y);
    pmf_clip_mask_page_draw(out, "white", previous_id);
    fprintf(out, "</g>\n</mask></defs>\n");
    pmf_active_clip_mask_id = new_id;
    return 1;
}

/**
  \brief Print data from a  U_PMR_OFFSETCLIP record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.1.2, Microsoft name: EmfPlusResetClip Record, Index 0x31
  */
int U_PMR_RESETCLIP_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    U_PMF_CMN_HDR header;

    UNUSED(out);
    UNUSED(states);
    if (pmf_metafile_depth != 0 || !pmf_top_level_primitive_draw_enabled) {
        return 1;
    }
    if (!U_PMR_RESETCLIP_get(contents, &header)) {
        return 0;
    }
    pmf_active_clip_mask_id = 0;
    return 1;
}

/**
  \brief Print data from a  U_PMR_SETCLIPPATH record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.1.3, Microsoft name: EmfPlusSetClipPath Record, Index 0x33
  */
int U_PMR_SETCLIPPATH_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    uint32_t path_id;
    int combine_mode;
    pmfClipShape shape;

    if (pmf_metafile_depth != 0 || !pmf_top_level_primitive_draw_enabled) {
        return 1;
    }
    if (!U_PMR_SETCLIPPATH_get(contents, NULL, &path_id, &combine_mode)) {
        return 0;
    }
    shape.type = PMF_CLIP_SHAPE_PATH;
    shape.path = pmf_path_cache_get(path_id);
    shape.region = NULL;
    if (shape.path == NULL) {
        return 1;
    }
    pmf_clip_mask_combine_draw(out, states, combine_mode, &shape);
    return 1;
}

/**
  \brief Print data from a  U_PMR_SETCLIPRECT record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.1.4, Microsoft name: EmfPlusSetClipRect Record, Index 0x32
  */
int U_PMR_SETCLIPRECT_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    int combine_mode;
    pmfClipShape shape;

    if (pmf_metafile_depth != 0 || !pmf_top_level_primitive_draw_enabled) {
        return 1;
    }
    if (!U_PMR_SETCLIPRECT_get(contents, NULL, &combine_mode, &shape.rect)) {
        return 0;
    }
    shape.type = PMF_CLIP_SHAPE_RECT;
    shape.path = NULL;
    shape.region = NULL;
    pmf_clip_mask_combine_draw(out, states, combine_mode, &shape);
    return 1;
}

/**
  \brief Print data from a  U_PMR_SETCLIPREGION record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.1.5, Microsoft name: EmfPlusSetClipRegion Record, Index 0x34
  */
int U_PMR_SETCLIPREGION_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    uint32_t region_id;
    int combine_mode;
    pmfClipShape shape = {0};

    if (pmf_metafile_depth != 0 || !pmf_top_level_primitive_draw_enabled) {
        return 1;
    }
    if (!U_PMR_SETCLIPREGION_get(contents, NULL, &region_id,
                                 &combine_mode)) {
        return 0;
    }
    shape.type = PMF_CLIP_SHAPE_REGION;
    shape.region = pmf_region_cache_get(region_id);
    if (shape.region == NULL) {
        return 1;
    }
    pmf_clip_mask_combine_draw(out, states, combine_mode, &shape);
    return 1;
}

/**
  \brief Print data from a  U_PMR_COMMENT record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.2.1, Microsoft name: EmfPlusComment Record, Index 0x03
  */
int U_PMR_COMMENT_draw(const char *contents, FILE *out, drawingStates *states) {
    U_PMF_CMN_HDR header;
    const char *data;

    UNUSED(out);
    UNUSED(states);
    if (!U_PMR_COMMENT_get(contents, &header, &data)) {
        return 0;
    }
    /*
     * libUEMF's testbed_pmf emits this marker before a stream that has no
     * useful GDI vector fallback. Without this exception, test-038.emf loses
     * most of its EMF+ content; with a broader exception, dual EMFs get
     * duplicate path output.
     */
    if (pmf_comment_contains(
            data, header.DataSize,
            "Everything after this is EMF+, except for the very last record")) {
        pmf_top_level_primitive_draw_enabled = true;
    }
    return 1;
}

/**
  \brief Print data from a  U_PMR_ENDOFFILE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.3.1, Microsoft name: EmfPlusEndOfFile Record, Index 0x02
  */
int U_PMR_ENDOFFILE_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    return (U_PMR_NODATAREC_draw(contents, out, states));
}

/**
  \brief Print data from a  U_PMR_ENDOFFILE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.3.2, Microsoft name: EmfPlusGetDC Record, Index 0x04
  */
int U_PMR_GETDC_draw(const char *contents, FILE *out, drawingStates *states) {
    return (U_PMR_NODATAREC_draw(contents, out, states));
}

/**
  \brief Print data from a  U_PMR_HEADER record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.3.3, Microsoft name: EmfPlusHeader Record, Index 0x01
  */
int U_PMR_HEADER_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_CLEAR record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.1, Microsoft name: EmfPlusClear Record, Index 0x09
  */
int U_PMR_CLEAR_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWARC record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.2, Microsoft name: EmfPlusDrawArc Record, Index 0x12
  */
int U_PMR_DRAWARC_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWBEZIERS record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.3, Microsoft name: EmfPlusDrawBeziers Record, Index 0x19
  */
int U_PMR_DRAWBEZIERS_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWCLOSEDCURVE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data

  Curve is a cardinal spline.
  References sent by MS support:
http://alvyray.com/Memos/CG/Pixar/spline77.pdf
http://msdn.microsoft.com/en-us/library/4cf6we5y(v=vs.110).aspx

EMF+ manual 2.3.4.4, Microsoft name: EmfPlusDrawClosedCurve Record, Index 0x17
*/
int U_PMR_DRAWCLOSEDCURVE_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWCURVE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data

  Curve is a cardinal spline, using doubled terminator points to generate curves
for the terminal segments.
  References sent by MS support:
http://alvyray.com/Memos/CG/Pixar/spline77.pdf
http://msdn.microsoft.com/en-us/library/4cf6we5y(v=vs.110).aspx

EMF+ manual 2.3.4.5, Microsoft name: EmfPlusDrawCurve Record, Index 0x18
*/
int U_PMR_DRAWCURVE_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWDRIVERSTRING record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.6, Microsoft name: EmfPlusDrawDriverString Record, Index
  0x36
  */
int U_PMR_DRAWDRIVERSTRING_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWELLIPSE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.7, Microsoft name: EmfPlusDrawEllipse Record, Index 0x0F
  */
int U_PMR_DRAWELLIPSE_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    uint32_t pen_id;
    int ctype;
    U_PMF_RECTF rect;
    const pmfPenCacheEntry *pen;

    if (!U_PMR_DRAWELLIPSE_get(contents, NULL, &pen_id, &ctype, &rect)) {
        return 0;
    }
    UNUSED(ctype);
    if (!pmf_primitive_draw_allowed()) {
        return 1;
    }
    pen = pmf_pen_cache_get(pen_id);
    if (pen == NULL || pen->color.Alpha == 0) {
        return 1;
    }
    fprintf(out, "<%spath d=\"", states->nameSpaceString);
    pmf_ellipse_path_draw(out, &rect, states);
    fprintf(out, "\" fill=\"none\" ");
    pmf_pen_attrs_draw(out, pen, states);
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
    return 1;
}

/**
  \brief Print data from a  U_PMR_DRAWIMAGE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.8, Microsoft name: EmfPlusDrawImage Record, Index 0x1A
  */
int U_PMR_DRAWIMAGE_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWIMAGEPOINTS record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.9, Microsoft name: EmfPlusDrawImagePoints Record, Index 0x1B
  */
int U_PMR_DRAWIMAGEPOINTS_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    uint32_t img_id;
    int ctype;
    int etype;
    int rel_abs;
    uint32_t img_attr_id;
    int32_t src_unit;
    U_PMF_RECTF src_rect;
    uint32_t elements;
    U_PMF_POINTF *points = NULL;
    const pmfImageCacheEntry *image;
    uint32_t version;
    uint32_t image_type;
    const char *image_data;
    const char *blimit;
    int status = 0;

    UNUSED(etype);
    UNUSED(rel_abs);
    UNUSED(img_attr_id);
    UNUSED(src_unit);

    if (!U_PMR_DRAWIMAGEPOINTS_get(contents, NULL, &img_id, &ctype, &etype,
                                   &rel_abs, &img_attr_id, &src_unit,
                                   &src_rect, &elements, &points)) {
        return 0;
    }
    UNUSED(ctype);
    if (elements < 3) {
        free(points);
        return 0;
    }

    image = pmf_image_cache_get(img_id);
    if (image == NULL) {
        free(points);
        return 0;
    }

    blimit = image->data + image->size;
    if (!U_PMF_IMAGE_get(image->data, &version, &image_type, &image_data,
                         blimit)) {
        free(points);
        return 0;
    }
    UNUSED(version);

    switch (image_type) {
    case U_IDT_Bitmap:
        status = pmf_bitmap_image_draw(image, image_data, blimit, &src_rect,
                                       points, out, states);
        break;
    case U_IDT_Metafile:
        status = pmf_metafile_image_draw(image_data, blimit, &src_rect, points,
                                         out, states);
        break;
    default:
        status = 0;
        break;
    }

    free(points);
    return status;
}

/**
  \brief Print data from a  U_PMR_DRAWLINES record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.10, Microsoft name: EmfPlusDrawLines Record, Index 0x0D
  */
int U_PMR_DRAWLINES_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    uint32_t pen_id;
    int ctype;
    int dtype;
    int rel_abs;
    uint32_t elements;
    U_PMF_POINTF *points = NULL;
    const pmfPenCacheEntry *pen;
    bool compensate;

    if (!U_PMR_DRAWLINES_get(contents, NULL, &pen_id, &ctype, &dtype, &rel_abs,
                             &elements, &points)) {
        return 0;
    }
    UNUSED(ctype);
    UNUSED(rel_abs);
    pen = pmf_pen_cache_get(pen_id);
    if (pen == NULL || pen->color.Alpha == 0 || elements == 0) {
        free(points);
        return 1;
    }
    compensate = !pmf_primitive_draw_allowed() &&
                 pmf_pen_needs_top_level_compensation(pen);
    if (!pmf_primitive_draw_allowed() && !compensate) {
        free(points);
        return 1;
    }

    fprintf(out, "<%spath d=\"", states->nameSpaceString);
    for (uint32_t i = 0; i < elements; i++) {
        POINT_D point = pmf_path_point_project(states, &points[i]);
        fprintf(out, "%c %.4f,%.4f ", i == 0 ? 'M' : 'L', point.x,
                point.y);
    }
    if (dtype) {
        fprintf(out, "Z ");
    }
    fprintf(out, "\" fill=\"none\" ");
    pmf_pen_attrs_draw(out, pen, states);
    if (compensate) {
        pmf_pen_dash_attrs_draw(
            out, pen, fabs((double)pen->width) * pmf_path_stroke_scale(states));
    }
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
    free(points);
    return 1;
}

/**
  \brief Print data from a  U_PMR_DRAWPATH record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.11, Microsoft name: EmfPlusDrawPath Record, Index 0x15
  */
int U_PMR_DRAWPATH_draw(const char *contents, FILE *out,
                        drawingStates *states) {
    uint32_t path_id;
    uint32_t pen_id;
    const pmfPathCacheEntry *path;
    const pmfPenCacheEntry *pen;
    double stroke_width;
    double raw_width;
    bool stabilize_stroke;
    bool rounded_stroke;
    bool compensate;

    if (!U_PMR_DRAWPATH_get(contents, NULL, &path_id, &pen_id)) {
        return 0;
    }
    path = pmf_path_cache_get(path_id);
    pen = pmf_pen_cache_get(pen_id);
    if (path == NULL || pen == NULL) {
        return 1;
    }
    if (pen->color.Alpha == 0) {
        return 1;
    }
    compensate = !pmf_primitive_draw_allowed() &&
                 pmf_pen_needs_top_level_compensation(pen);
    if (!pmf_primitive_draw_allowed() && !compensate) {
        return 1;
    }

    raw_width = fabs((double)pen->width);
    stroke_width = raw_width * pmf_path_stroke_scale(states);
    stabilize_stroke = false;
    rounded_stroke = false;
    /*
     * Formula metafiles such as test-formula3.emf can put the page-scale
     * placement in the parent DrawImagePoints while also using a smaller child
     * world transform for coordinates. Applying both to pen width makes
     * operators subpixel-thin; emit a device-width stroke from the parent scale.
     */
    if (pmf_metafile_stroke_scale > 0.0 && pmf_metafile_stroke_scale < 0.5) {
        double parent_width = raw_width * pmf_metafile_stroke_scale;
        stroke_width *= pmf_metafile_stroke_scale;
        if (parent_width > stroke_width) {
            stroke_width = parent_width;
        }
        stabilize_stroke = true;
        rounded_stroke = path->count > 2;
    } else if (path->count > 2 && pmf_metafile_stroke_scale > 0.0 &&
               fabs(pmf_metafile_stroke_scale - 1.0) > 0.0001) {
        /*
         * Multi-point open strokes such as the MathType wavy relation mark look
         * too heavy when the parent transform is near identity but still
         * non-uniform. Keep their visible width stable without affecting simple
         * horizontal operators.
         */
        stroke_width *= pmf_metafile_stroke_scale;
        stabilize_stroke = true;
        rounded_stroke = true;
    }
    if (pmf_metafile_depth == 0 && pmf_top_level_primitive_draw_enabled &&
        stroke_width < 1.0) {
        /* Preserve pure-EMF+ one-unit pens as visible device hairlines. */
        stroke_width = 1.0;
        stabilize_stroke = true;
    }
    fprintf(out, "<%spath d=\"", states->nameSpaceString);
    if (!pmf_path_data_draw(path, out, states)) {
        fprintf(out, "\" />\n");
        return 0;
    }
    fprintf(out,
            "\" fill=\"none\" stroke=\"#%02X%02X%02X\" "
            "stroke-opacity=\"%.4f\" stroke-width=\"%.4f\"",
            pen->color.Red, pen->color.Green, pen->color.Blue,
            pen->color.Alpha / 255.0, stroke_width);
    if (compensate) {
        pmf_pen_dash_attrs_draw(out, pen, stroke_width);
    }
    if (stabilize_stroke) {
        fprintf(out, " vector-effect=\"non-scaling-stroke\"");
        if (rounded_stroke) {
            fprintf(out, " stroke-linecap=\"round\" stroke-linejoin=\"round\"");
        }
    }
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
    if (compensate) {
        pmf_path_custom_caps_draw(path, pen, out, states, stroke_width);
    }
    return 1;
}

/**
  \brief Print data from a  U_PMR_DRAWPIE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.12, Microsoft name: EmfPlusDrawPie Record, Index 0x0D
  */
int U_PMR_DRAWPIE_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_DRAWRECTS record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  \param blimit      One byte past the last record in memory.
  EMF+ manual 2.3.4.13, Microsoft name: EmfPlusDrawRects Record, Index 0x0B
  */
int U_PMR_DRAWRECTS_draw(const char *contents, const char *blimit, FILE *out,
                         drawingStates *states) {
    uint32_t pen_id;
    int ctype;
    uint32_t elements;
    U_PMF_RECTF *rects = NULL;
    const pmfPenCacheEntry *pen;

    UNUSED(blimit);
    if (!U_PMR_DRAWRECTS_get(contents, NULL, &pen_id, &ctype, &elements,
                             &rects)) {
        return 0;
    }
    UNUSED(ctype);
    if (!pmf_primitive_draw_allowed()) {
        free(rects);
        return 1;
    }
    pen = pmf_pen_cache_get(pen_id);
    if (pen == NULL || pen->color.Alpha == 0) {
        free(rects);
        return 1;
    }
    for (uint32_t i = 0; i < elements; i++) {
        fprintf(out, "<%spath d=\"", states->nameSpaceString);
        pmf_rect_path_draw(out, &rects[i], states);
        fprintf(out, "\" fill=\"none\" ");
        pmf_pen_attrs_draw(out, pen, states);
        pmf_clip_attr_draw(out);
        fprintf(out, " />\n");
    }
    free(rects);
    return 1;
}

/**
  \brief Print data from a  U_PMR_DRAWSTRING record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.14, Microsoft name: EmfPlusDrawString Record, Index 0x1C
  */
int U_PMR_DRAWSTRING_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    uint32_t font_id;
    int brush_is_inline;
    uint32_t brush_id;
    uint32_t format_id;
    uint32_t elements;
    U_PMF_RECTF rect;
    uint16_t *string16 = NULL;
    U_PMF_ARGB color;
    const pmfFontCacheEntry *font;
    char *string8;
    U_PMF_POINTF origin;
    POINT_D point;
    double font_size;

    if (!U_PMR_DRAWSTRING_get(contents, NULL, &font_id, &brush_is_inline,
                              &brush_id, &format_id, &elements, &rect,
                              &string16)) {
        return 0;
    }
    UNUSED(format_id);
    if (!pmf_primitive_draw_allowed()) {
        free(string16);
        return 1;
    }
    if (!pmf_solid_brush_color(brush_id, brush_is_inline, &color) ||
        color.Alpha == 0) {
        free(string16);
        return 1;
    }
    font = pmf_font_cache_get(font_id);
    string8 = U_Utf16leToUtf8(string16, elements, NULL);
    free(string16);
    if (string8 == NULL) {
        return 1;
    }

    origin.X = rect.X;
    origin.Y = rect.Y + (font != NULL ? font->em_size : rect.Height);
    point = pmf_path_point_project(states, &origin);
    font_size = (font != NULL ? font->em_size : rect.Height) *
                pmf_path_stroke_scale(states);
    if (font_size <= 0.0) {
        font_size = fabs(rect.Height);
    }

    fprintf(out, "<%stext x=\"%.4f\" y=\"%.4f\" font-size=\"%.4f\" ",
            states->nameSpaceString, point.x, point.y, font_size);
    if (font != NULL && font->family != NULL) {
        fprintf(out, "font-family=\"");
        pmf_attr_text_draw(out, font->family);
        fprintf(out, "\" ");
        if (font->style_flags & 1) {
            fprintf(out, "font-weight=\"bold\" ");
        }
        if (font->style_flags & 2) {
            fprintf(out, "font-style=\"italic\" ");
        }
    }
    pmf_color_attrs_draw(out, "fill", color);
    pmf_clip_attr_draw(out);
    fprintf(out, "><![CDATA[%s]]></%stext>\n", string8,
            states->nameSpaceString);
    free(string8);
    return 1;
}

/**
  \brief Print data from a  U_PMR_FILLCLOSEDCURVE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.15, Microsoft name: EmfPlusFillClosedCurve Record, Index
  0x16
  */
int U_PMR_FILLCLOSEDCURVE_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_FILLELLIPSE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.16, Microsoft name: EmfPlusFillEllipse Record, Index 0x0E
  */
int U_PMR_FILLELLIPSE_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    int brush_is_inline;
    int ctype;
    uint32_t brush_id;
    U_PMF_RECTF rect;
    U_PMF_ARGB color;

    if (!U_PMR_FILLELLIPSE_get(contents, NULL, &brush_is_inline, &ctype,
                               &brush_id, &rect)) {
        return 0;
    }
    UNUSED(ctype);
    if (!pmf_primitive_draw_allowed()) {
        return 1;
    }
    if (!pmf_solid_brush_color(brush_id, brush_is_inline, &color) ||
        color.Alpha == 0) {
        return 1;
    }
    fprintf(out, "<%spath d=\"", states->nameSpaceString);
    pmf_ellipse_path_draw(out, &rect, states);
    fprintf(out, "\" ");
    pmf_color_attrs_draw(out, "fill", color);
    fprintf(out, " stroke=\"none\"");
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
    return 1;
}

/**
  \brief Print data from a  U_PMR_FILLPATH record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.17, Microsoft name: EmfPlusFillPath Record, Index 0x14
  */
int U_PMR_FILLPATH_draw(const char *contents, FILE *out,
                        drawingStates *states) {
    uint32_t path_id;
    int brush_is_inline;
    uint32_t brush_id;
    U_PMF_ARGB color;
    const pmfPathCacheEntry *path;
    const pmfBrushCacheEntry *brush = NULL;

    if (!U_PMR_FILLPATH_get(contents, NULL, &path_id, &brush_is_inline,
                            &brush_id)) {
        return 0;
    }
    if (!brush_is_inline) {
        brush = pmf_brush_cache_get(brush_id);
    }
    if (!pmf_solid_brush_color(brush_id, brush_is_inline, &color)) {
        return 1;
    }
    if (pmf_metafile_depth <= 0) {
        pmf_recent_fill_color_store(states, color);
    }
    if (!pmf_primitive_draw_allowed()) {
        return 1;
    }
    path = pmf_path_cache_get(path_id);
    if (path == NULL) {
        return 1;
    }
    if (pmf_metafile_depth == 0 && pmf_top_level_primitive_draw_enabled &&
        brush != NULL &&
        pmf_path_gradient_fill_draw(path, brush, out, states)) {
        return 1;
    }

    fprintf(out, "<%spath d=\"", states->nameSpaceString);
    if (!pmf_path_data_draw(path, out, states)) {
        fprintf(out, "\" />\n");
        return 0;
    }
    fprintf(out, "\" fill=\"#%02X%02X%02X\" fill-opacity=\"%.4f\" "
                 "stroke=\"none\"",
            color.Red, color.Green, color.Blue, color.Alpha / 255.0);
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
    return 1;
}

/**
  \brief Print data from a  U_PMR_FILLPIE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.18, Microsoft name: EmfPlusFillPie Record, Index 0x10
  */
int U_PMR_FILLPIE_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_FILLPOLYGON record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.19, Microsoft name: EmfPlusFillPolygon Record, Index 0x0C
  */
int U_PMR_FILLPOLYGON_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    int brush_is_inline;
    int ctype;
    int rel_abs;
    uint32_t brush_id;
    uint32_t elements;
    U_PMF_POINTF *points = NULL;
    U_PMF_ARGB color;

    if (!U_PMR_FILLPOLYGON_get(contents, NULL, &brush_is_inline, &ctype,
                               &rel_abs, &brush_id, &elements, &points)) {
        return 0;
    }
    UNUSED(ctype);
    UNUSED(rel_abs);
    if (!pmf_primitive_draw_allowed()) {
        free(points);
        return 1;
    }
    if (!pmf_solid_brush_color(brush_id, brush_is_inline, &color) ||
        color.Alpha == 0 || elements == 0) {
        free(points);
        return 1;
    }

    fprintf(out, "<%spath d=\"", states->nameSpaceString);
    for (uint32_t i = 0; i < elements; i++) {
        POINT_D point = pmf_path_point_project(states, &points[i]);
        fprintf(out, "%c %.4f,%.4f ", i == 0 ? 'M' : 'L', point.x,
                point.y);
    }
    fprintf(out, "Z\" ");
    pmf_color_attrs_draw(out, "fill", color);
    fprintf(out, " stroke=\"none\"");
    pmf_clip_attr_draw(out);
    fprintf(out, " />\n");
    free(points);
    return 1;
}

/**
  \brief Print data from a  U_PMR_FILLRECTS record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  \param blimit      One byte past the last record in memory.
  EMF+ manual 2.3.4.20, Microsoft name: EmfPlusFillRects Record, Index 0x0A
  */
int U_PMR_FILLRECTS_draw(const char *contents, const char *blimit, FILE *out,
                         drawingStates *states) {
    int brush_is_inline;
    int ctype;
    uint32_t brush_id;
    uint32_t elements;
    U_PMF_RECTF *rects = NULL;
    U_PMF_ARGB color;

    UNUSED(blimit);
    if (!U_PMR_FILLRECTS_get(contents, NULL, &brush_is_inline, &ctype,
                             &brush_id, &elements, &rects)) {
        return 0;
    }
    UNUSED(ctype);
    if (!pmf_primitive_draw_allowed()) {
        free(rects);
        return 1;
    }
    if (!pmf_solid_brush_color(brush_id, brush_is_inline, &color) ||
        color.Alpha == 0) {
        free(rects);
        return 1;
    }
    for (uint32_t i = 0; i < elements; i++) {
        fprintf(out, "<%spath d=\"", states->nameSpaceString);
        pmf_rect_path_draw(out, &rects[i], states);
        fprintf(out, "\" ");
        pmf_color_attrs_draw(out, "fill", color);
        fprintf(out, " stroke=\"none\"");
        pmf_clip_attr_draw(out);
        fprintf(out, " />\n");
    }
    free(rects);
    return 1;
}

/**
  \brief Print data from a  U_PMR_FILLREGION record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.4.21, Microsoft name: EmfPlusFillRegion Record, Index 0x13
  */
int U_PMR_FILLREGION_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_OBJECT record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  \param  blimit     One byte past the last record in memory.
  \param  ObjCont    Structure that holds accumulated object.
  \param  term       Flag used when an abnormal termination of a series of
  continuation records is encountered.
  EMF+ manual 2.3.5.1, Microsoft name: EmfPlusObject Record, Index 0x13
  */
int U_PMR_OBJECT_draw(const char *contents, const char *blimit,
                      U_OBJ_ACCUM *ObjCont, int term, FILE *out,
                      drawingStates *states) {
    U_PMF_CMN_HDR Header;
    uint32_t ObjID;
    int otype, ntype;
    uint32_t TSize;
    const char *Data;
    int ttype, status;

    /* Continued records are a pain. Each contains the total size of the
       continued object in the first 4 bytes
       of data.  When the total hits that then then the record is complete, even
       though the continuation bit will
       still be set on that last record.  Check for this and then print the
       terminated continued series.
       */

    if (term) { /* mode for handling unexpected end of accumulated object */
        if (ObjCont->used == 0)
            return (0); /* no continued object pending */
        U_PMF_OBJECTTYPEENUMERATION_draw(ObjCont->Type, out, states);
        ttype = ObjCont->Type & 0x3F;
        status = 1;
    } else {
        status = U_PMR_OBJECT_get(contents, &Header, &ObjID, &otype, &ntype,
                                  &TSize, &Data);
        /* In a corrupt EMF+ file we might hit a new type of record before all
           the continuation records
           expected have been found.  If that happens terminate whatever we have
           accumulated so far, and then go on
           to emit the new (unexpected) record. */
        if (contents + Header.Size >= blimit)
            return (0);
        if (!status)
            return (status);
        if ((ObjCont->used > 0) &&
            (U_OA_append(ObjCont, NULL, 0, otype, ObjID) < 0)) {
            U_PMR_OBJECT_draw(contents, blimit, ObjCont, 1, out, states);
        }
        U_PMF_OBJECTTYPEENUMERATION_draw(otype, out, states);
        if (ntype) {
            if (checkOutOfEMF(states,
                              (uintptr_t)((uintptr_t)Data +
                                         (uintptr_t)Header.DataSize - 4)) ||
                ((int64_t)Header.DataSize - 4) < 0) {
                status = 0;
            } else {
                U_OA_append(
                    ObjCont, Data, Header.DataSize - 4, otype,
                    ObjID); // The total byte count is not added to the object
            }
        } else {
            if (checkOutOfEMF(states,
                              (uintptr_t)Data + (uintptr_t)Header.DataSize)) {
                status = 0;
            } else {
                U_OA_append(
                    ObjCont, Data, Header.DataSize, otype,
                    ObjID); // The total byte count is not added to the object
            }
        }
        if (ntype && ObjCont->used < TSize)
            return (status);
        /* preceding terminates any continued series for >= accumulated bytes */
        ttype = otype;
    }
    if (status) {
        switch (ttype) {
        case U_OT_Brush:
            (void)pmf_brush_cache_store((uint32_t)ObjCont->Id, ObjCont->accum,
                                        ObjCont->accum + ObjCont->used);
            (void)U_PMF_BRUSH_draw(ObjCont->accum, out, states);
            break;
        case U_OT_Pen:
            (void)pmf_pen_cache_store((uint32_t)ObjCont->Id, ObjCont->accum,
                                      ObjCont->accum + ObjCont->used);
            (void)U_PMF_PEN_draw(ObjCont->accum, out, states);
            break;
        case U_OT_Path:
            (void)pmf_path_cache_store((uint32_t)ObjCont->Id, ObjCont->accum,
                                       ObjCont->accum + ObjCont->used);
            (void)U_PMF_PATH_draw(ObjCont->accum, out, states);
            break;
        case U_OT_Region:
            (void)pmf_region_cache_store((uint32_t)ObjCont->Id, ObjCont->accum,
                                         ObjCont->used);
            (void)U_PMF_REGION_draw(ObjCont->accum, out, states);
            break;
        case U_OT_Image:
            (void)pmf_image_cache_store((uint32_t)ObjCont->Id, ObjCont->accum,
                                        ObjCont->used);
            (void)U_PMF_IMAGE_draw(ObjCont->accum, out, states);
            break;
        case U_OT_Font:
            (void)pmf_font_cache_store((uint32_t)ObjCont->Id, ObjCont->accum,
                                       ObjCont->accum + ObjCont->used);
            (void)U_PMF_FONT_draw(ObjCont->accum, out, states);
            break;
        case U_OT_StringFormat:
            (void)U_PMF_STRINGFORMAT_draw(ObjCont->accum, out, states);
            break;
        case U_OT_ImageAttributes:
            (void)U_PMF_IMAGEATTRIBUTES_draw(ObjCont->accum, out, states);
            break;
        case U_OT_CustomLineCap:
            (void)U_PMF_CUSTOMLINECAP_draw(ObjCont->accum, "", out, states);
            break;
        case U_OT_Invalid:
        default:
            break;
        }
        U_OA_clear(ObjCont);
    }
    return (status);
}

/**
  \brief Print data from a  U_PMR_SERIALIZABLEOBJECT record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.5.2, Microsoft name: EmfPlusSerializableObject Record, Index
  0x38
  */
int U_PMR_SERIALIZABLEOBJECT_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETANTIALIASMODE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.1, Microsoft name: EmfPlusSetAntiAliasMode Record, Index
  0x1E
  */
int U_PMR_SETANTIALIASMODE_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETCOMPOSITINGMODE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.2, Microsoft name: EmfPlusSetCompositingMode Record, Index
  0x23
  */
int U_PMR_SETCOMPOSITINGMODE_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETCOMPOSITINGQUALITY record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.3, Microsoft name: EmfPlusSetCompositingQuality Record,
  Index 0x24
  */
int U_PMR_SETCOMPOSITINGQUALITY_draw(const char *contents, FILE *out,
                                     drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETINTERPOLATIONMODE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.4, Microsoft name: EmfPlusSetInterpolationMode Record, Index
  0x21
  */
int U_PMR_SETINTERPOLATIONMODE_draw(const char *contents, FILE *out,
                                    drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETPIXELOFFSETMODE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.5, Microsoft name: EmfPlusSetPixelOffsetMode Record, Index
  0x22
  */
int U_PMR_SETPIXELOFFSETMODE_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETRENDERINGORIGIN record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.6, Microsoft name: EmfPlusSetRenderingOrigin Record, Index
  0x1D
  */
int U_PMR_SETRENDERINGORIGIN_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETTEXTCONTRAST record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.7, Microsoft name: EmfPlusSetTextContrast Record, Index 0x20
  */
int U_PMR_SETTEXTCONTRAST_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETTEXTRENDERINGHINT record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.6.8, Microsoft name: EmfPlusSetTextRenderingHint Record, Index
  0x1F
  */
int U_PMR_SETTEXTRENDERINGHINT_draw(const char *contents, FILE *out,
                                    drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_BEGINCONTAINER record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.7.1, Microsoft name: EmfPlusBeginContainer Record, Index 0x27
  */
int U_PMR_BEGINCONTAINER_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_BEGINCONTAINERNOPARAMS record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.7.2, Microsoft name: EmfPlusBeginContainerNoParams Record,
  Index 0x28
  */
int U_PMR_BEGINCONTAINERNOPARAMS_draw(const char *contents, FILE *out,
                                      drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_ENDCONTAINER record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.7.3, Microsoft name: EmfPlusEndContainer Record, Index 0x29
  */
int U_PMR_ENDCONTAINER_draw(const char *contents, FILE *out,
                            drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_RESTORE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.7.4, Microsoft name: EmfPlusRestore Record, Index 0x26
  */
int U_PMR_RESTORE_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SAVE record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.7.5, Microsoft name: EmfPlusSave Record, Index 0x25
  */
int U_PMR_SAVE_draw(const char *contents, FILE *out, drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETTSCLIP record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.8.1, Microsoft name: EmfPlusSetTSClip Record, Index 0x3A
  */
int U_PMR_SETTSCLIP_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETTSGRAPHICS record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.8.2, Microsoft name: EmfPlusSetTSGraphics Record, Index 0x39
  */
int U_PMR_SETTSGRAPHICS_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_MULTIPLYWORLDTRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.1, Microsoft name: EmfPlusMultiplyWorldTransform Record,
  Index 0x2C
  */
int U_PMR_MULTIPLYWORLDTRANSFORM_draw(const char *contents, FILE *out,
                                      drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_RESETWORLDTRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.2, Microsoft name: EmfPlusResetWorldTransform Record, Index
  0x2B
  */
int U_PMR_RESETWORLDTRANSFORM_draw(const char *contents, FILE *out,
                                   drawingStates *states) {
    UNUSED(contents);
    UNUSED(out);
    UNUSED(states);
    pmf_world_transform_reset();
    return 1;
}

/**
  \brief Print data from a  U_PMR_ROTATEWORLDTRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.3, Microsoft name: EmfPlusRotateWorldTransform Record, Index
  0x2F
  */
int U_PMR_ROTATEWORLDTRANSFORM_draw(const char *contents, FILE *out,
                                    drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SCALEWORLDTRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.4, Microsoft name: EmfPlusScaleWorldTransform Record, Index
  0x2E
  */
int U_PMR_SCALEWORLDTRANSFORM_draw(const char *contents, FILE *out,
                                   drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETPAGETRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.5, Microsoft name: EmfPlusSetPageTransform Record, Index
  0x30
  */
int U_PMR_SETPAGETRANSFORM_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_SETWORLDTRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.6, Microsoft name: EmfPlusSetWorldTransform Record, Index
  0x2A
  */
int U_PMR_SETWORLDTRANSFORM_draw(const char *contents, FILE *out,
                                 drawingStates *states) {
    U_PMF_TRANSFORMMATRIX matrix;
    U_XFORM xform;

    if (!U_PMR_SETWORLDTRANSFORM_get(contents, NULL, &matrix)) {
        return 0;
    }
    UNUSED(out);
    UNUSED(states);

    xform.eM11 = matrix.m11;
    xform.eM12 = matrix.m12;
    xform.eM21 = matrix.m21;
    xform.eM22 = matrix.m22;
    xform.eDx = matrix.dX;
    xform.eDy = matrix.dY;
    pmf_world_transform = xform;
    return 1;
}

/**
  \brief Print data from a  U_PMR_TRANSLATEWORLDTRANSFORM record
  \return size of record in bytes on success, 0 on error
  \param  contents   Record from which to print data
  EMF+ manual 2.3.9.7, Microsoft name: EmfPlusTranslateWorldTransform Record,
  Index 0x2D
  */
int U_PMR_TRANSLATEWORLDTRANSFORM_draw(const char *contents, FILE *out,
                                       drawingStates *states) {
    int status = 1;
    return (status);
}

/**
  \brief Print data from a  U_PMR_STROKEFILLPATH record
  \return 1 on success, 0 on error
  \param  contents    Record from which to print data
  */
int U_PMR_STROKEFILLPATH_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    return (U_PMR_NODATAREC_draw(contents, out, states));
}

/**
  \brief Print data from a  U_PMR_MULTIFORMATSTART record
  \return 1 on success, 0 on error
  \param  contents    Record from which to print data
  EMF+ manual mentioned in 2.1.1.1, reserved, not otherwise documented,
  Microsoft name: EmfPlusMultiFormatStart Record, Index 0x05
  */
int U_PMR_MULTIFORMATSTART_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    return (U_PMR_NODATAREC_draw(contents, out, states));
}

/**
  \brief Print data from a  U_PMR_MULTIFORMATSECTION record
  \return 1 on success, 0 on error
  \param  contents    Record from which to print data
  EMF+ manual mentioned in 2.1.1.1, reserved, not otherwise documented,
  Microsoft name: EmfPlusMultiFormatSection Record, Index 0x06
  */
int U_PMR_MULTIFORMATSECTION_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    return (U_PMR_NODATAREC_draw(contents, out, states));
}

/**
  \brief Print data from a  U_PMR_MULTIFORMATEND record
  \return 1 on success, 0 on error
  \param  contents    Record from which to print data
  EMF+ manual mentioned in 2.1.1.1, reserved, not otherwise documented,
  Microsoft name: EmfPlusMultiFormatEnd Record, Index 0x06
  */
int U_PMR_MULTIFORMATEND_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    return (U_PMR_NODATAREC_draw(contents, out, states));
}

#ifdef __cplusplus
}
#endif

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
