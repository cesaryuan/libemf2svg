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

/**
  \brief Return whether two world transforms are equivalent for SVG grouping.

  RestoreDC can replace the current EMF world transform without emitting a
  SETWORLDTRANSFORM record. Comparing with a tiny tolerance avoids rewriting
  SVG groups for float round-off only.
  */
static bool world_transform_equal(const U_XFORM *a, const U_XFORM *b) {
    const double tolerance = 0.000001;

    return fabs((double)a->eM11 - (double)b->eM11) <= tolerance &&
           fabs((double)a->eM12 - (double)b->eM12) <= tolerance &&
           fabs((double)a->eM21 - (double)b->eM21) <= tolerance &&
           fabs((double)a->eM22 - (double)b->eM22) <= tolerance &&
           fabs((double)a->eDx - (double)b->eDx) <= tolerance &&
           fabs((double)a->eDy - (double)b->eDy) <= tolerance;
}

/**
  \brief Close a stale SVG transform group after RestoreDC changes the DC.

  RestoreDC restores the world transform as part of the saved device context,
  but no separate EMR_SETWORLDTRANSFORM record follows. Without closing the old
  SVG group, later page-space records can remain inside a stale transform and be
  transformed twice, as in test-171.emf connector lines.
  */
static void close_stale_restore_transform_group(FILE *out,
                                                drawingStates *states,
                                                const U_XFORM *previous) {
    const U_XFORM *current = &states->currentDeviceContext.worldTransform;

    if (states->transform_open && !world_transform_equal(previous, current)) {
        fprintf(out, "</%sg>\n", states->nameSpaceString);
        states->transform_open = false;
    }
}

void U_EMRINVERTRGN_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRINVERTRGN_print(contents, states);
    }
}
void U_EMRMOVETOEX_draw(const char *contents, FILE *out,
                        drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRMOVETOEX_print(contents, states);
    }

    if (states->inPath) {
        fprintf(out, "M ");
        moveto_draw("U_EMRMOVETOEX", "ptl:", "", contents, out, states);
    } else {
        PU_EMRGENERICPAIR pEmr = (PU_EMRGENERICPAIR)(contents);
        U_POINT pt = pEmr->pair;

        states->cur_x = pt.x;
        states->cur_y = pt.y;
    }
}
void U_EMRPIXELFORMAT_draw(const char *contents, FILE *out,
                           drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRPIXELFORMAT_print(contents, states);
    }
    // PU_EMRPIXELFORMAT pEmr = (PU_EMRPIXELFORMAT)(contents);
}
void U_EMRRESTOREDC_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRRESTOREDC_print(contents, states);
    }
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETMAPMODE)(contents);
    U_XFORM previous = states->currentDeviceContext.worldTransform;
    restoreDeviceContext(states, pEmr->iMode);
    if (!states->Error) {
        close_stale_restore_transform_group(out, states, &previous);
    }
}
void U_EMRSAVEDC_draw(const char *contents, FILE *out, drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSAVEDC_print(contents, states);
    }
    saveDeviceContext(states);
    UNUSED(contents);
}
void U_EMRSCALEVIEWPORTEXTEX_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSCALEVIEWPORTEXTEX_print(contents, states);
    }
}
void U_EMRSCALEWINDOWEXTEX_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSCALEWINDOWEXTEX_print(contents, states);
    }
}
void U_EMRSETARCDIRECTION_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSETARCDIRECTION_print(contents, states);
    }
    PU_EMRSETARCDIRECTION pEmr = (PU_EMRSETARCDIRECTION)contents;
    switch (pEmr->iArcDirection) {
    case U_AD_CLOCKWISE:
        states->currentDeviceContext.arcdir = 1;
        break;
    case U_AD_COUNTERCLOCKWISE:
        states->currentDeviceContext.arcdir = -1;
        break;
    }
}
void U_EMRSETBKCOLOR_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSETBKCOLOR_print(contents, states);
    }
    PU_EMRSETBKCOLOR pEmr = (PU_EMRSETBKCOLOR)(contents);
    states->currentDeviceContext.bk_red = pEmr->crColor.Red;
    states->currentDeviceContext.bk_blue = pEmr->crColor.Blue;
    states->currentDeviceContext.bk_green = pEmr->crColor.Green;
}
void U_EMRSETBKMODE_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSETBKMODE_print(contents, states);
    }
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETMAPMODE)(contents);
    states->currentDeviceContext.bk_mode = pEmr->iMode;
}
void U_EMRSETBRUSHORGEX_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    FLAG_UNUSED;
    if (states->verbose) {
        U_EMRSETBRUSHORGEX_print(contents, states);
    }
}
void U_EMRSETCOLORADJUSTMENT_draw(const char *contents, FILE *out,
                                  drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSETCOLORADJUSTMENT_print(contents, states);
    }
    // PU_EMRSETCOLORADJUSTMENT pEmr = (PU_EMRSETCOLORADJUSTMENT)(contents);
}
void U_EMRSETICMMODE_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    FLAG_UNUSED;
    if (states->verbose) {
        U_EMRSETICMMODE_print(contents, states);
    }
}
void U_EMRSETLAYOUT_draw(const char *contents, FILE *out,
                         drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSETLAYOUT_print(contents, states);
    }
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETLAYOUT)(contents);
    states->text_layout = pEmr->iMode;
}
void U_EMRSETMAPMODE_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    FLAG_PARTIAL;
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETMAPMODE)(contents);
    states->MapMode = pEmr->iMode;
    if (states->verbose) {
        U_EMRSETMAPMODE_print(contents, states);
    }
}
void U_EMRSETMAPPERFLAGS_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSETMAPPERFLAGS_print(contents, states);
    }
    // PU_EMRSETMAPPERFLAGS pEmr = (PU_EMRSETMAPPERFLAGS)(contents);
}
void U_EMRSETMETARGN_draw(const char *contents, FILE *out,
                          drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSETMETARGN_print(contents, states);
    }
    UNUSED(contents);
}
void U_EMRSETMITERLIMIT_draw(const char *contents, FILE *out,
                             drawingStates *states) {
    FLAG_SUPPORTED;
    PU_EMRSETMITERLIMIT pEmr = (PU_EMRSETMITERLIMIT)(contents);
    states->currentDeviceContext.miterLimit = pEmr->eMiterLimit;
    if (states->verbose) {
        U_EMRSETMITERLIMIT_print(contents, states);
    }
}
void U_EMRSETPOLYFILLMODE_draw(const char *contents, FILE *out,
                               drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSETPOLYFILLMODE_print(contents, states);
    }
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETMAPMODE)(contents);
    states->currentDeviceContext.fill_polymode = pEmr->iMode;
}
void U_EMRSETROP2_draw(const char *contents, FILE *out, drawingStates *states) {
    FLAG_IGNORED;
    if (states->verbose) {
        U_EMRSETROP2_print(contents, states);
    }
}
void U_EMRSETSTRETCHBLTMODE_draw(const char *contents, FILE *out,
                                 drawingStates *states) {
    FLAG_PARTIAL;
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETMAPMODE)(contents);
    states->currentDeviceContext.stretchMode = pEmr->iMode;
    if (states->verbose) {
        U_EMRSETSTRETCHBLTMODE_print(contents, states);
    }
}
void U_EMRSETTEXTALIGN_draw(const char *contents, FILE *out,
                            drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSETTEXTALIGN_print(contents, states);
    }
    PU_EMRSETMAPMODE pEmr = (PU_EMRSETMAPMODE)(contents);
    states->currentDeviceContext.text_align = pEmr->iMode;
}
void U_EMRSETTEXTCOLOR_draw(const char *contents, FILE *out,
                            drawingStates *states) {
    FLAG_PARTIAL;
    if (states->verbose) {
        U_EMRSETTEXTCOLOR_print(contents, states);
    }
    PU_EMRSETTEXTCOLOR pEmr = (PU_EMRSETTEXTCOLOR)(contents);
    states->currentDeviceContext.text_red = pEmr->crColor.Red;
    states->currentDeviceContext.text_blue = pEmr->crColor.Blue;
    states->currentDeviceContext.text_green = pEmr->crColor.Green;
}
void U_EMRSETVIEWPORTEXTEX_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSETVIEWPORTEXTEX_print(contents, states);
    }
    PU_EMRSETVIEWPORTEXTEX pEmr = (PU_EMRSETVIEWPORTEXTEX)(contents);

    states->viewPortExX = (double)pEmr->szlExtent.cx;
    states->viewPortExY = (double)pEmr->szlExtent.cy;
    states->viewPortExSet = true;
}
void U_EMRSETVIEWPORTORGEX_draw(const char *contents, FILE *out,
                                drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSETVIEWPORTORGEX_print(contents, states);
    }
    PU_EMRSETVIEWPORTORGEX pEmr = (PU_EMRSETVIEWPORTORGEX)(contents);
    states->viewPortOrgX = (double)pEmr->ptlOrigin.x;
    states->viewPortOrgY = (double)pEmr->ptlOrigin.y;
}
void U_EMRSETWINDOWEXTEX_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSETWINDOWEXTEX_print(contents, states);
    }

    PU_EMRSETWINDOWEXTEX pEmr = (PU_EMRSETVIEWPORTEXTEX)(contents);
    states->windowExX = (double)pEmr->szlExtent.cx;
    states->windowExY = (double)pEmr->szlExtent.cy;
    states->windowExSet = true;
}
void U_EMRSETWINDOWORGEX_draw(const char *contents, FILE *out,
                              drawingStates *states) {
    FLAG_SUPPORTED;
    if (states->verbose) {
        U_EMRSETWINDOWORGEX_print(contents, states);
    }

    PU_EMRSETWINDOWORGEX pEmr = (PU_EMRSETWINDOWORGEX)(contents);
    states->windowOrgX = (double)pEmr->ptlOrigin.x;
    states->windowOrgY = (double)pEmr->ptlOrigin.y;
}

#ifdef __cplusplus
}
#endif
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
