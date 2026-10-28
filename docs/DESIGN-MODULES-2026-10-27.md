# CommandCapture 模块化设计

**日期**: 2026-10-27
**状态**: 三个模块全部实现完成，命名规范统一，门禁已建立

---

## 1. 为什么要模块化

当前 `ccap` 是一个单 exe，把三件事混在一起：

1. **捕获**（进程创建观测 + 脚本静态分析）
2. **命令解析**（从捕获结果里认出哪些是编译命令）
3. **编译命令消费**（把编译命令转成 clang 编译数据库 / 传给分析器）

这三件事的**生命周期不同**：

- 捕获是一次性的，跑完就有数据
- 解析是对**已有数据**的操作，可以反复做、可以只做一次解析多次消费
- 消费的目标各不相同（clang 数据库 / clang 本身 / 分析器前端）

业界静态分析工具的做法正是这样分层的：

```
configure  →  配置"哪些编译器算编译命令"（一次配置，长期复用）
build      →  捕获（写 build-log.txt + 中间目录）
translate  →  消费（把编译命令翻译成分析器调用）
analyze    →  分析
```

**每一步的输入输出都是磁盘上的文件**，而不是内存里的共享状态。
这样每一步可以独立调试、独立重跑 —— 这是值得借鉴的地方。

---

## 2. 目标架构

```
┌─────────────────────────────────────────────────────────────────┐
│  ccap.exe  (入口 / CLI)                                          │
│    run    → 调用 ccap_capture                                    │
│    scan   → 调用 ccap_capture                                    │
│    compdb → 调用 ccap_compdb                                     │
└───────────────┬──────────────────────────────┬──────────────────┘
                │                              │
    ┌───────────▼──────────┐      ┌────────────▼─────────────┐
    │  ccap_capture         │      │  ccap_compdb             │
    │  (静态库 + so/dll)    │      │  (静态库 + so/dll)       │
    │                       │      │                          │
    │  - 进程捕获           │      │  - 编译器识别            │
    │    Win: DEBUG_PROCESS │      │  - 命令解析              │
    │    Lin: LD_PRELOAD    │      │  - 过滤 / 别名           │
    │  - 脚本静态分析       │      │  - clang 数据库生成      │
    │  - 统一事件模型       │      │  - 编译器适配器          │
    └───────────────────────┘      └──────────────────────────┘
                │                              │
                └──────────────┬───────────────┘
                               │
                    ┌──────────▼──────────┐
                    │  ccap_core          │
                    │  (纯跨平台，无平台)  │
                    │  - 事件模型         │
                    │  - JSON             │
                    │  - 命令行切分       │
                    │  - 路径归一化       │
                    │  - 静态分析         │
                    │  - 过滤器 / 树      │
                    └─────────────────────┘
```

### 为什么这样切

| 模块 | 职责 | 依赖 |
|---|---|---|
| `ccap_core` | 纯数据与纯算法 | 无（只有 STL） |
| `ccap_capture` | 需要 OS 能力的部分 | `ccap_core` + 平台 API |
| `ccap_compdb` | 纯文本处理 + 编译器知识 | `ccap_core` |
| `ccap`（exe） | CLI 胶水 | 上面全部 |

**关键约束**：`ccap_compdb` **不依赖** `ccap_capture`。
它只吃"一串命令行"和"一个工作目录"，不关心这些命令是怎么来的。

这带来一个直接的好处：**可以用别人产生的编译命令**（
CMake 的 `CMAKE_EXPORT_COMPILE_COMMANDS`、ninja 的 `-t commands`），
不需要自己跑一遍捕获。

---

## 3. 模块一：ccap_capture（现有能力封装）

### 产物

- `ccap_capture` 静态库（供内部链接）
- `ccap_capture.so` / `ccap_capture.dll`（**可被外部程序动态加载**）

### 为什么还要出 so/dll

用户明确要求"生成 so 和 dll"。用途：

1. **嵌入式**：别的工具（IDE 插件、CI 脚本）想用我们的捕获能力，
   不想重新编译整个 exe
2. **多语言绑定**：Python/Go/Rust 通过 FFI 调用
3. **长期演进**：ABI 稳定，主程序随便改

### C ABI

动态库必须导出**稳定的 C 接口**（C++ ABI 不可跨编译器复用）：

```c
// ccap_capture.h
typedef struct ccap_session ccap_session;   // 不透明句柄

ccap_session* ccap_session_create(void);
void         ccap_session_destroy(ccap_session* s);

// 跑一次捕获，事件以 JSON 行写到回调
typedef void (*ccap_event_cb)(void* user, const char* json_line, size_t len);
typedef void (*ccap_note_cb)(void* user, const char* note);

int ccap_session_run(ccap_session* s,
                     const char* target,
                     const char* const* args, size_t nargs,
                     const ccap_run_options* opts,
                     ccap_event_cb on_event,
                     ccap_note_cb  on_note,
                     void* user);
```

这样 Python 用 `ctypes`、Go 用 `cgo`、Rust 用 `bindgen` 都能直接用。

---

## 4. 模块二：ccap_compdb（编译命令识别与解析）

### 4.1 编译器全量列表

**参考业界工具的 `--list-compiler-types` 风格**：
各家工具都有一张"编译器类型"表，每种类型描述了
编译器可执行名的模式、参数翻译规则、系统头文件路径等。

我们的对应物：

```
CompilerKind:
  gcc        gcc, g++, cc, c++, gcc-<ver>, g++-<ver>, <target>-gcc ...
  clang      clang, clang++, clang-<ver>, clang-cl
  msvc       cl, cl.exe
  ...
```

每种 kind 带一个**匹配规则**：

- 精确名字（`gcc`、`g++`、`clang`、`cl`）
- 前缀模式（`arm-none-eabi-g++` → 交叉编译器，仍是 gcc kind）
- 后缀模式（`gcc-11` → 版本后缀）

### 4.2 软连接与别名的互认

**这是用户明确要求的核心功能。**

现实中的情况：

```
/usr/bin/gcc        -> /usr/bin/gcc-11        (硬链接/软链)
/usr/bin/cc         -> /usr/bin/gcc
/usr/bin/c++        -> /usr/bin/g++
/usr/bin/g++        -> /usr/bin/g++-11
```

工具链还可能自己定义别名：

```
/opt/toolchain/bin/arm-none-eabi-g++  ->  /opt/toolchain/bin/arm-none-eabi-gcc
```

**规则**：

1. 解析**真实路径**（`realpath` / `GetFinalPathNameByHandle`）
2. 同一真实路径的所有名字**互为别名**
3. 别名集合里**任意一个**匹配到编译器 kind，整组都算该 kind
4. 反过来，如果一个别名被配置成"要抓的编译器"，
   它的所有别名也要抓（不能因为构建系统用了 `cc` 就漏掉）

业界工具的对应行为：`configure --compiler /usr/bin/gcc` 配置了 gcc，
构建里用 `cc` 调用时也可能识别（取决于具体实现），
但**同名不同路径**（`arm-none-eabi-g++` vs `g++`）必须分开配置。

我们的做法更严格：软连接**自动**互认，
因为这是文件系统层面的确定事实，不是猜测。

### 4.3 命令来源

三种来源，都要能解析：

| 来源 | 格式 | 说明 |
|---|---|---|
| **运行时捕获** | `ccap` 自己的事件流 | `ccap run <build>` 的结果 |
| **文件** | 文本文件，每行一条命令 | 用户从日志里粘出来的 |
| **构建日志 build-log** | `EXECUTING '...'` / `COMPILING '...'` / `CWD = ...` | 直接读 build-log.txt |

常见的 build-log.txt 格式（静态分析工具的 build wrapper 产物）：

```
EXECUTING 'make all '
EXECUTING '/bin/sh -c cd qmake && make '
CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qmake
COMPILING '/export/home/acc/prevent/bin/<wrapper> g++ -c -o property.o -I. ... property.cpp '
```

规则：
- `EXECUTING` = 构建系统执行的命令，**跟编译无关**
- `COMPILING` = 被识别为编译器的调用
- `CWD = <path>` = 后续命令的工作目录

**我们的实现要能读这个格式**，这样用户可以直接用现成 build wrapper 的产物。

### 4.4 过滤：只保留真正的编译命令

要滤掉的：

- **脚本**（`sh`、`bash`、`cmd.exe`、`powershell`、`.bat`、`.sh`、`.ps1`）
- **链接器**（`ld`、`link`、`collect2`、`ld.lld`）—— 可配置
- **汇编器**（`as`、`ml64`）—— 可配置
- **归档器**（`ar`、`lib`）—— 可配置

要保留的：**真正的编译调用**（有源文件、有 `-c`、走编译器前端）。

判定逻辑（按优先级）：

1. 编译器 kind 匹配（配置里的编译器列表）→ 是编译命令
2. 命令行里有 `-c` 且有源文件 → 可能是编译命令
3. 排除列表命中 → 不是
4. 都不是 → 不是

### 4.5 提取头文件、宏、各类信息

从编译命令里抽出：

| 类别 | 来源 | 例 |
|---|---|---|
| **include 路径** | `-I<dir>` / `-I <dir>` / `/I<dir>` / `-isystem <dir>` | `-I/usr/include` |
| **宏定义** | `-D<name>[=<val>]` / `/D<name>[=<val>]` | `-DFOO=1` |
| **系统头文件** | 编译器内置搜索路径 + `-isystem` | gcc 的 `/usr/lib/gcc/x86_64-linux-gnu/11/include` |
| **源文件** | 非选项参数里扩展名是 `.c/.cc/.cpp/.cxx/.m/.mm` | `main.cpp` |
| **输出文件** | `-o <file>` | `main.obj` |
| **语言标准** | `-std=c++17` / `/std:c++17` | |
| **其它选项** | 剩下全部 | |

**系统头文件**的来源：
- 编译器自己知道（`gcc -E -v` 会打印），但我们不跑编译器
- 所以：靠**配置**（每个编译器 kind 配一组默认系统 include 路径）
- 用户可以覆盖

### 4.6 输出：clang 编译数据库

`compile_commands.json` 格式（clang 官方规范）：

```json
[
  {
    "directory": "/home/user/proj/build",
    "command": "c++ -I../include -DFOO=1 -c ../src/main.cpp",
    "file": "/home/user/proj/src/main.cpp"
  },
  {
    "directory": "...",
    "arguments": ["c++", "-I../include", "-c", "../src/main.cpp"],
    "file": "..."
  }
]
```

注意：
- `command` 和 `arguments` 二选一（我们用 `arguments`，更好处理转义）
- `directory` 是**工作目录**（必须有 —— 相对路径的 `-I` 靠它解析）
- `file` 是源文件的**绝对路径**（规范建议绝对）

---

## 5. 模块三：编译器适配器（consume 侧）

### 5.1 接口

```cpp
// ccap_compdb/compiler_adapter.h
class CompilerAdapter {
public:
    virtual ~CompilerAdapter() = default;

    // 这个适配器能不能处理这条命令？
    virtual bool matches(const CompileCommand& cmd) const = 0;

    // 把原生编译命令翻译成"给分析器用的参数"
    // （分析器风格：--sys_include / --preinclude / -D / -I ...）
    virtual std::vector<std::string> translate(
        const CompileCommand& cmd,
        const AdapterContext& ctx) const = 0;

    // 该适配器的 kind
    virtual CompilerKind kind() const = 0;
};
```

### 5.2 每类编译器一个实现，互相隔离

```
adapters/
  gcc_adapter.cpp       GCC / G++ / 交叉编译器
  clang_adapter.cpp     Clang / Clang++
  msvc_adapter.cpp      MSVC cl.exe
  generic_adapter.cpp   兜底：只做 -I/-D 的通用提取
```

**互相隔离的含义**：
- 每个 adapter 只认识自己那套选项语法
- 加一个新编译器 = 加一个新文件，**不动其它任何文件**
- adapter 之间不互相调用

### 5.3 配置驱动

配置文件（JSON）：

```json
{
  "capture": {
    "compilers": [
      { "kind": "gcc", "paths": ["/usr/bin/gcc", "/usr/bin/g++"], "enabled": true },
      { "kind": "clang", "paths": ["/usr/lib/llvm-15/bin/clang"], "enabled": true }
    ],
    "exclude": ["sh", "bash", "cmd.exe", "ld", "as", "ar", "collect2"],
    "include_aliases": true
  },
  "system_includes": {
    "gcc": ["/usr/lib/gcc/x86_64-linux-gnu/11/include", "/usr/include/c++/11"]
  },
  "output": {
    "format": "clang",
    "path": "compile_commands.json"
  }
}
```

**关键**：`compilers` 列表支持**通配前缀**，比如 `"prefix": "arm-none-eabi-"`，
匹配所有 `arm-none-eabi-*` 编译器。

---

## 6. CLI 设计

```
ccap run <script|exe> [args...]     捕获（现有）
ccap scan <script>                  静态分析（现有）
ccap compdb <子命令>                 编译数据库工具（新增）

ccap compdb capture <build-cmd...>   跑一次构建并抓编译命令
ccap compdb parse <文件...>          从文件里解析编译命令
ccap compdb list                    列出已知的编译器类型（全量列表）
ccap compdb aliases <编译器路径>     查一个编译器的所有别名
ccap compdb show <文件...>           展示解析出的编译命令详情
ccap compdb export --out <file>      生成 compile_commands.json
```

### 输入来源可组合

```
# 从运行捕获来
ccap compdb capture -- make -j8

# 从文件来
ccap compdb parse build.log

# 从构建日志来
ccap compdb parse build-int/build-log.txt

# 从已有的 compile_commands.json 来
ccap compdb parse compile_commands.json
```

---

## 7. 实施顺序

| 步骤 | 内容 | 验收 | 状态 |
|---|---|---|---|
| 1 | `ccap_core` 抽出 + `ccap_capture` 库化 + C ABI + so/dll | 现有 73 项测试不回归；so/dll 能被 `ctypes` 加载并跑一次捕获 | ✅ 完成 |
| 2 | 编译器 kind 表 + 匹配器 | `ccap compdb list` 输出全量列表 | ✅ 完成 |
| 3 | 别名解析（软连接互认） | `ccap compdb aliases /usr/bin/gcc` 列出 `cc`/`gcc-11` 等 | ✅ 完成 |
| 4 | 命令解析器（三种来源） | 能读 build-log.txt、能读普通文本、能读 json | ✅ 完成（4 种来源） |
| 5 | 过滤 + 头文件/宏提取 | `ccap compdb show` 展示结构化详情 | ✅ 完成 |
| 6 | clang 数据库导出 | 生成合法 `compile_commands.json` | ✅ 完成 |
| 7 | 编译器适配器（gcc/clang/msvc） | 参数翻译正确，adapter 互相隔离 | ✅ 完成（+ iar / generic） |
| 8 | 配置解析 + CLI 接入 | 端到端可用 | ✅ 完成 |
| 9 | LLVM/clang 命名规范统一 | C++ 侧 camelBack / PascalCase，文件 snake_case，C ABI 保持 snake_case | ✅ 完成 |
| 10 | 命名门禁 | 故意注入违规能报出；缺工具/编译错误判失败 | ✅ 完成（`tests/check_naming.py`） |
| 11 | shim ABI 符号表门禁 | `nm -D` 断言 9 个 exec/spawn 符号存在 | ✅ 完成（在 `run_tests_linux_depth.sh`） |
| 12 | 门禁双分支 pass | `#if` / `#else` 两侧都被检查 | ✅ 完成（native + `-U_WIN32`） |
| 13 | 内部结构体字段统一 camelBack | `RunOptions` / `cli::Options` 不再靠豁免蒙混 | ✅ 完成 |
| 14 | 改名脚本的字符串字面量 diff | 确认只有 include 路径变，JSON 键等契约未动 | ✅ 完成（找出并修掉 `system_includes`） |

### 落地后的实际目录

```
modules/
  ccap_core/core/            纯算法，无平台 API
    event.*  json.*  json_parser.*
    command_line.*  normalize.*
    script_scanner.*  powershell_scanner.*
    tree.*  filter.*
  ccap_capture/
    capture/                 平台实现 + C ABI
      platform.*  windows_debugger.*  peb_reader.*
      linux_preload.*  linux_shim.*  capi.cpp
    include/ccap_capture.h   稳定 C ABI（对外唯一契约）
  ccap_compdb/
    compdb/                  识别 / 解析 / 过滤 / 导出 / 配置
      compiler_table.cpp  alias_resolver.cpp  command_parser.cpp
      compile_command.cpp  filter_rules.cpp  compdb_exporter.cpp  config.cpp
    adapters/                gcc / clang / msvc / generic（互相隔离）
    include/ccap_compdb.h
  ccap/app/                  CLI 入口（main / cli / compdb_cmd）
```

CMake target：

| target | 类型 | Windows 产物 | Linux 产物 |
|---|---|---|---|
| `ccap_core` | STATIC | `ccap_core.lib` | `libccap_core.a` |
| `ccap_capture` | STATIC | `ccap_capture.lib` | `libccap_capture.a` |
| `ccap_capture_shared` | SHARED | `shared/Release/ccap_capture.dll` | `shared/libccap_capture.so` |
| `ccap_compdb` | STATIC | `ccap_compdb.lib` | `libccap_compdb.a` |
| `ccap_compdb_shared` | SHARED | `shared/Release/libccap_compdb.dll` | `shared/libccap_compdb.so` |
| `ccap` | EXE | `Release/ccap.exe` | `ccap` |
| `ccap_preload` | SHARED (Linux) | — | `ccap_shim.so` |
| `test_cmdline` / `test_compdb` / `test_capi` | EXE | `Release/` | `build-linux/` |

> 共享库输出目录**必须和静态库分开** ——
> MSVC 的 import lib 也叫 `<name>.lib`，Linux 的共享库也叫 `lib<name>.so`，
> 放一起会互相覆盖（Windows 上曾因此链不到 `run_capture`，
> Linux 上曾因此 `test_capi` 找不到 `libccap_capture.so`）。

### 实施中发现并修掉的问题

| 问题 | 根因 | 修法 |
|---|---|---|
| 构建日志带 UTF-8 BOM 时**解析出 0 条命令**且不报错 | BOM 让 `rfind("EXECUTING ", 0)` 前缀匹配失败 | `read_file` 统一剥 BOM，一处解决所有消费方 |
| 软连接别名测试在 Linux 上失败 | 测试自己先 `rename(real, alias)` 把源文件移走，软连接成了断链 | 测试改成先建文件再 `ln -sf` |
| `test_capi` 没进 CTest | 只有手工验证，没门禁 | 加 `add_test(NAME capi ...)`，按平台传不同目标 |
| Linux `libccap_capture.so` 找不到 | 共享库输出目录没设在 `shared/` | 与 Windows 分支对齐 |
| Linux 静态库链进共享库报 `relocation R_X86_64_PC32 ... can not be used when making a shared object` | 静态库没开 PIC | `ccap_core` / `ccap_capture` / `ccap_compdb` 都设 `POSITION_INDEPENDENT_CODE ON` |
| Windows 共享库覆盖同名静态库 | MSVC 的 import lib 也叫 `<name>.lib` | 共享库输出移到 `build/shared/Release/` |
| 批量改名把 `posix_spawn` 改成 `posixSpawn` | 风格统一误伤动态符号名 | 恢复符号名 + `nm -D` 符号表门禁（见 §8.4） |
| 批量改名把配置 JSON 键 `system_includes` 改成 `systemIncludes` | 脚本分不清"标识符"和"数据契约" | 恢复键名 + 逐文件 diff 字符串字面量的验证方法（见 §8.8） |
| 内部结构体字段一直是 snake_case | 被塞进 `.clang-tidy` 豁免列表，用豁免掩盖"没改" | 内部字段真改成 camelBack，豁免只留 ABI（见 §8.7） |
| 门禁看不见 `#else` 分支里的违规 | clang-tidy 只检查预处理后还存在的代码 | 每个文件跑两遍（native + `-U_WIN32`，见 §8.6） |
| 命名门禁在编译错误时显示绿色 | clang-tidy 遇编译错误就不输出命名警告 | 编译错误判失败（退出码 1），见 §8.5 |
| 命名门禁一次传多个 TU 时后半段"文件找不到" | clang-tidy 合并编译上下文，`--extra-arg` 只作用首文件 | 逐文件执行 |
| `test_capi` 编译报 C4819 | MSVC 用系统代码页（936/GBK）读 UTF-8 源文件 | 给 `test_capi` 加 `/utf-8`（其它 target 早有） |
| 树渲染器丢掉孙进程 | `std::move` 把节点搬出 pool，pid 索引仍指向 moved-from 节点 | 改成稳定节点池 + 子节点索引；加"每个事件恰好渲染一次"的回归断言 |
| PowerShell 把 `Invoke-Expression "git status"` 里的命令漏掉 | 只认 cmdlet 名，没解析参数字面量 | 解析 `Invoke-Expression` / `iex` 的**字面量**参数；变量形式不猜 |
| PowerShell 把哈希表键和 splatting 当命令 | `@{ Path = "x" }` / `@params` 是数据不是命令 | 排除哈希表键和 splatting token |
| PowerShell 把 here-string 内容当命令 | 内容是文本不是代码 | here-string 按文本处理，终止符不产生伪命令，行号保持 |
| 命名门禁在 CTest 里红（`'cstdio' file not found`） | 环境变量 `INCLUDE` 指向**已卸载**的 MSVC 版本，旧逻辑"INCLUDE 非空就用它" → 唯一的 include 路径是死的 → 编译错误 → 命名警告不输出 → 门禁静默变空检查 | 合并环境变量 + `vswhere`（MSVC 工具集）+ 注册表（Windows SDK）三个来源，只丢弃不存在的目录 |

---

## 8. 命名规范与 ABI 边界

完整规则见 [`NAMING-CONVENTION.md`](NAMING-CONVENTION.md)。这里只记录**设计决策**
和那条最重要的分界线。

### 8.1 C++ 侧：LLVM / clang 风格

| 类别 | 风格 | 例子 |
|---|---|---|
| 类 / struct / enum | `PascalCase` | `CommandEvent`、`CompilerKind::Gcc` |
| 函数 / 方法 / 变量 / 参数 | `camelBack` | `splitCommandLine` |
| 私有成员 | `camelBack` + 尾 `_` | `member_` |
| 常量 | `k` + `PascalCase` | `kMaxDepth` |
| 命名空间 | 小写 | `ccap`、`ccap::compdb` |
| 源文件 | `snake_case.cpp` | `command_parser.cpp` |

由 `.clang-tidy` 的 `readability-identifier-naming` 强制执行，
由 `tests/check_naming.py` 驱动。

### 8.2 文件重命名

13 个源文件按 clang 惯例改成 snake_case，且**文件名与主类名对应**：

| 改前 | 改后 |
|---|---|
| `core/cmdline.*` | `core/command_line.*` |
| `core/json_parse.*` | `core/json_parser.*` |
| `core/ps_scan.*` | `core/powershell_scanner.*` |
| `core/script_scan.*` | `core/script_scanner.*` |
| `compdb/compdb_export.cpp` | `compdb/compdb_exporter.cpp` |
| `capture/win_debugger.*` | `capture/windows_debugger.*` |
| `capture/win_peb.*` | `capture/peb_reader.*` |

批量改名涉及 341 个标识符。**字符串字面量、JSON 字段名、include 路径**
都受保护 —— 输出契约没变。

### 8.3 分界线：哪些名字不能跟着风格改

这是本节最重要的一条：

> **ABI 名字永远不参与风格统一。**

三类：

| 类别 | 例子 | 为什么不能改 |
|---|---|---|
| C ABI 函数 / 类型 / 枚举值 / 字段 | `ccap_session_run`、`ccap_run_options` | 对外契约，改了就是破坏接口 |
| glibc hook 符号 | `posix_spawn`、`posix_spawnp`、`execve` … | **动态符号名**，改了 hook 静默失效 |
| Windows NT ABI 字段 | `PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId` | 必须与 ntdll 的结构体布局一一对应 |

### 8.4 `posix_spawn` 事故（真实发生的）

批量改名时，Linux shim 的 `posix_spawn` / `posix_spawnp` 被改成了
`posixSpawn` / `posixSpawnp`。

**症状**：`make` 只捕获到根事件，子进程全部丢失。

**为什么没有任何报错**：shim 正常加载、正常导出、正常退出。
只是它导出的 `posixSpawn` **没人调用**，而 `posix_spawn` 仍然指向 glibc 的真实现。
`LD_PRELOAD` 是**按名字抢符号**的 —— 名字不对，抢不到。

**修法**：

1. 恢复精确符号名（9 个 exec/spawn 符号）。
2. 加**符号表门禁**：`tests/run_tests_linux_depth.sh` 里用
   `nm -D --defined-only build-linux/ccap_shim.so` 断言这 9 个符号存在。

**Lesson —— 这种 bug 只在特定代码路径上显形，光靠"跑一遍没报错"抓不住。**
门禁的价值就在于把"必须保持不变的东西"变成可断言的。

### 8.5 命名门禁的三条原则

`tests/check_naming.py` 的设计不是"跑 clang-tidy 看有没有输出"，
而是**区分"没检查"和"检查过了没违规"**：

1. **缺 clang-tidy → 退出码 2**（失败），不是跳过。
   一个"跑不通就跳过"的门禁等于没有门禁 —— 它会一直显示绿色。
2. **编译错误 → 退出码 1**（失败），并打印"命名检查**不完整**"。
   clang-tidy 在文件有编译错误时**不输出命名警告**，
   "0 条警告"不等于"符合规范"。
3. **逐文件执行**，不是一次传全部 TU。
   一次传多个文件时 clang-tidy 合并编译上下文，
   `--extra-arg` 只对第一个文件生效 → 后续文件编译失败 → 检查被跳过。

另外还必须**验证门禁自己会红**：
故意注入一个 `bad_variable_name`，确认它真的报：

```
FAIL: 1 处命名违规
modules\ccap_core\core\event.cpp:68:9  local variable 'bad_variable_name'
```

一个从没被验证过会红的门禁，跟没有门禁是一回事。

### 8.6 双分支 pass：`#else` 里的代码从来没被检查过

上面那条解决的是"文件间"的问题。还有一个"文件内"的问题：

> **clang-tidy 只检查预处理后还存在的东西。**

代码里的 `#if defined(_WIN32) ... #else ... #endif`，
在 Windows 上只有 `_WIN32` 分支活下来，`#else` 分支被预处理掉 ——
**那段代码从来没被分析过**。

实测：`normalize.cpp` 里 `#else` 分支藏了个 `size_t name_start`，
门禁报"0 处违规"。

更隐蔽的是：**MSVC 的 SDK 头会隐式带上 `_WIN32`**，
所以即使不显式传 `-D_WIN32`，走的还是 Windows 分支。
必须用 `-U_WIN32` **取消**它。

修法：每个文件跑**两遍**。

| pass | 参数 | 覆盖 |
|---|---|---|
| native | 默认 | 当前平台的 `#if` 分支 |
| no-win32 | `--extra-arg -U_WIN32` | 另一侧的 `#else` 分支 |

no-win32 pass 必然有编译错误（那一侧的平台专有头文件在本机不存在），
但那**不影响结论** —— 编译器在报错前已经把能解析的部分检查过了。
所以：**native pass 的编译错误判失败，no-win32 pass 的只记 note**。

### 8.7 豁免规则必须能回答"我为什么豁免这个"

早期版本把内部结构体 `ccap::RunOptions` 的 snake_case 字段
也塞进 `.clang-tidy` 的豁免列表（`PublicMemberIgnoredRegexp`），
门禁立刻变绿。

但那些字段**不是 ABI**：

| 结构体 | 是不是 ABI | 风格 |
|---|---|---|
| `ccap_run_options`（C ABI） | ✅ 对外契约 | snake_case |
| `ccap::RunOptions`（内部 C++） | ❌ | camelBack |
| `ccap::cli::Options`（内部 C++） | ❌ | camelBack |

而且豁免是**按名字**匹配的 —— `timeout_ms` 一旦在列表里，
**任何**叫 `timeout_ms` 的字段都豁免，包括内部那些。
一个名字同时出现在 ABI 和内部结构体里时，豁免根本区分不了。

正确做法：内部字段**真的改成** camelBack，豁免列表只留 ABI 那些。

### 8.8 批量改名不能碰数据契约

批量改名脚本分不清「标识符」和「字符串字面量里的数据契约」，
于是把配置文件的 JSON 键也改了：

```cpp
"system_includes": { ... }   // 正确
"systemIncludes": { ... }    // 改坏了
```

解析器读 `"system_includes"` —— 配置里的系统头文件路径**静默丢失**。

**Lesson —— JSON 键、命令行选项、输出字段名、文件路径不是标识符，
它们是接口。改了就是破坏兼容。**

验证手段：改名前后**逐文件 diff 所有字符串字面量**，
确认只有 include 路径变了。这次 diff 出来的唯一一处丢失，就是那个 JSON 键。

### 8.9 Windows 上的额外坑

| 坑 | 症状 | 修法 |
|---|---|---|
| MSVC 的 include 路径不在 `compile_commands.json` 里 | `'cstdint' file not found` | 合并环境变量 + `vswhere` + 注册表三个来源补 `-isystem` |
| CMake 记的是 MSVC 语法的 `-I<path>`（单横杠） | 项目自己的头文件全找不到 | 额外补 clang 语法的 `-I` |
| `_WIN32` 没定义 | 报"Linux 头文件找不到"，根因却是"Windows 宏没定义" | 补 `-D_WIN32` |
| `INCLUDE` 里有**过期**的 MSVC 版本目录 | 报一堆 file not found，真实问题被淹没 | 跳过不存在的目录，并打印 note |
| **`INCLUDE` 非空但整条都是过期目录** | 唯一的 include 路径是死的 → 编译错误 → **命名警告不输出** → 门禁显示"0 违规" | 合并所有来源（不只信 `INCLUDE`），只丢弃不存在的目录 |
| Windows SDK 装在 VS 安装目录**之外** | `vswhere -find` 对它永远返回空 | 从注册表 `KitsRoot10` 拿，取版本号最大的 SDK |

> 最后两行是同一类问题的两面：**门禁自己的输入也要做存在性校验**。
> "INCLUDE 没设置" 和 "INCLUDE 指向已删除的目录" 在代码里长得一模一样，
> 所以修法不能只是加一个 `if not INCLUDE` 分支。

---

## 9. 风险与取舍

| 风险 | 对策 |
|---|---|
| 软连接解析在 Windows 上需要 `GetFinalPathNameByHandle` | 有 `CreateFile` 权限问题时会退化成路径归一化，notes 里说明 |
| 系统头文件路径因机器而异 | 走配置，默认值只是兜底；notes 里明确标"这是配置值不是编译器实测值" |
| 编译器 kind 表永远不完整 | 提供 `generic` 兜底 + 配置文件里加自定义 kind |
| MSVC 的 `/D` 和 `-D` 混用 | msvc adapter 独立处理，不污染 gcc adapter |
| 文件带 BOM 导致前缀匹配静默失效 | `read_file` 统一剥 BOM；解析器的前缀匹配只对**内容**生效 |
| 风格统一误伤 ABI 名字 | 8.3 的分界线 + 符号表门禁；ABI 名字不进风格统一流程 |
| 命名门禁在缺工具/编译错误时假绿 | 缺工具 → 退出码 2；编译错误 → 退出码 1 |
| 命名门禁在 CTest 里因缺 clang-tidy 永久红灯 | 退出码 2 → CTest `SKIP_RETURN_CODE 2`，显示 skipped 而日志写明"未检查" |
| 命名门禁在 CTest 里因环境差异假红 | include 路径合并多个来源；逐文件执行；native / `-U_WIN32` 双 pass |

---

## 10. 参考

- **静态分析 build wrapper 的 `build-log.txt` 格式**（`EXECUTING` / `COMPILING` / `CWD =` 三种行）——
  这是本项目 `parseBuildLog()` 识别的输入格式。
  格式说明见调研报告
  [`RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](RESEARCH-COMMAND-CAPTURE-2026-10-27.md) 的引用来源。
- **编译器名字匹配是精确且区分大小写的** —— 这是我们用软连接互认（按真实路径归组）而不是按名字模糊匹配的理由。
- clang JSON Compilation Database 规范：
  <https://clang.llvm.org/docs/JSONCompilationDatabase.html>
