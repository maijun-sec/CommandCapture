// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb_registry - the compiler registry
//
// WHY THIS FILE EXISTS
//
// ccap_compdb_map.h models a compiler as a **flat profile**: an id, a
// kind, a list of names, and a list of switch rules. That is enough to
// map "gcc -Ifoo -c bar.c" and it is enough for a demo.
//
// It is **not** enough for a real toolchain integration, and the reason
// is visible in any full-coverage reference installation:
//
//   - 86 compiler template directories, 610 files.
//   - One family alone: 52 KB of config XML, 45 KB of switch table,
//     44 KB of custom C++.
//   - Another family: 37 KB of custom C++ with **eight** per-version
//     translator classes.
//   - 416 elements in a formal configuration schema.
//   - ~50 switch attributes in the attribute definition file.
//
// The switch table is NOT where the cost is. The cost is:
//
//   1. the same *executable name* meaning different things on different
//      systems ("cc" is gcc on one, clang on another, and a vendor
//      wrapper on a third);
//   2. the same *family* meaning different things at different versions
//      (TI's CL2000 and CL6X need different translators);
//   3. the same *option* meaning different things in different
//      sub-tables ("-Wl,-a" is a linker option, "-Wp,-D" is a
//      preprocessor option, and both are "a gcc option" to the naive
//      view).
//
// So this file adds the three dimensions that the flat profile lacks:
//
//   AXIS 3 -- the compiler registry
//     A compiler is identified by a **family** (gnu, msvc, iar, ti),
//     which owns a **chain of variants** (gcc -> g++ -> g++cc), and each
//     variant is matched by a **glob pattern** ("gcc-*", "*-gcc") or by
//     a **redirect** ("replace/gcc/g++" -- this executable name means
//     that other compiler).
//
//   AXIS 4 -- the version predicate
//     A rule can say "this applies from version 4.7 onwards". The
//     version is detected by running a probe switch and matching an
//     **ordered** list of regexes, most specific first.
//
//   AXIS 5 -- the rule group
//     A group of rules that applies to a **subset** of the variants
//     ("this -v flag means 'dump the include search path' for gcc, but
//     something else for hightec"). This is called
//     `<option_group><applies_to>`.
//
// And it adds the thing that makes the whole thing usable in practice:
//
//   THE USER OVERRIDE
//     Without a first-class way for a user to say "in the gcc table,
//     drop -Werror" or "in the msvc table, keep /W4 at the AST level",
//     the built-in tables are a liability: they are either too small
//     (and the user cannot extend them) or too big (and the user cannot
//     trim them).
//
// THE PRECEDENCE, STATED ONCE AND ENFORCED EVERYWHERE
//
//   1. the explicit user compiler mapping (by exact name or glob)
//   2. the explicit user redirect
//   3. the built-in glob match, **longest pattern wins**
//   4. the default profile
//   5. the generic profile
//
// At each level, the rules are resolved like this:
//
//   a. the base rules of the matched profile
//   b. the rules inherited from the parent profile (parent first)
//   c. the profile's own rules
//   d. the rule groups whose scope matches the resolved variant
//   e. the user's rule overrides (add / replace / disable)
//
// Every step is "later wins", and every step is recorded in the audit
// report, so the answer to "why was this option kept?" is always
// traceable to one of these steps.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO
//
// It does not run the compiler. The feature-probing attributes and
// config_probe run the toolchain to learn what it supports; we make
// that *configurable* but the default path stays static, because a
// mapping engine that requires the toolchain to be installed is a
// mapping engine that cannot be used on a build log from another
// machine. That trade is stated in
// docs/*/COMPILER-CONFIG-GAP-ANALYSIS-*.md.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "ccap_compdb_map.h"

namespace ccap {
namespace compdb {

// ═══════════════════════════════════════════════════════════════════════════
// The built-in profiles
// ═══════════════════════════════════════════════════════════════════════════
//
// These are the switch tables that ship with the tool. They live in
// option_mapping.cpp and are exposed here so the family builder can
// assemble them into families.
//
// A user profile with the same id **replaces** the built-in one
// entirely -- not a merge -- because the stock rules underneath would
// otherwise still win on the longer option names.

// The gcc / clang compatible table.
CompilerProfile makeGnuFamilyProfile();

// The MSVC table.
CompilerProfile makeMsvcFamilyProfile();

// The clang table (a clone of the gcc table with clang's own quirks).
CompilerProfile makeClangFamilyProfile();

// The IAR EW table.
CompilerProfile makeIarFamilyProfile();

// The conservative fallback: the intersection of the common option
// spellings, and nothing else.
CompilerProfile makeGenericFamilyProfile();

}  // namespace compdb
}  // namespace ccap
