# calltrace —— Intel Pin 函数调用链追踪工具

使用 Intel Pin（动态二进制插桩框架）在程序**运行时**获取函数调用链。

## 目录结构

```
pin-tool/
├── calltrace.cpp      # Pin 工具源码（核心）
├── Makefile           # 基于 Pin 官方构建系统
├── README.md
└── test/
    ├── testapp.c      # 演示用的被测程序（C，普通 gcc 编译）
    ├── testcpp.cpp    # 演示用的被测程序（C++，用于验证 -demangle）
    └── Makefile
```

## 安装 Intel Pin

需要完整的 Pin 3.x 安装包（自带构建系统与头文件）：

- 下载页：<https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-binary-instrumentation-tool-downloads.html>
- 本仓库实测可用的 Pin 3.30 直链（文件名含 git 哈希，压缩后约 32MB）：
  `https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.30-98830-g1d7b601b3-gcc-linux.tar.gz`
- 解压后得到一个 `pin-3.30-98830-g1d7b601b3-gcc-linux` 目录，记下其绝对路径。

本仓库在 **Pin 3.30 + Ubuntu 24.04 + gcc 13.3** 环境下编译并实测通过。

## 编译

```bash
cd pin-tool
make PIN_ROOT=/path/to/pin-3.x TARGET=intel64
# 产物：obj-intel64/calltrace.so
```

编译测试程序（不需要 Pin）：

```bash
make -C test          # 生成 test/testapp 与 test/testcpp
```

## 运行

```bash
# 基本用法：追踪全部调用
/path/to/pin-3.x/pin -t obj-intel64/calltrace.so -o trace.out -- ./test/testapp

# 只关心某个函数，打印每次进入它时的完整调用链（比如谁调用了 malloc）
/path/to/pin-3.x/pin -t obj-intel64/calltrace.so -on malloc -- ./test/testapp

# 只追踪名字含 foo 的函数，深度限制 8
/path/to/pin-3.x/pin -t obj-intel64/calltrace.so -filter foo -maxdepth 8 -- ./test/testapp
```

## 选项

| 选项 | 说明 |
|---|---|
| `-filter <substr>` | 只追踪名字包含 `<substr>` 的例程 |
| `-maxdepth <n>` | 打印深度不超过 `<n>` |
| `-maxcalls <n>` | 累计记录 `<n>` 次调用后停止追踪 |
| `-on <substr>` | 进入名字包含 `<substr>` 的例程时，打印当前**完整活动调用链** |
| `-ret 0\|1` | 额外打印例程返回 |
| `-demangle 0\|1` | 反修饰（demangle）C++ 符号名 |
| `-verbose 0\|1` | 每行额外打印调用返回地址 |
| `-o <file>` | 输出文件，默认 `calltrace.out` |

> 注意：Pin 的 `KNOB<BOOL>` 参数需要显式取值，布尔类选项写作 `-ret 1` / `-ret 0` 的形式。
> 工具自定义参数必须经 KNOB 注册（见源码 `Knob*` 声明），否则 `PIN_Init` 会报
> `unknown option`。工具自带参数须写在 `-t <tool>.so` 之后、`--` 之前。

## 输出示例

`-on malloc` 的输出片段（`__libc_malloc` 即 glibc 中的 `malloc` 实现）：

```
>>> call chain reaching __libc_malloc @0x7f... [libc.so.6]
0 main @0x4011c6 [testapp]
  1 do_work @0x4011f6 [testapp]
    2 __libc_malloc @0x7f... [libc.so.6]
---
```

普通追踪（默认）是一棵按深度缩进的调用树，行首数字为深度：

```
1 main @0x4011c6 [testapp]
  2 do_work @0x4011f6 [testapp]
    3 fib @0x401166 [testapp]
      4 fib @0x401166 [testapp]
    3 __libc_malloc @0x7f... [libc.so.6]
    3 _IO_puts @0x7f... [libc.so.6]
```

程序退出时还会输出一份**最常调用函数**的汇总（Top 20）。

## 工作原理

1. `RTN_AddInstrumentFunction` 在每个镜像加载时对其全部例程插桩；
2. 例程入口（`IPOINT_BEFORE`）插入 `RecordCall`：打印一条带深度缩进的记录，
   并把"例程信息 + 期望返回地址（`IARG_RETURN_IP`）"压入**每线程影子调用栈**；
3. 例程内每条 `RET` 指令前插入 `RecordRet`，传入 RET 的实际跳转目标
   （`IARG_BRANCH_TARGET_ADDR`）：
   - 栈顶帧的期望返回地址 == 实际目标 → 正常弹出；
   - 不匹配（尾调用 `jmp` 泄漏、setjmp/longjmp 造成的帧残留）→
     从栈顶向下找第一个匹配帧并**一次性截断**，自动修复深度漂移；
4. `-on` 命中时直接把影子栈整体打印出来，即为当时的**活动调用链**；
5. 所有共享状态用 `PIN_GetLock` 保护，支持多线程目标程序；
6. 退出时（`Fini`）输出统计汇总。

工具自身与 Pin 运行时被 `SkipModule` 跳过，避免追踪到插桩框架本身。

## 已知限制

- **setjmp/longjmp 与异常展开**：控制流被强制回卷时影子栈中会残留若干帧，
  直到后续某个 `RET` 的返回地址与其匹配才会被一次性清理；期间 `-on` 的
  调用链可能多出一两个已不在现场的帧。
- **内联函数**：被内联的函数在二进制中不存在独立例程，不会出现。
  若想和源码对应，请用 `-O0` 编译被测程序。
- **符号缺失**：没有符号表的函数（如 `static` 且未导出）会显示为地址或
  段名（如 `.text`、`.plt.sec`）。用 `-verbose` 可查看精确地址。
- 深度/次数限制只影响打印，不影响调用计数与影子栈维护。

## 扩展思路

- 把 `callCounts` 换成**调用边**（caller→callee）计数，即可在退出时输出调用图；
- 结合 `PIN_GetSourceFileName` 输出源码行号；
- 用 `PIN_GetContextReg` / 栈指针做真实的栈深度校验（当前用返回地址配对已足够）。
