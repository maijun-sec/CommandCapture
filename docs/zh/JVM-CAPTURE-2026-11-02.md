# JVM 编译捕获（ccap_jvm）

> 设计文档 · 2026-11-02
>
> 模块位置：`modules/ccap_jvm/`
> 状态：已实现，Maven 89 个测试通过（其中 4 个 Java 8 端到端测试在提供 JDK 8 时才跑）；
> ECJ / Kotlin / Scala 3 三个端到端测试驱动真实编译器。
> 支持的 JDK：**8 及以上** —— agent 自身编译为 Java 8 字节码。

---

## 1. 这个模块解决什么问题

有些工程无法静态分析：classpath 可能是脚本拼出来的、插件塞进去的，或者 IDE 临时决定的。
对这类工程，唯一可信的事实是**编译器真正执行的那次编译**。

`ccap_jvm` 是一个 ByteBuddy agent，它挂在编译器的入口点上，把那次编译记录下来 —— 不是记录
*请求*编译的��数行，而是记录**生效的编译**：

- 真正用的 classpath（不是命令行上写的那个）
- 编译器真正解析的源文件（包括它在 source path 上自己找到的）
- 编码、语言级别、输出目录、module path、processor path

**为什么不复用 Plumb 的实现？** Plumb 的 `plumb-build-capture` 用的是 btrace：它需要提权，
需要原生 agent，而且在 Windows 上基本不可用。ByteBuddy 走的是标准的 `java.lang.instrument`
接口，普通用户权限就能跑。

---

## 2. 三种语言，三套入口点

这是整个模块的核心事实：**"编译命令行"在任何一个语言里都不是一个方法**，而三种 JVM 语言
甚至连形状都不一样。

| 语言 | 命令行入口 | 选项怎么送进来 |
|---|---|---|
| javac | 8 个方法（命令行 / `javax.tools` API / IDE 路径 / run 风格 API） | CLI 是 `String[]`；API 是 `Iterable<String>` + 另一个 `Iterable<FileObject>` |
| kotlinc | `CLICompiler#exec(PrintStream, String...)` | argv 是**自由参数**；classpath 在**生成的参数对象**里 |
| scalac | `Main#main` / `Main#process` | 纯 `String[]` |

所以一个"通用反射提取器"必须同时知道三种形状，而它至少对一种是错的。

入口点是**声明式**的（`CompilerEntryPoints`）：加一个编译器、一个入口点，是改数据，不是改代码。

### 2.1 每个入口点都要核对真实的 jar

入口点的类名和方法名**不是猜的**，是用 `javap` 对着真实的编译器 jar 读出来的：

```
javap -p -cp kotlin-compiler-embeddable-2.2.0.jar org.jetbrains.kotlin.cli.common.CLICompiler
javap -p -cp scala3-compiler_3-3.10.0.jar dotty.tools.dotc.Driver
javap -p -cp ecj-3.33.0.jar org.eclipse.jdt.internal.compiler.Compiler
```

这条规矩救过好几次命（见第 6 节）。

---

## 3. 捕获在什么时候写

**编译结束时**写，不是开始时。

原因：编译器真正编译了哪些源文件，只有编译跑完才知道。命令行只说了它拿到了哪些文件；
编译器还会在 source path 上自己找到并解析别的文件 —— 而那些恰恰是静态分析器必须看的代码。

所以流程是：

1. **开始时**：argv 已经知道了（命令行路径），或者刚刚被交到 task 上（API 路径）。
   把它**暂存**（`PENDING_COMPILES`）。
2. **结束时**：编译器解析过的源文件知道了。取回暂存的编译，写出捕获。

### 3.1 API 路径的选项从哪来

`JavacTaskImpl#call()` 和 `#generate()` **不带任何选项** —— 选项是在 task 创建时
（`JavacTool#getTask`）设好的。

而且**读不回来**：`com.sun.tools.javac.api` 不被 `jdk.compiler` 模块导出，所以任何反射读
task 的 `options` 字段都会抛 `InaccessibleObjectException`，除非 JVM 加上
`--add-opens jdk.compiler/com.sun.tools.javac.api=ALL-UNNAMED`。要求这个 flag 会让 agent
在普通构建里没法用。

所以选项在**还能看见它的地方**记下来 —— `getTask` 那里 —— 然后在 task 被驱动的时候按
**对象身份**查回来。两个 task 不会共用一份选项。

---

## 4. bootstrap class loader：整个模块最微妙的地方

agent 的代码跑在编译器进程里，但**被增强的字节码住在编译器自己的类里**
（`com.sun.tools.javac.Main` 和朋友们），那些类由**应用类加载器**加载。agent jar 只有
agent 自己的类加载器看得见。所以编译器直接调 agent 的类会 `NoClassDefFoundError`。

解决办法是把一个小组件 **append 到 bootstrap class loader**：bootstrap 是所有加载器的祖先，
所以放在那里的类，编译器、应用、agent 都够得着。

### 4.1 但模块看不见它

**这是本模块最难的一处，且失败是彻底静默的。**

`jdk.compiler` 模块只读 `java.base`、`java.compiler`、`jdk.internal.opt`、`jdk.zipfs` ——
**不读应用类加载器的 unnamed module**。所以 advice 里直接调 agent 自己类加载器上的类，
会抛 `IllegalAccessError`；而 advice 的 `suppress = Throwable` 把它吞得干干净净。

结果就是：编译成功，agent 打印 "agent installed"，捕获**永远不出现**，而且**一句错都没有**。

真正能跨模块的是 **bootstrap loader 上的类** —— boot layer 的 unnamed module 是所有模块都读的。

所以规则是：

> **advice 只能调 `CompiledSourcesBridge`**（它在 bootstrap 上），由 bridge 反射去够别的。

`BootstrapJarTest.theAdviceOnlyCallsBootstrapClasses` 就是守这条规则的。

### 4.2 bootstrap jar 的类闭包是**算出来的**，不是列出来的

bootstrap loader 只看得见 JDK。所以 recorder 引用的每一个类都得在 bootstrap jar 里 ——
**包括编译器替它生成的那些**：

- `javac` 为 enum 上的 `switch` 生成的合成类 `Foo$1`（把 enum 常量映射到稠密 int 数组）
- record / enum 的嵌套类 `Foo$Bar`
- lambda 产生的合成方法 `Foo$1`

手写的清单**一定会漏**。这里就漏过：`JvmOptions$1` —— `JvmOptions#isSourceFile` 的
switch-map 类。捕获失败在一个 `NoClassDefFoundError` 上，而 advice 把它吞了。

所以闭包是**从字节码推出来的**：`BootstrapClosure` 读每个类的常量池（权威的"我引用了谁"），
跟着本包里的引用一直走。给 recorder 加一个类，闭包自动跟上，因为清单是**实际发布的那份
字节码**算出来的。

> **教训**：`Foo$1` 是编译器生成的，**写代码的人不会想到它**。任何"手工枚举依赖"的清单，
> 都会在某个时刻漏掉一个合成类 —— 而失败的形态是"完全静默、彻底丢失"。

---

## 5. 捕获文件格式

每编译一次写一个 JSON 文件：

```json
{
  "api": "cli",
  "compiler": "javac",
  "language": "java",
  "argv": ["-d", "out", "Hello.java"],
  "encoding": null,
  "languageLevel": null,
  "outputDirectory": "D:\\build\\out",
  "classpath": ["D:\\build\\lib.jar"],
  "sourcePath": [],
  "modulePath": [],
  "processorPath": [],
  "bootClassPath": [],
  "sourceFiles": ["D:\\build\\Hello.java"],
  "addModules": [], "addExports": [], "addReads": [],
  "preview": false,
  "compilation": true,
  "javaVersion": "25.0.1",
  "javaHome": "D:\\jdk-25",
  "workingDirectory": "D:\\build"
}
```

`compiler` 的取值（`javac` / `ecj` / `sjavac` / `kotlin` / `scala3` / `scala2`）与
`ccap_compdb` 的编译器表**同名**，所以两边不需要映射表就能对上。

`language` 与 `compiler` 分开：javac 和 ECJ 编译的是同一种语言，但消费者通常关心语言，
而不是哪个前端做的。

---

## 6. 每种语言的入口点，以及它们踩过的坑

### 6.1 javac

| 入口点 | 作用 |
|---|---|
| `com.sun.tools.javac.Main#compile(String[])` | 命令行 |
| `com.sun.tools.javac.main.Main#compile(String[])` | 命令行（实例版） |
| `com.sun.tools.javac.api.JavacTool#getTask(...)` | `javax.tools` API —— Gradle / Maven / IDE |
| `com.sun.tools.javac.api.JavacTool#run(...)` | run 风格 API |
| `com.sun.tools.javac.api.JavacTaskImpl#call()` / `#generate()` | IDE 路径（不带选项） |
| `com.sun.tools.javac.main.JavaCompiler#parse(...)` | 解析步骤 —— 真正编译的源文件 |
| `com.sun.tools.javac.main.JavaCompiler#compile(...)` / `#generate(...)` | 编译结束 |

**坑**：`javax.tools.JavaCompiler` 在现代 JDK 上是**接口**，只有 `getTask` 和
`getStandardFileManager`，**没有 `run`**。`run` 在 `com.sun.tools.javac.api.JavacTool` 上。
挂在接口上的 hook 永远不触发。

**坑**：`run(InputStream, OutputStream, OutputStream, String...)` 的 argv 在**第 4 个参数**。
绑到参数 0 会让 advice 拿到一个 `InputStream`，ByteBuddy 拒绝应用 —— 而且它只在 transform
时喊，编译照跑，捕获静默丢失。

### 6.2 ECJ

| 入口点 | 作用 |
|---|---|
| `org.eclipse.jdt.internal.compiler.batch.Main#compile(String[])` | 命令行 |
| `org.eclipse.jdt.internal.compiler.Compiler#beginToCompile(ICompilationUnit[])` | 解析步骤 |
| `org.eclipse.jdt.internal.compiler.Compiler#compile(ICompilationUnit[])` | 编译结束 |

**坑**：ECJ 的选项不是 javac 的选项：classpath 拼作 `--class-path`，还有 javac 没有的
`-src` / `-profile` / `-log`。用 javac 的表去解析会把 classpath 整个丢掉 —— 而 classpath
恰恰是静态分析器唯一不能缺的东西。

**坑**：ECJ 的 `ICompilationUnit#getFileName()` 返回 `char[]`，不是 `String`。
三种语言里唯一不是 String 的。

### 6.3 sjavac

sjavac **自己什么都不编译**，它驱动 javac。所以它编译的源文件由 javac 的解析 hook 看到，
编译本身由 javac 的编译 hook 看到。

sjavac 需要的是**自己的选项表**（它消费 javac 不认识的选项）和**自己的 argv**。

### 6.4 Kotlin

| 入口点 | 作用 |
|---|---|
| `org.jetbrains.kotlin.cli.common.CLICompiler#exec(...)` ×4 | 命令行 —— 4 个重载，argv 位置各不相同 |
| `org.jetbrains.kotlin.cli.jvm.K2JVMCompiler#main(String[])` | 命令行 |
| `org.jetbrains.kotlin.psi.KtPsiFactory#createFile(String)` | 解析步骤 —— 每个源文件都过这里 |
| `org.jetbrains.kotlin.cli.common.CLICompiler#exec(...)` 的返回 | 编译结束（退出码） |

**坑一**：Kotlin 的 `exec` 有**四个重载**，argv 分别在第 1、2、3 个参数：

```
exec(PrintStream, String...)                          -> argv 在 1
exec(PrintStream, MessageRenderer, String...)         -> argv 在 2
exec(PrintStream, Services, MessageRenderer, String[])-> argv 在 3
exec(MessageCollector, Services, A)                   -> 根本没有 argv
```

按"参数个数"匹配会同时命中多个；一个 advice 类绑不了四个不同的位置。所以每个重载都单独
声明，**并且带上 argv 的类型** —— 只有两个三参数的 `exec`，其中一个的第三个参数是参数对象
而不是 `String[]`。

**坑二**：Kotlin 2.x 的 CLI 走的是**分阶段（phased）编译**路径（`doExecutePhased`），
它**不经过**那些看起来像"编译步骤"的方法：

- `KotlinCoreEnvironment#getSourceFiles()` —— 环境里确实存着正确的文件，但分阶段路径
  **不问**环境要文件，advice 永远不触发
- `KotlinToJVMBytecodeCompiler#compileModules$cli(...)` —— 字节码里 `K2JVMCompiler` 确实
  在 offset 852 调它，但那条路径分阶段编译器不走
- `CoreEnvironmentUtilsKt#createSourceFilesFromSourceRoots(...)` —— 环境构造器会调它填
  `sourceFiles` 字段，但编译路径不走那儿

最后落在 `KtPsiFactory#createFile(String)`：**每一个**被编译的源文件都从这里造出来，
每条路径都过。它是唯一可靠到达的点。

**坑三**：Kotlin 的 `-source` 是**源根目录**，不是语言级别。用 javac 的表解析会把源根目录
记成语言级别 —— 而且是自信地记错。

### 6.5 Scala 3

| 入口点 | 作用 |
|---|---|
| `dotty.tools.dotc.Main#main(String[])` | 命令行（前向壳） |
| `dotty.tools.dotc.Driver#main(String[])` | 命令行（真身） |
| `dotty.tools.dotc.Driver#process(String[])` | 命令行 |
| `dotty.tools.dotc.Driver#doCompile(Compiler, List<AbstractFile>, Context)` | 编译结束 + 源文件 |

**坑**：`dotty.tools.dotc.Main#main` 只是前向壳 —— 它调 `Main$.main`，而那是
`Driver#main`（Scala 的 `object Main` 继承 `Driver` 类）。同样地，
`Main#process` 也只是 `Driver#process` 的转发。

Scala 的文件列表是 `scala.collection.immutable.List`，agent 没法直接命名这个类型，
所以用反射遍历。

### 6.6 Scala 2

| 入口点 | 作用 |
|---|---|
| `scala.tools.nsc.Main#main(String[])` / `#process(String[])` | 命令行 |
| `scala.tools.nsc.Global#compile(List<String>)` | 编译结束 + 源文件 |

Scala 2 的选项表和 Scala 3 有几处不同（`-dependencyfile`、`-Ystop-after`），所以它有自己的
advice。

---

## 7. 选项表：每个编译器一张

选项按 `(名字, 编译器)` 索引，所以 ECJ / sjavac / Kotlin / Scala 的选项不会互相撞。

三个语言对"什么是源文件"的判断也不同（扩展名）：

| 语言 | 源文件扩展名 |
|---|---|
| Java | `.java` |
| Kotlin | `.kt` / `.kts` |
| Scala | `.scala` / `.sc` |

**参数文件展开**也是分语言的：javac 和 scalac 按空白切分并去掉引号（不分组）；
**Kotlin 根本没有 `@file` 约定**，给它展开一个就是凭空发明一个功能。

---

## 8. 失败模式：这一节是本模块最有价值的部分

**这个模块的每一个坑，失败形态都是同一个：编译成功，捕获不出现，而且没有报错。**

原因链条是固定的：

1. advice 被应用了（transform 成功）
2. 但 advice 里的某个假设不对 —— 类不可见 / 参数位置不对 / 方法根本没被调用
3. `suppress = Throwable` 把异常吞掉，或者方法压根没被调到
4. 捕获不出现

所以有几条硬规矩：

| 规矩 | 为什么 |
|---|---|
| **advice 只调 bootstrap 上的 bridge** | 跨模块看不见 agent 的类 |
| **bootstrap 闭包是算出来的** | 手写清单一定会漏合成类 |
| **参数位置和 argv 的**类型**都要匹配** | 按个数匹配会命中错误的重载 |
| **入口点要对着真实 jar 用 `javap` 核过** | 猜的入口点永远不触发 |
| **bridge 里的失败要报告，不要静默吞** | 静默的失败 = 看不见的不完整捕获 |
| **端到端测试跑真实编译器** | 单元测试证明不了 advice 真的触发 |

最后一条是血的教训。mock 能证明 mock 是好的。

---

## 9. 与 ccap_compdb 的集成

`ccap_compdb` 多了五种编译器 kind（`javac` / `ecj` / `kotlin` / `scala3` / `scala2`），
各自一个 adapter，一个新的 `jvm_capture_parser.cpp` 读捕获目录。

**为什么 classpath 是单独的字段。** `CompileCommand` 里原来只有 `includeDirs`（`-I`），
那是**头文件搜索路径**。JVM 编译由 **classpath** 描述 —— 一堆 jar 和 class 目录。
这两件事**真的不一样**：消费者可能要去一个 jar 里找 class，而不是在目录里找头文件。
所以加了 `classpathDirs` / `moduleDirs` / `processorDirs` 三个独立字段，而不是复用
`includeDirs`。

**为什么每个语言一个 kind，而不是一个 "jvm" kind。** 每个编译器的选项语法不同，
adapter 相互隔离是这个项目一贯的约定。而且消费者要能区分 Kotlin 编译和 Scala 编译。

**为什么不重复解析选项。** agent 已经解析过了 —— 它有每编译器一张的选项表，在 Java 那边，
在知道编译器语义的地方。在这里再解析一遍，就是同一张表的两个实现，然后它们会各自漂移。
所以结构化字段照单全收。

CLI 侧：`ccap compdb parse <目录>` 会自动认出"JVM 捕获目录"—— 按**内容**判断
（有 `compiler` 和 `language` 字段的 JSON 目录），不是按名字，所以用户不用特意起名字。

---

## 10. 支持的 JDK：8 及以上

agent 是用 `-javaagent` 挂进**编译器的 JVM** 里的，所以它必须能在**构建所用的那个 JDK** 上
加载。至今仍在用 JDK 8 的工程很多 —— 而一个 Java 11 的 agent 在那儿根本加载不了：构建照常
成功，捕获一个都没有，而且没有任何报错。

所以 agent 自身被编译成 **Java 8 字节码**（class major version 52）。

### 10.1 两个独立的限制

"支持哪些 JDK" 有两个互相独立的答案：

| 问题 | 谁决定 | 结果 |
|---|---|---|
| 能读哪些 class 文件 | ByteBuddy 的版本 | 1.18.x 认到 Java 28，且自己的 base class 是 Java 8 |
| 能被哪个 JVM 加载 | **我们自己的** `maven.compiler.release` | `release=8` → major 52 |

**ByteBuddy 从来不是瓶颈。** 1.18.17 的 base class 是 major 52（Java 8），`META-INF/versions/`
里才是新的那些。所以"一个 ByteBuddy 版本覆盖整个 JDK 8+ 范围"是真的 —— **不需要多实现、多包**。

（唯一需要多变体的是要跑在 Java 5/6/7 上，官方有 `1.18.7-jdk5` 变体。）

### 10.2 降级不是改一行 pom

把 `release` 从 17 改成 8，编译器会报出这些错误，每一类都要真的改写：

| 类别 | 用到的地方 |
|---|---|
| `record`（Java 16） | `CaptureEntry`、`CaptureRecorder.PendingCompile`、`CompilerEntryPoints.EntryPoint` |
| switch 表达式 / 箭头规则（Java 14） | `JvmOptions`、`CompilerHooks`、`CaptureRecorder`、`CaptureEntry`、`BootstrapClosure`、`CaptureReader` |
| `instanceof` 模式匹配（Java 16） | `SourcePaths`（13 处）、`CaptureRecorder`（2 处） |
| Java 9+ API | `Path.of`、`String.isBlank`、`List.of`、`List.copyOf`、`Files.readString/writeString`、`InputStream.readAllBytes`、`Stream.toList`、`Method.trySetAccessible`、`ClassLoader.getPlatformClassLoader` |

所有 Java 9+ 的 API 调用都收进 `Jdk8` 这一个类 —— 它是"agent 允许用哪些 API"的**唯一清单**。

**测试源码不受这个限制**：它们跑在构建机上，不需要这个地板。所以编译插件把两个分开配：

```xml
<maven.compiler.release>8</maven.compiler.release>       <!-- 主源码 -->
<maven.compiler.testRelease>17</maven.compiler.testRelease> <!-- 测试源码 -->
```

### 10.3 门禁

`AgentJdkCompatibilityTest` 是这个约束的守门人，它检查的是**发布出去的字节码**，不是构建配置：

- agent 自己的 class 文件版本必须是 **52**（Java 8）；
- agent 的字节码里**不能出现**任何 Java 9+ 的 API 引用 —— 这是关键的一条，因为编译器只会
  校验*语言*级别，API 级别要靠这个门禁：`Path.of` 在新 JDK 上编译得过，在 Java 8 上运行是
  `NoSuchMethodError`，而那发生在编译器的关键路径上；
- ByteBuddy 自己的地板必须是 Java 8 —— 证明它不是瓶颈。

`Jdk8EndToEndTest` 则是**真实验证**：它在一个真的 Java 8 JVM 上跑真的编译。JDK 通过环境变量
`CCAP_TEST_JDK8` 指定；没设置就跳过（100 MB 的 JDK 不该进仓库）。

### 10.4 JDK 8 上的编译器版本

agent 在 JDK 8 上能跑，但**编译器本身**也得能在 JDK 8 上跑：

| 语言 | JDK 8 上可用的版本 | 说明 |
|---|---|---|
| javac | 8 | |
| ECJ | 任意较新版本 | ECJ 一直支持 Java 8 |
| sjavac | 随 JDK 8 | |
| Kotlin | **≤ 2.4.x** | 2.5 把最低要求提到 JDK 17 |
| Scala 3 | **3.3 LTS ~ 3.7** | 3.8 把最低要求提到 JDK 17 |
| Scala 2 | 2.13 | |

**入口点表不受影响**：Kotlin 的 `K2JVMCompiler#main` 和 `KtPsiFactory#createFile` 从 1.3 就有，
Scala 3 的 `Driver#doCompile` 和 `Run#files` 从 3.0 就有。实测（javap 核对真实 jar）：

- kotlin-compiler-embeddable **1.9.22**：`KtPsiFactory#createFile(String)` 存在；
  `CLICompiler#exec(PrintStream, String...)` **不存在**（2.x 才加），但 `K2JVMCompiler#main(String[])`
  存在，所以 argv 依然能抓到。
- scala3-compiler **3.3.4**：`Driver#doCompile`、`Driver#main`、`Run#files` 与 3.10.0 完全一致；
  而且整个 jar 的 class 文件版本都是 **52**（纯 Java 8 字节码）。

### 10.5 Scala 标准库不是跟着编译器版本走的

这是一个很容易踩的坑：**Scala 3 的编译器版本和它用的标准库版本不是同一个号**。

| Scala 3 编译器 | 配套的 `scala-library` |
|---|---|
| 3.3.x | **2.13.x**（Scala 2 的标准库，Scala 3 从里面读 `scala` 包） |
| 3.10.x | **3.10.x**（`scala-library-bootstrapped`，另一个产物） |

所以 `scala-library` 必须**单独跟踪版本**。如果图省事写成 `${scala3.version}`，
在 Scala 3.3.4 上就会去拉一个 **根本不存在**的 `scala-library:3.3.4` ——
Maven 只会留下一堆 `.lastUpdated` 空壳，依赖解析直接失败。

对应到构建上，`modules/ccap_jvm/pom.xml` 里有 `scalaLibrary.version`，
`jdk8-compilers` profile 会把它和 `scala3.version` 一起换掉。

### 10.6 编译器自己认不认这个 JDK，是另一条独立的轴

"编译器能在 JDK 8 上跑"和"编译器能在**构建所用的那个 JDK** 上跑"是两个问题。

Kotlin 的编译器带了一份 relocated 的 IntelliJ `JavaVersion`，它负责解析 `java.version`。
碰到它不认识的 JDK 版本，这个解析会直接抛：

```
java.lang.IllegalArgumentException: 25.0.1
  at com.intellij.util.lang.JavaVersion.parse(JavaVersion.java:307)
  at com.intellij.util.lang.JavaVersion.current(JavaVersion.java:176)
  at org.jetbrains.kotlin.cli.jvm.modules.JavaVersionUtilsKt.isAtLeastJava9(...)
```

这是 **Kotlin 自己的限制**（KT-73967），跟捕获无关：agent 装上了、入口点也全 hook 了，
编译器在读第一个源文件之前就死了。实测 Kotlin **2.1.0 在 JDK 25 上必然失败**，
**2.2.0 没问题**。

因此 `MultiLanguageEndToEndTest` 对 Kotlin 用的是**能力探测**，而不是写死的版本表：
它直接问编译器自己那份 `JavaVersion` 能不能读懂当前 JDK。

```
CompilerJdkSupport.compilerUnderstandsThisJdk()
```

问的正是编译器马上要问自己的那个问题，所以答案是权威的。写死
"Kotlin 2.1.0 不认 JDK 25" 那种表，每边动一次就得改一次。

> 顺带一个反直觉的点：JDK 8 场景下用的 Kotlin **恰恰是** 2.1.0 这种老版本，
> 而它们在新的 JDK 上跑不起来 —— 所以 JDK 8 的端到端测试**必须**把编译器跑在
> JDK 8 的 JVM 上（`Jdk8EndToEndTest` 就是这么做的），而不是在构建 JDK 上。

### 10.7 JDK 8 上跑 javac 要自己把 tools.jar 放进 classpath

Java 8 的编译器类**不在运行时 classpath 上**：`com.sun.tools.javac.Main` 住在
`lib/tools.jar` 里，`javac` 启动器是显式把它加进去的。

所以直接 `java -cp ... com.sun.tools.javac.Main` 会得到
`ClassNotFoundException: com.sun.tools.javac.Main`。Java 9 之后编译器变成了模块
（`jdk.compiler`），不需要这种处理 —— 这正是这个问题**只在** Java 8 测试里出现的原因。

`Jdk8EndToEndTest` 因此用 `jdk8CompilerClasspath()` 把 `tools.jar` 拼进去。

### 10.8 编译器能不能在 Java 8 上加载，是看 class 文件版本

默认 profile 的 Scala 3.10.0 整个 jar 是 **class 文件版本 61**（Java 17 字节码）。
在 Java 8 的 JVM 上加载它的主类时直接炸：

```
java.lang.UnsupportedClassVersionError: dotty/tools/dotc/Main has been compiled by a
more recent version of the Java Runtime (class file version 61.0), this version of the
Java Runtime only recognizes class file versions up to 52.0
```

`jdk8-compilers` profile 换成的 Scala 3.3.4 则**通体是 52**。

所以 `Jdk8EndToEndTest` 用 class 文件版本来判断该不该跳过，而不是靠 profile 名：

```
CompilerJdkSupport.runsOnJava8("scala3-compiler_3-")
```

这样默认 profile 下跑测试也不会因为"这个编译器不是给 Java 8 用的"而误报失败。

---

## 11. 用法

```bash
# 编译时挂上 agent
java -javaagent:ccap-jvm-capture.jar \
     -Dccap.capture.dir=/tmp/captures \
     -cp ... com.sun.tools.javac.Main -d out Hello.java

# 或者挂到已经在跑的 JVM 上
java -cp ccap-jvm-capture.jar org.ccap.jvm.capture.CaptureAgent attach <pid>

# 然后把捕获读进来
ccap compdb parse /tmp/captures
ccap compdb show /tmp/captures
ccap compdb export /tmp/captures -o compile_commands.json
```

Gradle 里用 agent 参数（因为 Gradle 会转发 `-javaagent`，但不转发 `-D`）：

```
org.gradle.jvmargs=-javaagent:/path/ccap-jvm-capture.jar=ccap.capture.dir=/tmp/captures
```

agent 的**参数**会变成系统属性 —— 这是 Gradle 里唯一能配的通道。

---

## 12. 测试

| 测试 | 覆盖 |
|---|---|
| `CompilerOptionTest` | 每编译器的选项表 |
| `JvmOptionsTest` | 选项解析、源文件识别、参数文件展开 |
| `CompilerEntryPointsTest` | 入口点表 —— 每个语言都有源文件 hook |
| `CaptureFormatTest` | 捕获的 JSON 格式 |
| `BootstrapJarTest` | bootstrap 闭包完整、advice 只调 bootstrap 类 |
| `AgentJdkCompatibilityTest` | **Java 8 地板**：class 版本 52、不许用 Java 9+ API |
| `CaptureEndToEndTest` | javac 命令行 + `javax.tools` API |
| `MultiLanguageEndToEndTest` | **ECJ / Kotlin / Scala 3 跑真实编译器** |
| `Jdk8EndToEndTest` | **在真的 Java 8 JVM 上跑真的编译** |
| `CompilerJdkSupport` | 探测编译器自己认不认当前 JDK（Kotlin 的版本解析） |

`MultiLanguageEndToEndTest` 是最关键的一个：它跑**真的**编译器（ECJ 3.33.0、
kotlin-compiler 2.2.0、scala3-compiler 3.10.0，都是 `test` scope 的依赖）。
因为 advice 和目标对不上时是**静默**的，只有真编译才能证明 advice 真的触发了。
