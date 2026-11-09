# Compiler Switch Table and Retention Levels

<!-- i18n:begin -->
中文 | [**English**](../en/COMPILER-SWITCH-TABLE-2026-11-03.md)
<!-- i18n:end -->

**Date**: 2026-11-03
**Status**: Implemented and unit tested

This document describes the **compiler switch table** and the **retention levels**: how
CommandCapture turns a native compile command into a clang command, which options survive
at which level, and what happens when the compiler is one nobody has ever seen.

---

## 1. The Problem

There are two questions that look alike but are not:

1. "Which options must survive so that clang parses this translation unit into the **same
   AST** the original build produced?"
2. "Which options must survive so that clang emits an **LLVM bitcode file** that is
   correct?"

The answers are different, and neither of them is "all of them":

- The optimisation level (`-O2`) changes the generated code but not the AST.
- The warning level (`-Wall`) changes nothing at all.
- The linker options (`-lfoo`, `-L/lib`) are not part of a compilation.
- The dependency generation flags (`-MMD`) are a build-graph concern.
- The **target triple** (`--target=...`) decides which builtin types exist, and therefore
  whether the code compiles at all -- so the AST needs it too. The **CPU feature level**
  (`-march=...`) decides the instruction set, and the AST does not care.

A translation that keeps everything is not "safe", it is *useless*: clang rejects options
it does not understand, and the ones it accepts silently change the meaning of the
translation unit.

A translation that keeps too little is **worse than useless**, because it looks right.
Dropping `-UFOO` while keeping `-DFOO=1` produces an AST that describes code that was
never compiled.

So the mapping has to be **explicit** about both halves: what is kept, and what is dropped
and why.

**And the retention rules are measured, not reasoned.** Section 3.1 shows the clang
measurements that decide them -- including the one that overturned the obvious answer
(the target *is* needed for the AST).

---

## 2. The Two Axes

The model is two-dimensional, and the axes are independent.

```
AXIS 1 -- the switch table
  "what does this option MEAN, and how does it map to clang?"

AXIS 2 -- the retention level
  "what does the CONSUMER care about?"
```

Adding a compiler family means adding rows to axis 1. Changing what a consumer needs means
changing axis 2. Neither touches the other.

### 2.1 Axis 1: the switch table

Each option is described by a **rule**:

| Field | Meaning |
|---|---|
| `name` | the option name without the leading dash(es) |
| `shape` | how the option is spelled (attached / unattached / equal / ...) |
| `class` | what the option **means** |
| `clang` | the clang spelling, as a template |

The `class` is the durable fact. It is what decides retention, so it is about *meaning*
rather than *syntax*: `-I` and `/I` are the same class, `-include` and `/FI` are the same
class.

### 2.2 Axis 2: the retention levels

| Level | What it is for |
|---|---|
| `ast` | the arguments needed for clang to parse the translation unit into a correct AST |
| `bc` | the arguments needed for clang to emit a correct LLVM bitcode file |

---

## 3. The Option Classes

| Class | gcc | MSVC | Kept at AST | Kept at BC |
|---|---|---|---|---|
| `include` | `-I` | `/I` | yes | yes |
| `system-include` | `-isystem` | `/external:I` | yes | yes |
| `framework` | `-F` | (n/a) | yes | yes |
| `macro` | `-D` | `/D` | yes | yes |
| `macro-undef` | `-U` | `/U` | yes | yes |
| `pre-include` | `-include` | `/FI` | yes | yes |
| `language-std` | `-std=` | `/std:` | yes | yes |
| `language` | `-x` | `/TP` `/TC` | yes | yes |
| `sysroot` | `--sysroot` | (n/a) | yes | yes |
| `compile-only` | `-c` | `/c` | yes | yes |
| `output` | `-o` | `/Fo` | yes | yes |
| `response-file` | `@file` | `@file` | yes | yes |
| `target` (the **triple**) | `--target` | (n/a) | **yes** | **yes** |
| `target-cpu` (the **CPU level**) | `-march` `-mcpu` | `/arch` | **no** | **yes** |
| `debug-info` | `-g` | `/Zi` | **no** | **yes** |
| `optimization` | `-O2` | `/O2` | no | no |
| `warning` | `-Wall` | `/W4` | no | no |
| `dependency` | `-MMD` | (n/a) | no | no |
| `linker` | `-l` `-L` `/link` | (n/a) | no | no |
| `codegen` | `-f...` | `/EHsc` | no | no |
| `static-analysis` | `--analyze` | `/analyze` | no | no |
| `coverage` | `--coverage` | (n/a) | no | no |
| `sanitizer` | `-fsanitize=` | (n/a) | no | no |
| `verbosity` | `-v` | (n/a) | no | no |
| `preprocess-only` | `-E` `-M` | `/E` | no | no |
| `pass` | `-fno-...` | (n/a) | no | no |

---

## 3.1 Why the Target Is Needed for the AST Too

This is the part that is easy to get wrong, so it was **measured** rather than reasoned
about.

The obvious reasoning is: "the AST describes the source, the bitcode describes the
machine, so the target is a bitcode-only thing." **That is wrong.**

Measured with clang 15.0.7 -- the same source, `-ast-dump`, with the pointer addresses
normalized:

| Target | `__int128` | `__builtin_va_list` | SVE vector types |
|---|---|---|---|
| `x86_64-linux-gnu` | yes | `__va_list_tag[1]` | no |
| `i386-linux-gnu` | **no** | `__va_list` | no |
| `aarch64-linux-gnu` | yes | `__va_list_tag[1]` | **yes (41)** |
| `armv7-linux-gnu` | **no** | `__va_list` | no |

The target triple **changes the AST**: it decides which builtin types exist. And it
changes more than the shape -- it decides whether the code compiles at all:

```
$ clang -target i386-linux-gnu -fsyntax-only t.c
error: __int128 is not supported on this target
```

So dropping the triple from an AST command does not produce "the same AST". It produces a
*different* AST, or a compile error.

### The line that actually matters: triple vs CPU level

| Option | Changes the AST? | Verdict |
|---|---|---|
| `--target=...` (the triple) | **yes** -- the builtin types, and whether the code compiles | needed at **both** levels |
| `-march=` / `-mcpu=` (the CPU level) | **no** -- the AST under `-march=x86-64`, `v2` and `v3` is identical | needed at the **BC** level |

That is why `target` and `target-cpu` are two separate classes. The triple decides the
builtin types and the ABI; the CPU level decides the instruction set.

### The other classes, measured the same way

| Class | Changes the AST? | Evidence |
|---|---|---|
| `macro` | **yes** | dropping `-DNDEBUG` changes the AST |
| `sysroot` | **yes** | without it the header is not found (`fatal error`) |
| `target` (triple) | **yes** | the builtin type table above |
| `codegen` (`-ffreestanding`) | no | the AST is identical |
| `target-cpu` (`-march`) | no | the AST is identical |
| `optimization` (`-O2`) | no | the AST is identical |
| `warning` (`-Wall`) | no | the AST is identical |

The rule that falls out: **the AST keeps everything that decides *which code exists*; the
bitcode adds everything that decides *how it is compiled*.**

---

## 4. The Two-Artefact Model, and Why We Adopt It

A mature toolchain solves exactly this problem, and does it with two artefacts per compiler
family:

| Artefact | What it holds |
|---|---|
| `<family>_config.xml` | the compiler identity, the env vars that feed includes, the special switches (`-c`, `-E`, `-o`), the version regex, the macro candidates |
| `<family>_switches.dat` | the option table: one line per switch, with flags describing the option's shape and how to translate it |

The split is good, and we adopt it. The two files answer different questions and change
for different reasons: the config changes when the *toolchain* changes, the switches file
changes when the *option set* changes.

We put the two in **one JSON file with two sections**, because a single file is easier to
review, easier to distribute, and easier to keep consistent than a pair of files with an
implicit coupling.

The switch shape vocabulary is the established one, because it is the vocabulary the
industry already uses for this problem:

| Flag | Meaning |
|---|---|
| `dash` | spelled with a single dash (`-I`) |
| `dash_dash` | spelled with a double dash (`--sysroot`) |
| `attached` | the value may be attached (`-Ifoo`) |
| `unattached` | the value may be separate (`-I foo`) |
| `equal` | the value may be given after `=` (`-I=foo`) |
| `optional` | the value may be omitted (`-O`) |
| `required` | the option is only valid with a value |
| `path` | the value is a filesystem path |
| `skip` | not a compile option at all; drop it silently |
| `prefix` | the option selects a sub-table (`-Wl,`) |
| `list` | the value is a comma-separated list |
| `takes_next` | the value is the next argv entry |

---

## 5. The Option Matcher

The matcher is the delicate part. The option syntaxes overlap in ways that make a naive
prefix scan wrong, and getting it wrong produces a command that compiles *different code*:

| Token | Naive match | Correct match | Why it matters |
|---|---|---|---|
| `-include` | `-I` | `-include` | the forced include changes the meaning of the TU |
| `-isystem` | `-isys` | `-isystem` | the include search order |
| `-c` | `-c99` | `-c` | compile-only vs a language mode |
| `-o` | `-O` | `-o` | the output file vs the optimisation level |

Three rules make it correct.

### 5.1 The longest match wins

The rule lookup returns the **longest** rule name that is a prefix of the token name.
That is what makes `-include` beat `-I` and `-isystem` beat `-isysroot`.

### 5.2 The option names are case-sensitive -- except for MSVC

This is the subtle one:

```
gcc's "-o"   is the output file
gcc's "-O2"  is the optimisation level
gcc's "-D"   is a macro definition
gcc's "-d"   is a debug flag
```

These are different options that differ only in case. Treating them as the same would
drop the output file (or the macro, or the debug flag) without a word -- the worst kind of
bug in a translation, because the resulting command still compiles and produces a
plausible-looking but wrong artifact.

MSVC is the opposite: its options are conventionally uppercase (`/I`, `/D`, `/Zi`), and a
user may write them in either case on Windows. So the MSVC rules are marked
`caseInsensitive`, and the unix-style ones are not.

### 5.3 The attached value is split by the rule, not by the tokenizer

The tokenizer only strips the leading dash(es) and splits on the first `=`. The **rule
lookup** decides where the name ends and the value begins:

```
"-Ifoo"      -> the rule "I"     -> name "I",    value "foo"
"-DFOO=1"    -> the rule "D"     -> name "D",    value "FOO=1"
"-std=c++17" -> the rule "std="  -> name "std=", value "c++17"
"/std:c++17" -> the rule "std:"  -> name "std:", value "c++17"
```

Note that the `=` has to be **put back** for `-DFOO=1`: the tokenizer threw it away, and
for a macro definition it is part of the value (`-DFOO=1` means "define FOO as 1", not
"define FOO1").

---

## 6. The Unknown Compiler

The question "what do we do with a compiler we do not recognise?" has three answers, and
the tool offers all three.

### 6.1 The generic profile (the default)

An unrecognised compiler gets the **generic** profile, which is a deliberate **subset** of
the gcc table: the intersection of the option spellings that practically every unix-style
compiler shares.

| Recognised | Not recognised |
|---|---|
| `-I` `-D` `-U` `-include` `-std=` `-c` `-o` `-L` `-l` | `-f...` `-m...` `--target` anything else |

Anything beyond that is reported as **unknown** rather than guessed at. That is the
honest answer: a `-Qsomething` on an unknown compiler might be a warning control, or it
might change the meaning of the code, and we cannot tell.

### 6.2 The explicit mapping ("treat it as the gcc family")

```
ccap compdb map build-log.txt --map-compiler "my-cool-cc=gnu"
```

The mapping is keyed by the compiler's base name. A key that ends with `*` is a **prefix**
match, and the longest prefix wins:

```json
{
  "compiler_profiles": {
    "my-cool-cc": "gnu",
    "arm-*": "generic",
    "arm-none-eabi-*": "gnu"
  }
}
```

Here `arm-none-eabi-gcc` resolves to `gnu` (the longer prefix), and `arm-something-else`
resolves to `generic`.

The explicit mapping **wins over the name matching**. The user knows something the name
does not tell: that `cc` on their system is a wrapper around a completely different
compiler.

### 6.3 The default profile

```json
{ "default_profile": "gnu" }
```

This says "treat every compiler I do not recognise as the gcc family".

It is the **last resort**, applied only after the name matching has had its chance. That
ordering matters: a default that overrode the name matching would make every built-in
profile unreachable.

### 6.4 A profile of your own

When the compiler really is something else, the whole switch table can be replaced:

```json
{
  "profiles": [
    {
      "id": "my-cc",
      "kind": "gcc",
      "names": ["my-cool-cc"],
      "driver": "clang",
      "cpp_driver": "clang++",
      "include_env": ["MYCPATH"],
      "no_equivalent": ["/MYONLY"],
      "rules": [
        {
          "name": "I",
          "shape": ["dash", "attached", "unattached", "path"],
          "class": "include",
          "clang": "-I%s",
          "note": "the include path"
        },
        {
          "name": "MYONLY",
          "shape": ["dash", "attached"],
          "class": "codegen",
          "clang": ""
        }
      ]
    }
  ]
}
```

A profile whose id matches a built-in one **replaces** it -- the intent of "override the
gcc table" is "my gcc does not behave like the stock one", and a merge would leave the
stock rules underneath, where the longer name would win -- the opposite of what was asked.

---

## 7. Nothing Is Dropped Silently

Every decision is recorded and explained. The report distinguishes three outcomes:

| Outcome | Meaning |
|---|---|
| **kept** | the option survives, possibly with a new spelling |
| **no equivalent** | the option is understood, and clang simply has no spelling for it (`/EHsc`) |
| **unknown** | the option is not in the table, so we do not even know what it does |

The second and third are different problems, and conflating them would lose information:
a known gap in the translation is not the same as an unrecognised option.

```
$ ccap compdb map build-log.txt --level ast --explain
retention level: ast

--- gcc main.cpp
  keep   -c
  keep   -I/inc            ->  -I/inc
  keep   -DFOO=1           ->  -DFOO=1
  drop   -O2               (optimisation level does not affect the translation unit)
  drop   -Wall             (warning level does not affect the translation unit)
  drop   -Qweird           (not in the switch table for the 'gnu' family)
  unknown options (not in the switch table): -Qweird
```

---

## 8. The CLI

```
ccap compdb map <file...> [--level ast|bc] [--explain]
                 [--map-compiler <name>=<profile>] [--default-profile <profile>]
                 [--compiler-version <version>] [--unknown-policy <policy>]
                 [--compiler-match <pattern>=<target>]
```

| Option | Meaning |
|---|---|
| `--level ast` | keep the arguments needed for a correct clang AST (the default) |
| `--level bc` | keep the arguments needed for a correct LLVM bitcode file |
| `--explain` | print the report (to stderr, so stdout stays pipeable) |
| `--map-compiler <name>=<profile>` | map a compiler to a profile; the name may end with `*` |
| `--default-profile <profile>` | the profile for a compiler that is not recognised |
| `--compiler-version <version>` | the version of the compiler, for the version-scoped rules |
| `--unknown-policy <policy>` | `report` (the default), `keep`, `drop` or `fail` |
| `--compiler-match <pattern>=<target>` | a glob or a redirect that binds a compiler name to a family |

The configuration file carries the same information, so a project can commit it:

```json
{
  "output": { "format": "clang-cmd", "level": "bc" },
  "compiler_profiles": { "my-cool-cc": "gnu" }
}
```

### 8.1 The unknown-option policy

An option that is not in the switch table may well be one that changes the meaning
of the translation unit, so it is never dropped without a word. The policy says
what to do with it:

| Policy | Behaviour |
|---|---|
| `report` | drop it from the command, report it (the default) |
| `keep` | pass it through unchanged, and still report it |
| `drop` | drop it, and count it in the policy bucket |
| `fail` | the mapping fails, with a reason |

`keep` exists because a project may well be using an option that clang does accept
(`-fno-some-vendor-extension` on a build that already targets clang). Dropping it
would lose information for no reason.

`fail` exists because a pipeline can stop on a suspicious translation rather than
produce an artifact nobody looked at.

### 8.2 The compiler match patterns

`--compiler-match` takes a pattern and a target:

| Pattern | Meaning |
|---|---|
| `*-gcc` | a glob: the name ends with `-gcc` |
| `gcc-*` | a glob: the name starts with `gcc-` |
| `replace/gcc/g++` | a redirect: this executable name means that other compiler |

A glob is matched against the compiler's basename. When two globs could match,
the **longest** pattern wins, so `arm-none-eabi-*` beats `arm-*`.

A redirect is the right tool for the case the name matching cannot express: on
your machine `cc` is a wrapper script that runs something else entirely.

### 8.3 The configuration keys

| Key | Meaning |
|---|---|
| `profiles` | the switch tables |
| `compiler_profiles` | a name-to-profile binding |
| `default_profile` | the profile for a compiler that is not recognised |
| `compiler_families` | the family graph: the variants, the chains, the probes |
| `compiler_matches` | the glob and redirect patterns |
| `profile_overrides` | the user overrides: add / replace / disable a rule |
| `unknown_policy` | the policy for the unknown options |
| `compiler_version` | the version of the compiler |
| `output.level` | the retention level |

The family graph is what lets one table describe `gcc`, `g++` and `g++cc` as
three variants of one family, each with its own switch table if it needs one.
See `docs/en/COMPILER-CONFIG-GAP-ANALYSIS-2026-11-03.md` for the model and the
reasoning behind it.

---

## 9. What Is Deliberately Not Here

| Not implemented | Why |
|---|---|
| The `oa_map` / `oa_split` / `oa_custom` translation functions | they need a code generator; the template form (`-D%s`) covers the cases that matter, and the rest can be expressed as a profile rule |
| The response file **expansion** | a response file's content is a command line in itself; expanding it is a separate concern |
| The env var include expansion | the profile records *which* env vars carry includes; reading the environment at mapping time is the caller's decision |
| The `comp_next_type` chain (MSVC C -> MSVC C++) | the driver choice is already made per source language, which covers the same ground |
| **Running** the native compiler to probe its version | the version is supplied by the caller, so a build log from another machine still works; the probe is configurable but not run by default |
| Feature probing (what the compiler actually supports) | it needs the toolchain installed; the model stays static by default |

---

## 10. The Files

| File | What it holds |
|---|---|
| `modules/ccap_compdb/include/ccap_compdb_map.h` | the model: the levels, the classes, the shapes, the rules, the profiles, the mapper |
| `modules/ccap_compdb/include/ccap_compdb_registry.h` | the registry: the family graph, the resolution, the glob and the version extraction |
| `modules/ccap_compdb/compdb/option_mapping.cpp` | the matcher, the built-in profiles, the retention matrix, the mapper |
| `modules/ccap_compdb/compdb/registry.cpp` | the registry model and the self-built regex engine |
| `modules/ccap_compdb/compdb/resolution.cpp` | the compiler resolution: explicit → redirect → exact → glob → default |
| `modules/ccap_compdb/compdb/families.cpp` | the built-in families: gnu, clang, msvc, iar, generic |
| `modules/ccap_compdb/compdb/config.cpp` | the configuration loading of the profiles and the mappings |
| `modules/ccap_compdb/compdb/compdb_exporter.cpp` | the level-aware export and the report |
| `modules/ccap/app/compdb_cmd.cpp` | the `map` subcommand |
| `tests/test_compdb_map.cpp` | the unit tests (227 assertions) |
