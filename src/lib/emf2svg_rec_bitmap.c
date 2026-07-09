#ifdef __cplusplus
extern "C" {
#endif

#ifndef DARWIN
#define _POSIX_C_SOURCE 200809L
#endif
#include "emf2svg_img_utils.h"
#include "emf2svg_private.h"
#include "emf2svg_print.h"
#include <png.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    POINT_D position;
    POINT_D size;
    bool flip_x;
    bool flip_y;
} imageDestBox;

typedef struct {
    bool open;
    bool moved_world_transform;
} imageClipContext;

/**
  \brief Return whether a bitmap axis must be mirrored in SVG output.

  StretchDIBits mirrors an image when source and destination extents have
  opposite signs. This preserves that sign information for files such as
  nerf-depth-maps.emf, where cDest.y is negative and the image otherwise renders
  upside down after conversion.
  */
static bool image_axis_flipped(double src_extent, double dest_start,
                               double dest_end) {
    bool source_reversed = src_extent < 0.0;
    bool destination_reversed = dest_end < dest_start;

    return source_reversed != destination_reversed;
}

/**
  \brief Convert an EMF destination point and extent into an SVG image box.

  Bitmap destination sizes are vectors, not points. We must transform both
  corners and subtract them, otherwise viewport/window origins leak into the
  size and shrink images like those in test-164.emf.
  */
static void image_dest_box(drawingStates *states, U_POINTL dest, U_POINTL cDest,
                           double cSrcX, double cSrcY, imageDestBox *box) {
    POINT_D corner_a = point_cal(states, (double)dest.x, (double)dest.y);
    POINT_D corner_b =
        point_cal(states, (double)(dest.x + cDest.x), (double)(dest.y + cDest.y));

    box->position.x = fmin(corner_a.x, corner_b.x);
    box->position.y = fmin(corner_a.y, corner_b.y);
    box->size.x = fabs(corner_b.x - corner_a.x);
    box->size.y = fabs(corner_b.y - corner_a.y);
    box->flip_x = image_axis_flipped(cSrcX, corner_a.x, corner_b.x);
    box->flip_y = image_axis_flipped(cSrcY, corner_a.y, corner_b.y);
}

/**
  \brief Start an SVG image element for a bitmap draw.
  */
static void image_draw_start(FILE *out, const imageDestBox *box) {
    fprintf(out,
            "<image width=\"%.4f\" height=\"%.4f\" x=\"%.4f\" y=\"%.4f\" ",
            box->size.x, box->size.y, box->position.x, box->position.y);

    if (box->flip_x || box->flip_y) {
        double tx = box->flip_x ? 2.0 * box->position.x + box->size.x : 0.0;
        double ty = box->flip_y ? 2.0 * box->position.y + box->size.y : 0.0;
        double sx = box->flip_x ? -1.0 : 1.0;
        double sy = box->flip_y ? -1.0 : 1.0;

        fprintf(out, "transform=\"translate(%.4f, %.4f) scale(%.4f, %.4f)\" ",
                tx, ty, sx, sy);
    }
}

/**
  \brief Return whether the SVG image needs a local transform.
  */
static bool image_has_transform(const imageDestBox *box) {
    return box->flip_x || box->flip_y;
}

/**
  \brief Return whether the current world transform changes bitmap coordinates.
  */
static bool image_has_world_transform(drawingStates *states) {
    U_XFORM transform = states->currentDeviceContext.worldTransform;

    return transform.eM11 != 1.0 || transform.eM12 != 0.0 ||
           transform.eM21 != 0.0 || transform.eM22 != 1.0 ||
           transform.eDx != 0.0 || transform.eDy != 0.0;
}

/**
  \brief Start an outer clip group for transformed bitmap images.

  SVG applies an element's active transform to its clip-path. For transformed
  STRETCHDIBITS records, such as framework-overview.emf's cropped depth-map
  images, that makes the page-space clip rectangle cut the wrong area. Wrapping
  the transformed image in a clipped group keeps the clip in page coordinates.
  */
static imageClipContext image_clip_group_start(FILE *out, drawingStates *states,
                                               const imageDestBox *box) {
    imageClipContext context = {false, false};

    if (!states->currentDeviceContext.clipID) {
        return context;
    }

    context.moved_world_transform =
        states->transform_open && image_has_world_transform(states);
    if (!image_has_transform(box) && !context.moved_world_transform) {
        return context;
    }

    if (context.moved_world_transform) {
        fprintf(out, "</%sg>\n", states->nameSpaceString);
        states->transform_open = false;
    }

    fprintf(out, "<%sg", states->nameSpaceString);
    clipset_draw(states, out);
    fprintf(out, ">\n");
    context.open = true;

    if (context.moved_world_transform) {
        transform_draw(states, out);
    }
    return context;
}

/**
  \brief Close an outer bitmap clip group if one was opened.
  */
static void image_clip_group_end(FILE *out, drawingStates *states,
                                 imageClipContext context) {
    if (!context.open) {
        return;
    }

    if (context.moved_world_transform && states->transform_open) {
        fprintf(out, "</%sg>\n", states->nameSpaceString);
        states->transform_open = false;
    }

    fprintf(out, "</%sg>\n", states->nameSpaceString);

    if (context.moved_world_transform) {
        transform_draw(states, out);
    }
}

/**
  \brief Close a pending SVG transform group before emitting a bitmap image.

  Some EMF+ fallback bitmap records already store device-space destinations.
  When that destination matches the record bounds, keeping an earlier SVG
  transform group open applies the transform a second time and moves the image
  outside the canvas, as in textured-mesh-renderings.emf.
  */
static bool image_dest_matches_bounds(const imageDestBox *box, U_RECTL bounds) {
    const double tolerance = 1.0;
    double left = fmin((double)bounds.left, (double)bounds.right);
    double right = fmax((double)bounds.left, (double)bounds.right);
    double top = fmin((double)bounds.top, (double)bounds.bottom);
    double bottom = fmax((double)bounds.top, (double)bounds.bottom);
    double box_right = box->position.x + box->size.x;
    double box_bottom = box->position.y + box->size.y;

    return fabs(box->position.x - left) <= tolerance &&
           fabs(box_right - right) <= tolerance &&
           fabs(box->position.y - top) <= tolerance &&
           fabs(box_bottom - bottom) <= tolerance;
}

/**
  \brief Close a pending transform only for device-space bitmap fallbacks.

  Local-space bitmaps, such as test-155.emf, need the current world transform to
  scale the image into its record bounds, so this must not close unconditionally.
  */
static void image_close_transform_group_if_device_box(FILE *out,
                                                      drawingStates *states,
                                                      const imageDestBox *box,
                                                      U_RECTL bounds) {
    if (states->transform_open && image_dest_matches_bounds(box, bounds)) {
        fprintf(out, "</%sg>\n", states->nameSpaceString);
        states->transform_open = false;
    }
}

/**
  \brief Copy an image box into the pending raster-op mask cache.
  */
static void image_box_to_pending(bitmapRopMask *mask, const imageDestBox *box) {
    mask->position = box->position;
    mask->size = box->size;
    mask->flip_x = box->flip_x;
    mask->flip_y = box->flip_y;
}

/**
  \brief Copy a pending raster-op mask box into the local image box type.
  */
static void image_box_from_pending(const bitmapRopMask *mask, imageDestBox *box) {
    box->position = mask->position;
    box->size = mask->size;
    box->flip_x = mask->flip_x;
    box->flip_y = mask->flip_y;
}

/**
  \brief Return whether a bitmap draw matches a cached raster-op mask.
  */
static bool image_box_matches_pending(const imageDestBox *box,
                                      const bitmapRopMask *mask) {
    const double tolerance = 0.01;

    return fabs(box->position.x - mask->position.x) <= tolerance &&
           fabs(box->position.y - mask->position.y) <= tolerance &&
           fabs(box->size.x - mask->size.x) <= tolerance &&
           fabs(box->size.y - mask->size.y) <= tolerance &&
           box->flip_x == mask->flip_x && box->flip_y == mask->flip_y;
}

/**
  \brief Consume a recently emitted EMF+ bitmap box matching a GDI fallback.

  PowerPoint writes some images twice in dual-mode EMF files: first as EMF+
  DrawImagePoints and then as an EMR_STRETCHDIBITS fallback. This only suppresses
  a fallback when its destination closely matches a bitmap that this converter
  already emitted from EMF+, fixing duplicated framework-overview.emf depth maps
  without skipping unrelated GDI fallback content. Some fallbacks are inset by 
  one pixel on each side and differ by a small float rounding tail, so the 
  tolerance must be slightly above two pixels.
  */
static bool image_consume_matching_emfplus_box(drawingStates *states,
                                               const imageDestBox *box) {
    const double tolerance = 2.25;

    for (size_t i = 0; i < EMFPLUS_RECENT_IMAGE_BOX_COUNT; ++i) {
        emfPlusImageBox *recent = &states->recentEmfPlusImages[i];
        if (!recent->active) {
            continue;
        }
        if (fabs(box->position.x - recent->position.x) <= tolerance &&
            fabs(box->position.y - recent->position.y) <= tolerance &&
            fabs(box->size.x - recent->size.x) <= tolerance &&
            fabs(box->size.y - recent->size.y) <= tolerance) {
            recent->active = false;
            return true;
        }
    }

    return false;
}

/**
  \brief Clear the pending raster-op mask cache.
  */
static void bitmap_rop_mask_clear(drawingStates *states) {
    memset(&states->pendingBitmapMask, 0, sizeof(states->pendingBitmapMask));
}

/**
  \brief Track the solid brush wrapped by a skipped PATINVERT operation.

  Some GDI fallback streams, including test-188.emf, surround a monochrome mask
  path with two PATINVERT BitBlt records. SVG cannot represent destination XOR,
  so the rectangle blits are skipped and the mask path is painted with the
  remembered solid brush color instead.
  */
static void bitblt_patinvert_brush_toggle(drawingStates *states,
                                          const imageDestBox *box) {
    uint8_t red = states->currentDeviceContext.fill_red;
    uint8_t green = states->currentDeviceContext.fill_green;
    uint8_t blue = states->currentDeviceContext.fill_blue;
    uint8_t alpha = 0xff;

    if (states->currentDeviceContext.fill_mode != U_BS_SOLID) {
        states->patinvertBrush.active = false;
        return;
    }

    if (states->recentEmfPlusFill.active) {
        if (states->recentEmfPlusFill.red == red &&
            states->recentEmfPlusFill.green == green &&
            states->recentEmfPlusFill.blue == blue) {
            alpha = states->recentEmfPlusFill.alpha;
        }
        states->recentEmfPlusFill.active = false;
    }

    if (states->patinvertBrush.active && states->patinvertBrush.red == red &&
        states->patinvertBrush.green == green &&
        states->patinvertBrush.blue == blue) {
        if (states->patinvertBrush.consumed ||
            states->patinvertBrush.alpha == alpha) {
            states->patinvertBrush.active = false;
            return;
        }
    }

    states->patinvertBrush.active = true;
    states->patinvertBrush.consumed = false;
    states->patinvertBrush.position = box->position;
    states->patinvertBrush.size = box->size;
    states->patinvertBrush.red = red;
    states->patinvertBrush.green = green;
    states->patinvertBrush.blue = blue;
    states->patinvertBrush.alpha = alpha;
}

/**
  \brief Emit a delayed PATINVERT rectangle if no path mask consumed it.

  Delaying preserves standalone PATINVERT rectangles in samples such as
  test-039.emf while still allowing test-188.emf's following mask path to use
  the remembered brush color and suppress the rectangle.
  */
void bitmap_patinvert_brush_flush(FILE *out, drawingStates *states) {
    pendingPatinvertBrush *brush = &states->patinvertBrush;

    if (!brush->active) {
        return;
    }
    if (!brush->consumed) {
        fprintf(out,
                "<%spath style=\"fill:#%02x%02x%02x\" "
                "d=\"M %.4f,%.4f L %.4f,%.4f L %.4f,%.4f L %.4f,%.4f Z\" ",
                states->nameSpaceString, brush->red, brush->green, brush->blue,
                brush->position.x, brush->position.y,
                brush->position.x + brush->size.x, brush->position.y,
                brush->position.x + brush->size.x,
                brush->position.y + brush->size.y, brush->position.x,
                brush->position.y + brush->size.y);
        if (brush->alpha != 0xff) {
            fprintf(out, "fill-opacity=\"%.4f\" ", brush->alpha / 255.0);
        }
        fprintf(out, "/>");
    }
    memset(brush, 0, sizeof(*brush));
}

/**
  \brief Return whether a STRETCHDIBITS record is the mask half of a ROP pair.

  Some EMF producers encode transparent images as a 1bpp SRCPAINT mask followed
  by a SRCAND color bitmap. The first record is not meant to be visible by
  itself; it prepares the destination for the following color image.
  */
static bool bitmap_is_rop_mask(PU_BITMAPINFOHEADER bmi, uint32_t rop) {
    return rop == U_SRCPAINT && bmi->biCompression == U_BI_RGB &&
           bmi->biBitCount == U_BCBM_MONOCHROME;
}

/**
  \brief Return whether a color bitmap can consume the pending raster-op mask.
  */
static bool bitmap_can_apply_rop_mask(const drawingStates *states,
                                      PU_BITMAPINFOHEADER bmi, uint32_t rop,
                                      const imageDestBox *box) {
    const bitmapRopMask *mask = &states->pendingBitmapMask;

    return mask->active && rop == U_SRCAND && bmi->biCompression == U_BI_RGB &&
           mask->width == (uint32_t)bmi->biWidth &&
           mask->height == (uint32_t)abs(bmi->biHeight) &&
           image_box_matches_pending(box, mask);
}

/**
  \brief Cache a 1bpp SRCPAINT mask for the next matching SRCAND bitmap.
  */
static void bitmap_rop_mask_store(drawingStates *states, const char *contents,
                                  PU_BITMAPINFOHEADER bmi,
                                  const unsigned char *bits, size_t bits_size,
                                  const imageDestBox *box, U_RECTL bounds) {
    bitmapRopMask *mask = &states->pendingBitmapMask;

    bitmap_rop_mask_clear(states);
    mask->active = true;
    image_box_to_pending(mask, box);
    mask->bounds = bounds;
    mask->contents = contents;
    mask->bmi = bmi;
    mask->bits = bits;
    mask->bits_size = bits_size;
    mask->width = (uint32_t)bmi->biWidth;
    mask->height = (uint32_t)abs(bmi->biHeight);
}

/**
  \brief Cache a ROP fallback mask that should suppress its matching color pass.

  Some dual EMF/EMF+ files draw an antialiased EMF+ compressed bitmap, then keep
  a matching 1bpp SRCPAINT/SRCAND GDI fallback. Once the EMF+ bitmap has been
  emitted, the fallback must be skipped or it paints jagged monochrome text over
  the high-quality image.
  */
static void bitmap_rop_mask_store_skipped_fallback(drawingStates *states,
                                                   PU_BITMAPINFOHEADER bmi,
                                                   const imageDestBox *box) {
    bitmapRopMask *mask = &states->pendingBitmapMask;

    bitmap_rop_mask_clear(states);
    mask->active = true;
    mask->skip_color = true;
    image_box_to_pending(mask, box);
    mask->width = (uint32_t)bmi->biWidth;
    mask->height = (uint32_t)abs(bmi->biHeight);
}

/**
  \brief Flush an unmatched raster-op mask as a normal image.

  This keeps non-transparent uses of SRCPAINT visible if they do not form the
  mask/color pair used by textured-mesh-renderings.emf.
  */
void bitmap_rop_mask_flush(FILE *out, drawingStates *states) {
    bitmapRopMask *mask = &states->pendingBitmapMask;
    imageDestBox box;

    if (!mask->active) {
        return;
    }
    if (mask->skip_color) {
        bitmap_rop_mask_clear(states);
        return;
    }

    image_box_from_pending(mask, &box);
    image_close_transform_group_if_device_box(out, states, &box, mask->bounds);
    imageClipContext clip_context = image_clip_group_start(out, states, &box);
    image_draw_start(out, &box);
    if (!clip_context.open) {
        clipset_draw(states, out);
    }
    dib_img_writer(mask->contents, out, states, mask->bmi, mask->bits,
                   mask->bits_size, false);
    fprintf(out, "/>\n");
    image_clip_group_end(out, states, clip_context);
    bitmap_rop_mask_clear(states);
}

void U_EMRALPHABLEND_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRALPHABLEND_print(contents, states);
    }

    PU_EMRALPHABLEND pEmr = (PU_EMRALPHABLEND)(contents);

    // check that the header is not outside of the emf file
    returnOutOfEmf(contents + pEmr->offBmiSrc);
    returnOutOfEmf(contents + pEmr->offBmiSrc + sizeof(U_BITMAPINFOHEADER));

    // get the header
    PU_BITMAPINFOHEADER BmiSrc =
        (PU_BITMAPINFOHEADER)(contents + pEmr->offBmiSrc);

    // check that the bitmap is not outside the emf file
    returnOutOfEmf(contents + pEmr->offBitsSrc);
    returnOutOfEmf(contents + pEmr->offBitsSrc + pEmr->cbBitsSrc);

    const unsigned char *BmpSrc =
        (const unsigned char *)(contents + pEmr->offBitsSrc);

    imageDestBox box;
    image_dest_box(states, pEmr->Dest, pEmr->cDest, pEmr->cSrc.x,
                   pEmr->cSrc.y, &box);
    image_close_transform_group_if_device_box(out, states, &box,
                                              pEmr->rclBounds);
    imageClipContext clip_context = image_clip_group_start(out, states, &box);
    image_draw_start(out, &box);

    float alpha = (float)pEmr->Blend.Global / 255.0;
    fprintf(out, " fill-opacity=\"%.4f\" ", alpha);
    if (!clip_context.open) {
        clipset_draw(states, out);
    }

    dib_img_writer(contents, out, states, BmiSrc, BmpSrc,
                   (size_t)pEmr->cbBitsSrc, false);
    fprintf(out, "/>\n");
    image_clip_group_end(out, states, clip_context);
}
void U_EMRBITBLT_draw(const char *contents, FILE *out, drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRBITBLT_print(contents, states);
    }
    PU_EMRBITBLT pEmr = (PU_EMRBITBLT)(contents);

    if (!(pEmr->cbBitsSrc == 0 && pEmr->dwRop == U_PATINVERT)) {
        bitmap_patinvert_brush_flush(out, states);
    }

    // if no bitmap, check for pattern brush
    // Should fill the output with the current brush and the raster operation
    if (pEmr->cbBitsSrc == 0) {
        char style[256];
        imageDestBox box;
        if (pEmr->dwRop == U_NOOP)
            return;
        POINT_D size =
            point_cal(states, (double)pEmr->cDest.x, (double)pEmr->cDest.y);
        POINT_D position =
            point_cal(states, (double)pEmr->Dest.x, (double)pEmr->Dest.y);
        box.position = position;
        box.size = size;
        box.flip_x = false;
        box.flip_y = false;
        if (pEmr->dwRop == U_PATINVERT) {
            bitblt_patinvert_brush_toggle(states, &box);
            // PATINVERT is an XOR operation; treating it as PATCOPY paints a
            // solid rectangle over files such as test-188.emf.
            verbose_printf("   Status:         %sSKIPPED UNSUPPORTED ROP%s\n",
                           KYEL, KNRM);
            return;
        }
        if (states->currentDeviceContext.fill_mode == U_BS_MONOPATTERN) {
            sprintf(style, "fill:url(#img-%d-ref);",
                    states->currentDeviceContext.fill_idx);
        } else if (states->currentDeviceContext.fill_mode == U_BS_SOLID) {
            sprintf(style, "fill:#%02x%02x%02x",
                    states->currentDeviceContext.fill_red,
                    states->currentDeviceContext.fill_green,
                    states->currentDeviceContext.fill_blue);
        } else {
            style[0] = '\0';
        }
        if (style[0]) {
            fprintf(out, "<%spath style=\"%s", states->nameSpaceString, style);
            fprintf(
                out,
                "\" d=\"M %.4f,%.4f L %.4f,%.4f L %.4f,%.4f L %.4f,%.4f Z\" />",
                position.x, position.y, position.x + size.x, position.y,
                position.x + size.x, position.y + size.y, position.x,
                position.y + size.y);
        }
        // else
        // FIXME - non MONOBRUSH
        return;
    }

    // FIXME doesn't handle ternary raster operation

    // check that the header is not outside of the emf file
    returnOutOfEmf(contents + pEmr->offBmiSrc);
    returnOutOfEmf(contents + pEmr->offBmiSrc + sizeof(U_BITMAPINFOHEADER));

    // get the header
    PU_BITMAPINFOHEADER BmiSrc =
        (PU_BITMAPINFOHEADER)(contents + pEmr->offBmiSrc);

    // check that the bitmap is not outside the emf file
    returnOutOfEmf(contents + pEmr->offBitsSrc);
    returnOutOfEmf(contents + pEmr->offBitsSrc + pEmr->cbBitsSrc);

    const unsigned char *BmpSrc =
        (const unsigned char *)(contents + pEmr->offBitsSrc);

    imageDestBox box;
    image_dest_box(states, pEmr->Dest, pEmr->cDest, fabs((double)pEmr->cDest.x),
                   fabs((double)pEmr->cDest.y), &box);
    image_close_transform_group_if_device_box(out, states, &box,
                                              pEmr->rclBounds);
    imageClipContext clip_context = image_clip_group_start(out, states, &box);
    image_draw_start(out, &box);
    if (!clip_context.open) {
        clipset_draw(states, out);
    }

    // float alpha = (float)pEmr->Blend.Global / 255.0;
    // fprintf(out, " fill-opacity=\"%.4f\" ", alpha);

    dib_img_writer(contents, out, states, BmiSrc, BmpSrc,
                   (size_t)pEmr->cbBitsSrc, false);
    fprintf(out, "/>\n");
    image_clip_group_end(out, states, clip_context);
}
void U_EMRMASKBLT_draw(const char *contents, FILE *out, drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRMASKBLT_print(contents, states);
    }
    // PU_EMRMASKBLT pEmr = (PU_EMRMASKBLT) (contents);
}
void U_EMRPLGBLT_draw(const char *contents, FILE *out, drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRPLGBLT_print(contents, states);
    }
    // PU_EMRPLGBLT pEmr = (PU_EMRPLGBLT) (contents);
}
void U_EMRSETDIBITSTODEVICE_draw(const char *contents, FILE *out,
                                 drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSETDIBITSTODEVICE_print(contents, states);
    }
    // PU_EMRSETDIBITSTODEVICE pEmr = (PU_EMRSETDIBITSTODEVICE) (contents);
}
void U_EMRSTRETCHBLT_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSTRETCHBLT_print(contents, states);
    }
    PU_EMRSTRETCHBLT pEmr = (PU_EMRSTRETCHBLT)(contents);
    // check that the header is not outside of the emf file
    returnOutOfEmf(contents + pEmr->offBmiSrc);
    returnOutOfEmf(contents + pEmr->offBmiSrc + sizeof(U_BITMAPINFOHEADER));

    // get the header
    PU_BITMAPINFOHEADER BmiSrc =
        (PU_BITMAPINFOHEADER)(contents + pEmr->offBmiSrc);

    // check that the bitmap is not outside the emf file
    returnOutOfEmf(contents + pEmr->offBitsSrc);
    returnOutOfEmf(contents + pEmr->offBitsSrc + pEmr->cbBitsSrc);

    const unsigned char *BmpSrc =
        (const unsigned char *)(contents + pEmr->offBitsSrc);

    imageDestBox box;
    image_dest_box(states, pEmr->Dest, pEmr->cDest, pEmr->cSrc.x,
                   pEmr->cSrc.y, &box);
    image_close_transform_group_if_device_box(out, states, &box,
                                              pEmr->rclBounds);
    imageClipContext clip_context = image_clip_group_start(out, states, &box);
    image_draw_start(out, &box);
    if (!clip_context.open) {
        clipset_draw(states, out);
    }

    dib_img_writer(contents, out, states, BmiSrc, BmpSrc,
                   (size_t)pEmr->cbBitsSrc, false);
    fprintf(out, "/>\n");
    image_clip_group_end(out, states, clip_context);
}

/**
  \brief Decode an uncompressed or RLE DIB into an RGBA bitmap.
  */
static bool dib_to_rgba_bitmap(PU_BITMAPINFOHEADER BmiSrc,
                               const unsigned char *BmpSrc, size_t size,
                               bool assign_mono_colors_from_dc,
                               drawingStates *states, RGBABitmap *bitmap) {
    RGBBitmap convert_in;
    RGBBitmap convert_out;
    const U_RGBQUAD *ct = NULL;
    U_RGBQUAD monoCt[2];
    uint32_t width, height, colortype, numCt, invert;
    char *rgba_px = NULL;
    char *in;
    size_t img_size;

    memset(bitmap, 0, sizeof(*bitmap));
    memset(&convert_out, 0, sizeof(convert_out));

    convert_in.size = size;
    convert_in.width = BmiSrc->biWidth;
    convert_in.height = BmiSrc->biHeight;
    convert_in.pixels = (RGBPixel *)BmpSrc;
    convert_in.bytewidth = BmiSrc->biWidth * 3;
    convert_in.bytes_per_pixel = 3;

    switch (BmiSrc->biCompression) {
    case U_BI_RLE8:
        convert_out = rle8ToRGB8(convert_in);
        break;
    case U_BI_RLE4:
        convert_out = rle4ToRGB(convert_in);
        break;
    }

    if (convert_out.pixels != NULL) {
        in = (char *)convert_out.pixels;
        img_size = convert_out.size;
    } else {
        in = (char *)convert_in.pixels;
        img_size = convert_in.size;
    }

    if (e2s_get_DIB_params((PU_BITMAPINFO)BmiSrc, (const U_RGBQUAD **)&ct,
                           &numCt, &width, &height, &colortype, &invert) ||
        width > MAX_BMP_WIDTH || height > MAX_BMP_HEIGHT) {
        free(convert_out.pixels);
        return false;
    }

    size_t offset_check =
        (size_t)((float)width * (float)height * get_pixel_size(colortype));
    if (((in + img_size) < in + offset_check)) {
        free(convert_out.pixels);
        return false;
    }

    if (colortype == U_BCBM_MONOCHROME && assign_mono_colors_from_dc) {
        monoCt[0].Red = states->currentDeviceContext.text_red;
        monoCt[0].Green = states->currentDeviceContext.text_green;
        monoCt[0].Blue = states->currentDeviceContext.text_blue;
        monoCt[0].Reserved = 0xff;
        monoCt[1].Red = states->currentDeviceContext.bk_red;
        monoCt[1].Green = states->currentDeviceContext.bk_green;
        monoCt[1].Blue = states->currentDeviceContext.bk_blue;
        monoCt[1].Reserved = 0xff;
        ct = monoCt;
    }

    if (DIB_to_RGBA(in, ct, numCt, &rgba_px, width, height, colortype, numCt,
                    invert) ||
        rgba_px == NULL) {
        free(convert_out.pixels);
        return false;
    }

    bitmap->size = width * 4 * height;
    bitmap->width = width;
    bitmap->height = height;
    bitmap->pixels = (RGBAPixel *)rgba_px;
    bitmap->bytewidth = BmiSrc->biWidth * 3;
    bitmap->bytes_per_pixel = 3;

    free(convert_out.pixels);
    return true;
}

/**
  \brief Write a SRCAND bitmap with alpha synthesized from a cached mask.

  This handles the GDI transparent-image sequence used by
  textured-mesh-renderings.emf: a white 1bpp SRCPAINT mask marks the pixels that
  should receive the following SRCAND color image.
  */
static bool dib_masked_img_writer(FILE *out, drawingStates *states,
                                  PU_BITMAPINFOHEADER BmiSrc,
                                  const unsigned char *BmpSrc, size_t size,
                                  const bitmapRopMask *mask) {
    RGBABitmap color;
    RGBABitmap mask_bitmap;
    char *png = NULL;
    char *b64 = NULL;
    size_t png_size;
    size_t b64_size;
    bool color_has_alpha = false;

    if (!dib_to_rgba_bitmap(BmiSrc, BmpSrc, size, false, states, &color)) {
        return false;
    }
    if (!dib_to_rgba_bitmap(mask->bmi, mask->bits, mask->bits_size, false,
                            states, &mask_bitmap)) {
        free(color.pixels);
        return false;
    }
    if (color.width != mask_bitmap.width || color.height != mask_bitmap.height) {
        free(color.pixels);
        free(mask_bitmap.pixels);
        return false;
    }

    for (size_t i = 0; i < color.width * color.height; ++i) {
        if (color.pixels[i].alpha) {
            color_has_alpha = true;
            break;
        }
    }

    for (size_t i = 0; i < color.width * color.height; ++i) {
        uint8_t mask_alpha = mask_bitmap.pixels[i].red > 127 ? 0xff : 0x00;
        uint8_t source_alpha = color_has_alpha ? color.pixels[i].alpha : 0xff;
        color.pixels[i].alpha = (uint8_t)(((uint16_t)source_alpha *
                                           (uint16_t)mask_alpha) /
                                          0xff);
    }

    if (rgb2png_with_alpha_mode(&color, &png, &png_size, false) || png == NULL) {
        free(color.pixels);
        free(mask_bitmap.pixels);
        return false;
    }

    b64 = base64_encode((unsigned char *)png, png_size, &b64_size);
    free(png);
    free(color.pixels);
    free(mask_bitmap.pixels);

    if (b64 == NULL) {
        return false;
    }

    fprintf(out, "xlink:href=\"data:image/png;base64,%s\" ", b64);
    free(b64);
    return true;
}

void U_EMRSTRETCHDIBITS_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSTRETCHDIBITS_print(contents, states);
    }
    PU_EMRSTRETCHDIBITS pEmr = (PU_EMRSTRETCHDIBITS)(contents);

    // check that the header is not outside of the emf file
    returnOutOfEmf(contents + pEmr->offBmiSrc);
    returnOutOfEmf(contents + pEmr->offBmiSrc + sizeof(U_BITMAPINFOHEADER));

    // get the header
    PU_BITMAPINFOHEADER BmiSrc =
        (PU_BITMAPINFOHEADER)(contents + pEmr->offBmiSrc);

    // check that the bitmap is not outside the emf file
    returnOutOfEmf(contents + pEmr->offBitsSrc);
    returnOutOfEmf(contents + pEmr->offBitsSrc + pEmr->cbBitsSrc);

    const unsigned char *BmpSrc =
        (const unsigned char *)(contents + pEmr->offBitsSrc);

    imageDestBox box;
    image_dest_box(states, pEmr->Dest, pEmr->cDest, pEmr->cSrc.x,
                   pEmr->cSrc.y, &box);

    if (bitmap_is_rop_mask(BmiSrc, pEmr->dwRop)) {
        bitmap_rop_mask_flush(out, states);
        if (states->emfplus && image_consume_matching_emfplus_box(states, &box)) {
            bitmap_rop_mask_store_skipped_fallback(states, BmiSrc, &box);
            verbose_printf("   Status:         %sSKIPPED EMF+ ROP MASK FALLBACK%s\n",
                           KYEL, KNRM);
            return;
        }
        bitmap_rop_mask_store(states, contents, BmiSrc, BmpSrc,
                              (size_t)pEmr->cbBitsSrc, &box, pEmr->rclBounds);
        return;
    }

    if (states->emfplus && pEmr->dwRop == U_SRCCOPY &&
        image_consume_matching_emfplus_box(states, &box)) {
        bitmap_rop_mask_flush(out, states);
        verbose_printf("   Status:         %sSKIPPED EMF+ BITMAP FALLBACK%s\n",
                       KYEL, KNRM);
        return;
    }

    bool use_pending_mask =
        bitmap_can_apply_rop_mask(states, BmiSrc, pEmr->dwRop, &box);
    if (use_pending_mask && states->pendingBitmapMask.skip_color) {
        bitmap_rop_mask_clear(states);
        verbose_printf("   Status:         %sSKIPPED EMF+ ROP COLOR FALLBACK%s\n",
                       KYEL, KNRM);
        return;
    }
    if (!use_pending_mask) {
        bitmap_rop_mask_flush(out, states);
    }

    image_close_transform_group_if_device_box(out, states, &box,
                                              pEmr->rclBounds);
    imageClipContext clip_context = image_clip_group_start(out, states, &box);
    image_draw_start(out, &box);
    if (!clip_context.open) {
        clipset_draw(states, out);
    }

    if (use_pending_mask &&
        dib_masked_img_writer(out, states, BmiSrc, BmpSrc,
                              (size_t)pEmr->cbBitsSrc,
                              &states->pendingBitmapMask)) {
        bitmap_rop_mask_clear(states);
    } else {
        if (use_pending_mask) {
            bitmap_rop_mask_clear(states);
        }
        dib_img_writer(contents, out, states, BmiSrc, BmpSrc,
                       (size_t)pEmr->cbBitsSrc, false);
    }
    fprintf(out, "/>\n");
    image_clip_group_end(out, states, clip_context);
}
void U_EMRTRANSPARENTBLT_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRTRANSPARENTBLT_print(contents, states);
    }
}

void dib_img_writer(const char *contents, FILE *out, drawingStates *states,
                    PU_BITMAPINFOHEADER BmiSrc, const unsigned char *BmpSrc,
                    size_t size, bool assign_mono_colors_from_dc) {
    char *b64Bmp = NULL;
    size_t b64s;
    char *tmp = NULL;

    // Handle simple cases first, no treatment needed for them
    switch (BmiSrc->biCompression) {
    case U_BI_JPEG:
        b64Bmp = base64_encode(BmpSrc, size, &b64s);
        fprintf(out, "xlink:href=\"data:image/jpg;base64,");
        break;
    case U_BI_PNG:
        b64Bmp = base64_encode(BmpSrc, size, &b64s);
        fprintf(out, "xlink:href=\"data:image/png;base64,");
        break;
    }
    if (b64Bmp != NULL) {
        fprintf(out, "%s\" ", b64Bmp);
        free(b64Bmp);
        return;
    }

    // more complexe treatment, with conversion to png
    RGBBitmap convert_in;
    convert_in.size = size;
    convert_in.width = BmiSrc->biWidth;
    convert_in.height = BmiSrc->biHeight;
    convert_in.pixels = (RGBPixel *)BmpSrc;
    convert_in.bytewidth = BmiSrc->biWidth * 3;
    convert_in.bytes_per_pixel = 3;

    RGBBitmap convert_out;
    convert_out.pixels = NULL;
    const U_RGBQUAD *ct = NULL;
    U_RGBQUAD monoCt[2];
    uint32_t width, height, colortype, numCt, invert;
    char *rgba_px = NULL;
    int dibparams;
    char *in;
    size_t img_size;

    RGBABitmap convert_inpng;

    // In any cases after that, we get a png blob
    fprintf(out, "xlink:href=\"data:image/png;base64,");

    switch (BmiSrc->biCompression) {
    case U_BI_RLE8:
        convert_out = rle8ToRGB8(convert_in);
        break;
    case U_BI_RLE4:
        convert_out = rle4ToRGB(convert_in);
        break;
    }

    if (convert_out.pixels != NULL) {
        in = (char *)convert_out.pixels;
        img_size = convert_out.size;
    } else {
        in = (char *)convert_in.pixels;
        img_size = convert_in.size;
    }

    dibparams =
        e2s_get_DIB_params((PU_BITMAPINFO)BmiSrc, (const U_RGBQUAD **)&ct,
                           &numCt, &width, &height, &colortype, &invert);
    // if enable to read header, then exit
    if (dibparams || width > MAX_BMP_WIDTH || height > MAX_BMP_HEIGHT) {
        free(convert_out.pixels);
        states->Error = true;
        return;
    }
    // check that what we will read in the DIB_to_RGBA conversion is actually
    // there
    size_t offset_check =
        (size_t)((float)width * (float)height * get_pixel_size(colortype));
    if (((in + img_size) < in + offset_check)) {
        free(convert_out.pixels);
        states->Error = true;
        return;
    }
    if (colortype == U_BCBM_MONOCHROME) {
        if (assign_mono_colors_from_dc) {
            monoCt[0].Red = states->currentDeviceContext.text_red;
            monoCt[0].Green = states->currentDeviceContext.text_green;
            monoCt[0].Blue = states->currentDeviceContext.text_blue;
            monoCt[0].Reserved = 0xff;
            monoCt[1].Red = states->currentDeviceContext.bk_red;
            monoCt[1].Green = states->currentDeviceContext.bk_green;
            monoCt[1].Blue = states->currentDeviceContext.bk_blue;
            monoCt[1].Reserved =
                0xff; // states->currentDeviceContext.bk_mode ? 0xff : 0;
            ct = monoCt;
        }
    }
    DIB_to_RGBA(in, ct, numCt, &rgba_px, width, height, colortype, numCt,
                invert);

    if (rgba_px != NULL) {
        convert_inpng.size = width * 4 * height;
        convert_inpng.width = width;
        convert_inpng.height = height;
        convert_inpng.pixels = (RGBAPixel *)rgba_px;
        convert_inpng.bytewidth = BmiSrc->biWidth * 3;
        convert_inpng.bytes_per_pixel = 3;

        rgb2png(&convert_inpng, &b64Bmp, &b64s);
        tmp = (char *)b64Bmp;
        b64Bmp = base64_encode((unsigned char *)b64Bmp, b64s, &b64s);
        free(convert_out.pixels);
        free(tmp);
        free(rgba_px);
    }

    if (b64Bmp != NULL) {
        fprintf(out, "%s\" ", b64Bmp);
        free(b64Bmp);
    } else {
        // transparent 5x5 px png
        fprintf(out, "iVBORw0KGgoAAAANSUhEUgAAAAUAAAAFCAYAAACNbyblAAAABGdBTUEAA"
                     "LGPC/xhBQAAAAZiS0dEAP8A/wD/"
                     "oL2nkwAAAAlwSFlzAAALEwAACxMBAJqcGAAAAAd0SU1FB+"
                     "ABFREtOJX7FAkAAAAIdEVYdENvbW1lbnQA9syWvwAAAAxJREFUCNdjYKA"
                     "TAAAAaQABwB3y+AAAAABJRU5ErkJggg==\" ");
    }
}

// Find an image that matches (otherwise return NULL)
emfImageLibrary *image_library_find(emfImageLibrary *lib,
                                    PU_BITMAPINFOHEADER BmiSrc, size_t size) {
    while (lib) {
        if (memcmp(BmiSrc, lib->content, size) == 0)
            return lib;
        lib = lib->next;
    }
    return NULL;
}

// Create a new image
emfImageLibrary *image_library_create(int id, PU_BITMAPINFOHEADER BmiSrc,
                                      size_t size) {
    emfImageLibrary *image =
        (emfImageLibrary *)calloc(1, sizeof(emfImageLibrary) + size);
    image->id = id;
    image->content = (PU_BITMAPINFOHEADER)(image + 1);
    memcpy(image->content, BmiSrc, size);
    return image;
}

// Add an image to the states image 'library'
emfImageLibrary *image_library_add(drawingStates *states,
                                   PU_BITMAPINFOHEADER BmiSrc, size_t size) {
    ++states->count_images;
    emfImageLibrary *image =
        image_library_create(states->count_images, BmiSrc, size);
    if (states->library) {
        emfImageLibrary *last = states->library;
        while (last->next) {
            last = last->next;
        }
        last->next = image;
    } else {
        states->library = image;
    }
    return image;
}

// Release image library;
void freeEmfImageLibrary(drawingStates *states) {
    emfImageLibrary *last = states->library;
    while (last) {
        emfImageLibrary *next = last->next;
        free(last);
        last = next;
    }
}

// Lookup existing - or create and emit new image reference for use with image
// brush
emfImageLibrary *image_library_writer(const char *contents, FILE *out,
                                      drawingStates *states,
                                      PU_BITMAPINFOHEADER BmiSrc, size_t size,
                                      const unsigned char *BmpSrc) {
    emfImageLibrary *image = image_library_find(states->library, BmiSrc, size);
    if (!image) {
        image = image_library_add(states, BmiSrc, size);
        if (image) {
            const U_RGBQUAD *ct = NULL;
            uint32_t width = 0, height = 0, colortype, numCt, invert;
            e2s_get_DIB_params((PU_BITMAPINFO)BmiSrc, (const U_RGBQUAD **)&ct,
                               &numCt, &width, &height, &colortype, &invert);
            if (width > 0 && height > 0) {
                fprintf(out, "<%sdefs><%simage id=\"img-%d\" x=\"0\" y=\"0\" "
                             "width=\"%d\" height=\"%d\" ",
                        states->nameSpaceString, states->nameSpaceString,
                        image->id, width, height);
                dib_img_writer(contents, out, states, BmiSrc, BmpSrc, size,
                               true);
                fprintf(out, " preserveAspectRatio=\"none\" />");
                fprintf(out, "<%spattern id=\"img-%d-ref\" x=\"0\" y=\"0\" "
                             "width=\"%d\" height=\"%d\" "
                             "patternUnits=\"userSpaceOnUse\" >\n",
                        states->nameSpaceString, image->id, width, height);
                fprintf(out,
                        "<%suse id=\"img-%d-ign\" xlink:href=\"#img-%d\" />",
                        states->nameSpaceString, image->id, image->id);
                fprintf(out, "</%spattern></%sdefs>\n", states->nameSpaceString,
                        states->nameSpaceString);
            };
        }
    }
    return image;
}
#ifdef __cplusplus
}
#endif
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
