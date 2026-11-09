# 输出格式

<!-- i18n:begin -->
[**中文**](./OUTPUT-FORMATS-2026-11-05.md) | [English](../en/OUTPUT-FORMATS-2026-11-05.md)
<!-- i18n:end -->

**日期**：2026-11-05
**状态**：已实现，并已对真实工具链实测

本文说明 `ccap build` 写出的东西：每种语言家族、每种输出格式分别是什么样。
这里每个例子都是**真实捕获** —— C++ 的来自一次 CMake 构建，Java 的来自 `javac`，
C# 的来自 `dotnet build` —— 不是手写的示意。

---

## 1. 三种格式

| 格式 | 是什么 | 谁要它 |
|---|---|---|
| `clang` | 编译数据库，原始命令逐字保留 | clang 工具链、IDE、`clangd` |
| `clang-cmd` | 每行一条改写后的 clang 命令 | 构建系统、shell 脚本、重跑工具 |
| `translated` | 每行一条分析器风格的命令 | 有自己参数形状的静态分析器 |

`clang` 是**默认**格式，也是唯一保留原始命令的格式。另外两种会改写命令，
每次改写都由该语言的适配器驱动。

格式按次运行选：

```
ccap build <cmd> [args...] --build-format clang
```

---

## 2. `clang` 格式 —— 编译数据库

标准编译数据库：一个 JSON 数组，每个编译单元一个对象。

```json
[
  {
    "directory": "D:/work/.../.trash/poly/cpp/build",
    "arguments": ["D:\\development\\w64devkit\\bin\\c++.exe", "-std=gnu++17", "-MD",
                  "-MT", "CMakeFiles/poly.dir/main.cpp.obj",
                  "-MF", "CMakeFiles\\poly.dir\\main.cpp.obj.d",
                  "-o", "CMakeFiles/poly.dir/main.cpp.obj", "-c",
                  "D:/work/.../.trash/poly/cpp/main.cpp"],
    "file": "D:/work/.../.trash/poly/cpp/main.cpp"
  }
]
```

三个字段，对所有语言都一样：

| 字段 | 含义 |
|---|---|
| `directory` | 那次编译的绝对工作目录 |
| `arguments` | 编译器和它的参数，是一个**数组** —— 每个元素一个 token |
| `file` | 编译单元的绝对路径 |

用 `arguments` 而不是 `command`，是因为规范两者都支持，但数组不需要命令行引号转义。
`clangd`、`ccls` 这类消费方都接受。

### 2.1 每种语言的 arguments 长什么样

**形状**相同；**内容**是原始命令行，逐字保留。

| 语言 | `arguments[0]` | 典型内容 |
|---|---|---|
| C / C++ | `D:\...\c++.exe` | `-std=`、`-I`、`-D`、`-c`、`-o` —— 原始的 gcc/clang/MSVC 拼写 |
| Java | `D:\...\javac.exe` | `-classpath`、`-d`、`-sourcepath` |
| Kotlin / Scala | 编译器启动器 | `-classpath`、`-d`、`-language-version` / `-source` |
| C# | `C:\Program Files\dotnet\dotnet.exe` | `exec`、`...\csc.dll`，然后是 `csc` 的开关 |

C# 那条最长，因为 .NET 编译器是通过宿主调用的，而且命令行是完全展开的：

```
["C:\\Program Files\\dotnet\\dotnet.exe", "exec",
 "C:\\Program Files\\dotnet\\sdk\\9.0.101\\Roslyn\\bincore\\csc.dll",
 "/noconfig", "/unsafe-", "/checked-", "/nowarn:1701,1702", "/fullpaths",
 "/nostdlib+", "/warn:9", "/define:TRACE;DEBUG;NET;NET9_0",
 "/reference:C:\\...\\ref\\net9.0\\System.dll", ...,
 "/out:obj\\Debug\\net9.0\\Hello.dll", "/target:exe", "/langversion:13.0",
 "Hello.cs"]
```

实测：单文件项目 207 个参数。那些 reference 是 .NET 的**引用程序集** —— 扮演 C 里
`-I` 路径的角色，但它们是某个目录下的 `.dll` 文件，不是头文件。

---

## 3. `clang-cmd` 格式

每行一条改写后的 clang 命令，可以直接喂给 shell。

```
clang++ -std=gnu++17 -c -o CMakeFiles/poly.dir/main.cpp.obj D:/work/.../main.cpp
clang -o .trash/java\out D:/work/.../.trash/java/Hello.java
clang -o obj\Debug\net9.0\Hello.dll D:/work/.../.trash/poly/cs/Hello.cs
```

驱动变成 `clang` 或 `clang++`，选项被翻译成 clang 的拼写。每条的 `directory`
**没有**被带过来 —— 一行是命令，不是编译单元，要让它有意义就得在对的目录里跑。

> 这是唯一一种输出依赖工作目录的格式，编译数据库则不依赖。
> 消费方是「读数据库的工具」就用 `clang`，消费方是「要把这些行跑起来的东西」就用 `clang-cmd`。

---

## 4. `translated` 格式

每行一条分析器风格的命令。这是各语言差别最大的格式，因为每个语言家族对
「一次编译由什么描述」有自己的看法。

```
D:\development\w64devkit\bin\c++.exe -std=gnu++17 D:/work/.../main.cpp
D:\development\jdk-25.0.1\bin\javac.exe --output-dir .trash/java\out D:/work/.../Hello.java
C:\Program Files\dotnet\sdk\9.0.101\Roslyn\bincore\csc.dll --sys_include C:\...\System.dll ... D:/work/.../Hello.cs
```

### 4.1 C / C++

原生形状：头文件搜索路径、宏、语言级别、输出。

| 概念 | 选项 |
|---|---|
| 头文件搜索路径 | `-I` |
| 系统头文件路径 | `-isystem` |
| 宏 | `-D` |
| 语言级别 | `-std=` |
| 输出文件 | `-o` |

### 4.2 Java / Kotlin / Scala

**JVM 里「头文件搜索路径」的对应物是 classpath。** 一次 JVM 编译由编译器读取类型的
那些 jar 和 class 目录来描述 —— 没有同意义的 `-I`，也没有同意义的 `-D`。

| 概念 | 选项 |
|---|---|
| classpath | `--classpath` |
| 源路径 | `--sourcepath` |
| 模块路径 | `--module-path` |
| 注解处理器路径 | `--processor-path` |
| 语言级别 | `--language-level` |
| 输出目录 | `--output-dir` |

拼写对 `javac`、`kotlinc`、`scalac`、`ecj` 是**统一**的：捕获 agent 已经用各编译器
自己的选项表解析过了，所以传到翻译器这里的东西已经是结构化的，一种拼写就够用。
编译器种类仍然被记下来，所以需要区分 Kotlin 编译和 Scala 编译的消费方仍然分得清。

### 4.3 C# / VB / F#

.NET 的形状。引用程序集是有意思的那部分：它是系统头文件路径的 .NET 对应物，
以 `--sys_include` 给出，每个程序集一个。

| 概念 | 选项 |
|---|---|
| 引用程序集 | `--sys_include` |
| 分析器 / 源生成器 | `--analyzer` |
| 输出程序集 | `-o` |
| 源文件 | 末尾的位置参数 |

分析器被保留，是因为分析器**会改变编译器看到的东西**：源生成器在编译前改写源文件，
分析器报出普通编译器不会报的诊断。要复现这次编译的消费方必须带上它们。

---

## 5. 刻意不被翻译的东西

`translated` 和 `clang-cmd` 两种格式由**开关表**驱动，而开关表有两条轴。
第一条决定一个选项**是什么意思**；第二条决定它为了某个消费方**要不要留下**。

| 保留级别 | 用途 |
|---|---|
| `ast` | 让 clang 把翻译单元解析成正确 AST 所需的参数 |
| `bc` | 让 clang 产出正确 LLVM bitcode 文件所需的参数 |

两者的差别很小，而且这个差别是**实测**出来的，不是假设的：

| 类别 | AST 保留 | BC 保留 |
|---|---|---|
| include、system-include、framework、macro、pre-include | 是 | 是 |
| 语言标准、language、sysroot、target **三元组** | 是 | 是 |
| target **CPU 级别**（`-march`、`/arch`） | 否 | **是** |
| 调试信息（`-g`、`/Zi`） | 否 | **是** |
| 优化级别、警告、依赖生成 | 否 | 否 |

target 三元组两级都保留，因为它决定哪些内建类型存在，也因此决定代码能不能编过。
CPU 级别不保留：它决定指令集，AST 不关心。

---

## 6. 审计

不静默丢弃任何东西。每个没能留下的选项都带着原因报出来，原因分成不同的桶：

| 桶 | 含义 |
|---|---|
| `kept` | 原样保留 |
| `mapped` | 保留，但换了拼写 |
| `dropped_no_equivalent` | 这个选项没有 clang 等价物 |
| `dropped_by_level` | 因为保留级别被丢弃 |
| `dropped_by_policy` | 被显式规则丢弃（比如版本排除） |
| `unknown` | 开关表不认识这个选项 |

```
ccap compdb map build-log.txt --level ast --explain
```

`unknown` 桶的策略可配：

| 策略 | 效果 |
|---|---|
| `report` | 丢弃但记录 —— 默认 |
| `keep` | 原样透传 |
| `drop` | 静默丢弃 |
| `fail` | 映射失败 |

---

## 7. 一个已知的重复

一次编译可能在输出里出现**两次**。原因是两级编译器包装，这不是捕获的 bug：

```
D:/development/w64devkit/bin/c++.exe -std=gnu++17 ... main.cpp   <- 启动器
c++ -std=gnu++17 ... main.cpp                                    <- 真正的编译器
```

启动器是个程序，它启动真正的编译器，所以**两者**都是进程，都会被捕获。
w64devkit 工具链对每次编译都是这个形状。

怎么区分两者：启动器的 `argv` 是完整路径，真正编译器的是裸名字，而且真正编译器是
**子进程**（在进程树里深一层）。产出目标文件的是子进程那个。

同样的形状会出现在任何编译器的 shell 包装上，以及会重新 exec 真正驱动程序的
`cc`/`gcc` shim。

---

## 8. 该选哪种格式

| 消费方要什么 | 用 |
|---|---|
| 一个要读的编译数据库 | `clang` |
| 要再跑一遍的命令 | `clang-cmd` |
| 按语言的分析器风格参数 | `translated` |
| 想知道丢了什么、为什么丢 | `compdb map` 路径上的 `--explain` |

混合语言的情况下，三种格式都把**所有**语言放在同一个文件里 —— C++、Java、C#
的条目并排放着，按捕获顺序。

---

## 9. 本文背后的实测

| 什么 | 实测值 |
|---|---|
| `clang` 格式里的一次 C++ 编译 | 11 个参数 |
| `clang` 格式里的一次 Java 编译 | 3 个参数 |
| `clang` 格式里的一次 C# 编译 | 207 个参数 |
| 混合构建 | 9 条：8 条 C++ 加 1 条 C#，在同一个数据库里 |
| C# 的引用程序集 | 164 个 `/reference:` 参数，translated 形式里 164 个 `--sys_include` |
| C# 的分析器 | 8 个 `/analyzer:` 参数，translated 形式里 8 个 `--analyzer` |
| C# 的 translated 行 | 20 565 字符 |
| 控制台上的诊断级别 | 约 100 000 行 —— 这正是级别要加在 logger 上的原因 |

混合构建在 `clang` 格式下的样子，逐字：

```
.cpp  .../CMakeFiles/4.2.3/CompilerIdCXX/CMakeCXXCompilerId.cpp   argv: 2
.h    D:/development/w64devkit/share/cmake-4.2/Modules/CXX-DetectStdlib.h   argv: 7
.cpp  .../CMakeFiles/CMakeScratch/TryCompile-*/CMakeCXXCompilerABI.cpp     argv: 6
.cpp  .../.trash/poly/cpp/main.cpp                                  argv: 11
.cs   .../.trash/poly/cs/Hello.cs                                  argv: 207
```
