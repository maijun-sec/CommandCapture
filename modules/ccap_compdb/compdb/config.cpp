// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - configuration file parsing
//
// The configuration file is JSON. See section 5.3 of
// docs/en/DESIGN-MODULES-2026-10-27.md for the structure.
//
// Design principles (following the established configuration thinking):
//   1. **The configuration is optional** -- without a config file the built-in
//      defaults are used and everything works as usual.
//   2. **The configuration only adds** -- the user's rules are **appended**
//      to the built-in rules, not substituted for them.
//      That way configuring an arm-none-eabi-gcc does not lose gcc.
//   3. **The configuration does not affect recognition, only filtering and
//      output** -- recognition relies on the compiler name (a filesystem
//      fact), while the configuration tells the system
//      "which ones I want" and "where the system headers are".

#include "ccap_compdb.h"

#include "core/json_parser.h"
#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// Forward declarations.
//
// parseFamily() needs parseProfile() (a family embeds a profile), and
// parseProfile() is defined further down because it is the older of
// the two.
bool parseProfile(const JsonValue* v, CompilerProfile* out, std::string* err);

// Read a string array out of an object (the field may be absent)
std::vector<std::string> getArray(const JsonValue* obj, const char* key) {
    if (!obj) return {};
    const JsonValue* v = obj->get(key);
    if (!v) return {};
    return v->asStringArray();
}

// ── The shape flag names ─────────────────────────────────────────────────
//
// The configuration uses the same vocabulary as the built-in table, so
// a user can read the built-in rules and write their own in the same
// language.
//
// The names are the industry-standard ones (the ones the compiler
// configuration tools use), because that vocabulary already exists and
// a reader who knows it does not have to learn a new one.
bool shapeFlagFromName(const std::string& name, unsigned int* out) {
    struct Flag {
        const char* name;
        unsigned int bit;
    };
    static const Flag kFlags[] = {
        {"dash",         kShapeDash},
        {"dash_dash",    kShapeDashDash},
        {"attached",     kShapeAttached},
        {"unattached",   kShapeUnattached},
        {"equal",        kShapeEqual},
        {"optional",     kShapeOptionalValue},
        {"required",     kShapeRequiredValue},
        {"path",         kShapePath},
        {"skip",         kShapeSkip},
        {"prefix",       kShapePrefix},
        {"list",         kShapeList},
        {"takes_next",   kShapeTakesNext},
        // ── The extended vocabulary ──────────────────────────────────
        //
        // These are the established attributes that the original twelve
        // did not cover, and that a real toolchain integration needs.
        {"alternate_table",   kShapeAlternateTable},
        {"abbrev_match",      kShapeAbbrevMatch},
        {"hyphen_is_underscore", kShapeHyphenIsUnderscore},
        {"colon",             kShapeColon},
        {"parens",            kShapeParens},
        {"strip_quotes",      kShapeStripQuotes},
        {"split",             kShapeSplit},
        {"unsupported",       kShapeUnsupported},
    };
    for (const auto& f : kFlags) {
        if (name == f.name) {
            *out |= f.bit;
            return true;
        }
    }
    return false;
}

// Parse the shape flags.
//
// The field is an array of names: ["dash", "attached", "unattached"].
bool parseShape(const JsonValue* v, unsigned int* out, std::string* err) {
    if (!v) return true;
    if (!v->isArray()) {
        if (err) *err = "\"shape\" must be an array of flag names";
        return false;
    }
    for (const auto& item : v->array) {
        if (!item || !item->isString()) {
            if (err) *err = "\"shape\" entries must be strings";
            return false;
        }
        if (!shapeFlagFromName(item->asString(), out)) {
            if (err) {
                *err = "unknown shape flag: " + item->asString();
            }
            return false;
        }
    }
    return true;
}

// Parse the option class.
//
// The field is the class name: "include", "macro", "language-std" ...
bool parseClass(const std::string& name, OptionClass* out) {
    struct Entry {
        const char* name;
        OptionClass cls;
    };
    static const Entry kClasses[] = {
        {"unknown",         OptionClass::Unknown},
        {"include",         OptionClass::Include},
        {"system-include",  OptionClass::SystemInclude},
        {"framework",       OptionClass::Framework},
        {"macro",           OptionClass::Macro},
        {"macro-undef",     OptionClass::MacroUndef},
        {"pre-include",     OptionClass::PreInclude},
        {"language-std",    OptionClass::LanguageStd},
        {"language",        OptionClass::Language},
        {"target",          OptionClass::Target},
        {"target-cpu",      OptionClass::TargetCpu},
        {"sysroot",         OptionClass::Sysroot},
        {"debug-info",      OptionClass::DebugInfo},
        {"optimization",    OptionClass::Optimization},
        {"output",          OptionClass::Output},
        {"compile-only",    OptionClass::CompileOnly},
        {"preprocess-only", OptionClass::PreprocessOnly},
        {"warning",         OptionClass::Warning},
        {"dependency",      OptionClass::Dependency},
        {"linker",          OptionClass::Linker},
        {"codegen",         OptionClass::Codegen},
        {"static-analysis", OptionClass::StaticAnalysis},
        {"coverage",        OptionClass::Coverage},
        {"sanitizer",       OptionClass::Sanitizer},
        {"response-file",   OptionClass::ResponseFile},
        {"verbosity",       OptionClass::Verbosity},
        {"pass",            OptionClass::Pass},
    };
    for (const auto& e : kClasses) {
        if (name == e.name) {
            *out = e.cls;
            return true;
        }
    }
    return false;
}

// Parse one switch rule.
bool parseRule(const JsonValue* v, SwitchRule* out, std::string* err) {
    if (!v || !v->isObject()) {
        if (err) *err = "a switch rule must be an object";
        return false;
    }

    const JsonValue* name = v->get("name");
    if (!name || !name->isString() || name->asString().empty()) {
        if (err) *err = "a switch rule needs a non-empty \"name\"";
        return false;
    }
    out->name = name->asString();

    if (!parseShape(v->get("shape"), &out->shape, err)) return false;

    const JsonValue* cls = v->get("class");
    if (cls && cls->isString()) {
        if (!parseClass(cls->asString(), &out->cls)) {
            if (err) *err = "unknown option class: " + cls->asString();
            return false;
        }
    }

    const JsonValue* form = v->get("clang");
    if (form && form->isString()) out->clangForm = form->asString();

    const JsonValue* note = v->get("note");
    if (note && note->isString()) out->note = note->asString();

    const JsonValue* drop = v->get("drop");
    if (drop && drop->isString()) out->dropAt = drop->asString();

    return true;
}

// Parse one compiler family.
//
// A family is a profile plus the things a profile cannot express: the
// variant chain, the rule groups, and the version probe.
//
// The profile part is parsed by parseProfile(), so the two share the
// same field names ("names", "driver", "rules" ...). The family-only
// fields are:
//
//   "variants"       the variant chain
//   "groups"         the rule groups (rules scoped to a subset)
//   "version_probe"  how to detect the version
bool parseFamily(const JsonValue* v, CompilerFamily* out, std::string* err) {
    if (!v || !v->isObject()) {
        if (err) *err = "a compiler family must be an object";
        return false;
    }

    const JsonValue* id = v->get("id");
    if (!id || !id->isString() || id->asString().empty()) {
        if (err) *err = "a compiler family needs a non-empty \"id\"";
        return false;
    }
    out->id = id->asString();

    // The profile part.
    if (!parseProfile(v, &out->profile, err)) {
        if (err) *err = "family '" + out->id + "': " + *err;
        return false;
    }
    // The profile's id follows the family id unless it says otherwise.
    if (out->profile.id != out->id && v->get("id")) {
        // parseProfile set the profile id from the same "id" field, so
        // they agree by construction. Nothing to reconcile.
    }
    out->kind = out->profile.kind;

    // ── The variants ────────────────────────────────────────────────
    //
    // The chain: "gcc" -> "g++" -> "g++cc". The "next" field links one
    // variant to the next, and "head" marks the root of the chain.
    if (const JsonValue* variants = v->get("variants")) {
        if (!variants->isArray()) {
            if (err) *err = "family '" + out->id + "': \"variants\" must be an array";
            return false;
        }
        for (const auto& item : variants->array) {
            if (!item || !item->isObject()) continue;
            CompilerVariant var;
            const JsonValue* vid = item->get("id");
            if (!vid || !vid->isString() || vid->asString().empty()) {
                if (err) *err = "family '" + out->id + "': a variant needs an \"id\"";
                return false;
            }
            var.id = vid->asString();

            if (const JsonValue* lang = item->get("language")) {
                if (lang->isString()) var.language = lang->asString();
            }
            if (const JsonValue* next = item->get("next")) {
                if (next->isString()) var.nextVariantId = next->asString();
            }
            if (const JsonValue* from = item->get("derived_from")) {
                if (from->isString()) var.derivedFrom = from->asString();
            }
            if (const JsonValue* head = item->get("head")) {
                var.familyHead = head->asBool(false);
            }
            if (const JsonValue* rules = item->get("rules")) {
                if (!rules->isArray()) {
                    if (err) {
                        *err = "family '" + out->id + "': variant '" + var.id +
                               "': \"rules\" must be an array";
                    }
                    return false;
                }
                for (const auto& r : rules->array) {
                    SwitchRule rule;
                    if (!parseRule(r.get(), &rule, err)) {
                        if (err) {
                            *err = "family '" + out->id + "': variant '" + var.id +
                                   "': " + *err;
                        }
                        return false;
                    }
                    var.rules.push_back(rule);
                }
            }
            out->variants.push_back(var);
        }
    }

    // ── The rule groups ─────────────────────────────────────────────
    //
    // A group applies to a **subset** of the variants: "this -v means
    // 'dump the include search path' for gcc, but something else for
    // hightec".
    if (const JsonValue* groups = v->get("groups")) {
        if (!groups->isArray()) {
            if (err) *err = "family '" + out->id + "': \"groups\" must be an array";
            return false;
        }
        for (const auto& item : groups->array) {
            if (!item || !item->isObject()) continue;
            RuleGroup group;
            group.appliesTo = getArray(item.get(), "applies_to");
            const JsonValue* rules = item->get("rules");
            if (rules && rules->isArray()) {
                for (const auto& r : rules->array) {
                    SwitchRule rule;
                    if (!parseRule(r.get(), &rule, err)) {
                        if (err) {
                            *err = "family '" + out->id + "': group: " + *err;
                        }
                        return false;
                    }
                    group.rules.push_back(rule);
                }
            }
            out->groups.push_back(group);
        }
    }

    // ── The version probe ───────────────────────────────────────────
    //
    // The probe is a switch plus an **ordered** list of patterns. The
    // order matters: a greedy pattern would swallow the version of a
    // different compiler family.
    if (const JsonValue* probe = v->get("version_probe")) {
        if (!probe->isObject()) {
            if (err) {
                *err = "family '" + out->id + "': \"version_probe\" must be an object";
            }
            return false;
        }
        if (const JsonValue* sw = probe->get("switch")) {
            if (sw->isString()) out->versionProbe.switchText = sw->asString();
        }
        out->versionProbe.patterns = getArray(probe, "patterns");
    }

    return true;
}

// Parse one rule override.
//
// The scope is (profile, variant). The actions are:
//
//   "disable"    remove the rule
//   "clang"      replace the clang spelling
//   "class"      replace what the option means
//   "keep_at"    replace the retention levels
//   "shape"      replace the shape flags
bool parseRuleOverride(const JsonValue* v, RuleOverride* out,
                        std::string* err) {
    if (!v || !v->isObject()) {
        if (err) *err = "a rule override must be an object";
        return false;
    }

    const JsonValue* name = v->get("name");
    if (!name || !name->isString() || name->asString().empty()) {
        if (err) *err = "a rule override needs a non-empty \"name\"";
        return false;
    }
    out->name = name->asString();

    if (const JsonValue* d = v->get("disable")) {
        out->disable = d->asBool(false);
    }

    if (const JsonValue* form = v->get("clang")) {
        if (form->isString()) {
            out->hasClangForm = true;
            out->clangForm = form->asString();
        }
    }

    if (const JsonValue* cls = v->get("class")) {
        if (cls->isString()) {
            if (!parseClass(cls->asString(), &out->cls)) {
                if (err) *err = "unknown option class: " + cls->asString();
                return false;
            }
            out->hasClass = true;
        }
    }

    if (const JsonValue* keep = v->get("keep_at")) {
        if (!keep->isArray()) {
            if (err) *err = "\"keep_at\" must be an array of level names";
            return false;
        }
        out->hasLevelOverride = true;
        for (const auto& item : keep->array) {
            if (!item || !item->isString()) continue;
            TranslateLevel level = TranslateLevel::Ast;
            if (!translateLevelFromName(item->asString(), &level)) {
                if (err) *err = "unknown retention level: " + item->asString();
                return false;
            }
            out->keepAt.push_back(level);
        }
    }

    if (const JsonValue* shape = v->get("shape")) {
        if (!parseShape(shape, &out->shape, err)) return false;
        out->hasShape = true;
    }

    return true;
}

// Parse one profile override.
//
// The scope is (profile, variant). An empty profile applies to every
// profile; an empty variant to every variant of it.
bool parseProfileOverride(const JsonValue* v, ProfileOverride* out,
                          std::string* err) {
    if (!v || !v->isObject()) {
        if (err) *err = "a profile override must be an object";
        return false;
    }

    if (const JsonValue* p = v->get("profile")) {
        if (p->isString()) out->profile = p->asString();
    }
    if (const JsonValue* var = v->get("variant")) {
        if (var->isString()) out->variant = var->asString();
    }

    out->disable = getArray(v, "disable");

    if (const JsonValue* overrides = v->get("overrides")) {
        if (!overrides->isArray()) {
            if (err) *err = "\"overrides\" must be an array";
            return false;
        }
        for (const auto& item : overrides->array) {
            RuleOverride ro;
            if (!parseRuleOverride(item.get(), &ro, err)) return false;
            out->overrides.push_back(ro);
        }
    }

    if (const JsonValue* add = v->get("add")) {
        if (!add->isArray()) {
            if (err) *err = "\"add\" must be an array";
            return false;
        }
        for (const auto& item : add->array) {
            SwitchRule rule;
            if (!parseRule(item.get(), &rule, err)) return false;
            out->add.push_back(rule);
        }
    }

    return true;
}

// Parse one compiler match.
//
// Two forms:
//
//   {"pattern": "*-gcc"}              a glob
//   {"pattern": "replace/a/b"}        a redirect
//
// A redirect may name a family ("gnu") or a variant ("g++") as its
// target; the resolver tries the variant first, because it is the more
// precise statement.
bool parseCompilerMatch(const JsonValue* v, CompilerMatch* out,
                         std::string* err) {
    if (!v || !v->isObject()) {
        if (err) *err = "a compiler match must be an object";
        return false;
    }

    const JsonValue* pattern = v->get("pattern");
    if (!pattern || !pattern->isString() || pattern->asString().empty()) {
        if (err) *err = "a compiler match needs a non-empty \"pattern\"";
        return false;
    }
    out->pattern = pattern->asString();

    if (const JsonValue* target = v->get("target")) {
        if (target->isString()) out->variantId = target->asString();
    }
    if (const JsonValue* variant = v->get("variant")) {
        if (variant->isString()) out->variantId = variant->asString();
    }

    // A "replace/" pattern is a redirect by construction.
    out->isRedirect = out->pattern.compare(0, 8, "replace/") == 0;

    if (const JsonValue* redirect = v->get("redirect")) {
        out->isRedirect = redirect->asBool(out->isRedirect);
    }

    return true;
}

// Parse one compiler profile.
bool parseProfile(const JsonValue* v, CompilerProfile* out, std::string* err) {
    if (!v || !v->isObject()) {
        if (err) *err = "a compiler profile must be an object";
        return false;
    }

    const JsonValue* id = v->get("id");
    if (!id || !id->isString() || id->asString().empty()) {
        if (err) *err = "a compiler profile needs a non-empty \"id\"";
        return false;
    }
    out->id = id->asString();

    const JsonValue* kind = v->get("kind");
    if (kind && kind->isString()) {
        if (!compilerKindFromName(kind->asString(), &out->kind)) {
            if (err) {
                *err = "unknown compiler kind: " + kind->asString();
            }
            return false;
        }
    }

    out->exactNames = getArray(v, "names");
    out->prefixRules = getArray(v, "prefixes");

    const JsonValue* driver = v->get("driver");
    if (driver && driver->isString()) out->driver = driver->asString();

    const JsonValue* cppDriver = v->get("cpp_driver");
    if (cppDriver && cppDriver->isString()) {
        out->cppDriver = cppDriver->asString();
    }

    out->includeEnvVars = getArray(v, "include_env");
    out->noEquivalent = getArray(v, "no_equivalent");

    const JsonValue* rules = v->get("rules");
    if (rules && rules->isArray()) {
        for (const auto& item : rules->array) {
            SwitchRule rule;
            if (!parseRule(item.get(), &rule, err)) {
                if (err) {
                    *err = "profile '" + out->id + "': " + *err;
                }
                return false;
            }
            out->rules.push_back(rule);
        }
    }

    return true;
}

}  // namespace

bool CompdbConfig::load(const std::string& text, CompdbConfig* out,
                        std::string* err) {
    if (!out) return false;

    std::string perr;
    JsonPtr root = jsonParse(text, &perr);
    if (!root) {
        if (err) *err = "config parse error: " + perr;
        return false;
    }
    if (!root->isObject()) {
        if (err) *err = "config root must be an object";
        return false;
    }

    // ── capture section ────────────────────────────────────────────────
    if (const JsonValue* capture = root->get("capture")) {
        // Compiler list
        if (const JsonValue* comps = capture->get("compilers")) {
            if (comps->isArray()) {
                for (const auto& c : comps->array) {
                    if (!c || !c->isObject()) continue;
                    CompilerEntry e;
                    e.kind = c->get("kind") ? c->get("kind")->asString() : "";
                    e.id = c->get("id") ? c->get("id")->asString() : e.kind;
                    e.paths = getArray(c.get(), "paths");
                    e.names = getArray(c.get(), "names");
                    e.prefixes = getArray(c.get(), "prefixes");
                    if (const JsonValue* en = c->get("enabled")) {
                        e.enabled = en->asBool(true);
                    }
                    out->compilers.push_back(e);
                }
            }
        }

        // Filter rules
        if (const JsonValue* ex = capture->get("exclude")) {
            for (const auto& n : ex->asStringArray()) {
                out->filter.excludeNames.push_back(n);
            }
        }
        if (const JsonValue* ex = capture->get("exclude_prefixes")) {
            for (const auto& n : ex->asStringArray()) {
                out->filter.excludePrefixes.push_back(n);
            }
        }
        if (const JsonValue* inc = capture->get("include")) {
            for (const auto& n : inc->asStringArray()) {
                out->filter.includeNames.push_back(n);
            }
        }
    }

    // ── system_includes section ────────────────────────────────────────
    if (const JsonValue* si = root->get("system_includes")) {
        if (si->isObject()) {
            for (const auto& kv : si->object) {
                out->systemIncludes[kv.first] = kv.second->asStringArray();
            }
        }
    }

    // ── profiles section ──────────────────────────────────────────────
    //
    // The compiler profiles: the switch tables.
    if (const JsonValue* profiles = root->get("profiles")) {
        if (profiles->isArray()) {
            for (const auto& p : profiles->array) {
                CompilerProfile profile;
                if (!parseProfile(p.get(), &profile, err)) return false;
                out->profiles.push_back(profile);
            }
        } else if (profiles->isObject()) {
            // The object form: { "id": { ... } }
            //
            // It is convenient when the id is the key, but it is
            // ambiguous when a profile wants to override the id, so
            // both forms are accepted.
            for (const auto& kv : profiles->object) {
                CompilerProfile profile;
                if (!parseProfile(kv.second.get(), &profile, err)) return false;
                if (profile.id.empty()) profile.id = kv.first;
                out->profiles.push_back(profile);
            }
        }
    }

    // ── compiler_profiles section ─────────────────────────────────────
    //
    // The mapping of an unknown compiler to a profile.
    if (const JsonValue* cp = root->get("compiler_profiles")) {
        if (cp->isObject()) {
            for (const auto& kv : cp->object) {
                if (!kv.second || !kv.second->isString()) continue;
                out->compilerProfiles[kv.first] = kv.second->asString();
            }
        }
    }
    if (const JsonValue* dp = root->get("default_profile")) {
        if (dp->isString()) out->defaultProfile = dp->asString();
    }

    // ── compiler_families section ─────────────────────────────────────
    //
    // The family-aware layer: the variant chain, the rule groups and
    // the version probe.
    //
    // The object form is {"id": {...}} and the array form is
    // [{...}]; both are accepted, for the same reason as "profiles".
    if (const JsonValue* families = root->get("compiler_families")) {
        if (families->isArray()) {
            for (const auto& f : families->array) {
                CompilerFamily family;
                if (!parseFamily(f.get(), &family, err)) return false;
                out->families.push_back(family);
            }
        } else if (families->isObject()) {
            for (const auto& kv : families->object) {
                CompilerFamily family;
                if (!parseFamily(kv.second.get(), &family, err)) return false;
                if (family.id.empty()) family.id = kv.first;
                out->families.push_back(family);
            }
        }
    }

    // ── compiler_matches section ──────────────────────────────────────
    //
    // The executable-name patterns: the globs and the redirects.
    if (const JsonValue* matches = root->get("compiler_matches")) {
        if (matches->isArray()) {
            for (const auto& m : matches->array) {
                CompilerMatch match;
                if (!parseCompilerMatch(m.get(), &match, err)) return false;
                out->compilerMatches.push_back(match);
            }
        } else if (matches->isObject()) {
            // The object form: {"pattern": "target", ...}
            for (const auto& kv : matches->object) {
                CompilerMatch match;
                match.pattern = kv.first;
                if (kv.second && kv.second->isString()) {
                    match.variantId = kv.second->asString();
                }
                match.isRedirect = match.pattern.compare(0, 8, "replace/") == 0;
                out->compilerMatches.push_back(match);
            }
        }
    }

    // ── profile_overrides section ─────────────────────────────────────
    //
    // The user's rule changes, scoped to a (profile, variant) pair.
    if (const JsonValue* overrides = root->get("profile_overrides")) {
        if (!overrides->isArray()) {
            if (err) *err = "\"profile_overrides\" must be an array";
            return false;
        }
        for (const auto& item : overrides->array) {
            ProfileOverride ov;
            if (!parseProfileOverride(item.get(), &ov, err)) return false;
            out->profileOverrides.push_back(ov);
        }
    }

    // ── unknown_policy section ────────────────────────────────────────
    if (const JsonValue* up = root->get("unknown_policy")) {
        if (up->isString()) {
            UnknownPolicy policy = UnknownPolicy::Report;
            if (!unknownPolicyFromName(up->asString(), &policy)) {
                if (err) {
                    *err = "unknown unknown-option policy: " + up->asString();
                }
                return false;
            }
            out->unknownPolicy = up->asString();
        }
    }

    // ── compiler_version section ──────────────────────────────────────
    if (const JsonValue* cv = root->get("compiler_version")) {
        if (cv->isString()) out->compilerVersion = cv->asString();
    }

    // ── output section ─────────────────────────────────────────────────
    if (const JsonValue* o = root->get("output")) {
        if (const JsonValue* p = o->get("path")) {
            out->outputPath = p->asString(out->outputPath);
        }
        if (const JsonValue* f = o->get("format")) {
            out->outputFormat = f->asString(out->outputFormat);
        }
        if (const JsonValue* l = o->get("level")) {
            out->outputLevel = l->asString(out->outputLevel);
        }
    }

    return true;
}

bool CompdbConfig::loadFile(const std::string& path, CompdbConfig* out,
                             std::string* err) {
    std::string content;
    if (!readFile(path, &content)) {
        if (err) *err = "cannot read config file: " + path;
        return false;
    }
    return load(content, out, err);
}

// ═══════════════════════════════════════════════════════════════════════════
// Building the mapper
// ═══════════════════════════════════════════════════════════════════════════

OptionMapper buildMapper(const CompdbConfig& config) {
    OptionMapper mapper = OptionMapper::builtin();

    // ── The user families ────────────────────────────────────────────
    //
    // A user family whose id matches a built-in one **replaces** it.
    //
    // The reason is the same as for the profiles: the intent of
    // "override the gcc table" is "my gcc does not behave like the
    // stock one", and a merge would leave the stock rules underneath,
    // where the longer name would still win.
    //
    // The families are applied **before** the profiles, so that a
    // profile override can still replace the switch table of a family
    // the user has just defined.
    for (const CompilerFamily& family : config.families) {
        mapper.registry().upsertFamily(family);
    }

    // ── The user profiles ────────────────────────────────────────────
    //
    // A user profile whose id matches a built-in one **replaces** it.
    //
    // Why replace rather than merge: the intent of "override the gcc
    // table" is to say "my gcc does not behave like the stock one".
    // A merge would leave the stock rules underneath, and the longer
    // name would win -- which is the opposite of what the user asked
    // for.
    //
    // The profiles that are *not* overridden are added, so configuring
    // an unknown cross compiler never costs you the built-in gcc
    // behaviour.
    for (const auto& profile : config.profiles) {
        mapper.upsertProfile(profile);
    }

    // ── The explicit unknown-compiler mapping ────────────────────────
    for (const auto& kv : config.compilerProfiles) {
        mapper.setCompilerProfile(kv.first, kv.second);
    }
    if (!config.defaultProfile.empty()) {
        mapper.setDefaultProfile(config.defaultProfile);
    }

    // ── The executable-name match patterns ───────────────────────────
    //
    // The user's globs and redirects. They are added after the
    // built-in ones, so the user's patterns are tried **after** the
    // built-in ones -- which is the wrong order for a user who wants
    // to *override* a built-in pattern.
    //
    // So they are added in a way that puts them first: the registry
    // tries the matches in registration order, and the longest glob
    // wins among the globs. A user's redirect is always more specific
    // than a built-in glob, so it wins the redirect stage outright.
    //
    // For the globs, the user's pattern competes on length with the
    // built-in ones, which is the right rule: the longer pattern is
    // the more specific statement.
    for (const CompilerMatch& match : config.compilerMatches) {
        mapper.registry().addMatch(match);
    }

    // ── The rule overrides ───────────────────────────────────────────
    for (const ProfileOverride& ov : config.profileOverrides) {
        mapper.registry().addOverride(ov);
    }

    // ── The unknown-option policy ────────────────────────────────────
    if (!config.unknownPolicy.empty()) {
        UnknownPolicy policy = UnknownPolicy::Report;
        if (unknownPolicyFromName(config.unknownPolicy, &policy)) {
            mapper.setUnknownPolicy(policy);
        }
    }

    // ── The compiler version ─────────────────────────────────────────
    //
    // The version-scoped rules do not fire when the version is
    // unknown: a rule whose precondition could not be verified must
    // not be applied.
    if (!config.compilerVersion.empty()) {
        mapper.setCompilerVersion(config.compilerVersion);
    }

    return mapper;
}

}  // namespace compdb
}  // namespace ccap
