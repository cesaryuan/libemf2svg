/*
 * WebAssembly entry point for libemf2svg.
 *
 * This file adapts the native memory-buffer API to a stable Emscripten export.
 * JavaScript callers allocate the EMF input in wasm memory, pass its pointer and
 * size to emf2svg_wasm_convert(), then read the returned SVG pointer and length
 * through the output pointer slots. The returned SVG buffer is allocated by the
 * core library and must be released with the exported _free().
 */

#include "emf2svg.h"
#include <emscripten/emscripten.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Build generator options from the wasm ABI's scalar arguments.
 *
 * The wasm exports intentionally avoid exposing generatorOptions directly
 * because JavaScript callers should not need to mirror the native struct layout.
 */
static generatorOptions emf2svg_wasm_make_generator_options(int emfplus,
                                                            int svg_delimiter,
                                                            double width,
                                                            double height) {
    generatorOptions options;

    options.nameSpace = NULL;
    options.verbose = false;
    options.emfplus = emfplus != 0;
    options.svgDelimiter = svg_delimiter != 0;
    options.imgWidth = width;
    options.imgHeight = height;
    return options;
}

/*
 * Convert an EMF buffer in wasm memory to an allocated SVG buffer.
 *
 * This wrapper keeps the public wasm ABI simple because the native API returns
 * its SVG through char** and size_t*. A zero return value means conversion
 * failed, matching the existing emf2svg() success/failure convention.
 */
EMSCRIPTEN_KEEPALIVE
int emf2svg_wasm_convert(char *input, size_t input_len, int emfplus,
                         int svg_delimiter, double width, double height,
                         char **output, size_t *output_len) {
    generatorOptions options;

    if (input == NULL || output == NULL || output_len == NULL) {
        return 0;
    }

    options = emf2svg_wasm_make_generator_options(emfplus, svg_delimiter, width,
                                                  height);

    *output = NULL;
    *output_len = 0;
    return emf2svg(input, input_len, output, output_len, &options);
}

/*
 * Convert a WMF buffer in wasm memory to an allocated SVG buffer.
 *
 * WMF support is implemented as a two-stage bridge: first translate WMF bytes to
 * EMF bytes with wmf2emf(), then feed that temporary EMF buffer through the
 * existing SVG generator. The temporary EMF buffer is always freed before this
 * wrapper returns; the returned SVG buffer follows the same ownership rule as
 * emf2svg_wasm_convert() and must be released with the exported _free().
 */
EMSCRIPTEN_KEEPALIVE
int wmf2svg_wasm_convert(char *input, size_t input_len, int svg_delimiter,
                         double width, double height, char **output,
                         size_t *output_len) {
    generatorOptions generator_options;
    wmf2emfOptions wmf_options;
    char *emf_output = NULL;
    size_t emf_output_len = 0;
    int ok;

    if (input == NULL || output == NULL || output_len == NULL) {
        fprintf(stderr, "wmf2svg_wasm_convert: invalid argument\n");
        return 0;
    }

    *output = NULL;
    *output_len = 0;

    wmf_options.verbose = false;
    if (!wmf2emf(input, input_len, &emf_output, &emf_output_len,
                 &wmf_options)) {
        fprintf(stderr, "wmf2svg_wasm_convert: WMF to EMF conversion failed\n");
        return 0;
    }

    generator_options = emf2svg_wasm_make_generator_options(0, svg_delimiter,
                                                            width, height);
    ok = emf2svg(emf_output, emf_output_len, output, output_len,
                 &generator_options);
    if (!ok) {
        fprintf(stderr, "wmf2svg_wasm_convert: EMF to SVG conversion failed\n");
    }

    free(emf_output);
    return ok;
}
