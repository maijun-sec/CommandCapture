# 编译器开关表与保留层级

<!-- i18n:begin -->
English | [**中文**](../zh/COMPILER-SWITCH-TABLE-2026-11-03.md)
<!-- i18n:end -->

**日期**：2026-11-03
**状态**：已实现，并通过单元测试

本文档描述**编译器开关表**与**保留层级**：CommandCapture 如何把一条原生的编译命令转换成 clang
命令、哪些选项在哪个层级上必须保留、以及遇到一个谁都没见过的编译器时该怎么办。

---

## 1. 问题

有两个问题看起来很像，其实不是：

1. "哪些选项必须保留，clang 才能把这个编译单元解析成与原始构建**完全相同的 AST**？"
2. "哪些选项必须保留，clang 才能产出一份**正确的 LLVM bitcode 文件**？"

两者的答案不同，而且都不是"全都保留"：

- 优化级别（`-O2`）改变生成的代码，但不改变 AST。
- 警告级别（`-Wall`）什么都不改变。
- 链接器选项（`-lfoo`、`-L/lib`）根本不属于一次编译。
- 依赖生成选项（`-MMD`）是构建图层面的事情。
- **目标三元组**（`--target=...`）决定哪些内建类型存在，进而决定代码能不能编译 —— 所以
  AST 也需要它。**CPU 特性级别**（`-march=...`）决定指令集，AST 不在乎。

全都保留的翻译不是"安全"，而是**没用**：clang 会拒绝它不认识的选项，而它接受的那些选项
会静默改变编译单元的含义。

保留得太少的翻译比没用还**糟糕**，因为它看起来是对的。保留了 `-DFOO=1` 却丢掉了 `-UFOO`，
产出的 AST 描述的是一段从未被编译过的代码。

所以这个映射必须对两半都**显式**：保留了什么，以及丢掉了什么、为什么丢。

**而且保留规则是实测出来的，不是推出来的。** 第 3.1 节给出决定这些规则的 clang 实测 ——
包括推翻"显而易见答案"的那一条（target **确实**是 AST 需要的）。

---

## 2. 两条轴

模型是二维的，两条轴互相独立。

```
轴 1 —— 开关表
  "这个选项**是什么意思**，以及它如何映射到 clang？"

轴 2 —— 保留层级
  "**消费方**关心什么？"
```

新增一个编译器家族 = 给轴 1 加几行。改变消费方的需求 = 改轴 2。两者互不触碰。

### 2.1 轴 1：开关表

每个选项由一条**规则**描述：

| 字段 | 含义 |
|---|---|
| `name` | 去掉前导横线（们）后的选项名 |
| `shape` | 选项的拼写形态（attached / unattached / equal / ...） |
| `class` | 这个选项**意味着**什么 |
| `clang` | clang 的拼写，模板形式 |

`class` 是那个持久的事实。它决定保留与否，所以它关乎**含义**而不是**语法**：
`-I` 和 `/I` 是同一个 class，`-include` 和 `/FI` 也是同一个 class。

### 2.2 轴 2：保留层级

| 层级 | 用途 |
|---|---|
| `ast` | clang 把编译单元解析成正确 AST 所需的参数 |
| `bc` | clang 产出正确 LLVM bitcode 文件所需的参数 |

---

## 3. 选项分类

| Class | gcc | MSVC | AST 保留 | BC 保留 |
|---|---|---|---|---|
| `include` | `-I` | `/I` | 是 | 是 |
| `system-include` | `-isystem` | `/external:I` | 是 | 是 |
| `framework` | `-F` | （无） | 是 | 是 |
| `macro` | `-D` | `/D` | 是 | 是 |
| `macro-undef` | `-U` | `/U` | 是 | 是 |
| `pre-include` | `-include` | `/FI` | 是 | 是 |
| `language-std` | `-std=` | `/std:` | 是 | 是 |
| `language` | `-x` | `/TP` `/TC` | 是 | 是 |
| `sysroot` | `--sysroot` | （无） | 是 | 是 |
| `compile-only` | `-c` | `/c` | 是 | 是 |
| `output` | `-o` | `/Fo` | 是 | 是 |
| `response-file` | `@file` | `@file` | 是 | 是 |
| `target`（**三元组**） | `--target` | （无） | **是** | **是** |
| `target-cpu`（**CPU 级别**） | `-march` `-mcpu` | `/arch` | **否** | **是** |
| `debug-info` | `-g` | `/Zi` | **否** | **是** |
| `optimization` | `-O2` | `/O2` | 否 | 否 |
| `warning` | `-Wall` | `/W4` | 否 | 否 |
| `dependency` | `-MMD` | （无） | 否 | 否 |
| `linker` | `-l` `-L` `/link` | （无） | 否 | 否 |
| `codegen` | `-f...` | `/EHsc` | 否 | 否 |
| `static-analysis` | `--analyze` | `/analyze` | 否 | 否 |
| `coverage` | `--coverage` | （无） | 否 | 否 |
| `sanitizer` | `-fsanitize=` | （无） | 否 | 否 |
| `verbosity` | `-v` | （无） | 否 | 否 |
| `preprocess-only` | `-E` `-M` | `/E` | 否 | 否 |
| `pass` | `-fno-...` | （无） | 否 | 否 |

---

## 3.1 为什么 target 也是 AST 需要的

这部分很容易搞错，所以是**实测**出来的，不是推理出来的。

显而易见的推理是："AST 描述源码，bitcode 描述机器，所以 target 是只有 bitcode 才需要
的东西。"**这个推理是错的。**

用 clang 15.0.7 实测 —— 同一份源码，`-ast-dump`，把指针地址归一化之后：

| 目标 | `__int128` | `__builtin_va_list` | SVE 向量类型 |
|---|---|---|---|
| `x86_64-linux-gnu` | 有 | `__va_list_tag[1]` | 无 |
| `i386-linux-gnu` | **无** | `__va_list` | 无 |
| `aarch64-linux-gnu` | 有 | `__va_list_tag[1]` | **有（41 个）** |
| `armv7-linux-gnu` | **无** | `__va_list` | 无 |

目标三元组**会改变 AST**：它决定哪些内建类型存在。而且改变的不只是形状 —— 它决定代码
到底能不能编译：

```
$ clang -target i386-linux-gnu -fsyntax-only t.c
error: __int128 is not supported on this target
```

所以从 AST 命令里丢掉三元组，产出的不是"同一个 AST"，而是**另一个 AST**，或者一个编译
错误。

### 真正起作用的那条界线：三元组 vs CPU 级别

| 选项 | 改变 AST 吗 | 结论 |
|---|---|---|
| `--target=...`（三元组） | **会** —— 内建类型，以及代码能否编译 | **两个层级都要** |
| `-march=` / `-mcpu=`（CPU 级别） | **不会** —— `-march=x86-64`、`v2`、`v3` 下 AST 完全相同 | 只有 **BC** 要 |

这就是 `target` 和 `target-cpu` 是两个 class 的原因。三元组决定内建类型和 ABI；CPU 级别
决定指令集。

### 其它 class，用同样的方法实测

| Class | 改变 AST 吗 | 证据 |
|---|---|---|
| `macro` | **会** | 丢掉 `-DNDEBUG` 会改变 AST |
| `sysroot` | **会** | 没有它就找不到头文件（`fatal error`） |
| `target`（三元组） | **会** | 上面的内建类型表 |
| `codegen`（`-ffreestanding`） | 不会 | AST 完全相同 |
| `target-cpu`（`-march`） | 不会 | AST 完全相同 |
| `optimization`（`-O2`） | 不会 | AST 完全相同 |
| `warning`（`-Wall`） | 不会 | AST 完全相同 |

由此得到的规则是：**AST 保留所有决定"哪些代码存在"的东西；bitcode 再加上所有决定
"代码怎么编译"的东西。**

---

## 4. 两个产物的模型，以及我们为什么采用它

成熟的工具链解决的正是这个问题，它的做法是每个编译器家族两个文件：

| 文件 | 装什么 |
|---|---|
| `<family>_config.xml` | 编译器身份、提供 include 的环境变量、特殊开关（`-c`、`-E`、`-o`）、版本正则、宏候选 |
| `<family>_switches.dat` | 选项表：每个开关一行，用标志描述选项的形态和如何翻译 |

这个拆分是对的，我们采纳它。两个文件回答不同的问题、也因不同的原因而变化：config 随
**工具链**变化，switches 随**选项集**变化。

我们把两者放进**一个 JSON 文件的两个 section**，因为单个文件比一对带有隐式耦合的文件更容易
评审、更容易分发、也更容易保持一致。

开关形态的词汇表沿用既有的那套，因为那已经是业界处理这个问题时在用的词汇：

| 标志 | 含义 |
|---|---|
| `dash` | 用单个横线拼写（`-I`） |
| `dash_dash` | 用双横线拼写（`--sysroot`） |
| `attached` | 值可以紧贴（`-Ifoo`） |
| `unattached` | 值可以分开（`-I foo`） |
| `equal` | 值可以跟在 `=` 后面（`-I=foo`） |
| `optional` | 值可以整个省略（`-O`） |
| `required` | 该选项必须带值 |
| `path` | 值是文件系统路径 |
| `skip` | 根本不是编译选项；静默丢弃 |
| `prefix` | 该选项选择一张子表（`-Wl,`） |
| `list` | 值是逗号分隔的列表 |
| `takes_next` | 值是下一个 argv 项 |

---

## 5. 选项匹配

匹配是最微妙的部分。各家编译器的选项语法有大量重叠，天真的前缀扫描会出错，而出错会
产出一条编译出**不同代码**的命令：

| Token | 天真的匹配 | 正确的匹配 | 为什么重要 |
|---|---|---|---|
| `-include` | `-I` | `-include` | 强制包含会改变 TU 的含义 |
| `-isystem` | `-isys` | `-isystem` | include 搜索顺序 |
| `-c` | `-c99` | `-c` | 只编译 vs 语言模式 |
| `-o` | `-O` | `-o` | 输出文件 vs 优化级别 |

三条规则让它正确。

### 5.1 最长匹配胜出

规则查找返回的是**最长**的、能作为 token 名前缀的规则名。这就是 `-include` 赢过 `-I`、
`-isystem` 赢过 `-isysroot` 的原因。

### 5.2 选项名**大小写敏感** —— MSVC 除外

这条很微妙：

```
gcc 的 "-o"   是输出文件
gcc 的 "-O2"  是优化级别
gcc 的 "-D"   是宏定义
gcc 的 "-d"   是调试开关
```

它们是**不同的选项**，只差大小写。把它们当成同一个，会悄无声息地丢掉输出文件（或者宏、
或者调试开关）—— 这是翻译里最糟糕的一类 bug，因为产出的命令照样能编译，还会生成一个看着
挺像样但其实错误的产物。

MSVC 正好相反：它的选项约定是大写（`/I`、`/D`、`/Zi`），而在 Windows 上用户可能写成任
意大小写。所以 MSVC 规则标记为 `caseInsensitive`，unix 风格的不是。

### 5.3 紧贴的值由规则切分，不由分词器切分

分词器只做两件事：剥掉前导横线（们），以及在第一个 `=` 处切开。**规则查找**才决定名字
在哪里结束、值从哪里开始：

```
"-Ifoo"      -> 规则 "I"     -> 名 "I",    值 "foo"
"-DFOO=1"    -> 规则 "D"     -> 名 "D",    值 "FOO=1"
"-std=c++17" -> 规则 "std="  -> 名 "std=", 值 "c++17"
"/std:c++17" -> 规则 "std:"  -> 名 "std:", 值 "c++17"
```

注意 `-DFOO=1` 的 `=` 必须**放回去**：分词器把它丢掉了，而对宏定义来说它是值的一部分
（`-DFOO=1` 的意思是"把 FOO 定义为 1"，不是"定义 FOO1"）。

---

## 6. 不认识的编译器

"遇到一个我们不认识的编译器该怎么办？"有三个答案，工具三个都提供。

### 6.1 generic profile（默认）

不被识别的编译器会拿到 **generic** profile，它是 gcc 表的一个刻意**子集**：几乎所有
unix 风格编译器都共有的那部分选项拼写。

| 认识 | 不认识 |
|---|---|
| `-I` `-D` `-U` `-include` `-std=` `-c` `-o` `-L` `-l` | `-f...` `-m...` `--target` 其它一切 |

超出这个范围的一律报告为 **unknown**，而不是靠猜。这是诚实的答案：一个未知编译器上的
`-Qsomething` 可能是警告开关，也可能改变代码的含义，我们分辨不出来。

### 6.2 显式映射（"当成 gcc 大类"）

```
ccap compdb map build-log.txt --map-compiler "my-cool-cc=gnu"
```

映射以编译器的 basename 为键。以 `*` 结尾的键是**前缀**匹配，最长前缀胜出：

```json
{
  "compiler_profiles": {
    "my-cool-cc": "gnu",
    "arm-*": "generic",
    "arm-none-eabi-*": "gnu"
  }
}
```

这里 `arm-none-eabi-gcc` 解析为 `gnu`（更长的前缀），`arm-something-else` 解析为
`generic`。

显式映射**优先于名字匹配**。用户知道名字不会告诉他的东西：他的系统上 `cc` 其实是另一个
编译器的包装脚本。

### 6.3 默认 profile

```json
{ "default_profile": "gnu" }
```

意思是"凡是我不认识的编译器，都当成 gcc 大类"。

它是**最后一道**兜底，只在名字匹配试过之后才生效。这个顺序很重要：一个会盖掉名字匹配的
默认值，会让所有内置 profile 都变得不可达。

### 6.4 自己写一个 profile

当编译器真的与众不同的时候，整张开关表都可以替换：

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

id 与内置 profile 相同的用户 profile 会**替换**它 —— "覆盖 gcc 表"的意图是"我的 gcc 和
标准的不一样"，而合并会把标准规则留在底下，最终由更长的名字胜出 —— 与用户要的正好相反。

---

## 7. 没有任何东西被静默丢弃

每一个决定都被记录并给出理由。报告区分三种结果：

| 结果 | 含义 |
|---|---|
| **kept** | 该选项被保留，可能换了拼写 |
| **no equivalent** | 该选项我们理解，而 clang 就是没有对应写法（`/EHsc`） |
| **unknown** | 该选项不在表里，所以我们连它干什么都不知道 |

后两者是不同的毛病，混为一谈会丢信息：翻译里一个**已知的缺口**和**一个没认出来的选项**
不是一回事。

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

## 8. 命令行

```
ccap compdb map <file...> [--level ast|bc] [--explain]
                 [--map-compiler <name>=<profile>] [--default-profile <profile>]
                 [--compiler-version <version>] [--unknown-policy <policy>]
                 [--compiler-match <pattern>=<target>]
```

| 选项 | 含义 |
|---|---|
| `--level ast` | 保留正确的 clang AST 所需的参数（默认） |
| `--level bc` | 保留正确的 LLVM bitcode 文件所需的参数 |
| `--explain` | 打印报告（走 stderr，所以 stdout 仍然可以管道） |
| `--map-compiler <name>=<profile>` | 把某个编译器映射到某个 profile；名字可以以 `*` 结尾 |
| `--default-profile <profile>` | 不认识的编译器所用的 profile |
| `--compiler-version <version>` | 编译器的版本，供版本作用域规则使用 |
| `--unknown-policy <policy>` | `report`（默认）、`keep`、`drop` 或 `fail` |
| `--compiler-match <pattern>=<target>` | 把编译器名字绑定到某个家族的 glob 或 redirect |

配置文件里带的是同样的信息，所以一个项目可以把它提交进版本库：

```json
{
  "output": { "format": "clang-cmd", "level": "bc" },
  "compiler_profiles": { "my-cool-cc": "gnu" }
}
```

### 8.1 未知选项策略

一个不在开关表里的选项，很可能正好是那个会改变编译单元含义的选项，所以它绝不会被
无声丢掉。策略决定拿它怎么办：

| 策略 | 行为 |
|---|---|
| `report` | 从命令里丢掉，报告出来（默认） |
| `keep` | 原样透传，并且仍然报告 |
| `drop` | 丢掉，并计入策略桶 |
| `fail` | 映射失败，并给出原因 |

`keep` 存在，是因为一个项目完全可能正在用 clang 也接受的选项（一个本来就以 clang 为
目标的构建里的 `-fno-some-vendor-extension`）。丢掉它是无缘无故地丢信息。

`fail` 存在，是因为流水线可以选择在一个可疑的翻译上停下来，而不是产出一份没人看过的
产物。

### 8.2 编译器匹配模式

`--compiler-match` 接受一个模式和一个目标：

| 模式 | 含义 |
|---|---|
| `*-gcc` | glob：名字以 `-gcc` 结尾 |
| `gcc-*` | glob：名字以 `gcc-` 开头 |
| `replace/gcc/g++` | redirect：这个可执行文件名指的是另一个编译器 |

glob 与编译器的 basename 匹配。当两个 glob 都可能匹配时，**最长**的模式胜出，所以
`arm-none-eabi-*` 赢过 `arm-*`。

redirect 用来处理名字匹配表达不了的情况：在你的机器上 `cc` 其实是个包装脚本，跑的是
完全别的东西。

### 8.3 配置键

| 键 | 含义 |
|---|---|
| `profiles` | 开关表 |
| `compiler_profiles` | 名字到 profile 的绑定 |
| `default_profile` | 不认识的编译器所用的 profile |
| `compiler_families` | 家族图：变体、变体链、探测规则 |
| `compiler_matches` | glob 与 redirect 模式 |
| `profile_overrides` | 用户覆盖：add / replace / disable 一条规则 |
| `unknown_policy` | 未知选项的策略 |
| `compiler_version` | 编译器的版本 |
| `output.level` | 保留层级 |

家族图让一张表能描述 `gcc`、`g++` 和 `g++cc` —— 同一个家族的三个变体，各自在需要时
拥有自己的开关表。模型和背后的推理见 `docs/en/COMPILER-CONFIG-GAP-ANALYSIS-2026-11-03.md`。

---

## 9. 刻意没做的部分

| 未实现 | 为什么 |
|---|---|
| `oa_map` / `oa_split` / `oa_custom` 翻译函数 | 它们需要一个代码生成器；模板形式（`-D%s`）覆盖了真正重要的场景，其余的可以用一条 profile 规则表达 |
| response file 的**展开** | response file 的内容本身就是一条命令行；展开它是另一件事 |
| 环境变量 include 的展开 | profile 记录了*哪些*环境变量带 include；要不要读环境是调用方的事 |
| `comp_next_type` 链（MSVC C -> MSVC C++） | driver 的选择已经按源文件语言做了，覆盖的是同一块地 |
| **运行**原生编译器来探测版本 | 版本由调用方提供，所以来自另一台机器的 build log 仍然可用；探测规则可配置，但默认不运行 |
| 特性探测（编译器实际支持什么） | 它需要工具链已安装；模型默认保持静态 |

---

## 10. 相关文件

| 文件 | 内容 |
|---|---|
| `modules/ccap_compdb/include/ccap_compdb_map.h` | 模型：层级、分类、形态、规则、profile、mapper |
| `modules/ccap_compdb/include/ccap_compdb_registry.h` | 注册表：家族图、解析、glob、版本提取 |
| `modules/ccap_compdb/compdb/option_mapping.cpp` | 匹配器、内置 profile、保留矩阵、mapper |
| `modules/ccap_compdb/compdb/registry.cpp` | 注册表模型与自研正则引擎 |
| `modules/ccap_compdb/compdb/resolution.cpp` | 编译器解析：显式 → redirect → 精确 → glob → 默认 |
| `modules/ccap_compdb/compdb/families.cpp` | 内置家族：gnu、clang、msvc、iar、generic |
| `modules/ccap_compdb/compdb/config.cpp` | profile 与映射的配置加载 |
| `modules/ccap_compdb/compdb/compdb_exporter.cpp` | 分层导出与报告 |
| `modules/ccap/app/compdb_cmd.cpp` | `map` 子命令 |
| `tests/test_compdb_map.cpp` | 单元测试（227 条断言） |
