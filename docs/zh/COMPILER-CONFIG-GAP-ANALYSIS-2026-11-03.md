# 编译器配置差距分析 — 2026-11-03

> 范围：一个生产级编译命令映射引擎需要什么 —— 以一套参考工具链安装为基准衡量，
> 并对照本文档撰写时 `ccap_compdb` 的状态。

本文档是这次升级的**设计契约**。它说明缺了什么、每个差距为什么重要，以及目标模型长什么
样。英文对应文档是 `docs/en/COMPILER-CONFIG-GAP-ANALYSIS-2026-11-03.md`，章节结构完全
一致。

---

## 1. 编译器配置体系实际上是什么

一个成熟的静态分析工具链维护的不是"每个编译器一张开关表"，而是一套**编译器配置体系**，
共五层：

| 层 | 产物 | 回答什么问题 |
|---|---|---|
| 1. 编译器注册表 | `<family>_config.xml`、`<family>_compilers.cpp` | *这个家族包含哪些可执行文件？哪个变体/版本？* |
| 2. 开关表 | `<family>_switches.dat` | *每个选项怎么拼写，含义是什么？* |
| 3. 环境 / driver 模型 | `config.xml` 的 `<options>` | *include 路径从哪来？`-c` 开关是什么？* |
| 4. 特性探测 | `config_gen_info`、`config_probe` | *这个编译器**实际**支持什么？* |
| 5. 自定义翻译器 | `<family>_compilers.cpp` | *声明式表格表达不了的那 10%。* |

这类参考安装的规模：

- **86 个编译器模板目录**，610 个文件（203 个 `.dat`、149 个 `.xml`、
  47 个 `.cpp`、74 个 `.json`、134 个 `.h`）。
- 正式的配置 schema 里有 **416 个元素**。
- 属性定义文件里 **约 50 个开关属性**。
- 最吃力的那个厂商一家：52 KB config + 45 KB switches + 44 KB 自定义 C++。
  另一家：40 KB 自定义 C++。还有一家：37 KB 自定义 C++，带 **八个** 版本级翻译器类。

"几十名工程师"花在这些地方。不是开关表，而是**厂商级和版本级的漂移**。

---

## 2. 我们缺少的机制

### 2.1 编译器身份是图，不是名字列表

一个编译器被建模成一条**家族链**：

```xml
<compiler>
  <comp_name>gcc</comp_name>
  <comp_translator>gcc</comp_translator>
  <comp_lang>C</comp_lang>
  <comp_family_head>true</comp_family_head>
  <comp_next_type>g++</comp_next_type>      <!-- C -> C++ -->
</compiler>
```

`comp_next_type` 把 `gcc -> g++ -> g++cc` 串起来；`comp_family_head` 标出根。
一个变体自带自己的 `comp_translator`，所以同一个家族的 C 和 C++ driver 可以有
**不同的开关表**。

**差距 G1** —— 我们的 `CompilerProfile` 只有一个扁平的 `kind` + `exactNames` +
`prefixRules`。没有家族图，没有 C/C++ driver 分裂，没有 `comp_translator` 间接层。

### 2.2 名字匹配用的是 glob 和 redirect，不是前缀

```xml
<extra_comp>
  <comp_name>gcc-*</comp_name>     <!-- glob，双向 -->
  <comp_type>gcc</comp_type>
</extra_comp>
<extra_comp>
  <comp_name>replace/gcc/g++</comp_name>  <!-- redirect：gcc 以 g++ 的身份被调用 -->
  <comp_type>g++</comp_type>
</extra_comp>
```

`replace/` 前缀的意思是"这个可执行文件名实际上指的是另一个编译器"。我们没有 redirect
概念。

**差距 G2** —— 我们的 `exactNames` + `prefixRules` 无法表达
`*-gcc`（后缀匹配）或 `replace/gcc/g++`。

### 2.3 版本靠探测发现，再由有序正则过滤

```xml
<ident_switch>--version</ident_switch>
<ident_match_regex>g(cc|\+\+)(.*) [0-9]+\.[0-9]+\.[0-9]+.*</ident_match_regex>

<!-- Order Version Regex least likely to incorrectly match first -->
<version_regex>replace/.*g[c+][c+] version 2\.9.* \(2.96.*\).*/2.9.6</version_regex>
<version_regex>replace/.*g[c+][c+][^\s]+ [^\s]+ (\d+\.\d+(\.\d+)?) \(.*\).*/$1</version_regex>
...
```

那条注释**就是**设计：**有序**的正则列表，最具体的在前，每条都带
`replace/<regex>/<replacement>` 形式。

**差距 G3** —— 我们没有版本检测，也没有版本作用域规则。

### 2.4 版本驱动**不同的翻译器**

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

同一个编译器厂商的工具链，八个版本，八个翻译器类。声明式 `.dat` 表格覆盖常见选项，
版本相关的残余部分交给代码。

**差距 G4** —— 我们的模型没有地方挂"这个选项在版本 4.7 及之后是 X，之前是 Y"。

### 2.5 运行时检测可以重定向编译器类型

```xml
<comp_is_redirect>true</comp_is_redirect>
<redirect_regex>Tool: armclang.*</redirect_regex>
<comp_type_if_hit>armcc:armclang</comp_type_if_hit>
<comp_type_if_miss>armcc:armcc</comp_type_if_miss>
```

`armcc` 是一个 **redirector**：跑它可能实际调用的是 `armclang`。类型要靠运行编译器、
匹配输出来决定。

**差距 G5** —— 我们的管线里**完全没有**检测/探测阶段。

### 2.6 特性探测：这个编译器**实际**支持什么

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

工具链**运行**编译器来了解：它预定义了哪些宏、支不支持异常、系统 include 在哪、
目标平台是什么。

**差距 G6** —— 我们的模型是纯静态的。我们假设同一张表适用于地球上每一个 gcc。

### 2.7 选项组可以限定到一组编译器

```xml
<option_group>
  <applies_to>gcc,g++,g++cc,hightec:tricore,hightec_cxx:tricore,hightec_cxxcc:tricore,microchip:xc16</applies_to>
  <dryrun_switch>-v</dryrun_switch>
</option_group>
```

一张开关表，但某一*组*选项只对部分编译器生效。`hightec:tricore` 里的 `:` 用来分隔
家族和变体。

**差距 G7** —— 我们的规则在每个 profile 内是全有或全无的。

### 2.8 开关属性词汇表约 50 个标志，不是 12 个

既有的属性集区分了（不限于）：

- **语法**：`slash`、`dash`、`dash_dash`、`plus`、`noprefix`
- **值的形态**：`attached`、`unattached`、`equal`、`colon`、`optional`、
  `parens`、`split`、`strip_quotes`
- **匹配**：`case_insensitive`、`hyphen_is_underscore`、`abbrev_match`
- **动作**：`copy`、`map`、`keep`、`remove`、`skip_arg`、`unsupported`、
  `required`、`custom`
- **组合**：`alternate_table`、`append`、`prepend`、
  `discard_prefix`、`keep_prefix`、`keep_duplicate_prefix`、`subdir`

**差距 G8** —— 我们的 `OptionShape` 只有 12 个标志。特别缺的是：
`alternate_table`（分派到子表）、`abbrev_match`（无歧义前缀匹配）、
`hyphen_is_underscore`、`colon`、`parens`、`strip_quotes`、
`split`、`unsupported`、`required`。

`alternate_table` 机制是其中最关键的一个：

```json
{"Wl,", oa_dash|oa_attached|oa_split","|oa_alternate_table, "linker", oa_append|oa_keep_duplicate_prefix}
```

`-Wl,-a,-b` 会按 `,` 切开，每一段由 **linker** 表翻译。我们把 `-Wl,` 整个丢掉，
那是在丢真实信息。

### 2.9 三个翻译阶段，各有自己的选项命名空间

```xml
<pre_trans>  <options><intern_trans>gnu_pre_translate</intern_trans></options></pre_trans>
<trans>       <options><includes_from_env_var>CPATH</includes_from_env_var></options></trans>
<post_trans>  <options><prepend_arg>--c</prepend_arg></options></post_trans>
```

`pre_trans` 把**原生**命令行改写成**中间**命令行。`trans` 运行中间的编译器。
`post_trans` 调整输出。每个阶段有自己的开关表，而且各阶段的选项名不同 ——
例如 `-I` 在 intern 阶段变成 `-scan_sys_include`。

**差距 G9** —— 我们只有一张扁平的表、一个阶段。我们的输出直接兼容 clang，所以不需要三个
阶段，但我们**确实**需要"原生表"与"clang 表"分离的等价物，因为映射不总是一对一。

### 2.10 用户扩展与覆盖

面向用户的覆盖点：

- `<version_regex>` —— 加自己的版本模式。
- `<prepend_arg>` / `<prepend>` —— 注入参数。
- `<skip_arg>` / `<remove_arg>` / `<replace_arg>` —— 抑制或改写。
- `option_group` / `applies_to` —— 把一组选项限定到你的编译器。
- `<custom_test>` —— 加自己的特性探测。
- 一个独立的翻译可执行文件，客户拿它对着随包发布的翻译器头文件编译。

**差距 G10** —— 我们的用户扩展只限于"加一整张 profile"和"把编译器名映射到某个 profile id"。
没有办法说"在 gcc 表里丢掉 `-Werror`"或"在 msvc 表里加 `/permissive-`"。

---

## 3. 差距表

| # | 差距 | 严重程度 | 参考机制 | 我们的修法 |
|---|---|---|---|---|
| G1 | 编译器家族是扁平的 | 高 | `comp_next_type`、`comp_family_head` | 家族图 + 每变体独立翻译器 |
| G2 | 名字匹配不完整 | 中 | `extra_comp` glob + `replace/` redirect | glob 模式 + redirect |
| G3 | 没有版本检测 | 高 | `ident_switch` + 有序 `version_regex` | 探测 + 有序正则列表 |
| G4 | 没有版本作用域规则 | 高 | 每版本一个翻译器类 | 规则上的版本谓词 |
| G5 | 没有运行时类型检测 | 中 | `comp_is_redirect` + `redirect_regex` | 检测钩子 |
| G6 | 没有特性探测 | 中 | `config_gen_info` + `config_probe` | 探测配置（未来） |
| G7 | 规则全有或全无 | 高 | `option_group` + `applies_to` | 带作用域的规则组 |
| G8 | 形态词汇表太小 | 高 | 约 50 个属性 | 扩展 `OptionShape` |
| G9 | 单阶段 | 低 | pre/trans/post | 明确列为非目标；保留 1:1 表 + map |
| G10 | 用户扩展太弱 | **关键** | 多个覆盖点 | 规则级 add/replace/disable + 层级覆盖 |

G10 正是用户点名的那一条：*"还要能支持用户进一步配置"*。没有它，这只是一个带三张硬编码
表的 demo。

---

## 4. 目标模型

### 4.1 编译器身份

```
CompilerFamily
  id                  "gnu", "msvc", "iar", "ti"
  kind                CompilerKind
  variants[]          CompilerVariant
      id              "gcc", "g++", "g++cc"
      translatorId    "gnu" | "gnu_cxx"   -> 用哪张开关表
      lang            C | C++ | ObjC
      nextVariantId   "g++"             -> 家族链
      familyHead      bool
  match[]             CompilerMatch
      pattern         "gcc-*" | "*-gcc" | "replace/gcc/g++"
      variantId       "gcc"
  probes[]            CompilerProbe
      switch          "--version"
      regexes[]       有序, replace/<re>/<rep>
  groups[]            RuleGroup
      appliesTo       ["gnu:gcc", "gnu:g++", ...]
      rules[]
```

一条**规则**可以带版本谓词：

```json
{ "name": "std=", "class": "LanguageStd",
  "version": { "min": "4.7", "max": null } }
```

### 4.2 规则解析顺序（确定、可文档化）

1. 用户显式映射给出的精确变体匹配。
2. 用户显式映射按 glob 匹配。
3. 内置匹配按 glob（最长模式胜出）。
4. 默认 profile。

在一个 profile 内部，规则按这个顺序合并，后面的赢：

1. profile 的 `base` 规则。
2. 从父 profile（`extends`）继承来的规则，父在前。
3. profile 自己的 `rules`。
4. profile 的规则**组**，其中 `appliesTo` 匹配该变体的。
5. 来自配置文件的用户**覆盖**（add / replace / disable）。

### 4.3 用户覆盖词汇表

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

`level_override` 让保留矩阵可由用户编辑，这正是用户要的那条轴：
*"要对各类编译器都支持参数调整"*。

### 4.4 审计模型

每个选项最终落进且只落进一个桶，永远都会被报告：

| 桶 | 含义 |
|---|---|
| `kept` | 原样带进 clang 命令行 |
| `mapped` | 带进来了，但换了一种拼写 |
| `dropped_no_equivalent` | 这个概念在 clang 里不存在 |
| `dropped_by_level` | 当前保留层级不需要它 |
| `dropped_by_policy` | 用户禁用了它 |
| `unknown` | 表里没有它 —— **绝不静默丢弃** |

`unknown` 桶有一个可配置的策略：`report`（默认）、
`keep`（原样透传）、`drop`、`fail`。

---

## 5. 明确的非目标

把这些写出来是为了让范围保持诚实：

1. **我们不运行原生编译器。** 特性探测（G5、G6）需要工具链已安装且可执行。我们支持
   *配置*一个探测，但默认路径保持静态。这是最大的功能性差异，而且是有意为之。
2. **我们不复刻 pre/trans/post 管线。** 我们的输出直接面向 clang，
   所以一张 1:1 表加一个显式的 `map` 动作就够了。
3. **我们不复刻厂商兼容头。** 成熟的工具链会随包发布按厂商的兼容头
   （最大 494 KB），用来让原生编译器表现得像它的前端。那是代码生成问题，
   不是参数映射问题。
4. **我们不支持每一个厂商。** 86 个家族是全量安装的范围。我们提供主流的那些，
   并把扩展机制做到用户不需要改 C++ 就能加自己的。

---

## 6. 验收标准

这次升级在满足以下条件时算完成：

- [x] 一个 profile 可以声明带变体的家族、变体链、glob 匹配和 redirect。
- [x] 版本检测可以配置（探测开关 + 有序正则），规则可以按版本限定。
- [x] 规则组可以限定到一部分变体。
- [x] 形态词汇表至少覆盖：`alternate_table`、`abbrev_match`、
      `hyphen_is_underscore`、`colon`、`parens`、`strip_quotes`、`split`、
      `unsupported`、`required`。
- [x] 用户可以 add / replace / disable 一条规则，覆盖一条规则的保留层级，
      覆盖一条规则的 clang 拼写 —— 全程不需要动 C++。
- [x] 审计报告区分全部六个桶，且绝不静默丢弃未知选项。
- [x] 未知选项策略可配置，默认是 `report`。
- [x] 测试覆盖：同家族不同版本、同名不同变体、
      profile 继承冲突、用户覆盖与内置的优先级、
      未知选项审计、redirect 匹配。
- [x] 中英文文档结构保持一致。

### 6.1 "形态词汇表已覆盖"是什么意思

上面九个形态都是**已实现**的，不是只声明了名字：

| 形态 | 行为 | 测试 |
|---|---|---|
| `alternate_table` | 值被分派到一张命名的子表 | `the split option is not reported as unknown` |
| `abbrev_match` | 无歧义的缩写可以匹配；有歧义的被拒绝 | `the unambiguous abbreviation matches` / `the ambiguous abbreviation does not match either rule` |
| `hyphen_is_underscore` | 选项名里 `-` 与 `_` 等价 | `the hyphen form matches the dashed spelling` |
| `colon` | 值从冒号之后开始 | `the colon form takes the value after the colon` |
| `parens` | 值是括号里的文本 | `the parenthesised form takes the value inside the brackets` |
| `strip_quotes` | 去掉外层引号 | `the quoted form removes the quotes` |
| `split` | 值按逗号切开 | `the split option is not reported as unknown` |
| `unsupported` | 已知但无法翻译：被报告，不透传 | `the unsupported option is reported` |
| `required` | 该选项不能走通用路径翻译 | 由 `unsupported` 桶的路径覆盖 |

`abbrev_match` 和 `hyphen_is_underscore` 是**按规则开启**的，测试把两半都钉住了：
带标志时匹配发生，不带标志时该选项被报告为 unknown。一个对所有规则都悄悄生效的形态，
比一个缺失的形态更糟 —— 它会让一个选项的拼写顶替另一个选项的。

`abbrev_match` 跑在**第二遍**：常规的精确/前缀匹配永远优先，只有它什么都没找到时
才跑缩写那一遍。一个能被多条规则同时满足的缩写是被**拒绝**的，而不是靠猜来消解 ——
光看这个 token 分辨不出候选者。
