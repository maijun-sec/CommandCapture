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

// Read a string array out of an object (the field may be absent)
std::vector<std::string> getArray(const JsonValue* obj, const char* key) {
    if (!obj) return {};
    const JsonValue* v = obj->get(key);
    if (!v) return {};
    return v->asStringArray();
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

    // ── output section ─────────────────────────────────────────────────
    if (const JsonValue* o = root->get("output")) {
        if (const JsonValue* p = o->get("path")) {
            out->outputPath = p->asString(out->outputPath);
        }
        if (const JsonValue* f = o->get("format")) {
            out->outputFormat = f->asString(out->outputFormat);
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

}  // namespace compdb
}  // namespace ccap
