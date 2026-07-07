#ifdef __cplusplus
extern "C" {
#endif

#ifndef DARWIN
#define _POSIX_C_SOURCE 200809L
#endif
#include "emf2svg_private.h"
#include "emf2svg_print.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iconv.h>
#include <errno.h>
#include <ft2build.h>
#include <fontconfig/fontconfig.h>
#include FT_FREETYPE_H

void U_EMRNOTIMPLEMENTED_draw(const char *name, const char *contents, FILE *out,
                              drawingStates *states) {
    UNUSED(name);
    // if (states->verbose){U_EMRNOTIMPLEMENTED_print(contents, states);}
    UNUSED(contents);
}
void U_swap4(void *ul, unsigned int count);
//! \endcond

/**
  \brief Print rect and rectl objects from Upper Left and Lower Right corner
  points.
  \param rect U_RECTL object
  */
#ifndef _MSC_VER
double _dsign(double v) {
    if (v >= 0)
        return 1;
    else
        return -1;
}
#endif

void arc_circle_draw(const char *contents, FILE *out, drawingStates *states) {
    PU_EMRANGLEARC pEmr = (PU_EMRANGLEARC)(contents);
    startPathDraw(states, out);
    U_POINTL radii;
    int sweep_flag = 0;
    int large_arc_flag = 0;
    // FIXME calculate the real orientation
    if (states->currentDeviceContext.arcdir > 0) {
        sweep_flag = 1;
        large_arc_flag = 1;
    } else {
        sweep_flag = 0;
        large_arc_flag = 0;
    }
    radii.x = pEmr->nRadius;
    radii.y = pEmr->nRadius;

    fprintf(out, "M ");
    POINT_D start;
    double angle = pEmr->eStartAngle * U_PI / 180;

    addNewSegPath(states, SEG_LINE);
    start.x = pEmr->nRadius * cos(angle) + pEmr->ptlCenter.x;
    start.y = pEmr->nRadius * sin(angle) + pEmr->ptlCenter.y;
    point_draw_d(states, start, out);
    pointCurrPathAddD(states, start, 0);

    addNewSegPath(states, SEG_ARC);
    fprintf(out, "A ");
    point_draw(states, radii, out);
    pointCurrPathAdd(states, radii, 0);

    fprintf(out, "0 ");
    fprintf(out, "%d %d ", large_arc_flag, sweep_flag);

    angle = (pEmr->eStartAngle + pEmr->eSweepAngle) * U_PI / 180;
    POINT_D end;
    end.x = pEmr->nRadius * cos(angle) + pEmr->ptlCenter.x;
    end.y = pEmr->nRadius * sin(angle) + pEmr->ptlCenter.y;
    point_draw_d(states, end, out);
    pointCurrPathAddD(states, end, 1);

    endPathDraw(states, out);
}
void arc_draw(const char *contents, FILE *out, drawingStates *states,
              int type) {
    PU_EMRARC pEmr = (PU_EMRARC)(contents);
    startPathDraw(states, out);
    U_POINTL radii;
    int sweep_flag = 0;
    int large_arc_flag = 0;
    // FIXME calculate the real orientation
    if (states->currentDeviceContext.arcdir > 0) {
        sweep_flag = 1;
        large_arc_flag = 1;
    } else {
        sweep_flag = 0;
        large_arc_flag = 0;
    }
    radii.x = (pEmr->rclBox.right - pEmr->rclBox.left) / 2;
    radii.y = (pEmr->rclBox.bottom - pEmr->rclBox.top) / 2;

    addNewSegPath(states, SEG_LINE);
    fprintf(out, "M ");
    POINT_D start = int_el_rad(pEmr->ptlStart, pEmr->rclBox);
    point_draw_d(states, start, out);
    pointCurrPathAddD(states, start, 0);

    addNewSegPath(states, SEG_ARC);

    fprintf(out, "A ");
    point_draw(states, radii, out);
    pointCurrPathAdd(states, radii, 0);

    fprintf(out, "0 ");
    fprintf(out, "%d %d ", large_arc_flag, sweep_flag);

    POINT_D end = int_el_rad(pEmr->ptlEnd, pEmr->rclBox);
    point_draw_d(states, end, out);
    pointCurrPathAddD(states, end, 1);

    switch (type) {
    case ARC_PIE:
        fprintf(out, "L ");
        U_POINTL center;
        center.x = (pEmr->rclBox.right + pEmr->rclBox.left) / 2;
        center.y = (pEmr->rclBox.bottom + pEmr->rclBox.top) / 2;
        point_draw(states, center, out);
        addNewSegPath(states, SEG_LINE);
        pointCurrPathAdd(states, center, 0);
        fprintf(out, "Z ");
        addNewSegPath(states, SEG_END);
        endFormDraw(states, out);
        break;
    case ARC_CHORD:
        fprintf(out, "Z ");
        addNewSegPath(states, SEG_END);
        endFormDraw(states, out);
        break;
    default:
        endPathDraw(states, out);
        break;
    }
}
void basic_stroke(drawingStates *states, FILE *out) {
    color_stroke(states, out);
    width_stroke(states, out, states->currentDeviceContext.stroke_width);
}
bool checkOutOfEMF(drawingStates *states, uintptr_t address) {
    if (address > states->endAddress) {
        states->Error = true;
        return true;
    } else {
        return false;
    }
}
bool checkOutOfOTIndex(drawingStates *states, int64_t index) {
    if (index > states->objectTableSize) {
        states->Error = true;
        return true;
    } else {
        return false;
    }
}
void color_stroke(drawingStates *states, FILE *out) {
    fprintf(out, "stroke=\"#%02X%02X%02X\" ",
            states->currentDeviceContext.stroke_red,
            states->currentDeviceContext.stroke_green,
            states->currentDeviceContext.stroke_blue);
}
void copyDeviceContext(EMF_DEVICE_CONTEXT *dest, EMF_DEVICE_CONTEXT *src) {
    // copy simple data (int, double...)
    *dest = *src;

    // copy more complex data (pointers...)
    if (src->font_name != NULL) {
        dest->font_name =
            (char *)calloc(strlen(src->font_name) + 1, sizeof(char));
        strcpy(dest->font_name, src->font_name);
    }
    if (src->font_family != NULL) {
        dest->font_family =
            (char *)calloc(strlen(src->font_family) + 1, sizeof(char));
        strcpy(dest->font_family, src->font_family);
    }
    copy_path(src->clipRGN, &(dest->clipRGN));
}
void cubic_bezier16_draw(const char *name, const char *contents, FILE *out,
                         drawingStates *states, int startingPoint) {
    UNUSED(name);
    unsigned int i;
    PU_EMRPOLYBEZIER16 pEmr = (PU_EMRPOLYBEZIER16)(contents);
    startPathDraw(states, out);
    PU_POINT16 papts = (PU_POINT16)(&(pEmr->apts));
    returnOutOfEmf((intptr_t)papts +
                   (intptr_t)(pEmr->cpts) * sizeof(U_POINT16));
    if (startingPoint == 1) {
        fprintf(out, "M ");
        point16_draw(states, papts[0], out);
        addNewSegPath(states, SEG_MOVE);
        pointCurrPathAdd16(states, papts[0], 0);
    }
    const int ctrl1 = (0 + startingPoint) % 3;
    const int ctrl2 = (1 + startingPoint) % 3;
    const int to = (2 + startingPoint) % 3;
    int index = 0;
    for (i = startingPoint; i < pEmr->cpts; i++) {
        if ((i % 3) == ctrl1) {
            index = 0;
            addNewSegPath(states, SEG_BEZIER);
            pointCurrPathAdd16(states, papts[i], index);
            index++;
            fprintf(out, "C ");
            point16_draw(states, papts[i], out);
        } else if ((i % 3) == ctrl2) {
            point16_draw(states, papts[i], out);
            pointCurrPathAdd16(states, papts[i], index);
            index++;
        } else if ((i % 3) == to) {
            point16_draw(states, papts[i], out);
            pointCurrPathAdd16(states, papts[i], index);
            index++;
        }
    }
    endPathDraw(states, out);
}
void cubic_bezier_draw(const char *name, const char *contents, FILE *out,
                       drawingStates *states, int startingPoint) {
    UNUSED(name);
    unsigned int i;
    PU_EMRPOLYBEZIER pEmr = (PU_EMRPOLYBEZIER)(contents);
    startPathDraw(states, out);
    PU_POINT papts = (PU_POINT)(&(pEmr->aptl));
    returnOutOfEmf((intptr_t)papts + (intptr_t)pEmr->cptl * sizeof(U_POINT));
    if (startingPoint == 1) {
        fprintf(out, "M ");
        point_draw(states, papts[0], out);
        addNewSegPath(states, SEG_BEZIER);
        pointCurrPathAdd(states, papts[0], 0);
    }
    const int ctrl1 = (0 + startingPoint) % 3;
    const int ctrl2 = (1 + startingPoint) % 3;
    const int to = (2 + startingPoint) % 3;
    int index = 0;
    for (i = startingPoint; i < pEmr->cptl; i++) {
        if ((i % 3) == ctrl1) {
            index = 0;
            addNewSegPath(states, SEG_BEZIER);
            pointCurrPathAdd(states, papts[i], index);
            index++;
            fprintf(out, "C ");
            point_draw(states, papts[i], out);
        } else if ((i % 3) == ctrl2) {
            point_draw(states, papts[i], out);
            pointCurrPathAdd(states, papts[i], index);
            index++;
        } else if ((i % 3) == to) {
            point_draw(states, papts[i], out);
            pointCurrPathAdd(states, papts[i], index);
            index++;
        }
    }
    endPathDraw(states, out);
}
void endFormDraw(drawingStates *states, FILE *out) {
    if (!(states->inPath)) {
        fprintf(out, "\" ");
        bool filled = false;
        bool stroked = false;
        stroke_draw(states, out, &filled, &stroked);
        fill_draw(states, out, &filled, &stroked);
        if (!filled)
            fprintf(out, "fill=\"none\" ");
        if (!stroked)
            fprintf(out, "stroke=\"none\" ");
        fprintf(out, " />\n");
    }
}
void endPathDraw(drawingStates *states, FILE *out) {
    if (!(states->inPath)) {
        fprintf(out, "\" ");
        bool filled;
        bool stroked;
        stroke_draw(states, out, &filled, &stroked);
        fprintf(out, " fill=\"none\" />\n");
    }
}
void fill_draw(drawingStates *states, FILE *out, bool *filled, bool *stroked) {
    if (states->verbose) {
        fill_print(states);
    }
    char *fill_rule = calloc(40, sizeof(char));
    switch (states->currentDeviceContext.fill_mode) {
    case (U_ALTERNATE):
        sprintf(fill_rule, "fill-rule:\"evenodd\" ");
        break;
    case (U_WINDING):
        sprintf(fill_rule, "fill-rule:\"nonzero\" ");
        break;
    default:
        sprintf(fill_rule, " ");
        break;
    }
    switch (states->currentDeviceContext.fill_mode) {
    case U_BS_SOLID:
        *filled = true;
        fprintf(out, "%s", fill_rule);
        fprintf(out, "fill=\"#%02X%02X%02X\" ",
                states->currentDeviceContext.fill_red,
                states->currentDeviceContext.fill_green,
                states->currentDeviceContext.fill_blue);
        break;
    case U_BS_NULL:
        fprintf(out, "fill=\"none\" ");
        *filled = true;
        break;
    case U_BS_MONOPATTERN:
        fprintf(out, "fill=\"#img-%d-ref\" ",
                states->currentDeviceContext.fill_idx);
        *filled = true;
        break;
    case U_BS_HATCHED:
    case U_BS_PATTERN:
    case U_BS_INDEXED:
    case U_BS_DIBPATTERN:
    case U_BS_DIBPATTERNPT:
    case U_BS_PATTERN8X8:
    case U_BS_DIBPATTERN8X8:
    default:
        // partial
        fprintf(out, "fill=\"#%02X%02X%02X\" ",
                states->currentDeviceContext.fill_red,
                states->currentDeviceContext.fill_green,
                states->currentDeviceContext.fill_blue);
        *filled = true;
        break;
    }
    free(fill_rule);
    return;
}
void freeDeviceContext(EMF_DEVICE_CONTEXT *dc) {
    if (dc != NULL) {
        if (dc->font_name != NULL)
            free(dc->font_name);
        if (dc->font_family != NULL)
            free(dc->font_family);
        free_path(&(dc->clipRGN));
    }
}
void freeDeviceContextStack(drawingStates *states) {
    EMF_DEVICE_CONTEXT_STACK *stack_entry = states->DeviceContextStack;
    while (stack_entry != NULL) {
        EMF_DEVICE_CONTEXT_STACK *next_entry = stack_entry->previous;
        freeDeviceContext(&(stack_entry->DeviceContext));
        free(stack_entry);
        stack_entry = next_entry;
    }
}
void freeObject(drawingStates *states, uint16_t index) {
    if (states->objectTable[index].font_name != NULL)
        free(states->objectTable[index].font_name);
    if (states->objectTable[index].font_family != NULL)
        free(states->objectTable[index].font_family);
    states->objectTable[index] = (const emfGraphObject){0};
}
void freeObjectTable(drawingStates *states) {
    for (int32_t i = 0; i < (states->objectTableSize + 1); i++) {
        freeObject(states, i);
    }
}
void freePathStack(pathStack *stack) {
    while (stack != NULL) {
        // free(stack->pathStruct);
        pathStack *tmp = stack;
        stack = stack->next;
        free(tmp);
    }
    return;
}
int get_id(drawingStates *states) {
    states->uniqId = rand();
    return states->uniqId;
}
POINT_D int_el_rad(U_POINTL pt, U_RECTL rect) {
    POINT_D center, intersect, radii, pt_no;
    center.x = (rect.right + rect.left) / 2;
    center.y = (rect.bottom + rect.top) / 2;

    radii.x = (rect.right - rect.left) / 2;
    radii.y = (rect.bottom - rect.top) / 2;

    if ((radii.x == 0) || (radii.y == 0)) {
        return center;
    }

    // change orgin (new origin is ellipse center)
    pt_no.x = pt.x - center.x;
    pt_no.y = pt.y - center.y;

    if (pt_no.x == 0) {
        intersect.x = center.x;
        intersect.y = _dsign(pt_no.y) * radii.y + center.y;
        return intersect;
    }

    if (pt_no.y == 0) {
        intersect.x = _dsign(pt_no.x) * radii.x + center.x;
        intersect.y = center.y;
        return intersect;
    }

    // slope of the radial
    double slope = pt_no.y / pt_no.x;

    // Calculate the intersection.
    // With center as the origin:
    // * ellipse equation is: (x / radii.x)^2 + (y / radii.y) + 1
    // * radial equation is:  y = x * slope
    // Three part of the calculus:
    // * '_dsign(...) *' -> get correct quadrant
    // * 'sqrt(...)'     -> solve the equation
    // * '+ center.x/y'  -> back the EMF origin
    intersect.x =
        _dsign(pt_no.x) *
            sqrt(1 / ((pow(1 / radii.x, 2)) + pow((slope / radii.y), 2))) +
        center.x;
    intersect.y = _dsign(pt_no.y) * sqrt(1 / ((pow(1 / (slope * radii.x), 2)) +
                                              pow((1 / radii.y), 2))) +
                  center.y;

    return intersect;
}
void lineto_draw(const char *name, const char *field1, const char *field2,
                 const char *contents, FILE *out, drawingStates *states) {
    UNUSED(name);
    PU_EMRGENERICPAIR pEmr = (PU_EMRGENERICPAIR)(contents);
    startPathDraw(states, out);
    fprintf(out, "L ");
    point_draw(states, pEmr->pair, out);
    addNewSegPath(states, SEG_LINE);
    pointCurrPathAdd(states, pEmr->pair, 0);
    endPathDraw(states, out);
}
void moveto_draw(const char *name, const char *field1, const char *field2,
                 const char *contents, FILE *out, drawingStates *states) {
    UNUSED(name);
    PU_EMRGENERICPAIR pEmr = (PU_EMRGENERICPAIR)(contents);
    point_draw(states, pEmr->pair, out);
    addNewSegPath(states, SEG_MOVE);
    pointCurrPathAdd(states, pEmr->pair, 0);
}
void newPathStruct(drawingStates *states) {
    pathStack *new_entry = calloc(1, sizeof(pathStack));
    if (states->emfStructure.pathStack == NULL) {
        states->emfStructure.pathStack = new_entry;
        states->emfStructure.pathStackLast = new_entry;
    } else {
        states->emfStructure.pathStackLast->next = new_entry;
        states->emfStructure.pathStackLast = new_entry;
    }
}
void no_stroke(drawingStates *states, FILE *out) {
    if (states->currentDeviceContext.fill_mode != U_BS_NULL) {
        fprintf(out, "stroke-width=\"1px\" ");
        fprintf(out, "stroke=\"#%02X%02X%02X\" ",
                states->currentDeviceContext.fill_red,
                states->currentDeviceContext.fill_green,
                states->currentDeviceContext.fill_blue);
    } else {
        fprintf(out, "stroke=\"none\" ");
        fprintf(out, "stroke-width=\"0.0\" ");
    }
}
void point16_draw(drawingStates *states, U_POINT16 pt, FILE *out) {
    POINT_D ptd = point_cal(states, (double)pt.x, (double)pt.y);
    states->cur_x = pt.x;
    states->cur_y = pt.y;
    fprintf(out, "%.4f,%.4f ", ptd.x, ptd.y);
}

double scaleX(drawingStates *states, double x) {
    double ret;
    double scalingX;

    switch (states->MapMode) {
    case U_MM_TEXT:
        scalingX = 1.0;
        break;
    case U_MM_LOMETRIC:
        // convert to 0.1 mm to pixel and invert Y
        scalingX = states->pxPerMm * 0.1 * 1;
        break;
    case U_MM_HIMETRIC:
        // convert to 0.01 mm to pixel and invert Y
        scalingX = states->pxPerMm * 0.01 * 1;
        break;
    case U_MM_LOENGLISH:
        // convert to 0.01 inch to pixel and invert Y
        scalingX = states->pxPerMm * 0.01 * mmPerInch * 1;
        break;
    case U_MM_HIENGLISH:
        // convert to 0.001 inch to pixel and invert Y
        scalingX = states->pxPerMm * 0.001 * mmPerInch * 1;
        break;
    case U_MM_TWIPS:
        // convert to 1 twips to pixel and invert Y
        scalingX = states->pxPerMm / 1440 * mmPerInch * 1;
        break;
    case U_MM_ISOTROPIC:
        if (states->windowExSet && states->viewPortExSet) {
            scalingX = states->viewPortExX / states->windowExX;
        } else {
            scalingX = 1.0;
        }
        break;
    case U_MM_ANISOTROPIC:
        if (states->windowExSet && states->viewPortExSet) {
            scalingX = states->viewPortExX / states->windowExX;
        } else {
            scalingX = 1.0;
        }
        break;
    default:
        scalingX = 1.0;
    }
    ret = x * scalingX * states->scaling;
    return ret;
}

double scaleY(drawingStates *states, double y) {
    double ret;
    double scalingY;

    switch (states->MapMode) {
    case U_MM_TEXT:
        scalingY = 1.0;
        break;
    case U_MM_LOMETRIC:
        // convert to 0.1 mm to pixel and invert Y
        scalingY = states->pxPerMm * 0.1 * 1;
        break;
    case U_MM_HIMETRIC:
        // convert to 0.01 mm to pixel and invert Y
        scalingY = states->pxPerMm * 0.01 * 1;
        break;
    case U_MM_LOENGLISH:
        // convert to 0.01 inch to pixel and invert Y
        scalingY = states->pxPerMm * 0.01 * mmPerInch * 1;
        break;
    case U_MM_HIENGLISH:
        // convert to 0.001 inch to pixel and invert Y
        scalingY = states->pxPerMm * 0.001 * mmPerInch * 1;
        break;
    case U_MM_TWIPS:
        // convert to 1 twips to pixel and invert Y
        scalingY = states->pxPerMm / 1440 * mmPerInch * 1;
        break;
    case U_MM_ISOTROPIC:
        if (states->windowExSet && states->viewPortExSet) {
            scalingY = states->viewPortExX / states->windowExX;
        } else {
            scalingY = 1.0;
        }
        break;
    case U_MM_ANISOTROPIC:
        if (states->windowExSet && states->viewPortExSet) {
            scalingY = states->viewPortExY / states->windowExY;
        } else {
            scalingY = 1.0;
        }
        break;
    default:
        scalingY = 1.0;
    }
    ret = y * scalingY * states->scaling;
    return ret;
}

POINT_D point_cal(drawingStates *states, double x, double y) {
    POINT_D ret;
    double scalingX;
    double scalingY;
    double windowOrgX = 0.0;
    double windowOrgY = 0.0;
    double viewPortOrgX = 0.0;
    double viewPortOrgY = 0.0;

    switch (states->MapMode) {
    case U_MM_TEXT:
        scalingX = 1.0;
        scalingY = 1.0;
        break;
    case U_MM_LOMETRIC:
        // convert to 0.1 mm to pixel and invert Y
        scalingX = states->pxPerMm * 0.1 * 1;
        scalingY = states->pxPerMm * 0.1 * -1;
        break;
    case U_MM_HIMETRIC:
        // convert to 0.01 mm to pixel and invert Y
        scalingX = states->pxPerMm * 0.01 * 1;
        scalingY = states->pxPerMm * 0.01 * -1;
        break;
    case U_MM_LOENGLISH:
        // convert to 0.01 inch to pixel and invert Y
        scalingX = states->pxPerMm * 0.01 * mmPerInch * 1;
        scalingY = states->pxPerMm * 0.01 * mmPerInch * -1;
        break;
    case U_MM_HIENGLISH:
        // convert to 0.001 inch to pixel and invert Y
        scalingX = states->pxPerMm * 0.001 * mmPerInch * 1;
        scalingY = states->pxPerMm * 0.001 * mmPerInch * -1;
        break;
    case U_MM_TWIPS:
        // convert to 1 twips to pixel and invert Y
        scalingX = states->pxPerMm / 1440 * mmPerInch * 1;
        scalingY = states->pxPerMm / 1440 * mmPerInch * -1;
        break;
    case U_MM_ISOTROPIC:
        if (states->windowExSet && states->viewPortExSet) {
            scalingX = states->viewPortExX / states->windowExX;
        } else {
            scalingX = 1.0;
        }
        scalingY = scalingX;
        windowOrgX = states->windowOrgX;
        windowOrgY = states->windowOrgY;
        viewPortOrgX = states->viewPortOrgX;
        viewPortOrgY = states->viewPortOrgY;
        break;
    case U_MM_ANISOTROPIC:
        if (states->windowExSet && states->viewPortExSet) {
            scalingX = states->viewPortExX / states->windowExX;
            scalingY = states->viewPortExY / states->windowExY;
        } else {
            scalingX = 1.0;
            // If fixBrokenYTransform is true, we have to flip the Y axis.
            scalingY = (states->fixBrokenYTransform ? -1.0 : 1.0);
        }
        windowOrgX = states->windowOrgX;
        windowOrgY = states->windowOrgY;
        viewPortOrgX = states->viewPortOrgX;
        viewPortOrgY = states->viewPortOrgY;
        break;
    default:
        scalingX = 1.0;
        scalingY = 1.0;
    }
    ret.x = ((x - windowOrgX) * scalingX + viewPortOrgX) * states->scaling;
    ret.y = ((y - windowOrgY) * scalingY + viewPortOrgY) * states->scaling;
    return ret;
}

POINT_D point_s(drawingStates *states, U_POINT pt) {
    return point_cal(states, (double)pt.x, (double)pt.y);
}

POINT_D point_s16(drawingStates *states, U_POINT16 pt) {
    return point_cal(states, (double)pt.x, (double)pt.y);
}

void point_draw(drawingStates *states, U_POINT pt, FILE *out) {
    POINT_D ptd = point_cal(states, (double)pt.x, (double)pt.y);
    states->cur_x = pt.x;
    states->cur_y = pt.y;
    fprintf(out, "%.4f,%.4f ", ptd.x, ptd.y);
}
void point_draw_d(drawingStates *states, POINT_D pt, FILE *out) {
    POINT_D ptd = point_cal(states, pt.x, pt.y);
    states->cur_x = pt.x;
    states->cur_y = pt.y;
    fprintf(out, "%.4f,%.4f ", ptd.x, ptd.y);
}

void point_draw_raw_d(POINT_D pt, FILE *out) {
    fprintf(out, "%.4f,%.4f ", pt.x, pt.y);
}
void polyline16_draw(const char *name, const char *contents, FILE *out,
                     drawingStates *states, bool polygon) {
    UNUSED(name);
    unsigned int i;
    PU_EMRPOLYBEZIER16 pEmr = (PU_EMRPOLYBEZIER16)(contents);
    PU_POINT16 papts = (PU_POINT16)(&(pEmr->apts));
    returnOutOfEmf((intptr_t)papts +
                   (intptr_t)(pEmr->cpts) * sizeof(U_POINT16));
    startPathDraw(states, out);
    for (i = 0; i < pEmr->cpts; i++) {
        if (polygon && i == 0) {
            fprintf(out, "M ");
            addNewSegPath(states, SEG_MOVE);
        } else {
            fprintf(out, "L ");
            addNewSegPath(states, SEG_LINE);
        }
        pointCurrPathAdd16(states, papts[i], 0);
        point16_draw(states, papts[i], out);
    }
    endPathDraw(states, out);
}
void polyline_draw(const char *name, const char *contents, FILE *out,
                   drawingStates *states, bool polygon) {
    UNUSED(name);
    unsigned int i;
    PU_EMRPOLYLINETO pEmr = (PU_EMRPOLYLINETO)(contents);
    startPathDraw(states, out);
    PU_POINT papts = (PU_POINT)(&(pEmr->aptl));
    returnOutOfEmf((intptr_t)papts + (intptr_t)(pEmr->cptl) * sizeof(U_POINT));
    for (i = 0; i < pEmr->cptl; i++) {
        if (polygon && i == 0) {
            fprintf(out, "M ");
            addNewSegPath(states, SEG_MOVE);
        } else {
            fprintf(out, "L ");
            addNewSegPath(states, SEG_LINE);
        }
        point_draw(states, pEmr->aptl[i], out);
        pointCurrPathAdd(states, pEmr->aptl[i], 0);
    }
    endPathDraw(states, out);
}
void polypolygon16_draw(const char *name, const char *contents, FILE *out,
                        drawingStates *states, bool polygon) {
    UNUSED(name);
    unsigned int i;
    PU_EMRPOLYPOLYLINE16 pEmr = (PU_EMRPOLYPOLYLINE16)(contents);
    PU_POINT16 papts = (PU_POINT16)((char *)pEmr->aPolyCounts +
                                    sizeof(uint32_t) * pEmr->nPolys);
    returnOutOfEmf((intptr_t)papts +
                   (intptr_t)(pEmr->cpts) * sizeof(U_POINT16));

    int counter = 0;
    int polygon_index = 0;
    for (i = 0; i < pEmr->cpts; i++) {
        if (counter == 0) {
            fprintf(out, "M ");
            point16_draw(states, papts[i], out);
            addNewSegPath(states, SEG_MOVE);
            pointCurrPathAdd16(states, papts[i], 0);
        } else {
            fprintf(out, "L ");
            point16_draw(states, papts[i], out);
            addNewSegPath(states, SEG_LINE);
            pointCurrPathAdd16(states, papts[i], 0);
        }
        counter++;
        if (pEmr->aPolyCounts[polygon_index] == counter) {
            if (polygon) {
                fprintf(out, "Z ");
                addNewSegPath(states, SEG_END);
            }
            counter = 0;
            polygon_index++;
        }
    }
}
void polypolygon_draw(const char *name, const char *contents, FILE *out,
                      drawingStates *states, bool polygon) {
    UNUSED(name);
    unsigned int i;
    PU_EMRPOLYPOLYLINE16 pEmr = (PU_EMRPOLYPOLYLINE16)(contents);
    PU_POINT papts =
        (PU_POINT)((char *)pEmr->aPolyCounts + sizeof(uint32_t) * pEmr->nPolys);

    int counter = 0;
    int polygon_index = 0;
    returnOutOfEmf((intptr_t)papts + (intptr_t)(pEmr->cpts) * sizeof(U_POINT));
    for (i = 0; i < pEmr->cpts; i++) {
        if (counter == 0) {
            fprintf(out, "M ");
            point_draw(states, papts[i], out);
            addNewSegPath(states, SEG_MOVE);
            pointCurrPathAdd(states, papts[i], 0);
        } else {
            fprintf(out, "L ");
            point_draw(states, papts[i], out);
            addNewSegPath(states, SEG_LINE);
            pointCurrPathAdd(states, papts[i], 0);
        }
        counter++;
        if (pEmr->aPolyCounts[polygon_index] == counter) {
            if (polygon) {
                fprintf(out, "Z ");
                addNewSegPath(states, SEG_END);
            }
            counter = 0;
            polygon_index++;
        }
    }
}
void rectl_draw(drawingStates *states, FILE *out, U_RECTL rect) {
    U_POINT pt;
    fprintf(out, "M ");
    pt.x = rect.left;
    pt.y = rect.top;
    addNewSegPath(states, SEG_MOVE);
    pointCurrPathAdd(states, pt, 0);
    point_draw(states, pt, out);
    fprintf(out, "L ");
    pt.x = rect.right;
    pt.y = rect.top;
    addNewSegPath(states, SEG_LINE);
    pointCurrPathAdd(states, pt, 0);
    point_draw(states, pt, out);
    fprintf(out, "L ");
    pt.x = rect.right;
    pt.y = rect.bottom;
    addNewSegPath(states, SEG_LINE);
    pointCurrPathAdd(states, pt, 0);
    point_draw(states, pt, out);
    fprintf(out, "L ");
    pt.x = rect.left;
    pt.y = rect.bottom;
    addNewSegPath(states, SEG_LINE);
    pointCurrPathAdd(states, pt, 0);
    point_draw(states, pt, out);
    fprintf(out, "L ");
    pt.x = rect.left;
    pt.y = rect.top;
    addNewSegPath(states, SEG_LINE);
    pointCurrPathAdd(states, pt, 0);
    point_draw(states, pt, out);
    fprintf(out, "Z ");
    addNewSegPath(states, SEG_END);
}
void restoreDeviceContext(drawingStates *states, int32_t index) {
    EMF_DEVICE_CONTEXT_STACK *stack_entry = states->DeviceContextStack;
    EMF_DEVICE_CONTEXT_STACK *new_top;
    EMF_DEVICE_CONTEXT_STACK *entry_to_free;
    // we recover the 'abs(index)' element of the stack
    // we stop if the index was outside the DeviceContextStack
    int i = -1;
    for (; i > index && stack_entry != NULL; i--) {
        if (stack_entry->previous != NULL) {
            stack_entry = stack_entry->previous;
        } else {
            break;
        }
    }
    if (stack_entry == NULL || i != index) {
        states->Error = true;
        return;
    }
    // we copy it as the current device context
    freeDeviceContext(&(states->currentDeviceContext));
    states->currentDeviceContext = (EMF_DEVICE_CONTEXT){0};
    copyDeviceContext(&(states->currentDeviceContext),
                      &(stack_entry->DeviceContext));
    /* SaveDC/RestoreDC also restores the mapping state used by point_cal(). */
    states->viewPortOrgX = stack_entry->viewPortOrgX;
    states->viewPortOrgY = stack_entry->viewPortOrgY;
    states->viewPortExX = stack_entry->viewPortExX;
    states->viewPortExY = stack_entry->viewPortExY;
    states->viewPortExSet = stack_entry->viewPortExSet;
    states->windowOrgX = stack_entry->windowOrgX;
    states->windowOrgY = stack_entry->windowOrgY;
    states->windowExX = stack_entry->windowExX;
    states->windowExY = stack_entry->windowExY;
    states->windowExSet = stack_entry->windowExSet;
    states->MapMode = stack_entry->MapMode;
    states->text_layout = stack_entry->text_layout;
    /*
     * RestoreDC consumes the restored save point and any newer save points.
     * Leaving them on the stack makes repeated RestoreDC records restore the
     * same mapping state, which pushes later WMF text outside the canvas.
     */
    new_top = stack_entry->previous;
    entry_to_free = states->DeviceContextStack;
    while (entry_to_free != new_top) {
        EMF_DEVICE_CONTEXT_STACK *next = entry_to_free->previous;
        freeDeviceContext(&(entry_to_free->DeviceContext));
        free(entry_to_free);
        entry_to_free = next;
    }
    states->DeviceContextStack = new_top;
}
void saveDeviceContext(drawingStates *states) {
    // create the new device context in the stack
    EMF_DEVICE_CONTEXT_STACK *new_entry =
        (EMF_DEVICE_CONTEXT_STACK *)calloc(1, sizeof(EMF_DEVICE_CONTEXT_STACK));
    copyDeviceContext(&(new_entry->DeviceContext),
                      &(states->currentDeviceContext));
    /* SaveDC must preserve mapping state, not just selected GDI objects. */
    new_entry->viewPortOrgX = states->viewPortOrgX;
    new_entry->viewPortOrgY = states->viewPortOrgY;
    new_entry->viewPortExX = states->viewPortExX;
    new_entry->viewPortExY = states->viewPortExY;
    new_entry->viewPortExSet = states->viewPortExSet;
    new_entry->windowOrgX = states->windowOrgX;
    new_entry->windowOrgY = states->windowOrgY;
    new_entry->windowExX = states->windowExX;
    new_entry->windowExY = states->windowExY;
    new_entry->windowExSet = states->windowExSet;
    new_entry->MapMode = states->MapMode;
    new_entry->text_layout = states->text_layout;
    // put the new entry on the stack
    new_entry->previous = states->DeviceContextStack;
    states->DeviceContextStack = new_entry;
}
void setTransformIdentity(drawingStates *states) {
    states->currentDeviceContext.worldTransform.eM11 = 1.0;
    states->currentDeviceContext.worldTransform.eM12 = 0.0;
    states->currentDeviceContext.worldTransform.eM21 = 0.0;
    states->currentDeviceContext.worldTransform.eM22 = 1.0;
    states->currentDeviceContext.worldTransform.eDx = 0.0;
    states->currentDeviceContext.worldTransform.eDy = 0.0;
}
void startPathDraw(drawingStates *states, FILE *out) {
    if (!(states->inPath)) {
        fprintf(out, "<%spath ", states->nameSpaceString);
        clipset_draw(states, out);
        fprintf(out, "d=\"M ");
        U_POINT pt;
        pt.x = states->cur_x;
        pt.y = states->cur_y;
        point_draw(states, pt, out);
        addNewSegPath(states, SEG_MOVE);
        pointCurrPathAdd(states, pt, 0);
    }
}
void stroke_draw(drawingStates *states, FILE *out, bool *filled,
                 bool *stroked) {
    float unit_stroke =
        states->currentDeviceContext.stroke_width * states->scaling;
    float dash_len = unit_stroke * 5;
    float dot_len = unit_stroke;
    if (states->verbose) {
        stroke_print(states);
    }

    if ((states->currentDeviceContext.stroke_mode & 0x000000FF) == U_PS_NULL) {
        // no stroke with the fill color with a with of 1px
        no_stroke(states, out);
        *stroked = true;
        return;
    }
    // pen type
    switch (states->currentDeviceContext.stroke_mode & 0x000F0000) {
    case U_PS_COSMETIC:
        color_stroke(states, out);
        // WMF/EMF cosmetic pens can still carry a logical width.  Equation
        // WMFs use that for fraction bars; width 0 remains a hairline.
        width_stroke(states, out,
                     states->currentDeviceContext.stroke_width > 0
                         ? states->currentDeviceContext.stroke_width
                         : 1);
        *stroked = true;
        break;
    case U_PS_GEOMETRIC:
        basic_stroke(states, out);
        *stroked = true;
        break;
    }
    // line style.
    switch (states->currentDeviceContext.stroke_mode & 0x000000FF) {
    case U_PS_SOLID:
        break;
    case U_PS_DASH:
        fprintf(out, "stroke-dasharray=\"%.4f,%.4f\" ", dash_len, dash_len);
        break;
    case U_PS_DOT:
        fprintf(out, "stroke-dasharray=\"%.4f,%.4f\" ", dot_len, dot_len);
        break;
    case U_PS_DASHDOT:
        fprintf(out, "stroke-dasharray=\"%.4f,%.4f,%.4f,%.4f\" ", dash_len,
                dash_len, dot_len, dash_len);
        break;
    case U_PS_DASHDOTDOT:
        fprintf(out, "stroke-dasharray=\"%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\" ",
                dash_len, dash_len, dot_len, dot_len, dot_len, dash_len);
        break;
    case U_PS_INSIDEFRAME:
    case U_PS_USERSTYLE:
    case U_PS_ALTERNATE:
    default:
        // partial
        break;
    }
    // line cap.
    switch (states->currentDeviceContext.stroke_mode & 0x00000F00) {
    case U_PS_ENDCAP_ROUND:
        fprintf(out, " stroke-linecap=\"round\" ");
        break;
    case U_PS_ENDCAP_SQUARE:
        fprintf(out, " stroke-linecap=\"square\" ");
        break;
    case U_PS_ENDCAP_FLAT:
        fprintf(out, " stroke-linecap=\"butt\" ");
        break;
    default:
        break;
    }
    // line join.
    switch (states->currentDeviceContext.stroke_mode & 0x0000F000) {
    case U_PS_JOIN_ROUND:
        fprintf(out, " stroke-linejoin=\"round\" ");
        break;
    case U_PS_JOIN_BEVEL:
        fprintf(out, " stroke-linejoin=\"bevel\" ");
        break;
    case U_PS_JOIN_MITER:
        fprintf(out, " stroke-linejoin=\"miter\" ");
        if (states->currentDeviceContext.miterLimit)
            fprintf(out, " stroke-miterlimit=\"%.4f\" ",
                    states->scaling *
                        (double)states->currentDeviceContext.miterLimit);
        break;
    default:
        break;
    }
}

/*
 * Convert GDI LOGFONT height to SVG font-size while preserving signed height
 * semantics.  A global cell-to-em shrink for positive lfHeight makes EMF Dx
 * advances too loose, so both signs use the requested logical height here.
 */
static double svg_font_size_from_logfont_height(drawingStates *states) {
    int32_t logical_height = states->currentDeviceContext.font_height;
    double abs_height =
        logical_height < 0 ? -(double)logical_height : (double)logical_height;

    return fabs(scaleX(states, abs_height));
}

/* Return true when a CSS font-family name needs string quoting in SVG. */
static bool svg_font_family_needs_quotes(const char *family) {
    size_t i;
    bool token_start = true;

    if (family == NULL || family[0] == '\0') {
        return false;
    }
    for (i = 0; family[i] != '\0'; i++) {
        unsigned char c = (unsigned char)family[i];

        if (c == ' ') {
            token_start = true;
            continue;
        }
        if (token_start && c >= '0' && c <= '9') {
            return true;
        }
        if (!(c == '-' || c == '_' || (c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c >= 0x80)) {
            return true;
        }
        token_start = false;
    }
    return false;
}

/* Emit a font-family attribute, quoting names like "Wingdings 2" for CSS. */
static void svg_font_family_draw(FILE *out, const char *family) {
    const char *p;

    if (family == NULL) {
        return;
    }
    if (!svg_font_family_needs_quotes(family)) {
        fprintf(out, "font-family=\"%s\" ", family);
        return;
    }

    fprintf(out, "font-family=\"&quot;");
    for (p = family; *p != '\0'; p++) {
        switch (*p) {
        case '&':
            fprintf(out, "&amp;");
            break;
        case '<':
            fprintf(out, "&lt;");
            break;
        case '"':
            fprintf(out, "\\&quot;");
            break;
        case '\\':
            fprintf(out, "\\\\");
            break;
        default:
            fputc(*p, out);
            break;
        }
    }
    fprintf(out, "&quot;\" ");
}

void text_style_draw(FILE *out, drawingStates *states, POINT_D Org) {
    double font_height = svg_font_size_from_logfont_height(states);
    if (states->currentDeviceContext.font_family != NULL) {
        svg_font_family_draw(out, states->currentDeviceContext.font_family);
    }
    fprintf(out, "fill=\"#%02X%02X%02X\" ",
            states->currentDeviceContext.text_red,
            states->currentDeviceContext.text_green,
            states->currentDeviceContext.text_blue);
    int orientation = 1;
    if (scaleY(states, 1.0) > 0) {
        orientation = -1;
    } else {
        orientation = 1;
    }

    if (states->currentDeviceContext.font_escapement != 0) {
        fprintf(out, "transform=\"");
        fprintf(out, "rotate(%d, %.4f, %.4f) translate(0, %.4f)",
                (orientation *
                 (int)states->currentDeviceContext.font_escapement / 10),
                Org.x, (Org.y + font_height * 0.9), font_height * 0.9);
        fprintf(out, "\" ");
    }

    if (states->text_layout == U_LAYOUT_RTL) {
        fprintf(out, "writing-mode=\"rl-tb\" ");
    }

    if (states->currentDeviceContext.font_italic) {
        fprintf(out, "font-style=\"italic\" ");
    }

    fprintf(out, "style =\"white-space:pre;\" ");

    if (states->currentDeviceContext.font_underline &&
        states->currentDeviceContext.font_strikeout) {
        fprintf(out, "text-decoration=\"line-through,underline\" ");
    } else if (states->currentDeviceContext.font_underline) {
        fprintf(out, "text-decoration=\"underline\" ");
    } else if (states->currentDeviceContext.font_strikeout) {
        fprintf(out, "text-decoration=\"line-through\" ");
    }

    if (states->currentDeviceContext.font_weight != 0)
        fprintf(out, "font-weight=\"%d\" ",
                states->currentDeviceContext.font_weight);

    // horizontal position
    uint16_t align = states->currentDeviceContext.text_align;
    if ((align & U_TA_CENTER) == U_TA_CENTER) {
        fprintf(out, "text-anchor=\"middle\" ");
    } else if ((align & U_TA_CENTER2) == U_TA_CENTER2) {
        fprintf(out, "text-anchor=\"middle\" ");
    } else if ((align & U_TA_RIGHT) == U_TA_RIGHT) {
        fprintf(out, "text-anchor=\"end\" ");
    } else {
        fprintf(out, "text-anchor=\"start\" ");
    }
    // vertical position
    if ((align & U_TA_BOTTOM) == U_TA_BOTTOM) {
        fprintf(out, "x=\"%.4f\" y=\"%.4f\" ", Org.x, Org.y);
    } else if ((align & U_TA_BASELINE) == U_TA_BASELINE) {
        fprintf(out, "x=\"%.4f\" y=\"%.4f\" ", Org.x, Org.y);
    } else {
        fprintf(out, "x=\"%.4f\" y=\"%.4f\" ", Org.x,
                Org.y + font_height * 0.9);
    }
    fprintf(out, "font-size=\"%.4f\" ", font_height);
}

/* Build absolute SVG x positions from EMR text Dx advances. */
static double *text_dx_positions(drawingStates *states, const char *contents,
                                 const U_EMRTEXT *pemt, double origin_x) {
    uint32_t off = sizeof(U_EMRTEXT);
    uint32_t off_dx = 0;
    int64_t logical_x = 0;
    double *positions;
    uint32_t step;
    uint32_t i;

    if (states == NULL || contents == NULL || pemt == NULL || pemt->nChars < 2) {
        return NULL;
    }
    if (!(pemt->fOptions & U_ETO_NO_RECT)) {
        off += sizeof(U_RECTL);
    }
    if (checkOutOfEMF(states, (uintptr_t)((const char *)pemt + off + sizeof(off_dx)))) {
        return NULL;
    }
    memcpy(&off_dx, (const char *)pemt + off, sizeof(off_dx));
    step = (pemt->fOptions & U_ETO_PDY) ? 2 : 1;
    if (off_dx == 0 ||
        checkOutOfEMF(states, (uintptr_t)(contents + off_dx + pemt->nChars * step * sizeof(uint32_t)))) {
        return NULL;
    }
    for (i = 0; i < pemt->nChars; i++) {
        int32_t dx = 0;
        memcpy(&dx, contents + off_dx + i * step * sizeof(uint32_t),
               sizeof(dx));
        if (dx != 0) {
            break;
        }
    }
    if (i == pemt->nChars) {
        // WMF records without Dx arrive here as all-zero advances from
        // wmf2emf; treat that as no explicit per-character positioning.
        return NULL;
    }
    positions = (double *)calloc(pemt->nChars, sizeof(double));
    if (positions == NULL) {
        return NULL;
    }
    positions[0] = origin_x;
    for (i = 1; i < pemt->nChars; i++) {
        int32_t dx = 0;
        memcpy(&dx, contents + off_dx + (i - 1) * step * sizeof(uint32_t),
               sizeof(dx));
        logical_x += dx;
        positions[i] = origin_x + scaleX(states, (double)logical_x);
    }
    return positions;
}

// get the closest ttf file matching font_family, weight, italic
static int get_fontpath(char *font_family, int weight, int italic,
                        char **path) {
    FcPattern *pat;
    FcObjectSet *os = 0;
    FcResult result;
    FcPattern *match;
    FcFontSet *fs;

    pat = FcNameParse((FcChar8 *)font_family);
    if (!pat) {
        return 1;
    }

    FcConfigSubstitute(0, pat, FcMatchPattern);
    // FcDefaultSubstitute(pat);
    int fcweight;

    if (italic)
        FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
    if (weight) {
        switch (weight) {
        case U_PAN_WEIGHT_VERY_LIGHT:
            fcweight = FC_WEIGHT_EXTRALIGHT;
            break;
        case U_PAN_WEIGHT_LIGHT:
            fcweight = FC_WEIGHT_LIGHT;
            break;
        case U_PAN_WEIGHT_THIN:
            fcweight = FC_WEIGHT_THIN;
            break;
        case U_PAN_WEIGHT_BOOK:
            fcweight = FC_WEIGHT_BOOK;
            break;
        case U_PAN_WEIGHT_MEDIUM:
            fcweight = FC_WEIGHT_MEDIUM;
            break;
        case U_PAN_WEIGHT_DEMI:
            fcweight = FC_WEIGHT_DEMIBOLD;
            break;
        case U_PAN_WEIGHT_BOLD:
            fcweight = FC_WEIGHT_BOLD;
            break;
        case U_PAN_WEIGHT_HEAVY:
            fcweight = FC_WEIGHT_HEAVY;
            break;
        case U_PAN_WEIGHT_BLACK:
            fcweight = FC_WEIGHT_BLACK;
            break;
        case U_PAN_WEIGHT_NORD:
            fcweight = FC_WEIGHT_BLACK;
            break;
        default:
            fcweight = FC_WEIGHT_BOLD;
        }
        FcPatternAddInteger(pat, FC_WEIGHT, fcweight);
    }

    match = FcFontMatch(0, pat, &result);

    fs = FcFontSetCreate();
    if (match)
        FcFontSetAdd(fs, match);
    FcPatternDestroy(pat);

    if (fs) {
        int j;
        for (j = 0; j < fs->nfont; j++) {
            FcPattern *font;

            font = FcPatternFilter(fs->fonts[j], os);
            char *tmp;

            // FcPatternPrint (font);
            FcPatternGetString(font, FC_FILE, 0, (FcChar8 **)&tmp);
            *path = (char *)calloc(strlen(tmp) + 1, sizeof(char));
            strcpy(*path, tmp);
            FcPatternDestroy(font);
        }
        FcFontSetDestroy(fs);
    }

    if (os)
        FcObjectSetDestroy(os);

    FcFini();

    return 0;
}

// generate the reverse cmap from the ttf file
static int cmap_rev(const char *fpath, cmap_collection *rcmap) {
    FT_Library library;

    int error = FT_Init_FreeType(&library);
    FT_Face face;
    if (error) {
        return 1;
    }

    error = FT_New_Face(library, fpath, 0, &face);
    if (error == FT_Err_Unknown_File_Format) {
        // printf("%s not a font\n", fpath);
        return 1;
    } else if (error) {
        // printf("unknowm error %d\n", error);
        return 1;
    } else {
        // printf("font %s | name %s | style %s\n", fpath, face->family_name,
        // face->style_name);
        // printf("%d\n", face->num_charmaps);
        FT_UInt rmap_s = 1000;
        rcmap->uni = calloc(rmap_s, sizeof(uint32_t));
        FT_Select_Charmap(face, FT_ENCODING_UNICODE);
        FT_UInt gindex = 0;
        FT_ULong charcode = FT_Get_First_Char(face, &gindex);
        while (gindex != 0) {
            if (gindex >= rmap_s) {
                FT_UInt old_rmap_s = rmap_s;
                rmap_s = gindex + 1000;
                uint32_t *tmp = realloc(rcmap->uni, sizeof(uint32_t) * rmap_s);
                for (FT_UInt i = old_rmap_s; i < rmap_s; i++)
                    tmp[i] = 0;
                // free(rcmap->uni);
                rcmap->uni = tmp;
            }
            // printf("index: %d | charcode %d\n", gindex, charcode);
            rcmap->uni[gindex] = charcode;
            charcode = FT_Get_Next_Char(face, charcode, &gindex);
        }
        rcmap->size = rmap_s;
        FT_Done_Face(face);
        FT_Done_FreeType(library);
    }
    return 0;
}

// generate the reverse cmap of a given font_family
static int gen_reverse_map(char *font_family, int weight, bool italic,
                           cmap_collection *rcmap) {
    char *path = NULL;
    int ret = 0;
    ret = get_fontpath(font_family, weight, italic, &path);
    if (ret) {
        // printf("error while search font");
        free(path);
        return 1;
    }
    ret = cmap_rev(path, rcmap);
    if (ret) {
        // printf("error while generating reverse mapping");
        free(path);
        return 1;
    }
    free(path);
    return 0;
}

/*
 * EMF files can contain weird "encoding".
 * This file handles one of those:
 * if ETO_GLYPH_INDEX is set in *TEXTOUT options,
 * the encoding of a char is basically the index
 * of its corresponding glyph inside the font ttf file.
 *
 * That's great... Thanks a lot Microsoft for this crappy scheme.
 *
 * To handle this case, we define some reverse mapping tables
 * (index of glyph -> unicode).
 *
 * Recovering the ttf file is done using fontconfig (function: get_fontpath).
 * The reverse mapping is done with freetype (function: cmap_rev).
 *
 * The .ttf font must be present on your system and properly indexed by
 * fontconfig
 *
 */

static int fontindex_to_utf8(uint16_t *in, size_t size_in, char **out,
                             size_t *out_len, char *font_name, int weight,
                             bool italic) {
    int ret;
    *out_len = 0;
    cmap_collection rcmap;
    rcmap.uni = NULL;
    if (font_name == NULL) {
        *out = NULL;
        return 1;
    }
    ret = gen_reverse_map(font_name, weight, italic, &rcmap);

    if (rcmap.uni == NULL) {
        *out = NULL;
        return 1;
    }

    if (rcmap.uni && ret) {
        free(rcmap.uni);
        *out = NULL;
        return 1;
    }

    size_t buf_size_left = U_MAX(size_in, 5);
    char *buf = calloc(buf_size_left, sizeof(char));
    if (!buf) {
        *out = NULL;
        return -1;
    }

    for (int i = 0; i < size_in; i++) {
        uint16_t index = in[i];
        uint32_t codepoint = rcmap.uni[index];
        if (index < rcmap.size) {
            if (codepoint <= 0x7f) {
                buf[*out_len] = (codepoint & 0x7f);
                (*out_len)++;
                buf_size_left--;
            } else if (codepoint <= 0x7ff) {
                buf[*out_len] = (0xc0 | (codepoint >> 6));
                (*out_len)++;
                buf_size_left--;
                buf[*out_len] = (0x80 | (codepoint & 0x3f));
                (*out_len)++;
                buf_size_left--;
            } else if (codepoint <= 0xffff) {
                buf[*out_len] = (0xe0 | (codepoint >> 12));
                (*out_len)++;
                buf_size_left--;
                buf[*out_len] = (0x80 | ((codepoint >> 6) & 0x3f));
                (*out_len)++;
                buf_size_left--;
                buf[*out_len] = (0x80 | (codepoint & 0x3f));
                (*out_len)++;
                buf_size_left--;
            } else if (codepoint <= 0x1fffff) {
                buf[*out_len] = (0xf0 | (codepoint >> 18));
                (*out_len)++;
                buf_size_left--;
                buf[*out_len] = (0x80 | ((codepoint >> 12) & 0x3f));
                (*out_len)++;
                buf_size_left--;
                buf[*out_len] = (0x80 | ((codepoint >> 6) & 0x3f));
                (*out_len)++;
                buf_size_left--;
                buf[*out_len] = (0x80 | (codepoint & 0x3f));
                (*out_len)++;
                buf_size_left--;
            }
        }
        if (buf_size_left <= 5) {
            char *ptr;
            size_t increase = 50;
            ptr = realloc(buf, *out_len + increase + buf_size_left);
            if (!ptr) {
                free(buf);
                free(rcmap.uni);
                *out = NULL;
                return -1;
            }
            buf_size_left += increase;
            buf = ptr;
        }
    }
    free(rcmap.uni);
    buf[*out_len] = '\0';
    *out = buf;
    return 0;
}

static int enc_to_utf8(char *in, size_t size_in, char **out, size_t *out_len,
                       char *from_enc) {
    iconv_t cd;
    char *inbuf, *outbuf;
    size_t inbytesleft, outbytesleft, nchars, out_buf_len;

    cd = iconv_open("UTF-8", from_enc);
    if (cd == (iconv_t)-1) {
        *out = NULL;
        return 1;
    }

    inbytesleft = size_in;
    if (inbytesleft == 0) {
        iconv_close(cd);
        *out = NULL;
        return 1;
    }
    inbuf = in;
    out_buf_len = inbytesleft;
    *out = calloc(out_buf_len, 1);
    if (!*out) {
        iconv_close(cd);
        *out = NULL;
        return 1;
    }
    outbytesleft = out_buf_len;
    outbuf = *out;

    nchars = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
    while (nchars == (size_t)-1 && errno == E2BIG) {
        char *ptr;
        size_t increase = 50;
        size_t len;
        out_buf_len += increase;
        outbytesleft += increase;
        ptr = realloc(*out, out_buf_len);
        if (!ptr) {
            free(*out);
            iconv_close(cd);
            *out = NULL;
            return 1;
        }
        len = outbuf - *out;
        *out = ptr;
        outbuf = *out + len;
        nchars = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
    }
    if (outbytesleft == 0) {
        char *ptr;
        size_t increase = 50;
        out_buf_len += increase;
        outbytesleft += increase;
        ptr = realloc(*out, out_buf_len);
        if (!ptr) {
            free(*out);
            iconv_close(cd);
            *out = NULL;
            return 1;
        }
        *out = ptr;
    }
    if (nchars == (size_t)-1) {
        free(*out);
        iconv_close(cd);
        *out = NULL;
        return 1;
    }

    iconv_close(cd);
    *out_len = out_buf_len - outbytesleft;
    (*out)[(*out_len)] = '\0';
    return 0;
}

/* Append one Unicode code point encoded as UTF-8. */
static int utf8_append_codepoint(char *out, size_t capacity, size_t *offset,
                                 uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        if (*offset + 1 >= capacity) {
            return 0;
        }
        out[(*offset)++] = (char)codepoint;
    } else if (codepoint <= 0x7FF) {
        if (*offset + 2 >= capacity) {
            return 0;
        }
        out[(*offset)++] = (char)(0xC0 | (codepoint >> 6));
        out[(*offset)++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        if (*offset + 3 >= capacity) {
            return 0;
        }
        out[(*offset)++] = (char)(0xE0 | (codepoint >> 12));
        out[(*offset)++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[(*offset)++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        if (*offset + 4 >= capacity) {
            return 0;
        }
        out[(*offset)++] = (char)(0xF0 | (codepoint >> 18));
        out[(*offset)++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[(*offset)++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[(*offset)++] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        return 0;
    }
    return 1;
}

typedef struct symbol_cmap_range {
    unsigned char first;
    unsigned char last;
} symbol_cmap_range;

typedef enum symbol_cmap_match {
    SYMBOL_CMAP_EXACT,
    SYMBOL_CMAP_PREFIX
} symbol_cmap_match;

typedef enum symbol_cmap_charset_rule {
    SYMBOL_CMAP_SYMBOLISH_CHARSET,
    SYMBOL_CMAP_ANY_CHARSET
} symbol_cmap_charset_rule;

typedef struct symbol_cmap_font {
    const char *family;
    symbol_cmap_match match;
    symbol_cmap_charset_rule charset_rule;
    const symbol_cmap_range *ranges;
    size_t range_count;
} symbol_cmap_font;

static const symbol_cmap_range symbol_ranges[] = {{0x20, 0xFF}};

static const symbol_cmap_range wingdings_ranges[] = {
    {0x20, 0x7E}, {0x80, 0xFF}};

static const symbol_cmap_range wingdings_2_ranges[] = {
    {0x20, 0x7E}, {0x80, 0xF9}};

static const symbol_cmap_range wingdings_3_ranges[] = {
    {0x20, 0x7E}, {0x80, 0xF0}};

static const symbol_cmap_range marlett_ranges[] = {
    {0x30, 0x39}, {0x57, 0x57}, {0x61, 0x79}, {0xA1, 0xA3}};

static const symbol_cmap_range ms_reference_specialty_ranges[] = {
    {0x20, 0x20}, {0x23, 0xCB}};

static const symbol_cmap_range ms_outlook_ranges[] = {
    {0x20, 0x20}, {0x41, 0x47}, {0x49, 0x4A}, {0x4D, 0x4E},
    {0xA0, 0xA0}};

static const symbol_cmap_range bookshelf_symbol_7_ranges[] = {
    {0x20, 0x73}, {0x75, 0x7D}, {0x80, 0x85}, {0x87, 0x92}};

static const symbol_cmap_range mt_extra_ranges[] = {
    {0x20, 0x7E}, {0x80, 0xFF}};

static const symbol_cmap_range euclid_math_two_ranges[] = {
    {0x20, 0x2C}, {0x41, 0x5A}, {0x6B, 0x6B}, {0x80, 0xAB},
    {0xB0, 0xCB}, {0xD0, 0xE5}, {0xF0, 0xF5}};

static const symbol_cmap_range euclid_math_one_ranges[] = {
    {0x20, 0x2D}, {0x30, 0x39}, {0x41, 0x5A}, {0x80, 0x8D},
    {0x90, 0x99}, {0xA0, 0xAE}, {0xB0, 0xD3}, {0xE0, 0xEA},
    {0xF0, 0xFF}};

/*
 * Fonts with Windows platformID=3, encodingID=0 symbol cmaps often map
 * single-byte record data to PUA code points as F000+byte.  MathType fonts
 * keep narrower per-font ranges to avoid remapping bytes they do not encode.
 */
static const symbol_cmap_font symbol_cmap_fonts[] = {
    {"Symbol", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET, symbol_ranges,
     sizeof(symbol_ranges) / sizeof(symbol_ranges[0])},
    {"Wingdings 2", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     wingdings_2_ranges,
     sizeof(wingdings_2_ranges) / sizeof(wingdings_2_ranges[0])},
    {"Wingdings 3", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     wingdings_3_ranges,
     sizeof(wingdings_3_ranges) / sizeof(wingdings_3_ranges[0])},
    {"Wingdings", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     wingdings_ranges,
     sizeof(wingdings_ranges) / sizeof(wingdings_ranges[0])},
    {"Webdings", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     wingdings_ranges,
     sizeof(wingdings_ranges) / sizeof(wingdings_ranges[0])},
    {"Marlett", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     marlett_ranges,
     sizeof(marlett_ranges) / sizeof(marlett_ranges[0])},
    {"MS Reference Specialty", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     ms_reference_specialty_ranges,
     sizeof(ms_reference_specialty_ranges) /
         sizeof(ms_reference_specialty_ranges[0])},
    {"MS Outlook", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     ms_outlook_ranges,
     sizeof(ms_outlook_ranges) / sizeof(ms_outlook_ranges[0])},
    {"Bookshelf Symbol 7", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_SYMBOLISH_CHARSET,
     bookshelf_symbol_7_ranges,
     sizeof(bookshelf_symbol_7_ranges) /
         sizeof(bookshelf_symbol_7_ranges[0])},
    {"MT Extra", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_ANY_CHARSET, mt_extra_ranges,
     sizeof(mt_extra_ranges) / sizeof(mt_extra_ranges[0])},
    {"Euclid Math Two", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_ANY_CHARSET,
     euclid_math_two_ranges,
     sizeof(euclid_math_two_ranges) / sizeof(euclid_math_two_ranges[0])},
    {"Euclid Math One", SYMBOL_CMAP_EXACT, SYMBOL_CMAP_ANY_CHARSET,
     euclid_math_one_ranges,
     sizeof(euclid_math_one_ranges) / sizeof(euclid_math_one_ranges[0])}};

/* Return an ASCII-lowercase byte for case-insensitive font-family matching. */
static unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

/* Compare font-family names without depending on platform strcasecmp APIs. */
static bool font_family_equals(const char *family, const char *expected) {
    size_t i;

    if (family == NULL || expected == NULL) {
        return false;
    }
    for (i = 0; family[i] != '\0' && expected[i] != '\0'; i++) {
        if (ascii_lower((unsigned char)family[i]) !=
            ascii_lower((unsigned char)expected[i])) {
            return false;
        }
    }
    return family[i] == '\0' && expected[i] == '\0';
}

/* Match font-family prefixes for numbered legacy symbol families. */
static bool font_family_starts_with(const char *family, const char *prefix) {
    size_t i;

    if (family == NULL || prefix == NULL) {
        return false;
    }
    for (i = 0; prefix[i] != '\0'; i++) {
        if (family[i] == '\0' ||
            ascii_lower((unsigned char)family[i]) !=
                ascii_lower((unsigned char)prefix[i])) {
            return false;
        }
    }
    return true;
}

/*
 * Return true when the LOGFONT charset can address a Windows symbol cmap.
 * ANSI_CHARSET plus Wingdings can be a font-mapper fallback request whose bytes
 * must stay normal text, as seen in tests/resources/wmf/text.wmf.
 */
static bool symbol_cmap_font_accepts_charset(const symbol_cmap_font *font,
                                             uint8_t charset) {
    if (font == NULL) {
        return false;
    }
    if (font->charset_rule == SYMBOL_CMAP_ANY_CHARSET) {
        return true;
    }
    return charset == U_SYMBOL_CHARSET || charset == U_DEFAULT_CHARSET;
}

/* Return the symbol-cmap rule for the current font family, if one is known. */
static const symbol_cmap_font *find_symbol_cmap_font(drawingStates *states) {
    const char *family;
    uint8_t charset;
    size_t i;

    if (states == NULL) {
        return NULL;
    }

    family = states->currentDeviceContext.font_family;
    charset = states->currentDeviceContext.font_charset;
    for (i = 0; i < sizeof(symbol_cmap_fonts) / sizeof(symbol_cmap_fonts[0]);
         i++) {
        if (!symbol_cmap_font_accepts_charset(&symbol_cmap_fonts[i],
                                              charset)) {
            continue;
        }
        if (symbol_cmap_fonts[i].match == SYMBOL_CMAP_PREFIX &&
            font_family_starts_with(family, symbol_cmap_fonts[i].family)) {
            return &symbol_cmap_fonts[i];
        }
        if (symbol_cmap_fonts[i].match == SYMBOL_CMAP_EXACT &&
            font_family_equals(family, symbol_cmap_fonts[i].family)) {
            return &symbol_cmap_fonts[i];
        }
    }
    return NULL;
}

/* Map one symbol-cmap byte through the font's F000+byte PUA ranges. */
static uint32_t symbol_cmap_codepoint(unsigned char code,
                                      const symbol_cmap_font *font) {
    size_t i;

    if (font == NULL) {
        return code;
    }
    for (i = 0; i < font->range_count; i++) {
        if (code >= font->ranges[i].first && code <= font->ranges[i].last) {
            return 0xF000U + code;
        }
    }
    return code;
}

/* Convert symbol-cmap single-byte text to UTF-8 for SVG output. */
static int symbol_cmap_text_to_utf8(char *in, size_t size_in, char **out,
                                    size_t *out_len,
                                    const symbol_cmap_font *font) {
    size_t capacity = size_in * 4 + 1;
    size_t offset = 0;
    size_t i;

    if (font == NULL) {
        return 1;
    }
    *out = (char *)calloc(capacity, 1);
    if (*out == NULL) {
        return 1;
    }
    for (i = 0; i < size_in; i++) {
        if (!utf8_append_codepoint(*out, capacity, &offset,
                                   symbol_cmap_codepoint((unsigned char)in[i],
                                                         font))) {
            free(*out);
            *out = NULL;
            return 1;
        }
    }
    (*out)[offset] = '\0';
    *out_len = offset;
    return 0;
}

/* Return the Windows code page used by supported ExtTextOutA charsets. */
static const char *text_charset_encoding(drawingStates *states) {
    if (states == NULL) {
        return NULL;
    }

    switch (states->currentDeviceContext.font_charset) {
    case U_ANSI_CHARSET:
        return "CP1252";
    case U_SHIFTJIS_CHARSET:
        return "CP932";
    case U_HANGUL_CHARSET:
        return "CP949";
    case U_GB2312_CHARSET:
        return "CP936";
    case U_CHINESEBIG5_CHARSET:
        return "CP950";
    case U_GREEK_CHARSET:
        return "CP1253";
    case U_TURKISH_CHARSET:
        return "CP1254";
    case U_HEBREW_CHARSET:
        return "CP1255";
    case U_ARABIC_CHARSET:
        return "CP1256";
    case U_BALTIC_CHARSET:
        return "CP1257";
    case U_RUSSIAN_CHARSET:
        return "CP1251";
    case U_EASTEUROPE_CHARSET:
        return "CP1250";
    case U_THAI_CHARSET:
        return "CP874";
    default:
        return NULL;
    }
}

/* Return true when ExtTextOutA bytes must be decoded as a whole string. */
static bool text_charset_is_multibyte(drawingStates *states) {
    if (states == NULL) {
        return false;
    }

    switch (states->currentDeviceContext.font_charset) {
    case U_SHIFTJIS_CHARSET:
    case U_HANGUL_CHARSET:
    case U_GB2312_CHARSET:
    case U_CHINESEBIG5_CHARSET:
        return true;
    default:
        return false;
    }
}

/* Convert ExtTextOutA bytes through the current charset code page. */
static int charset_text_to_utf8(char *in, size_t size_in, char **out,
                                size_t *out_len, drawingStates *states) {
    const char *encoding = text_charset_encoding(states);

    if (encoding == NULL) {
        return 1;
    }
    return enc_to_utf8(in, size_in, out, out_len, (char *)encoding);
}

/* Draw per-character tspans when EMF supplies explicit Dx advances. */
static void text_positioned_chars_draw(char *contents, FILE *out,
                                       drawingStates *states, uint8_t type,
                                       uint32_t chars,
                                       const double *positions) {
    uint32_t i;

    for (i = 0; i < chars; i++) {
        char *string = NULL;
        size_t string_size = 0;
        char *char_data = contents + i;

        if (type == UTF_16 || type == FONTINDEX) {
            char_data = contents + i * 2;
        }
        text_convert(char_data, 1, &string, &string_size, type, states);
        fprintf(out, "<%stspan x=\"%.4f\">", states->nameSpaceString,
                positions[i]);
        if (string != NULL) {
            fprintf(out, "<![CDATA[%s]]>", string);
            free(string);
        } else {
            fprintf(out, "<![CDATA[]]>");
        }
        fprintf(out, "</%stspan>", states->nameSpaceString);
    }
}

/* Draw DBCS text at the lead-byte origin; the trail byte is only decoded. */
static void text_positioned_multibyte_chars_draw(char *contents, FILE *out,
                                                 drawingStates *states,
                                                 uint32_t chars,
                                                 const double *positions) {
    uint32_t i = 0;

    while (i < chars) {
        unsigned char code = (unsigned char)contents[i];
        uint32_t char_len = (code >= 0x80 && i + 1 < chars) ? 2 : 1;
        double x = positions[i];
        char *string = NULL;
        size_t string_size = 0;

        /*
         * EMR_EXTTEXTOUTA counts DBCS bytes, and WMF MathType records store
         * the visible advance on the lead byte with a zero trail-byte advance.
         */
        charset_text_to_utf8(contents + i, char_len, &string, &string_size,
                             states);
        fprintf(out, "<%stspan x=\"%.4f\"", states->nameSpaceString, x);
        fprintf(out, ">");
        if (string != NULL) {
            fprintf(out, "<![CDATA[%s]]>", string);
            free(string);
        } else {
            fprintf(out, "<![CDATA[]]>");
        }
        fprintf(out, "</%stspan>", states->nameSpaceString);
        i += char_len;
    }
}

void reverse_utf8(char *in, size_t size_in) {
    /* this assumes that str is valid UTF-8 */
    char *scanl, *scanr, *scanr2, c;

    /* first reverse the string */
    for ((scanl = in, scanr = in + size_in); scanl < scanr;) {
        c = *scanl;
        *scanl++ = *--scanr;
        *scanr = c;
    }

    /* then scan all bytes and reverse each multibyte character */
    scanl = scanr = in;
    for (; (c = *scanr++);) {
        if ((c & 0x80) == 0) // ASCII char
            scanl = scanr;
        else if ((c & 0xc0) == 0xc0) { // start of multibyte
            scanr2 = scanr;
            switch (scanr - scanl) {
            case 4:
                c = *scanl, *scanl++ = *--scanr, *scanr = c; // fallthrough
            case 3:                                          // fallthrough
            case 2:
                c = *scanl, *scanl++ = *--scanr, *scanr = c;
            }
            scanr = scanl = scanr2;
        }
    }
}

void text_convert(char *in, size_t size_in, char **out, size_t *size_out,
                  uint8_t type, drawingStates *states) {
    uint8_t *string;
    const symbol_cmap_font *symbol_font;
    int ret = 0;

    switch (type) {
    case UTF_16:
        returnOutOfEmf((intptr_t)in + 2 * (intptr_t)size_in);
        ret = enc_to_utf8(in, 2 * size_in, (char **)&string, size_out,
                          "UTF-16LE");

        break;
    case FONTINDEX:
        returnOutOfEmf((intptr_t)in + 2 * (intptr_t)size_in);
        ret = fontindex_to_utf8((uint16_t *)in, size_in, (char **)&string,
                                size_out,
                                states->currentDeviceContext.font_family,
                                states->currentDeviceContext.font_weight,
                                states->currentDeviceContext.font_italic);
        if (ret==0 && string!=NULL) {
            switch (states->currentDeviceContext.font_charset) {
            case U_HEBREW_CHARSET:
            case U_ARABIC_CHARSET:
                /* with Utf-8 strings, the strings must always be
                 * stored in logical order, not visual order.
                 * Unicode Bidirectional (bidi) Algorithm does the work
                 * of rendering the text properly.
                 * So we reverse the text to be in logical order.
                 * However, this seems imcomplete,
                 * Right to Left ordering can also be set in
                 * ExtTextOutOptions (EMR_*TEXTOUT* records) and EMR_SETLAYOUT
                 * records, and it's completely ignored here.
                 * FIXME this is probably to simplistic.
                 */
                reverse_utf8((char*)string, *size_out);
                break;
            case U_ANSI_CHARSET:
            case U_DEFAULT_CHARSET:
            case U_SYMBOL_CHARSET:
            case U_SHIFTJIS_CHARSET:
            case U_HANGUL_CHARSET:
            case U_GB2312_CHARSET:
            case U_CHINESEBIG5_CHARSET:
            case U_GREEK_CHARSET:
            case U_TURKISH_CHARSET:
            case U_BALTIC_CHARSET:
            case U_RUSSIAN_CHARSET:
            case U_EASTEUROPE_CHARSET:
            case U_THAI_CHARSET:
            case U_JOHAB_CHARSET:
            case U_MAC_CHARSET:
            case U_OEM_CHARSET:
            case U_VISCII_CHARSET:
            case U_TCVN_CHARSET:
            case U_KOI8_CHARSET:
            case U_ISO3_CHARSET:
            case U_ISO4_CHARSET:
            case U_ISO10_CHARSET:
            case U_CELTIC_CHARSET:
            default:
                break;
            }
        }
        break;
    default:
        if (checkOutOfEMF(states,
                          (uintptr_t)((uintptr_t)in + (uintptr_t)size_in))) {
            string = NULL;
        }
        else if ((symbol_font = find_symbol_cmap_font(states)) != NULL) {
            ret = symbol_cmap_text_to_utf8(in, size_in, (char **)&string,
                                           size_out, symbol_font);
            type = UTF_16;
        }
        else if (text_charset_encoding(states) != NULL) {
            ret = charset_text_to_utf8(in, size_in, (char **)&string, size_out,
                                       states);
            type = UTF_16;
        }
        else {
            string = (uint8_t *)calloc((size_in + 1), 1);
            strncpy((char *)string, in, size_in);
            *size_out = size_in;
        }
        break;
    }

    if (ret != 0)
        string = NULL;

    if (string == NULL) {
            return;
    }

    int i = 0;
    while (i < (*size_out) && string[i] != 0x0) {
        // Clean-up not printable ascii char like bells \r etc...
        if (string[i] < 0x20 && string[i] != 0x09 && string[i] != 0x0A &&
            string[i] != 0x0B && string[i] != 0x09) {
            string[i] = 0x20;
        }
        // If it's specified as ascii, it must be ascii,
        // so, replace any char > 127 with 0x20 (space)
        if (type == ASCII && string[i] > 0x7F) {
            string[i] = 0x20;
        }
        i++;
    }
    *out = (char *)string;
}

void text_draw(const char *contents, FILE *out, drawingStates *states,
               uint8_t type) {
    PU_EMRTEXT pemt =
        (PU_EMRTEXT)(contents + sizeof(U_EMREXTTEXTOUTA) - sizeof(U_EMRTEXT));
    double *positions = NULL;

    returnOutOfEmf(pemt);

    if (pemt->fOptions & U_ETO_GLYPH_INDEX) {
        type = FONTINDEX;
    }

    fprintf(out, "<%stext ", states->nameSpaceString);
    clipset_draw(states, out);
    POINT_D Org = point_cal(states, (double)pemt->ptlReference.x,
                            (double)pemt->ptlReference.y);
    positions = text_dx_positions(states, contents, pemt, Org.x);

    text_style_draw(out, states, Org);
    fprintf(out, ">");

    char *string = NULL;
    size_t string_size;
    if (positions != NULL) {
        if (text_charset_is_multibyte(states)) {
            text_positioned_multibyte_chars_draw(
                (char *)(contents + pemt->offString), out, states,
                pemt->nChars, positions);
        } else {
            text_positioned_chars_draw((char *)(contents + pemt->offString),
                                       out, states, type, pemt->nChars,
                                       positions);
        }
        free(positions);
    } else {
        text_convert((char *)(contents + pemt->offString), pemt->nChars,
                     &string, &string_size, type, states);

        if (string != NULL) {
            fprintf(out, "<![CDATA[%s]]>", string);
            free(string);
        } else {
            fprintf(out, "<![CDATA[]]>");
        }
    }
    fprintf(out, "</%stext>\n", states->nameSpaceString);
}
void transform_draw(drawingStates *states, FILE *out) {
    // transformation could be set inside path.
    // If we are in a path, we do nothing here.
    // However the transformation is set in BEGINPATH or ENDPATH.
    // The "pre" parsing is used to determine if such cases can occure
    // and records transformations that doesn't occure where the record is
    // declared.
    // (function U_emf_onerec_analyse)
    if (states->inPath)
        return;
    if (states->transform_open) {
        fprintf(out, "</%sg>\n", states->nameSpaceString);
    }
    fprintf(
        out, "<%sg transform=\"matrix(%.4f %.4f %.4f %.4f %.4f %.4f)\">\n",
        states->nameSpaceString,
        (double)states->currentDeviceContext.worldTransform.eM11,
        (double)states->currentDeviceContext.worldTransform.eM12,
        (double)states->currentDeviceContext.worldTransform.eM21,
        (double)states->currentDeviceContext.worldTransform.eM22,
        (double)scaleX(states, states->currentDeviceContext.worldTransform.eDx),
        (double)scaleY(states,
                       states->currentDeviceContext.worldTransform.eDy));
    states->transform_open = true;
}
bool transform_set(drawingStates *states, U_XFORM xform, uint32_t iMode) {
    switch (iMode) {
    case U_MWT_IDENTITY: {
        setTransformIdentity(states);
        return true;
    }
    case U_MWT_LEFTMULTIPLY: {
        float a11 = xform.eM11;
        float a12 = xform.eM12;
        float a13 = 0.0;
        float a21 = xform.eM21;
        float a22 = xform.eM22;
        float a23 = 0.0;
        float a31 = xform.eDx;
        float a32 = xform.eDy;
        float a33 = 1.0;

        float b11 = states->currentDeviceContext.worldTransform.eM11;
        float b12 = states->currentDeviceContext.worldTransform.eM12;
        // float b13 = 0.0;
        float b21 = states->currentDeviceContext.worldTransform.eM21;
        float b22 = states->currentDeviceContext.worldTransform.eM22;
        // float b23 = 0.0;
        float b31 = states->currentDeviceContext.worldTransform.eDx;
        float b32 = states->currentDeviceContext.worldTransform.eDy;
        // float b33 = 1.0;

        float c11 = a11 * b11 + a12 * b21 + a13 * b31;
        float c12 = a11 * b12 + a12 * b22 + a13 * b32;
        // float c13 = a11*b13 + a12*b23 + a13*b33;;
        float c21 = a21 * b11 + a22 * b21 + a23 * b31;
        float c22 = a21 * b12 + a22 * b22 + a23 * b32;
        // float c23 = a21*b13 + a22*b23 + a23*b33;;
        float c31 = a31 * b11 + a32 * b21 + a33 * b31;
        float c32 = a31 * b12 + a32 * b22 + a33 * b32;
        // float c33 = a31*b13 + a32*b23 + a33*b33;;

        states->currentDeviceContext.worldTransform.eM11 = c11;
        states->currentDeviceContext.worldTransform.eM12 = c12;
        states->currentDeviceContext.worldTransform.eM21 = c21;
        states->currentDeviceContext.worldTransform.eM22 = c22;
        states->currentDeviceContext.worldTransform.eDx = c31;
        states->currentDeviceContext.worldTransform.eDy = c32;

        return true;
    }
    case U_MWT_RIGHTMULTIPLY: {
        float a11 = states->currentDeviceContext.worldTransform.eM11;
        float a12 = states->currentDeviceContext.worldTransform.eM12;
        float a13 = 0.0;
        float a21 = states->currentDeviceContext.worldTransform.eM21;
        float a22 = states->currentDeviceContext.worldTransform.eM22;
        float a23 = 0.0;
        float a31 = states->currentDeviceContext.worldTransform.eDx;
        float a32 = states->currentDeviceContext.worldTransform.eDy;
        float a33 = 1.0;

        float b11 = xform.eM11;
        float b12 = xform.eM12;
        // float b13 = 0.0;
        float b21 = xform.eM21;
        float b22 = xform.eM22;
        // float b23 = 0.0;
        float b31 = xform.eDx;
        float b32 = xform.eDy;
        // float b33 = 1.0;

        float c11 = a11 * b11 + a12 * b21 + a13 * b31;
        float c12 = a11 * b12 + a12 * b22 + a13 * b32;
        // float c13 = a11*b13 + a12*b23 + a13*b33;;
        float c21 = a21 * b11 + a22 * b21 + a23 * b31;
        float c22 = a21 * b12 + a22 * b22 + a23 * b32;
        // float c23 = a21*b13 + a22*b23 + a23*b33;;
        float c31 = a31 * b11 + a32 * b21 + a33 * b31;
        float c32 = a31 * b12 + a32 * b22 + a33 * b32;
        // float c33 = a31*b13 + a32*b23 + a33*b33;;

        states->currentDeviceContext.worldTransform.eM11 = c11;
        states->currentDeviceContext.worldTransform.eM12 = c12;
        states->currentDeviceContext.worldTransform.eM21 = c21;
        states->currentDeviceContext.worldTransform.eM22 = c22;
        states->currentDeviceContext.worldTransform.eDx = c31;
        states->currentDeviceContext.worldTransform.eDy = c32;

        return true;
    }
    case U_MWT_SET: {
        states->currentDeviceContext.worldTransform = xform;

        return true;
    }
    default:
        return false;
    }
}
void width_stroke(drawingStates *states, FILE *out, double width) {
    double tmp_w = scaleX(states, width);
    // minimum size of a line seems to be 1px, even if smaller after resize
    // keeping this behavior
    if ((tmp_w / states->scaling) < 1.0) {
        fprintf(out, "stroke-width=\"1px\" ");
    } else {
        fprintf(out, "stroke-width=\"%.4f\" ", tmp_w);
    }
}

static char encoding_table[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

static int mod_table[] = {0, 2, 1};

char *base64_encode(const unsigned char *data, size_t input_length,
                    size_t *output_length) {
    *output_length = 4 * ((input_length + 2) / 3) + 3;

    char *encoded_data = calloc(*output_length, 1);
    if (encoded_data == NULL)
        return NULL;

    for (int i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
    }

    for (int i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[*output_length - 1 - i] = '=';

    return encoded_data;
}

void pointCurrPathAdd16(drawingStates *states, U_POINT16 pt, int index) {
    if (states->inPath) {
        states->currentPath->last->section.points[index] =
            point_s16(states, pt);
    }
}

void pointCurrPathAdd(drawingStates *states, U_POINT pt, int index) {
    if (states->inPath) {
        states->currentPath->last->section.points[index] = point_s(states, pt);
    }
}

void pointCurrPathAddD(drawingStates *states, POINT_D pt, int index) {
    if (states->inPath) {
        states->currentPath->last->section.points[index] = pt;
    }
}

void addNewSegPath(drawingStates *states, uint8_t type) {
    if (states->inPath) {
        PATH **path = &(states->currentPath);
        add_new_seg(path, type);
    }
}

void free_path(PATH **path) {
    if ((*path) == NULL) {
        return;
    }
    PATH *tmp1 = (*path);
    PATH *tmp2 = (*path);
    while (tmp1 != NULL) {
        tmp1 = tmp1->next;
        free(tmp2->section.points);
        free(tmp2);
        tmp2 = tmp1;
    }
    (*path) = NULL;
}

void draw_path(PATH *in, FILE *out) {
    PATH *tmp = in;
    while (tmp != NULL) {
        uint8_t type = tmp->section.type;
        POINT_D *pt = tmp->section.points;
        switch (type) {
        case SEG_END:
            fprintf(out, "Z ");
            break;
        case SEG_MOVE:
            fprintf(out, "M ");
            point_draw_raw_d(pt[0], out);
            break;
        case SEG_LINE:
            fprintf(out, "L ");
            point_draw_raw_d(pt[0], out);
            break;
        case SEG_ARC:
            fprintf(out, "A ");
            point_draw_raw_d(pt[0], out);
            point_draw_raw_d(pt[1], out);
            break;
        case SEG_BEZIER:
            fprintf(out, "C ");
            point_draw_raw_d(pt[0], out);
            point_draw_raw_d(pt[1], out);
            point_draw_raw_d(pt[2], out);
            break;
        }
        tmp = tmp->next;
    }
}

void copy_path(PATH *in, PATH **out) {
    PATH *tmp = in;
    PATH *out_current = NULL;
    while (tmp != NULL) {
        uint8_t type = tmp->section.type;
        POINT_D *pt = tmp->section.points;
        add_new_seg(&out_current, type);
        switch (type) {
        case SEG_END:
            break;
        case SEG_MOVE:
            out_current->last->section.points[0] = pt[0];
            break;
        case SEG_LINE:
            out_current->last->section.points[0] = pt[0];
            break;
        case SEG_ARC:
            out_current->last->section.points[0] = pt[0];
            out_current->last->section.points[1] = pt[1];
            break;
        case SEG_BEZIER:
            out_current->last->section.points[0] = pt[0];
            out_current->last->section.points[1] = pt[1];
            out_current->last->section.points[2] = pt[2];
            break;
        }
        tmp = tmp->next;
    }
    (*out) = out_current;
}

void offset_path(PATH *in, POINT_D pt) {
    PATH *tmp = in;
    while (tmp != NULL) {
        uint8_t type = tmp->section.type;
        switch (type) {
        case SEG_END:
            break;
        case SEG_MOVE:
            tmp->section.points[0].x += pt.x;
            tmp->section.points[0].y += pt.y;
            break;
        case SEG_LINE:
            tmp->section.points[0].x += pt.x;
            tmp->section.points[0].y += pt.y;
            break;
        case SEG_ARC:
            tmp->section.points[1].x += pt.x;
            tmp->section.points[1].y += pt.y;
            break;
        case SEG_BEZIER:
            tmp->section.points[2].x += pt.x;
            tmp->section.points[2].y += pt.y;
            break;
        }
        tmp = tmp->next;
    }
}

void add_new_seg(PATH **path, uint8_t type) {
    PATH *new_path = calloc(1, sizeof(PATH));
    POINT_D *new_seg;
    switch (type) {
    case SEG_END:
        new_seg = NULL;
        break;
    case SEG_MOVE:
        new_seg = calloc(1, sizeof(POINT_D));
        break;
    case SEG_LINE:
        new_seg = calloc(1, sizeof(POINT_D));
        break;
    case SEG_ARC:
        new_seg = calloc(2, sizeof(POINT_D));
        break;
    case SEG_BEZIER:
        new_seg = calloc(3, sizeof(POINT_D));
        break;
    default:
        new_seg = NULL;
        break;
    }
    new_path->section.points = new_seg;
    new_path->section.type = type;
    if (*path == NULL || (*path)->last == NULL) {
        *path = new_path;
        new_path->last = new_path;
    } else {
        (*path)->last->next = new_path;
        (*path)->last = new_path;
    }
}

void clipset_draw(drawingStates *states, FILE *out) {
    int clipID = states->currentDeviceContext.clipID;
    if (clipID)
        fprintf(out, " clip-path=\"url(#clip-%d)\" ", clipID);
}

#ifdef __cplusplus
}
#endif
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
