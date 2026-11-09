// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb_resolution - resolving a compiler to a rule set
//
// See ccap_compdb_registry.h for the design rationale.
//
// This file implements the resolution: given a compiler executable name
// (and optionally a version), produce the family, the variant and the
// effective rule set.
//
// THE RESOLUTION ORDER
//
// This is stated once, here, and enforced by the code below:
//
//   1. an explicit user mapping (exact name or glob)
//   2. a user redirect ("replace/a/b")
//   3. a built-in match (exact name)
//   4. a built-in match (glob, **longest pattern wins**)
//   5. a version-suffixed name ("gcc-11")
//   6. the default profile
//   7. the generic profile
//
// The rule set for a resolved variant is then assembled:
//
//   a. the family profile's base rules
//   b. the rules inherited from the derived-from variant (recursively)
//   c. the variant's own rules
//   d. the rule groups whose scope matches the variant
//   e. the user's rule overrides
//
// Every step is "later wins". The merge is by **rule name**, and the
// winner is the one with the **longest name**, because the option
// matcher itself resolves by longest name: if a user adds a rule named
// "std=" while the built-in table has "std", the "std=" rule has to
// win, or "-std=c++17" would match the shorter "std" first and pick up
// the wrong shape.

#include <algorithm>
#include <cctype>

#include "ccap_compdb_registry.h"
#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

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

// The compiler base name: the file name without the directory and
// without a trailing ".exe".
std::string compilerBaseName(const std::string& path) {
    std::string base = ccap::baseName(path);
    if (base.size() > 4) {
        std::string tail = base.substr(base.size() - 4);
        for (char& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (tail == ".exe") base = base.substr(0, base.size() - 4);
    }
    return base;
}

// Merge a rule list into an accumulator, "later wins by name".
//
// The accumulator keeps the rules in insertion order, and a rule that
// is replaced keeps its **original position** but takes the new
// content. That matters because the option matcher picks the longest
// matching name regardless of position, but the *report* lists the
// rules in a stable order.
void mergeRules(std::vector<SwitchRule>* into,
                const std::vector<SwitchRule>& from) {
    for (const SwitchRule& r : from) {
        bool replaced = false;
        for (SwitchRule& existing : *into) {
            if (iequals(existing.name, r.name)) {
                existing = r;
                replaced = true;
                break;
            }
        }
        if (!replaced) into->push_back(r);
    }
}

// The inheritance chain of a variant, from the most distant ancestor
// to the variant itself.
//
// The derived-from attribute does the same thing: the hexagon,
// keilarm and xtensa_clang families derive from another family's
// compiler.
//
// The chain is guarded against a cycle: a malformed configuration
// that says "a derives from b derives from a" must not hang.
std::vector<std::string> inheritanceChain(const CompilerFamily& family,
                                          const std::string& variantId) {
    std::vector<std::string> chain;
    std::string current = variantId;

    for (int guard = 0; guard < 16; ++guard) {
        const CompilerVariant* v = family.findVariant(current);
        if (!v) break;
        if (v->derivedFrom.empty()) break;

        // A cycle guard: the ancestor must not already be in the chain.
        bool seen = false;
        for (const std::string& s : chain) {
            if (iequals(s, v->derivedFrom)) {
                seen = true;
                break;
            }
        }
        if (seen) break;

        chain.push_back(v->derivedFrom);
        current = v->derivedFrom;
    }

    // The chain was built from the variant upwards, so it is in
    // reverse dependency order. The oldest ancestor has to be applied
    // first.
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// The level names, as the rule stores them.
std::string levelKey(TranslateLevel level) {
    return translateLevelName(level);
}

// Apply the user's rule overrides to a rule set.
//
// The override vocabulary:
//   disable   -- remove the rule
//   replace   -- change the clang spelling
//   class     -- change what the option means
//   keepAt    -- change the retention levels
//   shape     -- change the option shape
//   add       -- add a rule that does not exist in any table
//
// The scope is (profile, variant): an override with an empty profile
// applies to every profile, an override with an empty variant applies
// to every variant of that profile.
void applyOverrides(const CompilerRegistry& registry,
                    const std::string& familyId,
                    const std::string& variantId,
                    std::vector<SwitchRule>* rules) {
    for (const ProfileOverride& ov : registry.overrides()) {
        if (!ov.profile.empty() && !iequals(ov.profile, familyId)) continue;
        if (!ov.variant.empty() && !iequals(ov.variant, variantId)) continue;

        // ── The disables ────────────────────────────────────────────
        for (const std::string& name : ov.disable) {
            rules->erase(std::remove_if(rules->begin(), rules->end(),
                                        [&](const SwitchRule& r) {
                                            return iequals(r.name, name);
                                        }),
                         rules->end());
        }

        // ── The replacements and the additions ──────────────────────
        for (const RuleOverride& ro : ov.overrides) {
            if (ro.disable) {
                rules->erase(std::remove_if(rules->begin(), rules->end(),
                                            [&](const SwitchRule& r) {
                                                return iequals(r.name, ro.name);
                                            }),
                             rules->end());
                continue;
            }

            SwitchRule* existing = nullptr;
            for (SwitchRule& r : *rules) {
                if (iequals(r.name, ro.name)) {
                    existing = &r;
                    break;
                }
            }

            if (!existing) {
                // An override for a rule that does not exist creates it.
                // The class defaults to Unknown, which means the option
                // is reported rather than silently dropped.
                SwitchRule fresh;
                fresh.name = ro.name;
                fresh.cls = ro.hasClass ? ro.cls : OptionClass::Unknown;
                fresh.clangForm = ro.hasClangForm ? ro.clangForm : "";
                if (ro.hasShape) fresh.shape = ro.shape;
                rules->push_back(fresh);
                existing = &(*rules)[rules->size() - 1];
            }

            if (ro.hasClangForm) existing->clangForm = ro.clangForm;
            if (ro.hasClass) existing->cls = ro.cls;
            if (ro.hasShape) existing->shape = ro.shape;
            if (ro.hasLevelOverride) {
                existing->hasLevelOverride = true;
                existing->keepAtLevels.clear();
                for (TranslateLevel l : ro.keepAt) {
                    existing->keepAtLevels.push_back(levelKey(l));
                }
            }
        }

        // ── The plain additions ─────────────────────────────────────
        mergeRules(rules, ov.add);
    }
}

// Assemble the effective rule set for one family + variant.
//
// The order is:
//   a. the family profile's base rules
//   b. the inheritance chain, oldest ancestor first
//   c. the variant's own rules
//   d. the rule groups whose scope matches
//   e. the user's overrides
std::vector<SwitchRule> assembleRules(const CompilerRegistry& registry,
                                       const CompilerFamily& fam,
                                       const std::string& variantId) {
    std::vector<SwitchRule> rules = fam.profile.rules;

    // b. The inheritance chain.
    for (const std::string& ancestorId : inheritanceChain(fam, variantId)) {
        const CompilerVariant* ancestor = fam.findVariant(ancestorId);
        if (!ancestor) continue;
        mergeRules(&rules, ancestor->rules);
    }

    // c. The variant's own rules.
    const CompilerVariant* v = fam.findVariant(variantId);
    if (v) mergeRules(&rules, v->rules);

    // d. The rule groups.
    for (const RuleGroup& g : fam.groups) {
        if (g.appliesToVariant(fam.id, variantId)) {
            mergeRules(&rules, g.rules);
        }
    }

    // e. The user's overrides.
    applyOverrides(registry, fam.id, variantId, &rules);

    return rules;
}

// The first variant of a family (the family head).
std::string headVariant(const CompilerFamily& fam) {
    for (const auto& v : fam.variants) {
        if (v.familyHead) return v.id;
    }
    if (!fam.variants.empty()) return fam.variants[0].id;
    return fam.profile.id;
}

// The variant that an executable name selects.
//
// The variants of a family are named after the executables that select
// them: "g++" is the g++ variant of the gnu family, "clang++" is the
// clang++ variant of the clang family.
//
// WHY THIS MATTERS
//
// "gcc" and "g++" have almost the same options, but not the same:
// "g++" compiles C++ by default and "gcc" does not. A toolchain that
// flattens them into one table either loses that or has to
// special-case it by hand. Selecting the variant by name is what
// makes the distinction automatic.
//
// When no variant matches the name, the family head is used: that is
// the conservative choice, because the head is the variant whose rules
// the others inherit from.
std::string variantForName(const CompilerFamily& fam, const std::string& name) {
    // The exact match: the executable name *is* the variant id.
    for (const CompilerVariant& v : fam.variants) {
        if (iequals(v.id, name)) return v.id;
    }

    // The cross compilers: "arm-none-eabi-gcc" is the gcc variant of
    // the gnu family, even though the variant's id is just "gcc".
    //
    // The rule is "the name ends with the variant id, at a component
    // boundary": "arm-none-eabi-gcc" ends with "gcc", and the character
    // before it is a '-', so it is a whole component and not part of a
    // longer word.
    //
    // The boundary matters: a variant "cc" must not match "arm-none-eabi-gcc"
    // by accident -- but here it does not, because the character before
    // "cc" is a letter, not a separator.
    for (const CompilerVariant& v : fam.variants) {
        if (v.id.empty()) continue;
        if (name.size() <= v.id.size()) continue;
        if (!iequals(name.substr(name.size() - v.id.size()), v.id)) continue;

        const char before = name[name.size() - v.id.size() - 1];
        if (before == '-' || before == '_' || before == '.') {
            return v.id;
        }
    }

    return headVariant(fam);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The resolution
// ═══════════════════════════════════════════════════════════════════════════

CompilerResolution resolveCompiler(const CompilerRegistry& registry,
                                   const std::string& compilerPath,
                                   const std::string& version,
                                   const std::string& explicitProfileId,
                                   const std::string& defaultProfileId,
                                   const CompilerProfile& genericProfile) {
    CompilerResolution res;
    res.version = version;

    const std::string base = compilerBaseName(compilerPath);

    // ── 1: the explicit user mapping ───────────────────────────────
    //
    // The user's explicit mapping wins over everything, because the
    // user knows something the name does not tell: that "cc" on their
    // system is a wrapper around a completely different compiler.
    if (!explicitProfileId.empty()) {
        const CompilerFamily* fam = registry.findFamily(explicitProfileId);
        if (fam) {
            res.familyId = fam->id;
            res.profile = &fam->profile;
            res.variantId = headVariant(*fam);
            res.matchedBy = "explicit";
            res.rules = assembleRules(registry, *fam, res.variantId);
            return res;
        }

        // The explicit mapping names something that is not a family.
        // The honest answer is the generic table, not a silent guess.
        res.familyId = genericProfile.id;
        res.profile = &genericProfile;
        res.variantId = genericProfile.id;
        res.matchedBy = "generic";
        res.rules = genericProfile.rules;
        return res;
    }

    // ── 2: a user redirect ──────────────────────────────────────────
    //
    // A redirect says "this executable name actually means that
    // compiler". It is more specific than a glob, so it is tried
    // first.
    //
    // The target of a redirect may be a **family** ("gnu") or a
    // **variant** ("g++"). Both are meaningful:
    //
    //   replace/my-cc/gnu    "my-cc" is a gcc-family compiler
    //   replace/my-cc/g++     "my-cc" is specifically the g++ driver
    //
    // The second is the more precise statement, and it is the one that
    // carries the C-vs-C++ distinction. So the variant is looked up
    // first, and the family is the fallback.
    for (const CompilerMatch& m : registry.matches()) {
        if (!m.isRedirect) continue;
        std::string from, to;
        if (!CompilerMatch::parseRedirect(m.pattern, &from, &to)) continue;
        if (!iequals(from, base)) continue;

        const CompilerFamily* fam = nullptr;
        std::string variantId;

        // The variant form: the target names a variant of some family.
        for (const CompilerFamily& f : registry.families()) {
            if (!f.findVariant(to)) continue;
            fam = &f;
            variantId = to;
            break;
        }

        // The family form: the target names a family.
        if (!fam) {
            fam = registry.findFamily(to);
        }

        if (!fam) continue;

        res.familyId = fam->id;
        res.profile = &fam->profile;
        res.variantId = variantId.empty() ? headVariant(*fam) : variantId;
        res.matchedBy = "redirect";
        res.matchedPattern = m.pattern;
        res.rules = assembleRules(registry, *fam, res.variantId);
        return res;
    }

    // ── 3: an exact name ────────────────────────────────────────────
    //
    // Two things can match the executable name exactly:
    //
    //   a. the **family id** itself ("gnu" -- a user who names the
    //      family after the compiler);
    //   b. one of the family's **executable names** ("gcc", "g++",
    //      "cl", "clang++" -- how the compiler is actually invoked).
    //
    // (b) is the important one, and it is what makes the registry
    // usable at all: the family id is an abstract name ("gnu"), while
    // the executable names are the concrete ones that appear in a build
    // log. Without (b), "gcc" would resolve to nothing.
    //
    // The **variant** is chosen from the name when the family's
    // variants are named after executables: "g++" selects the g++
    // variant of the gnu family, because g++ compiles C++ by default
    // and gcc does not.
    for (const CompilerFamily& fam : registry.families()) {
        // (a) the family id
        if (iequals(fam.id, base)) {
            res.familyId = fam.id;
            res.profile = &fam.profile;
            res.variantId = headVariant(fam);
            res.matchedBy = "exact";
            res.rules = assembleRules(registry, fam, res.variantId);
            return res;
        }

        // (b) an executable name of the family
        for (const std::string& n : fam.profile.exactNames) {
            if (!iequals(n, base)) continue;

            res.familyId = fam.id;
            res.profile = &fam.profile;
            res.variantId = variantForName(fam, base);
            res.matchedBy = "exact";
            res.rules = assembleRules(registry, fam, res.variantId);
            return res;
        }

        // (c) a prefix rule of the family ("arm-none-eabi-").
        //
        // The prefix rules come before the glob match because they are
        // the more specific statement: "arm-none-eabi-gcc" must not be
        // matched as the plain "gcc".
        for (const std::string& prefix : fam.profile.prefixRules) {
            if (prefix.empty()) continue;
            if (base.size() < prefix.size()) continue;
            if (!iequals(base.substr(0, prefix.size()), prefix)) continue;

            res.familyId = fam.id;
            res.profile = &fam.profile;
            res.variantId = variantForName(fam, base);
            res.matchedBy = "exact";
            res.matchedPattern = prefix;
            res.rules = assembleRules(registry, fam, res.variantId);
            return res;
        }
    }

    // ── 4: a glob match, longest pattern wins ───────────────────────
    //
    // "arm-none-eabi-gcc" matches both "arm-*" and "*-gcc". The
    // longest pattern wins, because a longer pattern is a more
    // specific statement.
    {
        const CompilerMatch* best = nullptr;
        size_t bestLen = 0;
        for (const CompilerMatch& m : registry.matches()) {
            if (m.isRedirect) continue;
            if (!globMatch(m.pattern, base)) continue;
            if (m.pattern.size() > bestLen) {
                best = &m;
                bestLen = m.pattern.size();
            }
        }
        if (best) {
            // The match names a **family** ("gnu") or a **variant**
            // ("g++"). The family is the more common case -- the
            // built-in globs are "*-gcc" -> the gnu family.
            const CompilerFamily* fam = registry.findFamily(best->variantId);
            std::string variantId;

            if (!fam) {
                // It may name a variant of some family instead.
                for (const CompilerFamily& f : registry.families()) {
                    if (!f.findVariant(best->variantId)) continue;
                    fam = &f;
                    variantId = best->variantId;
                    break;
                }
            }

            if (!fam) {
                // The match names a family that does not exist.
                // Fall through to the generic table rather than
                // guessing.
                res.familyId = genericProfile.id;
                res.profile = &genericProfile;
                res.variantId = genericProfile.id;
                res.matchedBy = "generic";
                res.rules = genericProfile.rules;
                return res;
            }

            res.familyId = fam->id;
            res.profile = &fam->profile;

            // The variant comes from the executable name when the
            // pattern only named a family: "arm-none-eabi-gcc" is the
            // gcc variant of the gnu family, not its head.
            if (variantId.empty()) {
                variantId = variantForName(*fam, base);
            }
            res.variantId = variantId;
            res.matchedBy = "glob";
            res.matchedPattern = best->pattern;
            res.rules = assembleRules(registry, *fam, res.variantId);
            return res;
        }
    }

    // ── 5: the version-suffixed names ─────────────────────────────
    //
    // "gcc-11", "clang++-17", "g++-4.8". The rule is "the base name
    // minus the version suffix is a known name", which is exactly how
    // a distribution ships a specific compiler version.
    {
        static const char* kSuffixPrefixes[] = {
            "gcc-", "g++-", "cc-", "c++-", "clang-", "clang++-", "cl-",
            "icc-", "icpc-", "arm-none-eabi-gcc-"};

        for (const CompilerFamily& fam : registry.families()) {
            for (const char* vp : kSuffixPrefixes) {
                std::string prefix(vp);
                if (base.size() <= prefix.size()) continue;
                if (base.compare(0, prefix.size(), prefix) != 0) continue;

                std::string rest = base.substr(prefix.size());
                bool allDigits = !rest.empty();
                for (char c : rest) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        allDigits = false;
                        break;
                    }
                }
                if (!allDigits) continue;

                std::string stripped = prefix;
                res.familyId = fam.id;
                res.profile = &fam.profile;
                res.variantId = headVariant(fam);
                res.matchedBy = "exact";
                res.matchedPattern = stripped;

                res.rules = assembleRules(registry, fam, res.variantId);
                return res;
            }
        }
    }

    // ── 6: the default profile ──────────────────────────────────────
    //
    // The user said "treat every compiler I do not recognise as
    // this". It comes after the name matching, so it never overrides a
    // profile that actually describes the compiler.
    if (!defaultProfileId.empty()) {
        for (const CompilerFamily& fam : registry.families()) {
            if (!iequals(fam.id, defaultProfileId)) continue;
            res.familyId = fam.id;
            res.profile = &fam.profile;
            res.variantId = headVariant(fam);
            res.matchedBy = "default";
            res.rules = assembleRules(registry, fam, res.variantId);
            return res;
        }
    }

    // ── 7: the generic fallback ─────────────────────────────────────
    res.familyId = genericProfile.id;
    res.profile = &genericProfile;
    res.variantId = genericProfile.id;
    res.matchedBy = "generic";
    res.rules = genericProfile.rules;
    return res;
}

}  // namespace compdb
}  // namespace ccap
