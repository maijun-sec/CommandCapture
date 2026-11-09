// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the Go toolchain
//
// WHY GO IS A SEPARATE CASE
//
// A Go build does not invoke a compiler called "go". It invokes the
// **tools** of the toolchain, and the two that matter are:
//
//   <GOROOT>/pkg/tool/<goos>_<goarch>/compile
//       one package -> one archive
//
//   <GOROOT>/pkg/tool/<goos>_<goarch>/link
//       the archives -> the binary
//
// Three things follow from that, and none of them holds for a C or a
// Java build:
//
//   1. THE TOOLS ARE RECOGNIZED BY THEIR PATH, NOT BY THEIR NAME.
//
//      "compile" and "link" are ordinary program names. A build tool
//      called "link" that has nothing to do with Go exists, and
//      recognizing it as the Go compiler would parse its arguments
//      with Go's option syntax -- "-o" and "-p" mean something
//      completely different there. The directory shape
//      ("pkg/tool/<goos>_<goarch>") is what identifies the tool.
//
//   2. THE COMPILE STEP ALONE IS NOT THE BUILD.
//
//      The compile step sees one package and the packages it imports.
//      The **link** step sees every package that ends up in the
//      binary -- and its "-importcfg" is the authoritative statement
//      of that set. A consumer that only has the compile steps knows
//      what each package imports, not what the binary contains.
//
//   3. THE DEPENDENCIES ARE NOT HEADERS.
//
//      A Go package depends on other packages, and the compiler reads
//      their **export data** (the ".a" archives the compile step
//      produced), not their source. The manifest that maps an import
//      path to its export data is the "-importcfg" file.

#ifndef CCAP_COMPDB_GO_H
#define CCAP_COMPDB_GO_H

#include <map>
#include <string>
#include <vector>

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {

// The Go import configuration manifest.
//
// The file is a plain text file, one directive per line:
//
//   packagefile fmt=/usr/local/go/pkg/linux_amd64/fmt.a
//   packagefile github.com/x/y/pkg=/tmp/go-build/b001/_pkg_.a
//
// The value is the **export data** of that package: the archive the
// compile step produced, which carries the package's types without
// its source.
//
// Only the "packagefile" directives are read. The other directives
// the format allows ("packageshlib") describe shared libraries, and
// the toolchain does not write them for a normal build.
struct GoImportConfig {
    // The import path -> the export data file.
    //
    // A map rather than a list: the import path is the key, and two
    // entries with the same key cannot exist.
    std::map<std::string, std::string> packageFiles;

    // The directives that were not "packagefile" ones.
    //
    // They are kept so that a consumer can report what was in the
    // manifest rather than only what was understood.
    std::vector<std::string> otherDirectives;

    // Whether the manifest could be read at all.
    //
    // The manifest lives in the build's temporary directory, and the
    // build deletes that directory when it is done. So a manifest that
    // is gone is the **normal** case when a command line is parsed
    // after the fact -- and it is why the path of the manifest is
    // recorded next to the mapping.
    bool read = false;

    // Why it could not be read (empty when it could).
    std::string error;
};

// Read and parse an import configuration manifest.
//
// The path may be relative; it is resolved against `directory` (the
// working directory of the command that named it).
//
// A manifest that cannot be read is **not** an error of this call:
// the returned structure says so (read == false, error explains), and
// the caller keeps the path. That is the whole point -- the path is
// what a consumer needs when the file itself is gone.
GoImportConfig readGoImportConfig(const std::string& path,
                                  const std::string& directory);

// Parse a Go tool command line into the command.
//
// Only the options that describe the **package** are extracted:
//
//   -p <path>          the import path of the package
//   -lang=go1.23       the language version
//   -importcfg <file>  the manifest of the dependencies
//   -o <file>          the output (the archive, or the binary)
//   <file>.go          the sources (positional)
//
// WHY THE OPTIONS ARE PARSED HERE AND NOT IN THE GENERIC LOOP
//
//   Go's tools use the standard Go flag conventions, and they differ
//   from the C ones in two ways that matter:
//
//     * a value may be attached ("-lang=go1.23") or separate
//       ("-p github.com/x/y/pkg"), and both forms occur in real
//       command lines;
//     * the operands come last: the flag parsing stops at the first
//       operand, and the operands of a Go tool command are the source
//       files.
//
//   A generic "-x <value>" loop would also have to guess whether an
//   unknown flag takes a value, and guessing wrong swallows a source
//   file. So the Go options are read here, where the set of flags
//   that take a value is known.
void parseGoArgv(const std::vector<std::string>& argv, CompileCommand* cmd);

// Is this file a Go source file?
bool isGoSourceFile(const std::string& path);

}  // namespace compdb
}  // namespace ccap

#endif  // CCAP_COMPDB_GO_H
