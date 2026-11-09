# Compiler Configuration Gap Analysis — 2026-11-03

> Scope: what a production-grade compiler-command mapping engine needs,
> measured against a reference toolchain installation and against the state
> of `ccap_compdb` as of this document.

This document is the **design contract** for the upgrade. It states what is
missing, why each gap matters, and what the target model looks like. The
Chinese counterpart is `docs/zh/COMPILER-CONFIG-GAP-ANALYSIS-2026-11-03.md`
and has the same section structure.

---

## 1. What a compiler-configuration system actually is

A mature static-analysis toolchain does not maintain "a switch table per
compiler". It maintains a **compiler-configuration system** with five layers:

| Layer | Artefact | What it answers |
|---|---|---|
| 1. Compiler registry | `<family>_config.xml`, `<family>_compilers.cpp` | *Which executables are this family? Which variant/version?* |
| 2. Switch table | `<family>_switches.dat` | *How is each option spelled, and what does it mean?* |
| 3. Environment / driver model | `config.xml` `<options>` | *Where do include paths come from? What is the `-c` switch?* |
| 4. Feature probing | `config_gen_info`, `config_probe` | *What does this compiler actually support?* |
| 5. Custom translator | `<family>_compilers.cpp` | *The 10% the declarative table cannot express.* |

The scale of a reference installation of this kind:

- **86 compiler template directories**, 610 files (203 `.dat`, 149 `.xml`,
  47 `.cpp`, 74 `.json`, 134 `.h`).
- **416 elements** in the formal configuration schema.
- **~50 switch attributes** in the attribute definition file.
- The worst-offending vendor alone: 52 KB config + 45 KB switches + 44 KB
  custom C++. Another: 40 KB custom C++. Another: 37 KB custom C++ with
  **eight** per-version translator classes.

That is where "dozens of engineers" goes. It is not the switch table. It is
the per-vendor and per-version drift.

---

## 2. The mechanisms we did not have

### 2.1 Compiler identity is a graph, not a name list

A compiler is modelled as a **family chain**:

```xml
<compiler>
  <comp_name>gcc</comp_name>
  <comp_translator>gcc</comp_translator>
  <comp_lang>C</comp_lang>
  <comp_family_head>true</comp_family_head>
  <comp_next_type>g++</comp_next_type>      <!-- C -> C++ -->
</compiler>
```

`comp_next_type` links `gcc -> g++ -> g++cc`; `comp_family_head` marks the
root. A variant carries its own `comp_translator`, so the C and C++ drivers
of one family can have **different switch tables**.

**Gap G1** — our `CompilerProfile` has a flat `kind` + `exactNames` +
`prefixRules`. There is no family graph, no C/C++ driver split, no
`comp_translator` indirection.

### 2.2 Name matching uses a glob and a redirect, not prefixes

```xml
<extra_comp>
  <comp_name>gcc-*</comp_name>     <!-- glob, both directions -->
  <comp_type>gcc</comp_type>
</extra_comp>
<extra_comp>
  <comp_name>replace/gcc/g++</comp_name>  <!-- redirect: gcc invoked as g++ -->
  <comp_type>g++</comp_type>
</extra_comp>
```

The `replace/` prefix means "this executable name actually means that other
compiler". We have no redirect concept.

**Gap G2** — our `exactNames` + `prefixRules` cannot express
`*-gcc` (suffix match) or `replace/gcc/g++`.

### 2.3 Version is detected by probing, then filtered by ordered regexes

```xml
<ident_switch>--version</ident_switch>
<ident_match_regex>g(cc|\+\+)(.*) [0-9]+\.[0-9]+\.[0-9]+.*</ident_match_regex>

<!-- Order Version Regex least likely to incorrectly match first -->
<version_regex>replace/.*g[c+][c+] version 2\.9.* \(2.96.*\).*/2.9.6</version_regex>
<version_regex>replace/.*g[c+][c+][^\s]+ [^\s]+ (\d+\.\d+(\.\d+)?) \(.*\).*/$1</version_regex>
...
```

The comment *is* the design: an **ordered** list of regexes, most specific
first, each with a `replace/<regex>/<replacement>` form.

**Gap G3** — we have no version detection and no version-scoped rules.

### 2.4 Version drives a *different translator*

```cpp
void ti_cl2000_pre_translate(...);  // CL2000Translator
void ti_cl470_pre_translate(...);   // CL470Translator
void ti_cl500_pre_translate(...);   // CL500Translator
void ti_cl55_pre_translate(...);    // CL55Translator
void ti_cl6x_pre_translate(...);    // CL6XTranslator
void ti_armcl_pre_translate(...);   // ARMCLTranslator
void ti_cl7x_pre_translate(...);    // CL7XTranslator
void ti_cl430_pre_translate(...);   // CL430Translator
```

Eight versions of one compiler vendor's toolchain, eight translator classes.
The declarative `.dat` table covers the common options; the version-specific
residue is code.

**Gap G4** — our model has no place to hang "this option means X on version
4.7 and later but Y before".

### 2.5 Runtime detection can redirect the compiler type

```xml
<comp_is_redirect>true</comp_is_redirect>
<redirect_regex>Tool: armclang.*</redirect_regex>
<comp_type_if_hit>armcc:armclang</comp_type_if_hit>
<comp_type_if_miss>armcc:armcc</comp_type_if_miss>
```

`armcc` is a **redirector**: running it may actually invoke `armclang`. The
type is decided by running the compiler and matching the output.

**Gap G5** — no detection/probing stage at all in our pipeline.

### 2.6 Feature probing: what the compiler *actually* supports

```xml
<config_gen_info>
  <test_exceptions>true</test_exceptions>
  <test_stdarg>false</test_stdarg>
  <test_target_platform>true</test_target_platform>
  <test_sysroot>true</test_sysroot>
  <test_float80_128>true</test_float80_128>
  <macro_candidate>__GNUC__</macro_candidate>
  <excluded_macro_candidate>__cpp_modules</excluded_macro_candidate>
</config_gen_info>

<config_probe>
  <switch>-print-search-dirs</switch>
  <format>gcc_print_search_dirs</format>
  <output_stream>1</output_stream>
</config_probe>
```

The toolchain *runs* the compiler to learn: which macros it predefines,
whether it supports exceptions, where its system includes are, what its
target is.

**Gap G6** — our model is purely static. We assume the same table applies
to every gcc on earth.

### 2.7 Option groups scoped to a set of compilers

```xml
<option_group>
  <applies_to>gcc,g++,g++cc,hightec:tricore,hightec_cxx:tricore,hightec_cxxcc:tricore,microchip:xc16</applies_to>
  <dryrun_switch>-v</dryrun_switch>
</option_group>
```

One switch table, but a *group* of options applies to only some compilers.
The `:` in `hightec:tricore` separates family from variant.

**Gap G7** — our rules are all-or-nothing per profile.

### 2.8 The switch attribute vocabulary is ~50 flags, not 12

The established attribute set distinguishes, among others:

- **syntax**: `slash`, `dash`, `dash_dash`, `plus`, `noprefix`
- **value shape**: `attached`, `unattached`, `equal`, `colon`, `optional`,
  `parens`, `split`, `strip_quotes`
- **matching**: `case_insensitive`, `hyphen_is_underscore`, `abbrev_match`
- **action**: `copy`, `map`, `keep`, `remove`, `skip_arg`, `unsupported`,
  `required`, `custom`
- **composition**: `alternate_table`, `append`, `prepend`,
  `discard_prefix`, `keep_prefix`, `keep_duplicate_prefix`, `subdir`

**Gap G8** — our `OptionShape` has 12 flags. Missing in particular:
`alternate_table` (dispatch to a sub-table), `abbrev_match` (unambiguous
prefix matching), `hyphen_is_underscore`, `colon`, `parens`, `strip_quotes`,
`split`, `unsupported`, `required`.

The `alternate_table` mechanism is the big one:

```json
{"Wl,", oa_dash|oa_attached|oa_split","|oa_alternate_table, "linker", oa_append|oa_keep_duplicate_prefix}
```

`-Wl,-a,-b` is split on `,` and each piece is translated by the **linker**
table. We drop `-Wl,` wholesale, which loses real information.

### 2.9 Three translation phases, each with its own option namespace

```xml
<pre_trans>  <options><intern_trans>gnu_pre_translate</intern_trans></options></pre_trans>
<trans>       <options><includes_from_env_var>CPATH</includes_from_env_var></options></trans>
<post_trans>  <options><prepend_arg>--c</prepend_arg></options></post_trans>
```

`pre_trans` rewrites the *native* command line into the *intermediate* one.
`trans` runs the intermediate compiler. `post_trans` adjusts the output.
Each phase has its own switch table, and the phases have different option
names — e.g. `-I` becomes `-scan_sys_include` in the intern phase.

**Gap G9** — we have one flat table and one phase. Our output is directly
clang-compatible, so we do not need three phases, but we DO need the
equivalent of "the native table" vs "the clang table" separation, because
the mapping is not always 1:1.

### 2.10 User extension and override

The documented user-facing override points:

- `<version_regex>` — add your own version pattern.
- `<prepend_arg>` / `<prepend>` — inject arguments.
- `<skip_arg>` / `<remove_arg>` / `<replace_arg>` — suppress or rewrite.
- `option_group` / `applies_to` — scope a group to your compiler.
- `<custom_test>` — add your own feature probe.
- A separate translation binary the customer compiles against the shipped
  translator header.

**Gap G10** — our user extension is limited to "add a whole profile" and
"map a compiler name to a profile id". There is no way to say "in the gcc
table, drop `-Werror`" or "in the msvc table, add `/permissive-`".

---

## 3. Gap table

| # | Gap | Severity | Reference mechanism | Our fix |
|---|---|---|---|---|
| G1 | Compiler family is flat | High | `comp_next_type`, `comp_family_head` | family graph + per-variant translator |
| G2 | Name matching incomplete | Medium | `extra_comp` glob + `replace/` redirect | glob patterns + redirects |
| G3 | No version detection | High | `ident_switch` + ordered `version_regex` | probe + ordered regex list |
| G4 | No version-scoped rules | High | per-version translator class | version predicates on rules |
| G5 | No runtime type detection | Medium | `comp_is_redirect` + `redirect_regex` | detection hook |
| G6 | No feature probing | Medium | `config_gen_info` + `config_probe` | probe config (future) |
| G7 | Rules are all-or-nothing | High | `option_group` + `applies_to` | rule groups with scope |
| G8 | Shape vocabulary too small | High | ~50 attributes | extend `OptionShape` |
| G9 | Single phase | Low | pre/trans/post | documented as non-goal; keep 1:1 table + map |
| G10 | Weak user extension | **Critical** | many override points | rule-level add/replace/disable + level override |

G10 is the one the user called out: *"还要能支持用户进一步配置"*. Without it
this is a demo with three hard-coded tables.

---

## 4. Target model

### 4.1 Compiler identity

```
CompilerFamily
  id                  "gnu", "msvc", "iar", "ti"
  kind                CompilerKind
  variants[]          CompilerVariant
      id              "gcc", "g++", "g++cc"
      translatorId    "gnu" | "gnu_cxx"   -> which switch table
      lang            C | C++ | ObjC
      nextVariantId   "g++"             -> family chain
      familyHead      bool
  match[]             CompilerMatch
      pattern         "gcc-*" | "*-gcc" | "replace/gcc/g++"
      variantId       "gcc"
  probes[]            CompilerProbe
      switch          "--version"
      regexes[]       ordered, replace/<re>/<rep>
  groups[]            RuleGroup
      appliesTo       ["gnu:gcc", "gnu:g++", ...]
      rules[]
```

A **rule** may carry a version predicate:

```json
{ "name": "std=", "class": "LanguageStd",
  "version": { "min": "4.7", "max": null } }
```

### 4.2 Rule resolution order (deterministic, documented)

1. Exact variant match from an explicit user mapping.
2. Explicit user mapping by glob.
3. Built-in match by glob (longest pattern wins).
4. Default profile.

Within a profile, rules are merged in this order, later winning:

1. `base` rules of the profile.
2. Rules inherited from a parent profile (`extends`), parent first.
3. The profile's own `rules`.
4. The profile's rule **groups** whose `appliesTo` matches the variant.
5. User **overrides** (add / replace / disable) from the config file.

### 4.3 User override vocabulary

```json
{
  "profile_overrides": [
    {
      "profile": "gnu",
      "variant": "gcc",
      "disable":   ["Werror", "MD"],
      "replace":   [{ "name": "std=", "class": "LanguageStd",
                      "clang_form": "-std=%s" }],
      "add":       [{ "name": "fno-common", "class": "Codegen",
                      "shape": ["dash"] }],
      "level_override": [
        { "name": "O", "keep_at": ["ast", "bc"] }
      ]
    }
  ]
}
```

`level_override` is what makes the retention matrix user-editable, which is
the axis the user asked for: *"要对各类编译器都支持参数调整"*.

### 4.4 Audit model

Every option ends in exactly one bucket, always reported:

| Bucket | Meaning |
|---|---|
| `kept` | carried into the clang command line |
| `mapped` | carried, but rewritten to a different spelling |
| `dropped_no_equivalent` | the concept does not exist in clang |
| `dropped_by_level` | not needed at this retention level |
| `dropped_by_policy` | the user disabled it |
| `unknown` | the table does not know it — **never silently dropped** |

The `unknown` bucket has a configurable policy: `report` (default),
`keep` (pass through unchanged), `drop`, `fail`.

---

## 5. Explicit non-goals

Stating these keeps the scope honest:

1. **We do not run the native compiler.** Feature probing (G5, G6) needs
   the toolchain installed and executable. We support *configuring* a probe,
   but the default path stays static. This is the single largest functional
   difference and it is deliberate.
2. **We do not reproduce a pre/trans/post pipeline.** Our output
   targets clang directly, so a 1:1 table plus an explicit `map` action is
   enough.
3. **We do not reproduce the vendor compatibility headers.** A mature
   toolchain ships per-vendor compatibility headers (up to 494 KB) to make
   a native compiler behave like its front end. That is a code-generation
   problem, not an argument-mapping problem.
4. **We do not support every vendor.** 86 families is the scope of a
   full-coverage installation. We ship the mainstream ones and make the
   extension mechanism good enough that a user can add their own without
   patching C++.

---

## 6. Acceptance criteria

The upgrade is done when:

- [x] A profile can declare a family with variants, a variant chain, glob
      matches and redirects.
- [x] Version detection can be configured (probe switch + ordered regexes)
      and rules can be scoped by version.
- [x] Rule groups can be scoped to a subset of variants.
- [x] The shape vocabulary covers at least: `alternate_table`, `abbrev_match`,
      `hyphen_is_underscore`, `colon`, `parens`, `strip_quotes`, `split`,
      `unsupported`, `required`.
- [x] Users can add / replace / disable a rule, override the retention
      level of a rule, and override the clang spelling of a rule — without
      touching C++.
- [x] The audit report distinguishes all six buckets and never drops an
      unknown option silently.
- [x] The unknown-option policy is configurable and defaults to `report`.
- [x] Tests cover: same-family different-version, same-name different-variant,
      profile inheritance conflict, user override vs built-in precedence,
      unknown-option audit, redirect matching.
- [x] The Chinese and English documents stay structurally identical.

### 6.1 What "the shape vocabulary covers" means

Each of the nine shapes above is **implemented**, not merely declared:

| Shape | Behaviour | Test |
|---|---|---|
| `alternate_table` | the value is dispatched to a named sub-table | `the split option is not reported as unknown` |
| `abbrev_match` | an unambiguous abbreviation matches; an ambiguous one is rejected | `the unambiguous abbreviation matches` / `the ambiguous abbreviation does not match either rule` |
| `hyphen_is_underscore` | `-` and `_` are interchangeable in the name | `the hyphen form matches the dashed spelling` |
| `colon` | the value starts after the `:` | `the colon form takes the value after the colon` |
| `parens` | the value is the text inside the brackets | `the parenthesised form takes the value inside the brackets` |
| `strip_quotes` | the outer quote pair is removed | `the quoted form removes the quotes` |
| `split` | the value is split on commas | `the split option is not reported as unknown` |
| `unsupported` | known but untranslatable: reported, not passed through | `the unsupported option is reported` |
| `required` | the option cannot be translated by the generic path | covered by the `unsupported` bucket path |

`abbrev_match` and `hyphen_is_underscore` are **opt-in per rule**, and
the tests pin both halves: with the flag the match happens, without it the
option is reported as unknown. A shape that silently applied to every rule
would be worse than a missing one — it would let one option's spelling
stand in for another's.

`abbrev_match` runs as a **second pass**: the ordinary exact/prefix match
always wins, and the abbreviation pass only runs when it found nothing. An
abbreviation that more than one rule could satisfy is rejected rather than
resolved by guessing, because the token alone cannot tell the candidates
apart.
