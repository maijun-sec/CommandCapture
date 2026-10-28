# CommandCapture 命名规范（参考 LLVM / clang）

本文档定义本项目所有标识符的命名规则。

参考来源：

- LLVM Coding Standards — <https://llvm.org/docs/CodingStandards.html>
- clang-tidy `readability-identifier-naming` —
  <https://releases.llvm.org/18.1.6/tools/clang/tools/extra/docs/clang-tidy/checks/readability/identifier-naming.html>

规则由 `.clang-tidy` **强制执行**（见文末「怎么保证不跑偏」）。

---

## 1. 总表

| 类别 | 风格 | 例子 |
|---|---|---|
| 类 / struct / enum | `PascalCase` | `CommandEvent` |
| 函数 / 方法 | `camelBack` | `splitCommandLine` |
| 局部变量 / 参数 | `camelBack` | `sourceFile` |
| 成员变量 | `camelBack` | `eventCollector` |
| 静态成员变量 | `camelBack` | `instanceCount` |
| 命名空间 | 小写 | `ccap`、`ccap::compdb` |
| 私有成员 | `camelBack` + 尾 `_` | `member_` |
| 常量 | `k` + `PascalCase` | `kMaxDepth` |
| 枚举值 | `PascalCase` | `CompilerKind::Gcc` |
| 宏 | `UPPER_CASE` | `CCAP_PLATFORM_WINDOWS` |
| 源文件 | `snake_case.cpp` | `command_parser.cpp` |
| 头文件 | `snake_case.h` | `command_parser.h` |
| C API 函数 | `snake_case` + 模块前缀 | `ccap_session_run` |
| C API 结构体字段 | `snake_case` | `ccap_run_options.timeout_ms` |

**私有成员**额外加尾部下划线：`name_`（clang 用 `_` 结尾表示非公开成员）。

> 注意最后一行：**只有 C API 结构体的字段**保持 snake_case。
> 内部 C++ 结构体（`ccap::RunOptions` / `ccap::cli::Options`）
> 跟其它 C++ 标识符一样用 camelBack。

---

## 2. 为什么 C API 保持 snake_case

C ABI 是**对外契约**，不能随便改。LLVM 自己的 C API
（`clang-c/Index.h`）用的就是 `clang_` 前缀 + snake_case：

```c
CXIndex clang_createIndex(int excludeDeclarationsFromPCH);
void   clang_disposeIndex(CXIndex index);
```

所以 `ccap_session_create` / `ccap_session_run` 这种写法是**符合 LLVM 惯例**的，
不是例外。

C++ 侧则一律 `camelBack` / `PascalCase`。

### 2.1 分界线：哪些名字不参与风格统一

> **ABI 名字永远不参与风格统一。**

| 类别 | 例子 | 为什么不能改 |
|---|---|---|
| C ABI 函数 / 类型 / 枚举值 / 字段 | `ccap_session_run`、`ccap_run_options.timeout_ms` | 对外契约，改了就是破坏接口 |
| glibc hook 符号 | `posix_spawn`、`posix_spawnp`、`execve` … | **动态符号名**，改了 hook 静默失效 |
| Windows NT ABI 字段 | `PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId` | 必须与 ntdll 的结构体布局一一对应 |

**真实事故**：批量改名把 Linux shim 的 `posix_spawn` 改成了 `posixSpawn`。
shim 于是导出 `posixSpawn`，glibc 的 `posix_spawn` 根本没被拦截 ——
而 GNU make 优先走 `posix_spawn`，于是 make 的子进程**一个都抓不到**，
**且没有任何报错**。

这类 bug 只在特定代码路径上显形，所以必须由**符号表断言**守住
（`tests/run_tests_linux_depth.sh` 用 `nm -D` 断言那 9 个符号存在）。

---

## 3. 常量用 `k` 前缀

clang 的写法（LLVM core 用 `UPPER_CASE`，clang 自己用 `k` 前缀）：

```cpp
static constexpr int kMaxDepth = 64;
static const char* kTranslatorPrefixes[] = {...};
```

判断标准：**值在编译期确定、且不应被修改的东西**用 `k` 前缀。

---

## 4. 命名空间用小写

LLVM 的命名空间是 `llvm`、`clang`、`mlir` —— 都是**小写**，
因为它们是**首字母缩写**（acronym），不是普通单词。

本项目对应：

```cpp
namespace ccap {              // 顶层
namespace ccap::compdb {      // 编译数据库子模块
namespace ccap::compdb::adapters {  // 编译器适配器
}
```

`.clang-tidy` 里配的是 `NamespaceCase: lower_case`，
并用 `NamespaceIgnoredRegexp: '^[a-z][a-z0-9]*$'` 放行纯小写命名空间。

---

## 5. 私有成员带尾部下划线

clang 的惯例：**私有成员**（以及其它非公开成员）用尾部下划线标记：

```cpp
class EventCollector {
private:
  std::vector<CommandEvent> events_;
  uint64_t nextSeq_ = 1;
};
```

`.clang-tidy` 必须配 `PrivateMemberSuffix: '_'`，
否则 `events_` 会被判成 "invalid case style" ——
尾部的 `_` 让它不再是纯 camelBack。

**这一条不配，整个私有成员类别就变成"永远报错"**，
而报错信息看起来像是命名真的错了。

---

## 5. 文件名用 snake_case

clang 的文件命名规则（LLVM Coding Standards）：

> 文件名全部小写，用下划线分隔，单词之间不加下划线。

例子：`clang/Frontend/CompilerInstance.cpp`、`clang/Basic/Diagnostic.h`

本项目：

```
modules/ccap_core/core/command_parser.cpp
modules/ccap_core/core/command_parser.h
modules/ccap_compdb/compdb/compiler_table.cpp
modules/ccap_compdb/adapters/gcc_adapter.cpp
```

**一个类一个文件**，文件名与主类名对应（clang 的惯例是文件名用 snake_case，
而类名是 PascalCase）：

| 文件 | 主类 |
|---|---|
| `compiler_table.cpp` | `CompilerTable` |
| `alias_resolver.cpp` | `AliasResolver` |
| `command_parser.cpp` | `CommandParser` |
| `compile_command.cpp` | `CompileCommand` |
| `filter_rules.cpp` | `FilterRules` |
| `adapter_registry.cpp` | `AdapterRegistry` |
| `gcc_adapter.cpp` | `GccAdapter` |
| `clang_adapter.cpp` | `ClangAdapter` |
| `msvc_adapter.cpp` | `MsvcAdapter` |
| `generic_adapter.cpp` | `GenericAdapter` |

---

## 6. 枚举

clang 的 `enum class` 枚举值用 `PascalCase`：

```cpp
enum class CompilerKind {
  Unknown,
  Gcc,
  Clang,
  Msvc,
  Iar,
  Generic,
};
```

注意不是 `COMPILER_KIND_GCC`（那是 C 风格 / Windows SDK 风格）。

---

## 7. 怎么保证不跑偏

### 7.1 `.clang-format`

统一排版（缩进宽度、行宽、指针对齐等）：

```bash
clang-format -i modules/ccap_core/core/*.cpp
```

### 7.2 `.clang-tidy`

`readability-identifier-naming` 把上面那张表变成**可执行的检查**：

```bash
clang-tidy -p build modules/ccap_core/core/command_parser.cpp
```

不合规的命名会被报成 warning，**不是靠人自觉**。

### 7.3 命名检查不能只看"有没有报错"

`clang-tidy` 的 `readability-identifier-naming` 有个坑：
**如果某个 kind 的规则没配，它就不检查那一类** ——
看起来"没报错"，其实是"没检查"。

所以验证方式必须是：

```bash
# 故意写错一个名字，看它报不报
clang-tidy ... # 期望：报出命名违规
```

而不是"跑一遍没报错就算过"。

### 7.4 必须覆盖 `#if` 的两侧

**clang-tidy 只检查预处理后还存在的东西。**

```cpp
#if defined(_WIN32)
    // Windows 分支
#else
    size_t name_start = 1;   // ← 违规，但 Windows 上这行不存在
#endif
```

在 Windows 上跑，`_WIN32` 分支活下来，`#else` 分支被预处理掉 ——
**那段代码从来没被分析过**。

而且 **MSVC 的 SDK 头会隐式带上 `_WIN32`**，
所以就算不显式传 `-D_WIN32`，走的还是 Windows 分支。
要覆盖另一侧必须用 `-U_WIN32` **取消**它。

所以门禁对每个文件跑**两遍**：

| pass | 参数 | 覆盖 |
|---|---|---|
| native | 默认 | 当前平台的 `#if` 分支 |
| no-win32 | `--extra-arg -U_WIN32` | 另一侧的 `#else` 分支 |

no-win32 pass 必然有编译错误（那一侧的平台专有头文件在本机不存在），
但那不影响结论 —— 编译器在报错前已经把能解析的部分检查过了。
所以只有 **native pass 的编译错误判失败**。

### 7.5 豁免规则必须能回答"我为什么豁免这个"

`.clang-tidy` 里的 `*IgnoredRegexp` 是**按名字**匹配的 ——
一个名字进了豁免列表，**任何**同名标识符都豁免。

所以豁免列表里只应该放**确实是契约**的名字：

| 该豁免 | 不该豁免 |
|---|---|
| C ABI 的函数 / 类型 / 枚举值 / 字段 | 内部 C++ 结构体的同名字段 |
| glibc hook 符号（`posix_spawn` 等） | 我们自己的同名函数 |
| Windows NT ABI 结构体字段 | 普通的内部成员 |

> 内部结构体 `ccap::RunOptions` 和 C ABI 结构体 `ccap_run_options`
> 曾经有**完全同名的字段**（`timeout_ms` 等）。
> 结果豁免规则根本区分不了它们 —— 内部的一直没改名。
> 正确做法：内部字段真的改成 camelBack，豁免只留 ABI 那些。

**判据**：加进豁免列表之前先问"这是谁的契约？"
答不上来，就是它在替你**掩盖问题**。

### 7.6 门禁怎么跑

`tests/check_naming.py` 是执行者，已作为 `naming` 测试**纳入 CTest**：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

```
4/4 Test #4: naming ...........................   Passed  157.80 sec
```

VS 生成器不产 `compile_commands.json`，脚本会**自动生成一个 sibling 的 Ninja build**
当编译数据库 —— 不会去干扰开发者正在用的构建树。

退出码的语义：

| 退出码 | 含义 | CTest 里的表现 |
|---|---|---|
| 0 | 检查通过 | passed |
| 1 | 有命名违规，或 native pass 有编译错误 | failed |
| 2 | **没法检查**（缺 clang-tidy / 缺编译数据库） | **skipped** |

退出码 2 单独成一个值，就是为了让「本机没装 clang-tidy」在 CI 上
显示成 skipped 而不是 failed —— 但日志会明确写出"未检查"，
不会让人误以为它检查过了。

### 7.7 include 路径必须合并多个来源

clang-tidy 只拿到 `compile_commands.json` 里的编译命令，
而 **MSVC 的标准头路径不在命令行上** —— 它们来自环境变量 `INCLUDE`。

于是有个陷阱：

> 环境变量 `INCLUDE` **非空**，但它指向一个**已经被卸载**的工具集版本。

这种时候如果按"INCLUDE 非空就用它"的写法，
clang-tidy 一个标准头都找不到 → 每个文件都编译错误 →
**命名警告根本不输出** → 门禁报告"0 处违规" → 绿灯。

所以 include 路径要**合并**所有来源，只丢弃**不存在**的目录：

| 来源 | 怎么拿 | 为什么需要它 |
|---|---|---|
| 环境变量 `INCLUDE` | `os.environ` | vcvars 环境里是准的 |
| MSVC 工具集 | `vswhere -find VC\Tools\MSVC\**\include` | 环境变量过期时的兜底 |
| Windows SDK | 注册表 `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` → `KitsRoot10` | **SDK 装在 VS 安装目录之外，`vswhere` 结构性地查不到它** |

注册表那条是必要的，不是冗余：`vswhere` 只认识 Visual Studio 的安装树，
而 Windows SDK 通常在 `C:\Program Files (x86)\Windows Kits\10`，
`vswhere -find` 对它永远返回空。

**Lesson —— 环境变量是可能过期的缓存，权威来源才是真的。**
"有环境变量就用环境变量"是错的默认值。

**推论 —— 门禁自己的输入也要做存在性校验。**
"INCLUDE 没设置"和"INCLUDE 指向已删除的目录"在代码里长得一模一样，
所以修复不能只加一个 `if not INCLUDE` 分支 —— 那只会把一个 bug 换成另一个。

---

## 8. 已知的风格偏离

本项目**故意**不用 LLVM 风格的地方：

| 偏离 | 原因 |
|---|---|
| 缩进 4 空格（不是 LLVM 的 2） | 本项目原本就是 4 空格；`.clang-format` 里显式设定，改动面太大不值当 |
| 头文件用 `#pragma once`（LLVM 用 include guard） | 已是现状，且 `#pragma once` 在所有目标编译器上可用 |
| C++17（LLVM 用 C++17 但禁用部分特性） | 与 LLVM 保持一致 |

**明确遵守**的：命名风格（本文档的全部内容）。

排版和命名是两件事 —— LLVM 自己也是分开管的
（`clang-format` 管排版，`clang-tidy` 管命名）。
本项目只强制**命名**，排版不强制，避免产生大量无意义的 diff。
