// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the compiler switch mapping table
//
// See ccap_compdb_map.h for the design rationale.
//
// This file implements:
//   1. the option tokenizer (how a switch rule is matched against a
//      token, and how the value is extracted)
//   2. the built-in compiler profiles (gcc / clang / msvc / generic)
//   3. the mapping engine (the retention matrix)
//
// The tokenizer is the delicate part. The option syntaxes overlap in
// ways that make a naive prefix match wrong, and getting it wrong
// produces a command that compiles *different code*:
//
//   "-I"      vs  "-include"   -- both start with "-I"
//   "-isystem" vs "-isysroot"  -- both start with "-isys"
//   "-o"      vs  "-openmp"    -- both start with "-o"
//   "-c"      vs  "-c99"       -- both start with "-c"
//   "-D"      vs  "-Dfoo=bar"  -- the value is attached
//
// So the matcher does NOT use a prefix scan over the option list.
// It picks the **longest matching rule name** for the token, which
// makes "-isystem" win over "-isysroot" by length, and "-include"
// win over "-I" by length.

#include "ccap_compdb_map.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// The version range, used by SwitchRule::appliesToVersion.
//
// The registry header includes this one, so there is no circular
// dependency: the registry builds on the map, not the other way round.
#include "ccap_compdb_registry.h"

// The full definitions of CompileCommand and CompdbConfig.
//
// The map header only forward-declares them (it is included by
// ccap_compdb.h, which defines them), so the implementation needs this
// include to see the members.
#include "ccap_compdb.h"
#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// ── Case-insensitive compare (Windows paths, MSVC options) ───────────────
//
// This is used for the **path-like** comparisons (a profile id, a
// compiler name).
//
// It is NOT used for the option names: gcc's "-o" (the output file)
// and "-O2" (the optimisation level) differ only in case, and mixing
// them up would silently drop the output file. The option matching is
// therefore **case-sensitive** -- see matchesRuleName() below.
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// The **case-sensitive** option name comparison.
//
// Why the option names are case-sensitive and everything else is not:
//
//   gcc's "-o"    is the output file
//   gcc's "-O2"   is the optimisation level
//   gcc's "-D"    is a macro definition
//   gcc's "-d"    is a debug flag
//
// These are different options that happen to differ only in case.
// Treating them as the same would drop the output file (or the macro,
// or the debug flag) without a word -- the worst kind of bug in a
// translation, because the resulting command still compiles and
// produces a plausible-looking but wrong artifact.
//
// MSVC is the opposite: its options are conventionally uppercase
// ("/I", "/D", "/Zi"), and a user may write them in either case on
// Windows. So the MSVC rules are matched case-insensitively -- the
// table says so, per rule.
bool matchesRuleName(const std::string& tokenPart,
                      const std::string& ruleName,
                      bool caseInsensitive) {
    if (caseInsensitive) return iequals(tokenPart, ruleName);
    return tokenPart == ruleName;
}

// The hyphen/underscore equivalence.
//
// The `oa_hyphen_is_underscore` attribute: '--foo-bar' and '--foo_bar'
// are the same option, and the table spells it one way.
//
// The comparison is done on a **copy** rather than by rewriting the
// token: the token keeps its original spelling, so the attached value
// is still cut off at the right offset. The lengths are the same
// because '-' and '_' are both one character.
//
// WHY THIS IS NOT THE DEFAULT
//
// '-' and '_' are interchangeable in *some* option sets and not in
// others. In gcc's option set they are **different**:
//   --sysroot  vs  --sys_root   (only the first one exists)
// Treating them as equivalent by default would let a typo-ish spelling
// of one option silently match another. So it is a per-rule flag.
bool hyphenIsUnderscoreEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x == '-') x = '_';
        if (y == '-') y = '_';
        if (x != y) return false;
    }
    return true;
}

// The name comparison, with the hyphen/underscore equivalence applied
// when the rule asks for it.
bool matchesRuleName(const std::string& tokenPart,
                      const std::string& ruleName,
                      bool caseInsensitive,
                      bool hyphenIsUnderscore) {
    if (caseInsensitive) {
        if (iequals(tokenPart, ruleName)) return true;
        if (!hyphenIsUnderscore) return false;
        // The hyphen/underscore swap has to happen before the case
        // fold, otherwise the two spellings would not line up.
        std::string x = tokenPart;
        std::string y = ruleName;
        for (auto& c : x) if (c == '-') c = '_';
        for (auto& c : y) if (c == '-') c = '_';
        return iequals(x, y);
    }
    if (tokenPart == ruleName) return true;
    if (!hyphenIsUnderscore) return false;
    return hyphenIsUnderscoreEq(tokenPart, ruleName);
}

// The option token, split into (name, value).
//
// The value is empty when the option carries no value at all.
struct Token {
    std::string name;   // without the leading dash(es)
    std::string value;  // the attached / separate value
    bool hasValue = false;
    // The dash spelling: 1 = "-", 2 = "--", 0 = "/"
    int dashes = 1;
    // True when the value was separated by '='
    bool viaEqual = false;
};

// Split a token into its name and value.
//
// The rules:
//   "--sysroot=/x"  -> name "sysroot", value "/x", viaEqual
//   "-Ifoo"         -> name "I",    value "foo"
//   "-I"            -> name "I",    no value
//   "/Ifoo"         -> name "I",    value "foo"   (dashes = 0)
//   "/std:c++17"    -> name "std:", value "c++17"
//
// The MSVC ':' case is handled by the **rule table**, not here: the
// rule named "std:" says "the value is what follows the ':'".
//
// IMPORTANT: the attached value is NOT split here.
//
//   The tokenizer only strips the leading dash(es) and splits on the
//   first '='. The name is then the rest of the token, and the
//   *rule lookup* is what decides where the name ends and the value
//   begins.
//
//   Why: the split point depends on the rule, not on the token. "-Ifoo"
//   has the name "I" and the value "foo" for the rule "I", but the
//   name "Ifoo" and no value for a rule named "Ifoo". And "-isystem"
//   must not be split into "i" + "system" just because some rule
//   happens to be named "i".
//
//   So the rule lookup returns both the rule AND the offset at which
//   the name ends -- that offset is where the attached value starts.
Token splitToken(const std::string& raw) {
    Token t;

    size_t start = 0;
    if (!raw.empty() && raw[0] == '-') {
        start = 1;
        if (raw.size() > 1 && raw[1] == '-') {
            t.dashes = 2;
            start = 2;
        }
    } else if (!raw.empty() && raw[0] == '/') {
        t.dashes = 0;
        start = 1;
    }

    std::string rest = raw.substr(start);

    // The '=' split: only when the option is dash-spelled. MSVC has no
    // '=' convention (it uses ':'), and a Windows path can contain '='
    // legitimately ("-DFOO=a=b"), so the '=' is only honoured for the
    // dash forms and only at the first '='.
    if (t.dashes != 0) {
        size_t eq = rest.find('=');
        if (eq != std::string::npos && eq > 0) {
            t.name = rest.substr(0, eq);
            t.value = rest.substr(eq + 1);
            t.hasValue = true;
            t.viaEqual = true;
            return t;
        }
    }

    t.name = rest;
    return t;
}

// The result of a rule lookup.
struct RuleMatch {
    const SwitchRule* rule = nullptr;
    // The length of the matched name.
    //
    // This is what tells the caller where the **attached value**
    // starts: for "-Ifoo" matching the rule "I", the name is 1
    // character long and the value is "foo".
    size_t nameLen = 0;
    // A rule that matched by name but was **excluded by the version**.
    //
    // This is not a failure: the rule is scoped to a version range
    // ("from gcc 4.7"), and the compiler is not in that range. The
    // option is still recognized -- it just means something else in
    // this version -- so it is reported differently from an option the
    // table has never heard of.
    const SwitchRule* versionExcluded = nullptr;
};

// Find the rule that matches this token, by longest name.
//
// Returns a null rule when nothing matches.
//
// The longest-name rule is what makes "-include" beat "-I" and
// "-isystem" beat "-isysroot".
//
// The rules that carry a trailing '=' in their name are matched
// against the part of the token before the '=' -- that is how
// "-std=c++17" finds the rule named "std=".
//
// The match is a **prefix** match, not an equality match, because the
// value may be attached: "-Ifoo" has to find the rule "I". The
// longest matching rule wins, so "-isystem" is matched by the rule
// "isystem" and not by a shorter one.
//
// VERSION SCOPING
//
// A rule may carry a version range ("from gcc 4.7"). When the compiler
// version is outside the range, the rule does not apply -- and the
// *fallback* matters: a shorter rule of the same name may still apply.
//
//   "-std=c++11" with a rule "std=" scoped to ">= 4.7", on gcc 4.6.
//
// Here the scoped rule does not fire. If there is no other rule for
// "std", the option is reported as **version-excluded** rather than
// unknown, because the table does know the option -- just not for this
// compiler.
//
// A rule whose version range cannot be evaluated (the version is
// unknown) does **not** fire either. That is the safe direction: a
// rule whose precondition could not be verified must not be applied.
//
// ABBREVIATION MATCHING
//
// A rule may carry kShapeAbbrevMatch: the token may then be an
// **unambiguous prefix** of the rule name. "--sysstd" matches the rule
// "--sysroot" only when no other rule in the same table starts with
// "--sysstd".
//
// WHY IT IS A SECOND PASS
//
// The abbreviation is ambiguous by construction, so answering it needs
// the **whole rule set**, not one rule at a time. The first pass
// therefore looks for an exact/prefix match as usual, and only if it
// finds nothing does the second pass consider abbreviations.
//
// The order matters: an exact match always wins over an abbreviation.
// If the table has both "std=" and "std=c++11", then the token "std"
// matches "std=" exactly, and "std=c++11" is never considered -- the
// abbreviation would be ambiguous and would be rejected anyway.
//
// WHY AMBIGUITY IS REJECTED RATHER THAN RESOLVED
//
// Picking the longest candidate would be a guess. If the user writes
// "--sys" and the table has both "--sysroot" and "--sys-version", the
// two are indistinguishable from the token alone, and guessing wrong
// produces a command that compiles different code. So the option is
// reported as unknown instead, which is honest.
RuleMatch findRule(const std::vector<SwitchRule>& rules, const Token& tok,
                   const std::string& version) {
    RuleMatch best;
    size_t excludedLen = 0;

    // Does this rule apply to this compiler version?
    //
    // A rule with no version constraint always applies. A rule with a
    // constraint applies only when the version is known and in range.
    auto applies = [&](const SwitchRule& r) {
        if (r.versionMin.empty() && r.versionMax.empty()) return true;
        return r.appliesToVersion(version);
    };

    // Does this rule's name match the token's name?
    //
    // The shape decides how much of the token the name covers:
    //   prefix     the name is a prefix, the rest is the value
    //   attached   the name is a prefix, the rest is the value
    //   plain      the name is the whole token
    //   "name="    the name is matched without the '='
    //
    // It returns the length of the matched name, which is where the
    // attached value starts.
    auto matchLen = [&](const SwitchRule& r, size_t& out) -> bool {
        const std::string& n = r.name;
        out = 0;

        if (n.back() == '=') {
            // A name ending in '=' means "the value always follows an
            // '='". The token has already been split on '=' by the
            // tokenizer, so the match is against the name without the
            // '='.
            std::string prefix = n.substr(0, n.size() - 1);
            if (tok.name.size() >= prefix.size() &&
                matchesRuleName(tok.name.substr(0, prefix.size()), prefix,
                                r.caseInsensitive,
                                (r.shape & kShapeHyphenIsUnderscore) != 0)) {
                out = prefix.size();
                return true;
            }
            return false;
        }

        if ((r.shape & kShapePrefix) || (r.shape & kShapeAttached)) {
            // A prefix rule ("-f...", "-m...") matches any token that
            // starts with the rule name.
            //
            // A plain rule with an attached value behaves the same way:
            // the token name starts with the rule name and the rest is
            // the attached value.
            //
            // But only when the rule allows an attached value. A rule
            // without kShapeAttached matches the **whole** token name --
            // otherwise "-Wall" would match the rule "W" and the value
            // would be "all", which is wrong for a warning flag.
            if (tok.name.size() >= n.size() &&
                matchesRuleName(tok.name.substr(0, n.size()), n,
                                r.caseInsensitive,
                                (r.shape & kShapeHyphenIsUnderscore) != 0)) {
                out = n.size();
                return true;
            }
            return false;
        }

        // No attached value allowed: the match is an equality.
        if (matchesRuleName(tok.name, n, r.caseInsensitive,
                            (r.shape & kShapeHyphenIsUnderscore) != 0)) {
            out = n.size();
            return true;
        }
        return false;
    };

    // ── Pass 1: the ordinary match ─────────────────────────────────
    for (const auto& r : rules) {
        if (r.name.empty()) continue;

        size_t matchedLen = 0;
        if (!matchLen(r, matchedLen)) continue;

        if (applies(r)) {
            if (matchedLen > best.nameLen) {
                best.rule = &r;
                best.nameLen = matchedLen;
            }
        } else {
            // The rule matched by name but does not apply to this
            // compiler version. Remember it, in case nothing else
            // matches -- the option is then "recognized but not
            // applicable", which is a different report from "unknown".
            if (matchedLen > excludedLen) {
                excludedLen = matchedLen;
                best.versionExcluded = &r;
            }
        }
    }

    if (best.rule) return best;

    // A rule matched by name but was excluded by the version: the
    // option is **known** to the table, it just does not apply to this
    // compiler. That is a better answer than an abbreviation match,
    // and it is a different report ("recognized but not applicable"
    // instead of "unknown"). So the abbreviation pass does not run.
    if (best.versionExcluded) return best;

    // ── Pass 2: the unambiguous abbreviation ───────────────────────
    //
    // Only reached when nothing matched exactly. A rule with
    // kShapeAbbrevMatch may match a token that is a **prefix** of the
    // rule name.
    //
    // The token must be a *proper* prefix (shorter than the rule
    // name): an equal-length match would be an exact match, which pass
    // 1 already handled.
    //
    // The value follows the token, so the matched length is the
    // **token's** length, not the rule's: the token carries the
    // abbreviation, and everything the user wrote after it is the
    // value.
    const SwitchRule* abbrevRule = nullptr;
    size_t abbrevCount = 0;

    for (const auto& r : rules) {
        if (r.name.empty()) continue;
        if (!(r.shape & kShapeAbbrevMatch)) continue;

        // The comparison is against the rule name without a trailing
        // '=', for the same reason as in pass 1.
        std::string n = r.name;
        if (n.back() == '=') n = n.substr(0, n.size() - 1);
        if (n.empty()) continue;

        // The token must be a proper prefix of the rule name.
        if (tok.name.size() >= n.size()) continue;

        if (!matchesRuleName(tok.name, n.substr(0, tok.name.size()),
                             r.caseInsensitive,
                             (r.shape & kShapeHyphenIsUnderscore) != 0)) {
            continue;
        }

        // The rule must apply to this compiler version, otherwise the
        // abbreviation would be reported against a rule that is out of
        // scope.
        if (!applies(r)) continue;

        abbrevRule = &r;
        ++abbrevCount;
        if (abbrevCount > 1) break;
    }

    if (abbrevCount == 1 && abbrevRule) {
        best.rule = abbrevRule;
        // The matched name is the token: everything the user wrote
        // after the abbreviation is the value.
        best.nameLen = tok.name.size();
    }

    return best;
}

// Apply the clang form template.
//
// "%s" is replaced by the value. A form without "%s" is a fixed
// replacement: the value is dropped.
std::string applyForm(const std::string& form, const std::string& value) {
    if (form.empty()) return "";
    size_t p = form.find("%s");
    if (p == std::string::npos) return form;
    std::string out = form;
    out.replace(p, 2, value);
    return out;
}

// Split a value on a separator.
//
// The empty pieces are **kept** as empty strings rather than dropped,
// because the position of an empty piece is information: "-Wl,-a,,-b"
// has an empty second argument, and dropping it would change the
// argument count.
//
// The caller skips the empty pieces when it does not care (the
// translation of a list of options).
std::vector<std::string> splitList(const std::string& value, char sep) {
    std::vector<std::string> out;
    if (value.empty()) {
        out.push_back(value);
        return out;
    }

    size_t start = 0;
    while (true) {
        size_t pos = value.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(value.substr(start));
            break;
        }
        out.push_back(value.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

// ── The rule builders ────────────────────────────────────────────────────
//
// Small helpers so the profile tables below read like a table rather
// than like a wall of braces.
SwitchRule r(const std::string& name, unsigned int shape, OptionClass cls,
             const std::string& clangForm = "", const std::string& note = "") {
    SwitchRule rule;
    rule.name = name;
    rule.shape = shape;
    rule.cls = cls;
    rule.clangForm = clangForm;
    rule.note = note;
    return rule;
}

// The MSVC variant of the rule builder.
//
// The MSVC options are conventionally uppercase ("/I", "/D", "/Zi") and
// a user may write them in either case on Windows, so they are matched
// case-insensitively.
//
// The unix-style options are **not**: gcc's "-o" and "-O2" are
// different options.
SwitchRule mr(const std::string& name, unsigned int shape, OptionClass cls,
              const std::string& clangForm = "", const std::string& note = "") {
    SwitchRule rule = r(name, shape, cls, clangForm, note);
    rule.caseInsensitive = true;
    return rule;
}

}  // namespace

// ── The retention matrix ─────────────────────────────────────────────────
//
// Which option classes survive at which level.
//
// This is the table the whole two-axis model exists to support. It is
// written as one predicate per level rather than a class-by-class
// matrix, because the predicate is easier to read and to review --
// and the review is the point: this table decides what the AST and
// the BC actually see.
bool keptAtLevel(OptionClass cls, TranslateLevel level) {
    switch (cls) {
        // ── The source semantics: needed for a correct AST ───────────
        case OptionClass::Include:
        case OptionClass::SystemInclude:
        case OptionClass::Framework:
        case OptionClass::Macro:
        case OptionClass::MacroUndef:
        case OptionClass::PreInclude:
        case OptionClass::LanguageStd:
        case OptionClass::Language:
        case OptionClass::Sysroot:
        case OptionClass::CompileOnly:
        case OptionClass::ResponseFile:
            return true;

        // ── The target semantics ───────────────────────────────────
        //
        // The **triple** decides the builtin types and the ABI, so it
        // is needed at **both** levels.
        //
        // This is not a guess: the AST of the same source differs
        // between targets (the __int128 / __builtin_va_list / SVE
        // builtin sets), and under some targets the code does not even
        // compile ("__int128 is not supported on this target").
        // Dropping the triple from an AST command therefore produces
        // a *different* AST, not the same one.
        //
        // See the measurement table in ccap_compdb_map.h.
        case OptionClass::Target:
            return true;

        // ── The CPU feature level ───────────────────────────────────
        //
        // "-march" / "-mcpu" decide the instruction set, not the AST:
        // the AST of the same source under -march=x86-64-v2 and
        // -march=x86-64-v3 is identical. So the AST level does not
        // need it, but the bitcode does (the generated code must match
        // the instruction set the rest of the build assumes).
        case OptionClass::TargetCpu:
            return level == TranslateLevel::Bc;

        // ── Debug info: the BC keeps it (so the BC has debug
        //    metadata), the AST does not need it.
        case OptionClass::DebugInfo:
            return level == TranslateLevel::Bc;

        // ── The output file: the AST level does not write anything,
        //    but the BC level does (the .bc file), so it is kept at
        //    both -- the AST caller can simply ignore it.
        case OptionClass::Output:
            return true;

        // ── Everything below is either noise or actively wrong ──────
        case OptionClass::Optimization:
        case OptionClass::Warning:
        case OptionClass::Dependency:
        case OptionClass::Linker:
        case OptionClass::Codegen:
        case OptionClass::StaticAnalysis:
        case OptionClass::Coverage:
        case OptionClass::Sanitizer:
        case OptionClass::Verbosity:
        case OptionClass::PreprocessOnly:
        case OptionClass::Pass:
        case OptionClass::Unknown:
            return false;
    }
    return false;
}

// The reason string for a dropped option.
std::string dropReason(OptionClass cls, TranslateLevel level) {
    switch (cls) {
        case OptionClass::Optimization:
            return "optimisation level does not affect the "
                   "translation unit";
        case OptionClass::Warning:
            return "warning level does not affect the translation unit";
        case OptionClass::Dependency:
            return "dependency generation is a build-graph concern";
        case OptionClass::Linker:
            return "linker option: not part of a compilation";
        case OptionClass::Codegen:
            return "code generation tuning does not affect the "
                   "translation unit";
        case OptionClass::StaticAnalysis:
            return "static analysis option: not part of a compilation";
        case OptionClass::Coverage:
            return "coverage instrumentation changes the code";
        case OptionClass::Sanitizer:
            return "sanitizer instrumentation changes the code";
        case OptionClass::Verbosity:
            return "verbosity flag";
        case OptionClass::PreprocessOnly:
            return "preprocessor-only mode";
        case OptionClass::Pass:
            return "language pass flag";
        case OptionClass::TargetCpu:
            return "the CPU feature level does not affect the "
                   "translation unit; it is only needed for bitcode";
        case OptionClass::DebugInfo:
            return "debug info is only needed for bitcode, not for an "
                   "AST";
        default:
            return "not needed at the " +
                   std::string(translateLevelName(level)) + " level";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// The names
// ═══════════════════════════════════════════════════════════════════════════

const char* translateLevelName(TranslateLevel level) {
    switch (level) {
        case TranslateLevel::Ast: return "ast";
        case TranslateLevel::Bc:  return "bc";
    }
    return "ast";
}

bool translateLevelFromName(const std::string& name, TranslateLevel* out) {
    if (!out) return false;
    std::string l;
    for (char c : name) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "ast" || l == "ast-level" || l == "clang-ast") {
        *out = TranslateLevel::Ast;
        return true;
    }
    if (l == "bc" || l == "bitcode" || l == "llvm-bc" || l == "bc-level") {
        *out = TranslateLevel::Bc;
        return true;
    }
    return false;
}

const char* optionClassName(OptionClass cls) {
    switch (cls) {
        case OptionClass::Unknown:        return "unknown";
        case OptionClass::Include:        return "include";
        case OptionClass::SystemInclude:  return "system-include";
        case OptionClass::Framework:      return "framework";
        case OptionClass::Macro:          return "macro";
        case OptionClass::MacroUndef:     return "macro-undef";
        case OptionClass::PreInclude:     return "pre-include";
        case OptionClass::LanguageStd:    return "language-std";
        case OptionClass::Language:       return "language";
        case OptionClass::Target:         return "target";
        case OptionClass::TargetCpu:      return "target-cpu";
        case OptionClass::Sysroot:        return "sysroot";
        case OptionClass::DebugInfo:      return "debug-info";
        case OptionClass::Optimization:   return "optimization";
        case OptionClass::Output:         return "output";
        case OptionClass::CompileOnly:    return "compile-only";
        case OptionClass::PreprocessOnly: return "preprocess-only";
        case OptionClass::Warning:        return "warning";
        case OptionClass::Dependency:     return "dependency";
        case OptionClass::Linker:         return "linker";
        case OptionClass::Codegen:        return "codegen";
        case OptionClass::StaticAnalysis: return "static-analysis";
        case OptionClass::Coverage:       return "coverage";
        case OptionClass::Sanitizer:      return "sanitizer";
        case OptionClass::ResponseFile:   return "response-file";
        case OptionClass::Verbosity:      return "verbosity";
        case OptionClass::Pass:           return "pass";
        case OptionClass::Reference:      return "reference";
        case OptionClass::AnalyzedFile:   return "analyzed-file";
        case OptionClass::SourceFile:     return "source-file";
        case OptionClass::Nullable:       return "nullable";
        case OptionClass::LangVersion:    return "lang-version";
        case OptionClass::Unsafe:         return "unsafe";
        case OptionClass::PlatformTarget: return "platform-target";
        case OptionClass::Optimize:       return "optimize";
        case OptionClass::DebugSymbols:   return "debug-symbols";
        case OptionClass::Define:         return "define";
    }
    return "unknown";
}

// ═══════════════════════════════════════════════════════════════════════════
// The built-in profiles
//
// These are **public**, not file-local: the compiler registry builds its
// families out of them (see families.cpp), and a user-defined family can
// inherit one. Putting them in an anonymous namespace would make them
// invisible to the registry and to every other translation unit.
// ═══════════════════════════════════════════════════════════════════════════

// ── The gcc / clang family ───────────────────────────────────────────────
//
// gcc and clang share the option syntax, so they share one table. The
// difference between them is the **driver** and the target, not the
// option spelling.
CompilerProfile makeGnuFamilyProfile() {
    CompilerProfile p;
    p.id = "gnu";
    p.kind = CompilerKind::Gcc;

    // The executable names.
    //
    // "cc" and "c++" are the *system* compilers: what they point at
    // depends on the distribution (gcc on Debian, clang on macOS), so
    // they are listed here as a convenience and the registry's glob
    // match is what actually decides. A user whose "cc" is a vendor
    // wrapper says so explicitly.
    p.exactNames = {"gcc", "g++", "cc", "c++", "cpp", "gcc-cc", "gpp"};

    // The cross compilers are matched by the registry's globs
    // ("*-gcc", "*-g++"), because the prefix is arbitrary:
    // "arm-none-eabi-gcc", "riscv64-unknown-elf-gcc",
    // "aarch64-linux-gnu-g++", "mipsel-linux-gnu-gcc-12".
    //
    // The version suffix is handled by the resolver's
    // version-suffixed-name stage, so "gcc-12" is the gcc variant of
    // the gnu family.

    p.driver = "clang";
    p.cppDriver = "clang++";
    p.includeEnvVars = {"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"};

    // ── Include paths ────────────────────────────────────────────────
    p.rules.push_back(r("I", kShapeDash | kShapeAttached | kShapeUnattached |
                            kShapeEqual | kShapePath,
                        OptionClass::Include, "-I%s",
                        "the user include path"));
    p.rules.push_back(r("isystem", kShapeDash | kShapeAttached |
                                   kShapeUnattached | kShapePath,
                        OptionClass::SystemInclude, "-isystem %s",
                        "a system include path (warnings suppressed)"));
    p.rules.push_back(r("idirafter", kShapeDash | kShapeAttached |
                                   kShapeUnattached | kShapePath,
                        OptionClass::SystemInclude, "-idirafter %s",
                        "a system include path searched last"));
    p.rules.push_back(r("iquote", kShapeDash | kShapeAttached |
                                 kShapeUnattached | kShapePath,
                        OptionClass::SystemInclude, "-iquote %s",
                        "a quote-only include path"));

    // ── Macros ───────────────────────────────────────────────────────
    p.rules.push_back(r("D", kShapeDash | kShapeAttached | kShapeUnattached |
                            kShapeEqual,
                        OptionClass::Macro, "-D%s",
                        "a macro definition"));
    p.rules.push_back(r("U", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::MacroUndef, "-U%s",
                        "a macro undef: dropping it changes the meaning "
                        "of every macro"));

    // ── The forced include ───────────────────────────────────────────
    p.rules.push_back(r("include", kShapeDash | kShapeAttached |
                                   kShapeUnattached | kShapeEqual,
                        OptionClass::PreInclude, "-include %s",
                        "a force-included header: it can define macros "
                        "that change which code compiles"));

    // ── The language ─────────────────────────────────────────────────
    p.rules.push_back(r("std=", kShapeDash | kShapeAttached | kShapeEqual,
                        OptionClass::LanguageStd, "-std=%s",
                        "the language standard"));
    p.rules.push_back(r("x", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::Language, "-x %s",
                        "the forced input language"));
    p.rules.push_back(r("ansi", kShapeDash, OptionClass::LanguageStd,
                        "-std=c89", "the C89 language mode"));

    // ── The target ───────────────────────────────────────────────────
    //
    // The **triple** and the **CPU feature level** are separate
    // classes, because they are needed at different levels:
    //
    //   --target   decides the builtin types and the ABI
    //              -> needed for the AST as well
    //   -march    decides the instruction set
    //   -mcpu        -> the AST does not care, the bitcode does
    p.rules.push_back(r("target", kShapeDashDash | kShapeAttached |
                                   kShapeEqual,
                        OptionClass::Target, "--target=%s",
                        "the target triple: it decides the builtin types "
                        "and the ABI"));
    p.rules.push_back(r("march", kShapeDash | kShapeAttached |
                                  kShapeUnattached | kShapeEqual,
                        OptionClass::TargetCpu, "-march=%s",
                        "the target CPU feature level"));
    p.rules.push_back(r("mcpu", kShapeDash | kShapeAttached |
                                 kShapeUnattached | kShapeEqual,
                        OptionClass::TargetCpu, "-mcpu=%s",
                        "the target CPU feature level"));
    p.rules.push_back(r("sysroot", kShapeDashDash | kShapeAttached |
                                     kShapeEqual,
                        OptionClass::Sysroot, "--sysroot=%s",
                        "the sysroot: it decides where the system "
                        "headers come from"));

    // ── Debug info ───────────────────────────────────────────────────
    p.rules.push_back(r("g", kShapeDash, OptionClass::DebugInfo, "-g",
                        "the debug info level"));
    p.rules.push_back(r("g1", kShapeDash, OptionClass::DebugInfo, "-g1",
                        "the debug info level"));
    p.rules.push_back(r("g2", kShapeDash, OptionClass::DebugInfo, "-g2",
                        "the debug info level"));
    p.rules.push_back(r("g3", kShapeDash, OptionClass::DebugInfo, "-g3",
                        "the debug info level"));
    p.rules.push_back(r("ggdb", kShapeDash, OptionClass::DebugInfo, "-g",
                        "the debug info level"));

    // ── The optimisation level ───────────────────────────────────────
    p.rules.push_back(r("O", kShapeDash | kShapeAttached |
                              kShapeOptionalValue,
                        OptionClass::Optimization, "-O%s",
                        "the optimisation level"));
    p.rules.push_back(r("Ofast", kShapeDash, OptionClass::Optimization,
                        "-Ofast", "the optimisation level"));
    p.rules.push_back(r("Os", kShapeDash, OptionClass::Optimization, "-Os",
                        "the optimisation level"));
    p.rules.push_back(r("Oz", kShapeDash, OptionClass::Optimization, "-Oz",
                        "the optimisation level"));
    p.rules.push_back(r("Og", kShapeDash, OptionClass::Optimization, "-Og",
                        "the optimisation level"));

    // ── The output file ──────────────────────────────────────────────
    p.rules.push_back(r("o", kShapeDash | kShapeAttached |
                             kShapeUnattached | kShapeTakesNext,
                        OptionClass::Output, "-o %s",
                        "the output file"));

    // ── Compile only ─────────────────────────────────────────────────
    p.rules.push_back(r("c", kShapeDash, OptionClass::CompileOnly, "-c",
                        "compile only, do not link"));

    // ── The preprocessor-only modes ──────────────────────────────────
    p.rules.push_back(r("E", kShapeDash, OptionClass::PreprocessOnly,
                        "", "preprocess only"));
    p.rules.push_back(r("M", kShapeDash, OptionClass::PreprocessOnly, "",
                        "preprocess only, print the dependencies"));
    p.rules.push_back(r("MM", kShapeDash, OptionClass::PreprocessOnly, "",
                        "preprocess only, print the dependencies"));
    p.rules.push_back(r("MD", kShapeDash, OptionClass::Dependency, "",
                        "generate the dependency file"));
    p.rules.push_back(r("MMD", kShapeDash, OptionClass::Dependency, "",
                        "generate the dependency file"));
    p.rules.push_back(r("MP", kShapeDash, OptionClass::Dependency, "",
                        "generate the phony dependency targets"));

    // ── The warnings ─────────────────────────────────────────────────
    p.rules.push_back(r("W", kShapeDash | kShapeAttached |
                             kShapeOptionalValue,
                        OptionClass::Warning, "-W%s",
                        "a warning control"));
    p.rules.push_back(r("Wall", kShapeDash, OptionClass::Warning, "-Wall",
                        "a warning control"));
    p.rules.push_back(r("Wextra", kShapeDash, OptionClass::Warning,
                        "-Wextra", "a warning control"));
    p.rules.push_back(r("Werror", kShapeDash, OptionClass::Warning, "",
                        "warnings are errors"));

    // ── The linker options ───────────────────────────────────────────
    p.rules.push_back(r("l", kShapeDash | kShapeAttached |
                             kShapeUnattached,
                        OptionClass::Linker, "",
                        "a library to link against"));
    p.rules.push_back(r("L", kShapeDash | kShapeAttached |
                             kShapeUnattached | kShapePath,
                        OptionClass::Linker, "",
                        "a library search path"));

    // "-Wl,-a,-b" dispatches to the **linker** sub-table.
    //
    // The prefix selects a different table: each piece after the comma
    // is a linker option, not a compiler option. Without the
    // sub-table, the whole thing is one opaque blob.
    {
        SwitchRule wl = r("Wl,", kShapeDash | kShapeAttached | kShapeSplit |
                                  kShapeAlternateTable,
                          OptionClass::Linker, "",
                          "a linker option");
        wl.alternateTable = "linker";
        p.rules.push_back(wl);

        // The linker sub-table.
        //
        // The entries are the linker options that clang spells the
        // same way. An option that is not in the table is kept
        // verbatim: dropping it would lose information, and clang
        // passes "-Wl," through to the linker anyway.
        std::vector<SwitchRule> linker;

        SwitchRule entry = r("entry", kShapeDash | kShapeAttached,
                             OptionClass::Linker, "",
                             "the linker entry point");
        linker.push_back(entry);

        entry = r("export-dynamic", kShapeDash,
                  OptionClass::Linker, "",
                  "export the dynamic symbols");
        linker.push_back(entry);

        entry = r("rpath", kShapeDash | kShapeAttached | kShapePath,
                  OptionClass::Linker, "",
                  "the runtime library search path");
        linker.push_back(entry);

        entry = r("z", kShapeDash | kShapeAttached,
                  OptionClass::Linker, "",
                  "a linker option");
        linker.push_back(entry);

        entry = r("soname", kShapeDash | kShapeAttached,
                  OptionClass::Linker, "",
                  "the shared object name");
        linker.push_back(entry);

        p.subTables["linker"] = linker;
    }

    // ── The code generation tuning ───────────────────────────────────
    //
    // The -f flags are numerous and mostly irrelevant to the AST. The
    // rule is on the bare "f" name, matched as a prefix -- which is
    // why the "f" rule has the prefix shape.
    p.rules.push_back(r("f", kShapeDash | kShapeAttached | kShapePrefix,
                        OptionClass::Codegen, "",
                        "a code generation flag"));
    p.rules.push_back(r("m", kShapeDash | kShapeAttached | kShapePrefix,
                        OptionClass::Codegen, "",
                        "a machine specific flag"));

    // ── The instrumentation ──────────────────────────────────────────
    p.rules.push_back(r("coverage", kShapeDashDash, OptionClass::Coverage,
                        "", "coverage instrumentation"));
    p.rules.push_back(r("profile-arcs", kShapeDash, OptionClass::Coverage,
                        "", "coverage instrumentation"));
    p.rules.push_back(r("profile-generate", kShapeDash,
                        OptionClass::Coverage, "",
                        "coverage instrumentation"));
    p.rules.push_back(r("sanitize", kShapeDash | kShapeAttached |
                                  kShapeEqual,
                        OptionClass::Sanitizer, "",
                        "sanitizer instrumentation"));

    // ── The static analysis options ──────────────────────────────────
    p.rules.push_back(r("analyze", kShapeDashDash, OptionClass::StaticAnalysis,
                        "", "run the static analyzer"));

    // ── The response file ────────────────────────────────────────────
    p.rules.push_back(r("@", kShapeNone, OptionClass::ResponseFile, "",
                        "a response file"));

    // ── The verbosity ────────────────────────────────────────────────
    p.rules.push_back(r("v", kShapeDash, OptionClass::Verbosity, "",
                        "verbose"));
    p.rules.push_back(r("verbose", kShapeDashDash, OptionClass::Verbosity,
                        "", "verbose"));

    return p;
}

// ── The MSVC family ─────────────────────────────────────────────────────
CompilerProfile makeMsvcFamilyProfile() {
    CompilerProfile p;
    p.id = "msvc";
    p.kind = CompilerKind::Msvc;

    // The executable names.
    //
    // "cl" is the MSVC driver. "clang-cl" is clang's **MSVC-compatible**
    // driver: it accepts the "/I" / "/D" / "/std:" spelling, so it
    // belongs to this family and not to the clang family.
    //
    // Getting this wrong is not cosmetic: the two families spell their
    // options differently. Sending a "clang-cl" command to the clang
    // table would leave "/Ifoo" untranslated, and the include path
    // would be silently lost.
    p.exactNames = {"cl", "clang-cl", "cl.exe"};

    // The prefixes: the versioned drivers that the Windows SDK and
    // Visual Studio install side by side.
    //
    //   "cl-19"          the versioned driver
    //   "vs" + 4 digits  the per-installation driver
    p.prefixRules = {"cl-"};

    p.driver = "clang-cl";
    p.cppDriver = "clang-cl";
    p.includeEnvVars = {"INCLUDE", "LIB"};

    // ── Include paths ────────────────────────────────────────────────
    p.rules.push_back(mr("I", kShapeNone | kShapeAttached | kShapePath,
                        OptionClass::Include, "-I%s",
                        "the user include path"));
    p.rules.push_back(mr("external:I", kShapeNone | kShapeAttached | kShapePath,
                        OptionClass::SystemInclude, "-isystem %s",
                        "a system include path"));
    p.rules.push_back(mr("imsvc", kShapeNone | kShapeAttached | kShapePath,
                        OptionClass::SystemInclude, "-isystem %s",
                        "a system include path"));

    // ── The framework path (macOS, for the clang-cl driver) ─────────
    p.rules.push_back(mr("F", kShapeNone | kShapeAttached | kShapePath,
                        OptionClass::Framework, "-F %s",
                        "a framework path"));

    // ── Macros ───────────────────────────────────────────────────────
    p.rules.push_back(mr("D", kShapeNone | kShapeAttached,
                        OptionClass::Macro, "-D%s",
                        "a macro definition"));
    p.rules.push_back(mr("U", kShapeNone | kShapeAttached,
                        OptionClass::MacroUndef, "-U%s",
                        "a macro undef: dropping it changes the meaning "
                        "of every macro"));

    // ── The forced include ───────────────────────────────────────────
    p.rules.push_back(mr("FI", kShapeNone | kShapeAttached,
                        OptionClass::PreInclude, "-include %s",
                        "a force-included header"));

    // ── The language ─────────────────────────────────────────────────
    //
    // The MSVC standard is spelled "/std:c++17"; the rule name is
    // "std:" and the value is the part after the colon.
    p.rules.push_back(mr("std:", kShapeNone | kShapeAttached,
                        OptionClass::LanguageStd, "-std=%s",
                        "the language standard"));
    p.rules.push_back(mr("TP", kShapeNone, OptionClass::Language, "-x c++",
                        "compile the following files as C++"));
    p.rules.push_back(mr("TC", kShapeNone, OptionClass::Language, "-x c",
                        "compile the following files as C"));

    // ── The target ───────────────────────────────────────────────────
    //
    // MSVC has no "--target" triple: the target comes from the
    // environment (the vcvarsall script sets INCLUDE / LIB / PATH).
    //
    // "/arch" is the CPU **feature level**, the counterpart of gcc's
    // "-march" -- not a triple. So it is the TargetCpu class.
    p.rules.push_back(mr("arch", kShapeNone | kShapeAttached,
                         OptionClass::TargetCpu, "-march=%s",
                         "the CPU feature level"));
    p.rules.push_back(mr("favor", kShapeNone | kShapeAttached,
                         OptionClass::TargetCpu, "",
                         "the CPU preference: no direct clang equivalent"));

    // ── Debug info ───────────────────────────────────────────────────
    p.rules.push_back(mr("Zi", kShapeNone, OptionClass::DebugInfo, "-g",
                        "the debug info"));
    p.rules.push_back(mr("Z7", kShapeNone, OptionClass::DebugInfo, "-g",
                        "the debug info"));

    // ── The optimisation level ───────────────────────────────────────
    p.rules.push_back(mr("O1", kShapeNone, OptionClass::Optimization, "-O1",
                        "the optimisation level"));
    p.rules.push_back(mr("O2", kShapeNone, OptionClass::Optimization, "-O2",
                        "the optimisation level"));
    p.rules.push_back(mr("Od", kShapeNone, OptionClass::Optimization, "-O0",
                        "the optimisation level"));
    p.rules.push_back(mr("Ox", kShapeNone, OptionClass::Optimization, "-O2",
                        "the optimisation level"));
    p.rules.push_back(mr("Os", kShapeNone, OptionClass::Optimization, "-Os",
                        "the optimisation level"));
    p.rules.push_back(mr("Ot", kShapeNone, OptionClass::Optimization, "-Os",
                        "the optimisation level"));

    // ── The output file ──────────────────────────────────────────────
    p.rules.push_back(mr("Fo", kShapeNone | kShapeAttached,
                        OptionClass::Output, "-o %s",
                        "the object file"));
    p.rules.push_back(mr("Fe", kShapeNone | kShapeAttached,
                        OptionClass::Output, "",
                        "the executable: this is a link output, not a "
                        "compile output"));

    // ── Compile only ─────────────────────────────────────────────────
    p.rules.push_back(mr("c", kShapeNone, OptionClass::CompileOnly, "-c",
                        "compile only, do not link"));

    // ── The warnings ─────────────────────────────────────────────────
    p.rules.push_back(mr("W1", kShapeNone, OptionClass::Warning, "",
                        "a warning level"));
    p.rules.push_back(mr("W2", kShapeNone, OptionClass::Warning, "",
                        "a warning level"));
    p.rules.push_back(mr("W3", kShapeNone, OptionClass::Warning, "",
                        "a warning level"));
    p.rules.push_back(mr("W4", kShapeNone, OptionClass::Warning, "",
                        "a warning level"));
    p.rules.push_back(mr("WX", kShapeNone, OptionClass::Warning, "",
                        "warnings are errors"));
    p.rules.push_back(mr("w", kShapeNone, OptionClass::Warning, "",
                        "warnings are off"));

    // ── The linker options ───────────────────────────────────────────
    p.rules.push_back(mr("link", kShapeNone, OptionClass::Linker, "",
                        "pass the rest to the linker"));
    p.rules.push_back(mr("DEFAULTLIB", kShapeNone | kShapeAttached,
                        OptionClass::Linker, "",
                        "a default library: a link concern"));

    // ── The preprocessor-only mode ───────────────────────────────────
    p.rules.push_back(mr("E", kShapeNone, OptionClass::PreprocessOnly, "",
                        "preprocess only"));
    p.rules.push_back(mr("P", kShapeNone, OptionClass::PreprocessOnly, "",
                        "preprocess only, no line markers"));

    // ── The response file ────────────────────────────────────────────
    p.rules.push_back(mr("@", kShapeNone, OptionClass::ResponseFile, "",
                        "a response file"));

    // ── The options with no clang equivalent ─────────────────────────
    //
    // These are listed so the report can say "no equivalent" instead
    // of "not needed at this level". The difference matters: the first
    // is a gap in the translation, the second is a deliberate choice.
    //
    // They are also given a **rule**, so they are classified (as
    // codegen / unknown-ish options) rather than reported as unknown.
    // The class does not matter much -- they are dropped either way --
    // but having them classified keeps them out of the "unknown" list,
    // which is reserved for the options we genuinely do not understand.
    p.rules.push_back(mr("EHsc", kShapeNone, OptionClass::Codegen, "",
                         "the exception handling model: no clang equivalent"));
    p.rules.push_back(mr("EHs", kShapeNone, OptionClass::Codegen, "",
                         "the exception handling model: no clang equivalent"));
    p.rules.push_back(mr("MD", kShapeNone, OptionClass::Codegen, "",
                         "the runtime library: it changes the ABI"));
    p.rules.push_back(mr("MT", kShapeNone, OptionClass::Codegen, "",
                         "the runtime library: it changes the ABI"));
    p.rules.push_back(mr("MDd", kShapeNone, OptionClass::Codegen, "",
                         "the runtime library: it changes the ABI"));
    p.rules.push_back(mr("MTd", kShapeNone, OptionClass::Codegen, "",
                         "the runtime library: it changes the ABI"));
    p.rules.push_back(mr("GR", kShapeNone, OptionClass::Codegen, "",
                         "the language strictness: no clang equivalent"));
    p.rules.push_back(mr("permissive-", kShapeNone, OptionClass::Codegen, "",
                         "the language strictness: no clang equivalent"));
    p.rules.push_back(mr("utf-8", kShapeNone, OptionClass::Codegen, "",
                         "the source charset (clang defaults to utf-8)"));
    p.rules.push_back(mr("JMC", kShapeNone, OptionClass::Codegen, "",
                         "the just-my-code debugging mode"));
    p.rules.push_back(mr("RTC1", kShapeNone, OptionClass::Codegen, "",
                         "the runtime checks"));
    p.rules.push_back(mr("sdl", kShapeNone, OptionClass::Codegen, "",
                         "the security development lifecycle checks"));
    p.rules.push_back(mr("Zl", kShapeNone, OptionClass::Codegen, "",
                         "the library inclusion: no clang equivalent"));
    p.rules.push_back(mr("ZI", kShapeNone, OptionClass::DebugInfo, "",
                         "the edit-and-continue debug info: no clang equivalent"));
    p.rules.push_back(mr("analyze", kShapeNone, OptionClass::StaticAnalysis, "",
                         "run the static analyzer"));

    p.noEquivalent = {
        "/EHsc",   // the exception handling model
        "/EHs",
        "/MD",     // the runtime library: it changes the ABI
        "/MT",
        "/MDd",
        "/MTd",
        "/GR",     // the language strictness
        "/permissive-",
        "/utf-8",  // the source charset (clang defaults to utf-8)
        "/JMC",
        "/RTC1",
        "/sdl",
        "/ZI",
        "/Zl",
        "/analyze",
    };

    return p;
}

// ── The generic family ──────────────────────────────────────────────────
//
// This is the answer to "an unknown compiler, treat it as the gcc
// family".
//
// The generic profile recognizes **only the option spelling that
// practically every unix-style compiler shares**: -I, -D, -U, -c, -o,
// -L, -l, and the dash-dash long options that are de-facto standard.
//
// Everything beyond that is reported as unknown rather than guessed
// at. That is the honest answer: a "-Qsomething" on an unknown
// compiler might be a warning control or might change the meaning of
// the code, and we cannot tell.
//
// The generic profile is deliberately a **subset** of the gcc table:
// it is the intersection, not a copy.
CompilerProfile makeGenericFamilyProfile() {
    CompilerProfile p;
    p.id = "generic";
    p.kind = CompilerKind::Generic;
    p.exactNames = {};
    p.driver = "clang";
    p.cppDriver = "clang++";

    p.rules.push_back(r("I", kShapeDash | kShapeAttached | kShapeUnattached |
                            kShapeEqual | kShapePath,
                        OptionClass::Include, "-I%s",
                        "the user include path"));
    p.rules.push_back(r("D", kShapeDash | kShapeAttached | kShapeUnattached |
                            kShapeEqual,
                        OptionClass::Macro, "-D%s",
                        "a macro definition"));
    p.rules.push_back(r("U", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::MacroUndef, "-U%s",
                        "a macro undef"));
    p.rules.push_back(r("include", kShapeDash | kShapeAttached |
                                   kShapeUnattached | kShapeEqual,
                        OptionClass::PreInclude, "-include %s",
                        "a force-included header"));
    p.rules.push_back(r("std=", kShapeDash | kShapeAttached | kShapeEqual,
                        OptionClass::LanguageStd, "-std=%s",
                        "the language standard"));
    p.rules.push_back(r("c", kShapeDash, OptionClass::CompileOnly, "-c",
                        "compile only, do not link"));
    p.rules.push_back(r("o", kShapeDash | kShapeAttached |
                             kShapeUnattached | kShapeTakesNext,
                        OptionClass::Output, "-o %s",
                        "the output file"));
    p.rules.push_back(r("L", kShapeDash | kShapeAttached |
                             kShapeUnattached | kShapePath,
                        OptionClass::Linker, "",
                        "a library search path"));
    p.rules.push_back(r("l", kShapeDash | kShapeAttached |
                             kShapeUnattached,
                        OptionClass::Linker, "",
                        "a library to link against"));

    return p;
}

// ── The clang family ─────────────────────────────────────────────────────
//
// clang accepts the gcc option spelling, so its table is the gcc table.
// The differences are the driver's own quirks, not the option syntax:
//
//   - clang has "--target=" and gcc does not.
//   - clang spells the language standard "-std=c++17" the same way.
//   - clang has "-Wno-" prefixed warnings; gcc has some of them too.
//
// So the clang profile **starts from the gcc table** and adds the
// clang-only options. This is the "extends" mechanism in miniature: a
// table that is a superset of another table, expressed as "start from
// there and add these".
CompilerProfile makeClangFamilyProfile() {
    CompilerProfile p = makeGnuFamilyProfile();
    p.id = "clang";
    p.kind = CompilerKind::Clang;

    // The executable names.
    //
    // "clang-cl" is deliberately **not** here: it is the MSVC-compatible
    // driver, so it belongs to the msvc family. Putting it here would
    // send a "/Ifoo" command to the gcc table, where the "/I" spelling
    // is not recognized -- and the include path would be silently lost.
    p.exactNames = {"clang", "clang++", "clang-cpp"};

    // The versioned drivers that the distributions install:
    // "clang-17", "clang++-17".
    p.prefixRules = {"clang-", "clang++-"};

    // clang's own target option. The gcc table has no "--target",
    // because gcc spells the target with "-march" / "-mtune" and the
    // triple comes from the build system.
    p.rules.push_back(r("target=", kShapeDashDash | kShapeAttached |
                                   kShapeEqual,
                        OptionClass::Target, "--target=%s",
                        "the target triple: it decides the builtin types "
                        "and the ABI, so it is needed at every level"));

    // clang's spelling of the CPU feature level.
    p.rules.push_back(r("mcpu=", kShapeDash | kShapeAttached | kShapeEqual,
                        OptionClass::TargetCpu, "-mcpu=%s",
                        "the CPU feature level: it decides the "
                        "instruction set, not the AST"));

    // The clang-only warning spellings that gcc does not have.
    p.rules.push_back(r("Wno-", kShapeDash | kShapePrefix,
                        OptionClass::Warning, "",
                        "a clang warning suppression"));

    return p;
}

// ── The IAR EW family ────────────────────────────────────────────────────
//
// IAR is a completely different option syntax: the options are spelled
// without a dash ("--cpu Cortex_M4"), the include paths are given
// through "-I" but the *system* paths through a config file, and the
// language standard is "-e" style.
//
// This table is deliberately **small**: it covers the options that
// change the meaning of the translation unit (the include paths, the
// defines, the language) and nothing else. Everything it does not
// know is reported as unknown, which is the honest answer for a
// compiler family we have not fully characterised.
CompilerProfile makeIarFamilyProfile() {
    CompilerProfile p;
    p.id = "iar";
    p.kind = CompilerKind::Iar;
    p.exactNames = {"iccarm", "icc", "icpcarm", "icpc"};
    p.driver = "clang";
    p.cppDriver = "clang++";

    p.rules.push_back(r("I", kShapeDash | kShapeAttached | kShapeUnattached |
                            kShapeEqual | kShapePath,
                        OptionClass::Include, "-I%s",
                        "the user include path"));
    p.rules.push_back(r("D", kShapeDash | kShapeAttached | kShapeUnattached |
                            kShapeEqual,
                        OptionClass::Macro, "-D%s",
                        "a macro definition"));
    p.rules.push_back(r("U", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::MacroUndef, "-U%s",
                        "a macro undef"));
    p.rules.push_back(r("e", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::LanguageStd, "-std=%s",
                        "the language standard"));
    p.rules.push_back(r("c", kShapeDash, OptionClass::CompileOnly, "-c",
                        "compile only"));
    p.rules.push_back(r("o", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::Output, "-o %s",
                        "the output file"));
    p.rules.push_back(r("cpu", kShapeDash | kShapeAttached | kShapeUnattached,
                        OptionClass::TargetCpu, "-mcpu=%s",
                        "the CPU feature level"));
    p.rules.push_back(r("cpu", kShapeDashDash | kShapeAttached,
                        OptionClass::TargetCpu, "-mcpu=%s",
                        "the CPU feature level (the long spelling)"));

    return p;
}


// ═══════════════════════════════════════════════════════════════════════════
// The rule-level retention and version predicates
// ═══════════════════════════════════════════════════════════════════════════

// The retention decision for one rule.
//
// The rule's own override wins over the class default, because the
// user override is the more specific statement: "keep -W4 at the AST
// level" says something the class ("warnings are noise") does not.
//
// An override list that is **present but empty** means "dropped at
// every level", which is different from "no override" (the class
// default applies). That distinction is why the check is
// "keepAtLevels.empty() && there was an override" rather than a plain
// emptiness test -- so the rule carries a flag for "the user set it
// to nothing".
bool SwitchRule::keptAt(TranslateLevel level, OptionClass cls) const {
    // The user override wins over the class default, because it is the
    // more specific statement: "keep -W4 at the AST level" says
    // something the class ("warnings are noise") does not.
    if (hasLevelOverride) {
        std::string key = translateLevelName(level);
        for (const std::string& k : keepAtLevels) {
            if (k == key) return true;
        }
        return false;
    }

    return keptAtLevel(cls, level);
}

bool SwitchRule::appliesToVersion(const std::string& version) const {
    VersionRange range;
    range.minVersion = versionMin;
    range.maxVersion = versionMax;
    return range.matches(version);
}

// ═══════════════════════════════════════════════════════════════════════════
// The mapper
// ═══════════════════════════════════════════════════════════════════════════

OptionMapper::OptionMapper() = default;

OptionMapper OptionMapper::builtin() {
    OptionMapper m;
    m.addProfile(makeMsvcFamilyProfile());
    m.addProfile(makeGnuFamilyProfile());
    m.addProfile(makeGenericFamilyProfile());

    // The registry is the family-aware layer. It is populated with the
    // same built-in families, so the two agree out of the box.
    m.registry_ = CompilerRegistry::builtin();
    return m;
}

// ── The resolution ───────────────────────────────────────────────────────
//
// The registry resolution and the flat profile lookup are two views of
// the same question: "which switch table applies to this compiler?".
//
// The registry view is the richer one -- it knows about variants,
// globs, redirects and version-scoped rules -- so it is the one the
// mapper uses. The flat lookup (findProfile) is kept for the callers
// that want to ask about a profile by name without running a whole
// resolution (the CLI's "show" subcommand, the tests).
CompilerResolution OptionMapper::resolve(const std::string& compilerBase,
                                         const std::string& version) const {
    // The explicit mapping is resolved here, not in the registry: the
    // mapper owns the user's mapping list, and the list has its own
    // (exact-then-longest-prefix) precedence.
    std::string explicitId = mappedProfileId(compilerBase);

    // The default profile is the user's "treat everything else as this".
    // It must name a **family**, because that is what the registry
    // resolves against.
    std::string defaultId = defaultProfile_;

    return resolveCompiler(registry_, compilerBase, version, explicitId,
                            defaultId, genericProfile());
}

void OptionMapper::addProfile(const CompilerProfile& profile) {
    profiles_.push_back(profile);
}

void OptionMapper::upsertProfile(const CompilerProfile& profile) {
    bool replaced = false;
    for (auto& existing : profiles_) {
        if (existing.id == profile.id) {
            existing = profile;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profiles_.push_back(profile);
    }

    // ── The registry must learn about it too ────────────────────────
    //
    // This is the step that makes "the user overrides the gcc table"
    // mean something in the family-aware path too.
    //
    // Without it, the user profile would be in the flat list but the
    // registry would still resolve "gcc" to the built-in gnu family --
    // and since the registry is what the mapper actually uses, the
    // override would silently do nothing.
    //
    // The family keeps its variants, groups and version probe: the
    // user is replacing the **switch table**, not the whole family
    // identity. A user who wants a different family identity defines
    // a family, not a profile.
    for (CompilerFamily& fam : registry_.familiesRef()) {
        if (fam.id != profile.id) continue;
        fam.profile = profile;
        return;
    }

    // The profile names a family that does not exist yet: add it as a
    // family of its own, so that the registry can resolve it.
    CompilerFamily fam;
    fam.id = profile.id;
    fam.kind = profile.kind;
    fam.profile = profile;

    CompilerVariant variant;
    variant.id = profile.id;
    variant.language = "C";
    variant.familyHead = true;
    fam.variants.push_back(variant);

    registry_.upsertFamily(fam);
}

const CompilerProfile* OptionMapper::findProfile(
    const std::string& compilerBase) const {
    if (compilerBase.empty()) return nullptr;

    // The base name, without the directory and without the .exe.
    std::string base = ccap::baseName(compilerBase);
    if (base.size() > 4) {
        std::string tail = base.substr(base.size() - 4);
        for (char& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (tail == ".exe") base = base.substr(0, base.size() - 4);
    }

    // The prefix rules come first: "arm-none-eabi-gcc" must not be
    // matched as the plain "gcc".
    for (const auto& p : profiles_) {
        for (const auto& prefix : p.prefixRules) {
            if (prefix.empty()) continue;
            if (base.size() >= prefix.size() &&
                base.compare(0, prefix.size(), prefix) == 0) {
                return &p;
            }
        }
    }

    for (const auto& p : profiles_) {
        for (const auto& n : p.exactNames) {
            if (iequals(n, base)) return &p;
        }
    }

    // The version suffixes: "gcc-11", "clang++-17".
    //
    // The check is "the base name minus the version is a known name",
    // which is the same rule the compiler table uses.
    {
        static const char* kPrefixes[] = {"gcc-", "g++-", "cc-", "c++-",
                                          "clang-", "clang++-"};
        for (const auto& p : profiles_) {
            for (const auto& n : p.exactNames) {
                for (const char* vprefix : kPrefixes) {
                    std::string vp(vprefix);
                    if (base.size() > vp.size() &&
                        base.compare(0, vp.size(), vp) == 0) {
                        std::string rest = base.substr(vp.size());
                        bool allDigits = !rest.empty();
                        for (char c : rest) {
                            if (!std::isdigit(static_cast<unsigned char>(c))) {
                                allDigits = false;
                                break;
                            }
                        }
                        if (allDigits && iequals(n, vp.substr(0, vp.size() - 1))) {
                            return &p;
                        }
                    }
                }
            }
        }
    }

    return nullptr;
}

const CompilerProfile& OptionMapper::profileFor(
    const std::string& compilerBase) const {
    // ── The registry resolution ─────────────────────────────────────
    //
    // This is the authoritative path, because it is the one that knows
    // about the families: the variant chain (gcc -> g++ -> g++cc), the
    // glob and redirect match patterns, and the version-scoped rules.
    //
    // The flat lookup below is the **fallback** for the case where the
    // registry knows nothing about this compiler but the flat profile
    // list does -- which happens when a user adds a profile without
    // also adding a family.
    CompilerResolution res = resolve(compilerBase, compilerVersion_);
    if (res.profile) return *res.profile;

    // ── The explicit mapping comes first ─────────────────────────────
    //
    // This is the "the user says: this compiler is the gcc family"
    // case. It has to win over the name matching, because the user
    // knows something the name does not tell: that "cc" on their
    // system is a wrapper around a completely different compiler.
    //
    // The explicit mapping is **only** the compiler_profiles list --
    // not the default profile. The default is the *last* resort, for
    // a compiler that is not in any profile at all.
    std::string mapped = mappedProfileId(compilerBase);
    if (!mapped.empty()) {
        for (const auto& p : profiles_) {
            if (iequals(p.id, mapped)) return p;
        }
        // The mapping names a profile that does not exist. The honest
        // answer is the generic table -- the conservative one -- rather
        // than a silent guess.
        return genericProfile();
    }

    // ── The profile that matches the name ────────────────────────────
    const CompilerProfile* p = findProfile(compilerBase);
    if (p) return *p;

    // ── The default profile ──────────────────────────────────────────
    //
    // The user said "treat every compiler I do not recognise as
    // this". That is the last resort -- it comes after the name
    // matching, so it never overrides a profile that actually
    // describes the compiler.
    if (!defaultProfile_.empty()) {
        for (const auto& prof : profiles_) {
            if (iequals(prof.id, defaultProfile_)) return prof;
        }
    }

    return genericProfile();
}

const CompilerProfile& OptionMapper::genericProfile() {
    static const CompilerProfile generic = makeGenericFamilyProfile();
    return generic;
}

// ═══════════════════════════════════════════════════════════════════════════
// The explicit unknown-compiler mapping
// ═══════════════════════════════════════════════════════════════════════════

void OptionMapper::setCompilerProfile(const std::string& namePattern,
                                       const std::string& profileId) {
    compilerProfiles_.emplace_back(namePattern, profileId);
}

void OptionMapper::setDefaultProfile(const std::string& profileId) {
    defaultProfile_ = profileId;
}

std::string OptionMapper::mappedProfileId(
    const std::string& compilerBase) const {
    if (compilerBase.empty()) return "";

    std::string base = ccap::baseName(compilerBase);
    if (base.size() > 4) {
        std::string tail = base.substr(base.size() - 4);
        for (char& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (tail == ".exe") base = base.substr(0, base.size() - 4);
    }

    // 1) The exact match.
    for (const auto& kv : compilerProfiles_) {
        if (kv.first.empty() || kv.first.back() == '*') continue;
        if (iequals(kv.first, base)) return kv.second;
    }

    // 2) The longest prefix match.
    //
    // The longest wins, so "arm-none-eabi-*" beats "arm-*".
    const std::pair<std::string, std::string>* best = nullptr;
    for (const auto& kv : compilerProfiles_) {
        if (kv.first.empty() || kv.first.back() != '*') continue;
        std::string prefix = kv.first.substr(0, kv.first.size() - 1);
        if (prefix.empty()) continue;
        if (base.size() < prefix.size()) continue;
        if (!iequals(base.substr(0, prefix.size()), prefix)) continue;
        if (!best || prefix.size() > best->first.size() - 1) best = &kv;
    }
    if (best) return best->second;

    // 3) Nothing matched explicitly.
    //
    // The default profile is NOT returned here: it is the **last**
    // resort in profileFor(), after the name matching has had its
    // chance. Returning it here would make the default override every
    // profile that actually describes the compiler by name.
    return "";
}

OptionClass OptionMapper::classify(const std::string& token,
                                    const CompilerProfile& profile) const {
    if (token.empty()) return OptionClass::Unknown;

    // The response file: "@file". It has no dash.
    if (token[0] == '@') return OptionClass::ResponseFile;

    Token tok = splitToken(token);
    if (tok.name.empty()) return OptionClass::Unknown;

    RuleMatch match = findRule(profile.rules, tok, compilerVersion_);
    if (!match.rule) return OptionClass::Unknown;

    return match.rule->cls;
}

std::vector<std::string> OptionMapper::map(const CompileCommand& cmd,
                                            TranslateLevel level,
                                            MappingReport* report) const {
    std::vector<std::string> out;
    MappingReport local;

    // ── The resolution ───────────────────────────────────────────────
    //
    // The resolution answers three questions at once:
    //
    //   1. which **profile** applies (the driver, the env vars, the
    //      no-equivalent list);
    //   2. which **rule set** applies -- the family's base rules merged
    //      with the variant's own rules, the matching rule groups and
    //      the user's overrides;
    //   3. how the compiler was **matched**, which is what the audit
    //      report shows so a surprising result can be traced.
    //
    // The rule set comes from the resolution rather than from
    // "profile.rules", because that is the whole point of the family
    // model: the g++ variant's rules and the user's overrides live
    // outside the profile.
    CompilerResolution res = resolve(cmd.compilerBase, compilerVersion_);
    const CompilerProfile& profile = res.profile ? *res.profile
                                                  : profileFor(cmd.compilerBase);

    // The effective rules. When the resolution produced none (the
    // generic fallback), the profile's own rules are used.
    const std::vector<SwitchRule>& rules =
        res.rules.empty() ? profile.rules : res.rules;

    local.familyId = res.familyId;
    local.variantId = res.variantId;
    local.matchedBy = res.matchedBy;
    local.matchedPattern = res.matchedPattern;
    local.compilerVersion = res.version;

    // ── The driver ───────────────────────────────────────────────────
    //
    // The C++ driver is needed when the translation unit is C++, because
    // clang++ links libstdc++ and clang does not. Picking the wrong
    // driver changes the **ABI** of the produced artifact.
    std::string driver = profile.driver;
    if (driver.empty()) driver = "clang";

    bool isCpp = false;
    for (const auto& src : cmd.sourceFiles) {
        std::string ext = ccap::extensionOf(src);
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++" ||
            ext == ".C" || ext == ".mm") {
            isCpp = true;
            break;
        }
    }
    if (cmd.forcedLanguage == "cpp") isCpp = true;
    if (cmd.forcedLanguage == "c") isCpp = false;
    for (const auto& kv : cmd.perFileLanguages) {
        if (kv.second == "cpp") isCpp = true;
    }

    if (isCpp && !profile.cppDriver.empty()) driver = profile.cppDriver;
    out.push_back(driver);

    // ── The options ──────────────────────────────────────────────────
    //
    // The raw argv is walked, not the structured fields, because the
    // structured fields have already lost the **order** and the
    // distinction between "this option was given" and "this option was
    // inferred". The order matters for the include paths and for the
    // macro definitions (a later -U undoes an earlier -D).
    //
    // The structured fields (cmd.includeDirs, cmd.defines, ...) are
    // used as a **cross-check**: if the raw walk found nothing but the
    // structured fields have entries, the raw walk missed something
    // and the structured fields are appended. That way a command that
    // was assembled programmatically (not from a raw argv) still maps.
    size_t i = 1;
    for (; i < cmd.argv.size(); ++i) {
        const std::string& raw = cmd.argv[i];

        // ── Not an option: a source file or a linker input ──────────
        if (raw.empty()) continue;
        if (raw[0] != '-' && raw[0] != '/' && raw[0] != '@') {
            // A source file goes to the end of the command; anything
            // else is a linker input and is dropped.
            std::string ext = ccap::extensionOf(raw);
            if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
                ext == ".c++" || ext == ".C" || ext == ".m" || ext == ".mm" ||
                ext == ".h" || ext == ".hh" || ext == ".hpp") {
                OptionDecision d;
                d.original = raw;
                d.cls = OptionClass::Unknown;
                d.kept = true;
                d.emitted = raw;
                d.bucket = AuditBucket::Kept;
                local.kept.push_back(raw);
                local.decisions.push_back(d);
                out.push_back(raw);
            } else {
                OptionDecision d;
                d.original = raw;
                d.cls = OptionClass::Linker;
                d.kept = false;
                d.reason = "linker input: not part of a compilation";
                d.bucket = AuditBucket::DroppedByLevel;
                local.decisions.push_back(d);
                local.droppedByLevel.push_back(d.reason + ": " + raw);
                local.droppedByLevelBucket.push_back(raw);
            }
            continue;
        }

        Token tok = splitToken(raw);
        RuleMatch match = findRule(rules, tok, res.version);
        const SwitchRule* rule = match.rule;

        // ── The "no equivalent" check ───────────────────────────────
        //
        // This comes **before** the unknown check, and it is a
        // separate list on purpose.
        //
        // The difference between the two:
        //   "no equivalent"  -- the option is known, it is understood,
        //                       and clang simply has no spelling for it
        //                       ("/EHsc" is the exception handling model)
        //   "unknown"        -- the option is not in the table at all,
        //                       so we do not even know what it does
        //
        // Conflating them would lose information: a known gap in the
        // translation is a different problem from an unrecognized
        // option, and the reader needs to tell them apart.
        //
        // The comparison is **exact**.
        //
        // Why not case-insensitive: "/Zi" (the debug info, which clang
        // has as "-g") and "/ZI" (the edit-and-continue debug info,
        // which clang does not have) differ only in case. Matching
        // them case-insensitively would make the two indistinguishable,
        // and one of them would silently get the wrong treatment.
        //
        // The list is written in the conventional spelling of each
        // option, and the user writes the option the same way, so the
        // exact match is the right one.
        bool isNoEquivalent = false;
        for (const auto& n : profile.noEquivalent) {
            if (n == raw) {
                isNoEquivalent = true;
                break;
            }
        }

        if (!rule) {
            // ── An unknown option ───────────────────────────────────
            //
            // It is NOT silently dropped: it is reported, because an
            // unknown option may well be one that changes the meaning
            // of the translation unit. The conservative choice is to
            // drop it from the *generated* command (an unknown option
            // would make clang fail to start) but to tell the caller
            // about it.
            //
            // WHAT THE POLICY CHANGES
            //
            // The default ("report") drops it from the command and
            // records it. The other three policies exist because
            // different projects need different things:
            //
            //   keep -- pass it through unchanged. For a project whose
            //          clang actually accepts the option, this is
            //          right, and dropping it would lose information.
            //   drop -- drop it *silently*. This is the one mode that
            //          can produce a wrong artifact without telling
            //          anybody, so it is not the default and it is
            //          still counted in the audit buckets.
            //   fail -- treat it as an error. The mapping fails, so a
            //          pipeline can stop rather than produce a
            //          suspicious artifact.
            OptionDecision d;
            d.original = raw;
            d.cls = OptionClass::Unknown;

            // A rule that matched by name but was excluded by the
            // version is not "unknown": the table knows the option, it
            // just does not apply to this compiler version. That is a
            // different fact and it gets a different reason and a
            // different bucket.
            //
            // It is **not** in the unknown list either: the unknown
            // list means "the table has never heard of this", and
            // conflating the two would make the report claim that the
            // option table is incomplete when it is in fact complete
            // but version-scoped.
            if (match.versionExcluded) {
                d.reason =
                    "the switch table knows this option, but the rule "
                    "does not apply to this compiler version";
                d.bucket = AuditBucket::DroppedByPolicy;
                d.kept = false;
                local.droppedByPolicy.push_back(raw);
                local.decisions.push_back(d);
                continue;
            }

            if (isNoEquivalent) {
                d.reason = "no clang equivalent for this option";
                d.bucket = AuditBucket::DroppedNoEquivalent;
                local.noEquivalent.push_back(raw);
                local.droppedNoEquivalent.push_back(raw);
                local.decisions.push_back(d);
                continue;
            }

            d.reason = "not in the switch table for the '" +
                       profile.id + "' family";
            d.bucket = AuditBucket::Unknown;
            local.unknown.push_back(raw);
            local.unknownBucket.push_back(raw);

            switch (unknownPolicy_) {
                case UnknownPolicy::Report:
                    d.kept = false;
                    break;
                case UnknownPolicy::Keep:
                    // Pass it through unchanged. The option is still
                    // unknown, so it is still reported -- but it is not
                    // lost.
                    d.kept = true;
                    d.emitted = raw;
                    out.push_back(raw);
                    break;
                case UnknownPolicy::Drop:
                    d.kept = false;
                    d.reason += " (dropped by policy)";
                    d.bucket = AuditBucket::DroppedByPolicy;
                    local.droppedByPolicy.push_back(raw);
                    break;
                case UnknownPolicy::Fail:
                    d.kept = false;
                    d.reason += " (fatal: the unknown-option policy is 'fail')";
                    d.bucket = AuditBucket::DroppedByPolicy;
                    local.droppedByPolicy.push_back(raw);
                    local.failed = true;
                    local.failureReason =
                        "unknown option '" + raw + "' in the '" + profile.id +
                        "' family";
                    break;
            }

            local.decisions.push_back(d);
            continue;
        }

        // ── The value ───────────────────────────────────────────────
        //
        // The value may be:
        //   attached    "-Ifoo"        -> the part after the rule name
        //   after '='   "-std=c++17"   -> already split by the tokenizer
        //   separate    "-I" "foo"     -> the next argv entry
        //
        // The attached case is the interesting one: the rule lookup
        // tells us how many characters of the token the **name**
        // occupies, and everything after that is the value.
        //
        // The '=' case has a subtlety. The tokenizer splits the token
        // at the FIRST '=', so "-DFOO=1" arrives as name "DFOO" and
        // value "1". But the rule "D" only occupies 1 character of
        // the name, so the value is really "FOO=1" -- the whole
        // remainder of the name, the '=' and the part after it.
        //
        // So the '=' has to be put back: the tokenizer threw it away,
        // and for a macro definition it is **part of the value**
        // ("-DFOO=1" means "define FOO as 1", not "define FOO1").
        std::string value = tok.value;
        bool hasValue = tok.hasValue;

        // ── The colon form ───────────────────────────────────────────
        //
        // The value is introduced by a ':' that is part of the option
        // token: "/std:c++17", "/clang:--target=x86_64-linux-gnu".
        //
        // The rule is named without the colon ("std"), and the colon
        // is the separator. That is more general than the hard-coded
        // form below, which requires the rule name to *end* with a
        // colon -- so a rule named "std" cannot use it.
        //
        // THE COLON COMES FIRST
        //
        // The colon is a **separator**, so the value starts *after*
        // it. If the plain attached form ran first, the value would be
        // ":c++17" -- the colon included -- and the translation would
        // be "-std=:c++17", which is not a valid option.
        //
        // So the colon form claims the value before the plain attached
        // form gets a chance.
        bool colonHandled = false;
        if ((rule->shape & kShapeColon) &&
            tok.name.size() > rule->name.size()) {
            std::string tail = tok.name.substr(rule->name.size());
            if (!tail.empty() && tail[0] == ':') {
                value = tail.substr(1);
                hasValue = true;
                colonHandled = true;
            }
        }

        if (!colonHandled && match.nameLen > 0 &&
            match.nameLen < tok.name.size() &&
            (rule->shape & kShapeAttached)) {
            std::string attached = tok.name.substr(match.nameLen);
            if (hasValue) {
                // The '=' case: the name's tail, the '=' and the value
                // belong together.
                value = attached + "=" + tok.value;
            } else {
                value = attached;
            }
            hasValue = true;
        }

        // The MSVC ':' convention: the rule named "std:" takes the
        // value from what follows the ':'.
        //
        // This is the **hard-coded** form. The general form is the
        // kShapeColon flag above, which lets a rule be named "std" and
        // still take the value from what follows the ':'.
        if (!hasValue && !rule->name.empty() && rule->name.back() == ':' &&
            tok.name.size() > rule->name.size()) {
            value = tok.name.substr(rule->name.size());
            hasValue = true;
        }

        // ── The parenthesised form ───────────────────────────────────
        //
        // The value is wrapped in parentheses: "--foo(bar)".
        //
        // The closing parenthesis must be the last character, otherwise
        // the token is not the shape the rule describes.
        if (rule->shape & kShapeParens) {
            std::string source = hasValue ? value : tok.name;
            size_t open = source.find('(');
            if (open != std::string::npos && source.back() == ')') {
                value = source.substr(open + 1, source.size() - open - 2);
                hasValue = true;
            }
        }

        // ── The quoted form ──────────────────────────────────────────
        //
        // The value is wrapped in a quote pair, and the quotes are
        // removed: '"/usr/include"' -> "/usr/include".
        //
        // The removal happens here rather than in the tokenizer
        // because the quotes are part of the **value**, not of the
        // option name: "-DFOO=\"bar baz\"" defines a macro whose value
        // contains a space.
        if ((rule->shape & kShapeStripQuotes) && hasValue &&
            value.size() >= 2) {
            char first = value.front();
            char last = value.back();
            if ((first == '"' && last == '"') ||
                (first == '\'' && last == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }

        if (!hasValue && (rule->shape & kShapeUnattached) &&
            i + 1 < cmd.argv.size()) {
            const std::string& next = cmd.argv[i + 1];
            // The next entry must not itself be an option -- otherwise
            // "-I -Dfoo" would swallow the -D.
            bool nextIsOption = !next.empty() && next[0] == '-';
            if (!nextIsOption) {
                value = next;
                hasValue = true;
                ++i;
            }
        }

        // ── The classification ──────────────────────────────────────
        OptionClass cls = rule->cls;

        // A prefix rule ("-f...", "-m...", "-Wl,...") matches by
        // prefix. The rule lookup already checked the prefix, so this
        // is only a defensive re-check.
        if (rule->shape & kShapePrefix) {
            bool exact = matchesRuleName(tok.name, rule->name,
                                         rule->caseInsensitive);
            if (!exact && tok.name.size() > rule->name.size()) {
                std::string prefix = tok.name.substr(0, rule->name.size());
                if (!matchesRuleName(prefix, rule->name,
                                     rule->caseInsensitive)) {
                    cls = OptionClass::Unknown;
                }
            }
        }

        OptionDecision d;
        d.original = raw;
        d.cls = cls;

        // ── The no-equivalent short-circuit ──────────────────────────
        //
        // An option that is known to have **no clang spelling** is
        // dropped here, before the retention decision.
        //
        // Why it has to come first: the MSVC "/ZI" and "/Zi" are the
        // same option spelled differently, and "/ZI" has no clang
        // equivalent while "/Zi" does. The no-equivalent list is the
        // more specific statement, so it wins.
        if (isNoEquivalent) {
            d.kept = false;
            d.reason = "no clang equivalent for this option";
            d.bucket = AuditBucket::DroppedNoEquivalent;
            local.decisions.push_back(d);
            local.noEquivalent.push_back(raw);
            local.droppedNoEquivalent.push_back(raw);
            continue;
        }

        // ── The skip flag ───────────────────────────────────────────
        //
        // "oa_skip_arg": the option is not a compile option at all.
        // It is dropped without a complaint, because the compiler's own
        // documentation says so.
        if (rule->shape & kShapeSkip) {
            d.kept = false;
            d.reason = "not a compile option";
            d.bucket = AuditBucket::DroppedByPolicy;
            local.decisions.push_back(d);
            local.droppedByLevel.push_back(d.reason + ": " + raw);
            local.droppedByPolicy.push_back(raw);
            continue;
        }

        // ── The "known but unsupported" flag ────────────────────────
        //
        // The compiler accepts the option, but the translation cannot
        // honour it. This is **not** the same as "no equivalent":
        // "no equivalent" means clang has no such concept, while
        // "unsupported" means we could translate it if we had the
        // feature.
        //
        // It is reported rather than passed through, because passing it
        // through would produce a command that clang rejects.
        if (rule->shape & kShapeUnsupported) {
            d.kept = false;
            d.reason = "known but unsupported by the translation";
            d.bucket = AuditBucket::DroppedNoEquivalent;
            local.decisions.push_back(d);
            local.noEquivalent.push_back(raw);
            local.droppedNoEquivalent.push_back(raw);
            continue;
        }

        // ── The retention decision ──────────────────────────────────
        //
        // The **rule's** decision wins over the class default, because
        // the rule is the more specific statement: "keep -W4 at the AST
        // level" says something the class ("warnings are noise") does
        // not.
        if (!rule->keptAt(level, cls)) {
            d.kept = false;
            d.reason = dropReason(cls, level);
            d.bucket = AuditBucket::DroppedByLevel;
            local.decisions.push_back(d);
            local.droppedByLevel.push_back(d.reason + ": " + raw);
            local.droppedByLevelBucket.push_back(raw);
            continue;
        }

        // ── The emission ────────────────────────────────────────────
        //
        // The rule's clang form is the template. When it is empty, the
        // original token is kept -- that is the common case, because
        // clang understands the gcc spelling.
        //
        // The clang form is a **template** that produces one or more
        // argv entries:
        //
        //   "-I%s"          -> ["-I", "<value>"]     the value is separate
        //   "-D%s"          -> ["-D<value>"]        the value is attached
        //   "-std=%s"       -> ["-std=<value>"]     the value is attached
        //   "-isystem %s"   -> ["-isystem", "<value>"]
        //   "-x c++"        -> ["-x", "c++"]        a fixed replacement
        //
        // The rule is: the form is split on the **first space**. The
        // head is the option, the tail is the value part. When the
        // tail contains a "%s", it is the value; when it does not, it
        // is a fixed literal.
        //
        // Why the space matters: an argv entry must not contain a
        // space, or the command line is ambiguous. Splitting on the
        // space is what turns "-isystem %s" into two argv entries.
        std::vector<std::string> emittedTokens;

        // ── The split form ───────────────────────────────────────────
        //
        // The value is a comma-separated list, and each piece is
        // translated separately. The canonical case is gcc's
        // "-Wl,-a,-b": the "-Wl," prefix selects the **linker** table,
        // and each piece after the comma is a linker option.
        //
        // The two flags work together:
        //
        //   kShapeSplit          the value is split on commas
        //   kShapeAlternateTable  each piece goes through the sub-table
        //                         named by the rule
        //
        // A split without an alternate table is still meaningful: the
        // pieces are joined back with commas, but each one is looked
        // up in the rule's own table, so a piece may be translated
        // differently from the whole.
        if (rule->shape & kShapeSplit) {
            std::vector<std::string> pieces = splitList(value, ',');

            const std::vector<SwitchRule>* subTable = nullptr;
            if (rule->shape & kShapeAlternateTable) {
                auto it = profile.subTables.find(rule->alternateTable);
                if (it != profile.subTables.end()) {
                    subTable = &it->second;
                }
            }

            // The pieces that could not be translated.
            std::vector<std::string> untranslated;

            for (const std::string& piece : pieces) {
                if (piece.empty()) continue;

                std::string translated;
                bool ok = false;

                if (subTable) {
                    // The sub-table's rules.
                    Token pieceTok = splitToken(piece);
                    RuleMatch pieceMatch =
                        findRule(*subTable, pieceTok, res.version);
                    if (pieceMatch.rule) {
                        if (pieceMatch.rule->clangForm.empty()) {
                            translated = piece;
                        } else {
                            translated =
                                applyForm(pieceMatch.rule->clangForm,
                                          pieceTok.value);
                        }
                        ok = true;
                    }
                } else {
                    // The rule's own table: the piece is looked up as if
                    // it were an option of its own.
                    Token pieceTok = splitToken(piece);
                    RuleMatch pieceMatch =
                        findRule(rules, pieceTok, res.version);
                    if (pieceMatch.rule && !pieceMatch.rule->clangForm.empty()) {
                        translated =
                            applyForm(pieceMatch.rule->clangForm,
                                      pieceTok.value);
                        ok = true;
                    }
                }

                if (!ok) {
                    // The piece is not translated. It is kept verbatim,
                    // because dropping it would lose information and
                    // rewriting it wrongly would be worse.
                    translated = piece;
                }
                untranslated.push_back(translated);
            }

            // The pieces are joined back with commas, and the rule's
            // clang form wraps them.
            std::string joined;
            for (size_t i = 0; i < untranslated.size(); ++i) {
                if (i) joined += ",";
                joined += untranslated[i];
            }

            if (rule->clangForm.empty()) {
                emittedTokens.push_back(joined);
            } else {
                const std::string& form = rule->clangForm;
                size_t sp = form.find(' ');
                std::string head = (sp == std::string::npos)
                                       ? form
                                       : form.substr(0, sp);
                std::string tail = (sp == std::string::npos)
                                       ? std::string()
                                       : form.substr(sp + 1);
                if (tail.empty()) {
                    emittedTokens.push_back(applyForm(head, joined));
                } else if (tail.find("%s") != std::string::npos) {
                    emittedTokens.push_back(head);
                    emittedTokens.push_back(applyForm(tail, joined));
                } else {
                    emittedTokens.push_back(head);
                    emittedTokens.push_back(tail);
                }
            }
        } else if (rule->clangForm.empty()) {
            // No clang form: the original token is kept verbatim.
            emittedTokens.push_back(raw);
        } else {
            const std::string& form = rule->clangForm;

            size_t sp = form.find(' ');
            std::string head = (sp == std::string::npos) ? form
                                                          : form.substr(0, sp);
            std::string tail = (sp == std::string::npos) ? std::string()
                                                          : form.substr(sp + 1);

            if (tail.empty()) {
                // The form is a single token: "-D%s" or "-std=%s".
                emittedTokens.push_back(applyForm(head, value));
            } else if (tail.find("%s") != std::string::npos) {
                // The form is an option and a value: "-isystem %s".
                emittedTokens.push_back(head);
                emittedTokens.push_back(applyForm(tail, value));
            } else {
                // A fixed replacement: "-x c++".
                emittedTokens.push_back(head);
                emittedTokens.push_back(tail);
            }
        }

        d.emitted = emittedTokens.front();
        d.kept = true;

        // ── Kept or mapped ──────────────────────────────────────────
        //
        // The distinction is not cosmetic: a **rewrite** is where a
        // semantic drift would hide. "-Ifoo" kept as "-Ifoo" is safe by
        // inspection; "/Ifoo" turned into "-Ifoo" is a translation that
        // has to be trusted.
        //
        // The test is whether the emitted form differs from the
        // original token. When it does not, the option was carried
        // through unchanged.
        if (d.emitted == raw) {
            d.bucket = AuditBucket::Kept;
            local.kept.push_back(raw);
        } else {
            d.bucket = AuditBucket::Mapped;
            local.mapped.push_back(raw);
        }

        local.decisions.push_back(d);
        for (const auto& t : emittedTokens) {
            out.push_back(t);
        }
    }

    // ── The source files ─────────────────────────────────────────────
    //
    // They go at the end, after the options. The raw walk already
    // emitted them in place; here we make sure every source file is
    // present exactly once.
    //
    // The dedup is by **base name**, not by the full path.
    //
    // Why: the raw walk emits the token as it was written ("main.cpp")
    // while the structured field holds the absolutized path
    // ("/build/main.cpp"). Comparing the full paths would find no
    // match and the source would be emitted twice -- the compiler would
    // then compile "main.cpp" twice.
    //
    // The base name is the right granularity: two different files with
    // the same base name in one command line is not a thing (it would
    // be ambiguous anyway), so a base-name match is safe.
    for (const auto& src : cmd.sourceFiles) {
        bool seen = false;
        std::string srcBase = ccap::baseName(src);
        for (const auto& d : local.decisions) {
            if (!d.kept) continue;
            if (d.emitted == src) {
                seen = true;
                break;
            }
            if (ccap::baseName(d.emitted) == srcBase) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            OptionDecision d;
            d.original = src;
            d.cls = OptionClass::Unknown;
            d.kept = true;
            d.emitted = src;
            d.bucket = AuditBucket::Kept;
            local.kept.push_back(src);
            local.decisions.push_back(d);
            out.push_back(src);
        }
    }

    // ── The structured fields the raw walk could not see ────────────
    //
    // A command parsed from a compilation database has its options in
    // the argv, so the raw walk sees everything. But a command
    // assembled by a caller (or a command whose options came from the
    // environment) may have structured fields with no argv
    // counterpart.
    //
    // So each structured field is appended **only if the raw walk did
    // not produce that class**.
    //
    // Why the guard matters: without it, every option that appears in
    // the argv AND in a structured field is emitted twice. The
    // command still compiles (a duplicate -I is harmless), but the
    // output is wrong -- and for a duplicate "-D" it is *not* harmless
    // if the two spellings disagree.
    {
        // Which classes did the raw walk already emit?
        bool sawInclude = false;
        bool sawSystemInclude = false;
        bool sawFramework = false;
        bool sawMacro = false;
        bool sawPreInclude = false;
        for (const auto& d : local.decisions) {
            if (!d.kept) continue;
            switch (d.cls) {
                case OptionClass::Include:       sawInclude = true; break;
                case OptionClass::SystemInclude: sawSystemInclude = true; break;
                case OptionClass::Framework:     sawFramework = true; break;
                case OptionClass::Macro:         sawMacro = true; break;
                case OptionClass::PreInclude:    sawPreInclude = true; break;
                default: break;
            }
        }

        if (!sawInclude) {
            for (const auto& dir : cmd.includeDirs) {
                out.push_back("-I");
                out.push_back(dir);
            }
        }
        if (!sawMacro) {
            for (const auto& kv : cmd.defines) {
                if (kv.second.empty()) {
                    out.push_back("-D" + kv.first);
                } else {
                    out.push_back("-D" + kv.first + "=" + kv.second);
                }
            }
        }
        if (!sawPreInclude) {
            for (const auto& f : cmd.preIncludes) {
                out.push_back("-include");
                out.push_back(f);
            }
        }
        if (!sawSystemInclude) {
            for (const auto& d : cmd.systemDirs) {
                out.push_back("-isystem");
                out.push_back(d);
            }
        }
        if (!sawFramework) {
            for (const auto& d : cmd.frameworkDirs) {
                out.push_back("-F");
                out.push_back(d);
            }
        }
    }

    // ── The output file ─────────────────────────────────────────────
    //
    // The raw walk handles "-o" through the rule table. This is the
    // fallback for a command whose output was set but not present in
    // the argv.
    {
        bool sawOutput = false;
        for (const auto& d : local.decisions) {
            if (d.kept && d.cls == OptionClass::Output) sawOutput = true;
        }
        if (!sawOutput && !cmd.outputFile.empty()) {
            out.push_back("-o");
            out.push_back(cmd.outputFile);
        }
    }

    // ── The compile-only flag ───────────────────────────────────────
    //
    // The raw walk handles "-c" through the rule table. The fallback
    // covers a command whose intent was recorded but not present in
    // the argv.
    {
        bool sawCompileOnly = false;
        for (const auto& d : local.decisions) {
            if (d.kept && d.cls == OptionClass::CompileOnly) sawCompileOnly = true;
        }
        if (!sawCompileOnly && cmd.compileOnly) {
            out.push_back("-c");
        }
    }

    if (report) *report = local;
    return out;
}

std::vector<size_t> MappingReport::bucketCounts(size_t bucketCount) const {
    std::vector<size_t> counts(bucketCount, 0);
    for (const OptionDecision& d : decisions) {
        size_t b = static_cast<size_t>(d.bucket);
        if (b < counts.size()) ++counts[b];
    }
    return counts;
}

std::string MappingReport::toText() const {
    std::string out;

    // ── The resolution header ────────────────────────────────────────
    //
    // The reader needs to know **which** table was applied, otherwise
    // a surprising result cannot be traced to a decision.
    if (!familyId.empty()) {
        out += "  family: " + familyId;
        if (!variantId.empty()) out += " (variant " + variantId + ")";
        if (!matchedBy.empty()) out += "  matched by: " + matchedBy;
        if (!matchedPattern.empty()) out += " [" + matchedPattern + "]";
        if (!compilerVersion.empty()) out += "  version: " + compilerVersion;
        out += "\n";
    }

    for (const auto& d : decisions) {
        out += d.kept ? "  keep   " : "  drop   ";
        out += d.original;
        if (d.kept && !d.emitted.empty() && d.emitted != d.original) {
            out += "  ->  " + d.emitted;
        }
        if (!d.kept && !d.reason.empty()) {
            out += "  (" + d.reason + ")";
        }
        out += "\n";
    }

    // ── The bucket summary ──────────────────────────────────────────
    //
    // The six buckets, counted. This is the line a reviewer reads
    // first: "3 kept, 1 mapped, 2 dropped by level, 1 unknown" tells
    // the whole story at a glance.
    {
        std::vector<size_t> counts = bucketCounts(6);
        out += "  buckets:";
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0) continue;
            out += " ";
            out += auditBucketName(static_cast<AuditBucket>(i));
            out += "=";
            out += std::to_string(counts[i]);
        }
        out += "\n";
    }

    if (failed) {
        out += "  FAILED: ";
        out += failureReason;
        out += "\n";
    }

    return out;
}

}  // namespace compdb
}  // namespace ccap
