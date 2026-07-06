/* wmf2emf-conv
 *
 * Command line entry point for converting Windows Metafile (WMF) bytes into
 * Enhanced Metafile (EMF) bytes. The heavy lifting is done by the library
 * function wmf2emf(): this file only parses arguments, reads the input file
 * into memory, calls the converter, and writes the generated EMF file.
 */

#include "emf2svg.h"
#include <argp.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define __STRINGIFY__(V) __STR__(V)
#define __STR__(V) #V

const char *argp_program_version = __STRINGIFY__(E2S_VERSION);

const char *argp_program_bug_address =
    "https://github.com/kakwa/libemf2svg/issues";

static char doc[] = "wmf2emf -- Windows Metafile to Enhanced Metafile converter";

static struct argp_option options[] = {
    {"verbose", 'v', 0, 0, "Produce verbose output"},
    {"version", 'V', 0, 0, "Print wmf2emf version"},
    {"input", 'i', "FILE", 0, "Input WMF file"},
    {"output", 'o', "FILE", 0, "Output EMF file"},
    {0}};

static char args_doc[] = "-i FILE -o FILE";

struct arguments {
    bool verbose, version;
    char *output;
    char *input;
};

/* Parse one command line option into the arguments structure. */
static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    struct arguments *arguments = (struct arguments *)state->input;

    switch (key) {
    case 'v':
        arguments->verbose = true;
        break;
    case 'o':
        arguments->output = arg;
        break;
    case 'i':
        arguments->input = arg;
        break;
    case 'V':
        arguments->version = true;
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc};

/* Read a whole binary file into memory for the library API. */
static bool read_binary_file(const char *path, std::vector<char> &contents) {
    std::ifstream in(path, std::ios::binary);

    if (!in.is_open()) {
        std::cerr << "[ERROR] Impossible to open input file '" << path << "'"
                  << std::endl;
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streampos size = in.tellg();
    if (size < 0) {
        std::cerr << "[ERROR] Impossible to read input file size '" << path
                  << "'" << std::endl;
        return false;
    }
    contents.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!contents.empty()) {
        in.read(contents.data(), size);
    }
    return in.good() || in.eof();
}

/* Write generated EMF bytes to disk. */
static bool write_binary_file(const char *path, const char *contents,
                              size_t length) {
    std::ofstream out(path, std::ios::binary);

    if (!out.is_open()) {
        std::cerr << "[ERROR] Impossible to open output file '" << path << "'"
                  << std::endl;
        return false;
    }
    out.write(contents, static_cast<std::streamsize>(length));
    return out.good();
}

/* Convert command line arguments into one WMF to EMF conversion run. */
int main(int argc, char *argv[]) {
    struct arguments arguments;
    std::vector<char> contents;
    char *emf_out = NULL;
    size_t emf_len = 0;
    wmf2emfOptions options;

    arguments.verbose = false;
    arguments.version = false;
    arguments.input = NULL;
    arguments.output = NULL;

    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    if (arguments.version) {
        std::cout << "wmf2emf version: " << __STRINGIFY__(E2S_VERSION)
                  << std::endl;
        return 0;
    }
    if (arguments.input == NULL) {
        std::cerr << "[ERROR] Missing --input=FILE argument" << std::endl;
        return 1;
    }
    if (arguments.output == NULL) {
        std::cerr << "[ERROR] Missing --output=FILE argument" << std::endl;
        return 1;
    }
    if (!read_binary_file(arguments.input, contents)) {
        return 1;
    }
    options.verbose = arguments.verbose;
    if (!wmf2emf(contents.data(), contents.size(), &emf_out, &emf_len,
                 &options)) {
        std::cerr << "[ERROR] WMF to EMF conversion failed" << std::endl;
        free(emf_out);
        return 1;
    }
    bool wrote = write_binary_file(arguments.output, emf_out, emf_len);
    free(emf_out);
    return wrote ? 0 : 1;
}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
