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
    if (input == NULL || output == NULL || output_len == NULL) {
        return 0;
    }

    generatorOptions options;
    options.nameSpace = NULL;
    options.verbose = false;
    options.emfplus = emfplus != 0;
    options.svgDelimiter = svg_delimiter != 0;
    options.imgWidth = width;
    options.imgHeight = height;

    *output = NULL;
    *output_len = 0;
    return emf2svg(input, input_len, output, output_len, &options);
}
