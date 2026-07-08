# WebAssembly build

This branch builds `libemf2svg` as an Emscripten JavaScript/WebAssembly module.
The verified output is:

- `build-wasm/emf2svg.js`
- `build-wasm/emf2svg.wasm`
- `build-wasm/libemf2svg.a`

## Toolchain

Activate Emscripten before configuring or building. This checkout was verified
with Emscripten 6.0.2.

```bash
cd /home/cesar/libemf2svg
source ./emsdk/emsdk_env.sh
emcc --version
```

Build Emscripten's dependency ports once. `embuilder` caches them under the
Emscripten sysroot, so later builds should be quick.

```bash
embuilder build zlib libpng
```

## Build

```bash
cd /home/cesar/libemf2svg
source ./emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -G Ninja \
  -DLONLY=ON \
  -DUSE_SYSTEM_LIBUEMF=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-wasm -j2
```

The Emscripten build defaults are:

- `BUILD_WASM_MODULE=ON`
- `ENABLE_GLYPH_INDEX_TEXT=OFF`
- static `emf2svg` library target
- vendored `libuemf`
- no host `/usr/include` paths

When `ENABLE_GLYPH_INDEX_TEXT=OFF`, CMake skips both Freetype and Fontconfig.

## JavaScript API

The wasm module exports:

- `_emf2svg_wasm_convert`
- `_wmf2svg_wasm_convert`
- `_malloc`
- `_free`
- `HEAPU8`
- `getValue`
- `setValue`
- `ccall`
- `cwrap`
- `UTF8ToString`

The wrapper function keeps the native `char **out` API usable from JavaScript:

```c
int emf2svg_wasm_convert(
    char *input,
    size_t input_len,
    int emfplus,
    int svg_delimiter,
    double width,
    double height,
    char **output,
    size_t *output_len);
```

WMF input uses a separate export that converts WMF to an intermediate EMF buffer
inside wasm memory before generating SVG:

```c
int wmf2svg_wasm_convert(
    char *input,
    size_t input_len,
    int svg_delimiter,
    double width,
    double height,
    char **output,
    size_t *output_len);
```

Return value follows the native library convention: non-zero means success. The
returned SVG buffer is allocated inside wasm memory and must be released with
`_free`.

## Smoke Test

This Node smoke test was verified against `tests/resources/emf/test-037.emf`.

```bash
node -e '
const fs = require("fs");
const createModule = require("./build-wasm/emf2svg.js");
createModule({ locateFile: (p) => "build-wasm/" + p }).then((Module) => {
  const data = fs.readFileSync("tests/resources/emf/test-037.emf");
  const inputPtr = Module._malloc(data.length);
  const outPtrPtr = Module._malloc(4);
  const outLenPtr = Module._malloc(4);

  Module.HEAPU8.set(data, inputPtr);
  Module.setValue(outPtrPtr, 0, "i32");
  Module.setValue(outLenPtr, 0, "i32");

  const ret = Module._emf2svg_wasm_convert(
    inputPtr, data.length, 0, 1, 0, 0, outPtrPtr, outLenPtr
  );
  const outPtr = Module.getValue(outPtrPtr, "i32");
  const outLen = Module.getValue(outLenPtr, "i32");
  const svg = Buffer.from(Module.HEAPU8.slice(outPtr, outPtr + outLen)).toString("utf8");

  console.log(JSON.stringify({
    ret,
    outLen,
    startsWithXml: svg.startsWith("<?xml"),
    hasSvg: svg.includes("<svg"),
    sample: svg.slice(0, 80)
  }));

  if (outPtr) Module._free(outPtr);
  Module._free(inputPtr);
  Module._free(outPtrPtr);
  Module._free(outLenPtr);
  if (!ret || !outLen || !svg.includes("<svg")) process.exit(2);
});
'
```

Verified result:

```json
{"ret":1,"outLen":469780,"startsWithXml":true,"hasSvg":true,"sample":"<?xml version=\"1.0\"  encoding=\"UTF-8\" standalone=\"no\"?>\n<svg version=\"1.1\" xmlns"}
```

This WMF smoke test was verified against `tests/resources/wmf/eq_034.wmf`.

```bash
node -e '
const fs = require("fs");
const createModule = require("./build-wasm/emf2svg.js");
createModule({ locateFile: (p) => "build-wasm/" + p }).then((Module) => {
  const data = fs.readFileSync("tests/resources/wmf/eq_034.wmf");
  const inputPtr = Module._malloc(data.length);
  const outPtrPtr = Module._malloc(4);
  const outLenPtr = Module._malloc(4);

  Module.HEAPU8.set(data, inputPtr);
  Module.setValue(outPtrPtr, 0, "i32");
  Module.setValue(outLenPtr, 0, "i32");

  const ret = Module._wmf2svg_wasm_convert(
    inputPtr, data.length, 1, 0, 0, outPtrPtr, outLenPtr
  );
  const outPtr = Module.getValue(outPtrPtr, "i32");
  const outLen = Module.getValue(outLenPtr, "i32");
  const svg = Buffer.from(Module.HEAPU8.slice(outPtr, outPtr + outLen)).toString("utf8");

  console.log(JSON.stringify({
    ret,
    outLen,
    startsWithXml: svg.startsWith("<?xml"),
    hasSvg: svg.includes("<svg"),
    sample: svg.slice(0, 80)
  }));

  if (outPtr) Module._free(outPtr);
  Module._free(inputPtr);
  Module._free(outPtrPtr);
  Module._free(outLenPtr);
  if (!ret || !outLen || !svg.includes("<svg")) process.exit(2);
});
'
```

Verified result:

```json
{"ret":1,"outLen":7482,"startsWithXml":true,"hasSvg":true,"sample":"<?xml version=\"1.0\"  encoding=\"UTF-8\" standalone=\"no\"?>\n<svg version=\"1.1\" xmlns"}
```

## Glyph-Index Warning Test

`tests/resources/emf/test-183.emf` contains `U_ETO_GLYPH_INDEX` text. The wasm
build intentionally does not attempt Unicode reverse mapping or visual glyph
path rendering for that record. It emits a warning and leaves an empty SVG text
placeholder.

```bash
node -e '
const fs = require("fs");
const createModule = require("./build-wasm/emf2svg.js");
const inputFile = "tests/resources/emf/test-183.emf";
const outputFile = "build-wasm/test-183-warning.svg";

createModule({ locateFile: (p) => "build-wasm/" + p }).then((Module) => {
  const data = fs.readFileSync(inputFile);
  const inputPtr = Module._malloc(data.length);
  const outPtrPtr = Module._malloc(4);
  const outLenPtr = Module._malloc(4);

  Module.HEAPU8.set(data, inputPtr);
  Module.setValue(outPtrPtr, 0, "i32");
  Module.setValue(outLenPtr, 0, "i32");

  const ret = Module._emf2svg_wasm_convert(
    inputPtr, data.length, 0, 1, 0, 0, outPtrPtr, outLenPtr
  );
  const outPtr = Module.getValue(outPtrPtr, "i32");
  const outLen = Module.getValue(outLenPtr, "i32");
  const svg = Buffer.from(Module.HEAPU8.slice(outPtr, outPtr + outLen)).toString("utf8");
  fs.writeFileSync(outputFile, svg);

  const emptyTextCount = (svg.match(/<!\\[CDATA\\[\\]\\]>/g) || []).length;
  console.log(JSON.stringify({ ret, outLen, emptyTextCount, hasSvg: svg.includes("<svg") }));

  if (outPtr) Module._free(outPtr);
  Module._free(inputPtr);
  Module._free(outPtrPtr);
  Module._free(outLenPtr);
  if (!ret || !svg.includes("<svg") || emptyTextCount === 0) process.exit(2);
});
'
```

Verified warning:

```text
WARNING: U_ETO_GLYPH_INDEX text is not supported; emitting empty SVG text.
```

Verified result:

```json
{"ret":1,"outLen":1067,"emptyTextCount":1,"hasSvg":true}
```

## Current Limitations

- `U_ETO_GLYPH_INDEX` text is not converted in Emscripten builds. The converter
  warns and emits empty text for that record because glyph-index content is
  font-specific, not Unicode text.
- `LibXml2` is skipped for Emscripten because current `src/` and `inc/` code does
  not include libxml headers.
- The module is validated in Node. Browser integration should use the same
  `createEmf2SvgModule()` factory and provide `locateFile` if the `.wasm` file
  is served from a different path.

## Documentation Checked

- Emscripten building projects docs: https://emscripten.org/docs/compiling/Building-Projects.html
- Emscripten modularized output docs: https://emscripten.org/docs/compiling/Modularized-Output.html
