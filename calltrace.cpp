/*
 * calltrace.cpp
 *
 * 一个 Intel(R) Pin 动态插桩工具：在程序运行过程中获取函数调用链（调用树）。
 *
 * 原理：
 *   - 用 RTN_AddInstrumentFunction 对每个例程 (RTN) 插桩；
 *   - 在例程入口 (IPOINT_BEFORE) 插入 RecordCall：打印一条带缩进的调用记录，
 *     并把"例程信息 + 期望返回地址"压入每线程影子调用栈；
 *   - 在例程内每一条 RET 指令前插入 RecordRet：传入 RET 的实际跳转目标，
 *     按返回地址校验弹栈；栈顶不匹配时向上找匹配帧截断，
 *     从而修复尾调用（jmp 结尾）导致的帧泄漏。
 *
 * 输出的缩进行即调用树；配合 -on <substr> 还可以在某个函数被调用时
 * 打印出"完整的活动调用链"（例如想知道谁调用了 malloc）。
 *
 * 编译：
 *   make PIN_ROOT=<你的 pin 安装路径> TARGET=intel64
 *
 * 运行：
 *   $PIN_ROOT/pin -t obj-intel64/calltrace.so -o trace.out -- ./app
 *   $PIN_ROOT/pin -t obj-intel64/calltrace.so -on malloc -- ./app
 *   $PIN_ROOT/pin -t obj-intel64/calltrace.so -filter my_ -maxdepth 8 -- ./app
 *
 * 选项（KNOB<BOOL> 需要显式 0/1）：
 *   -filter <substr>   只追踪名字包含 <substr> 的例程
 *   -maxdepth <n>      打印深度不超过 <n>
 *   -maxcalls <n>      累计记录 <n> 次调用后停止
 *   -on <substr>       进入名字包含 <substr> 的例程时，打印当前完整调用链
 *   -ret 0|1           额外打印例程返回
 *   -demangle 0|1      反修饰（demangle）C++ 符号名
 *   -verbose 0|1       每行额外打印返回地址
 *   -o <file>          输出文件（默认 calltrace.out）
 */

#include "pin.H"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using std::cerr;
using std::map;
using std::ofstream;
using std::pair;
using std::sort;
using std::string;
using std::vector;

/* ------------------------------------------------------------------
 *  配置项
 *
 *  工具的自定义命令行参数必须用 Pin 的 KNOB 系统注册，
 *  PIN_Init 才会接受它们（否则报 "unknown option"）。
 *  KNOB<BOOL> 的取值形式为 "-xxx 1" / "-xxx 0"。
 * ------------------------------------------------------------------ */
static KNOB<string> KnobFilter(KNOB_MODE_WRITEONCE, "pintool",
    "filter", "", "只追踪名字包含该子串的例程");
static KNOB<INT32>  KnobMaxDepth(KNOB_MODE_WRITEONCE, "pintool",
    "maxdepth", "-1", "不打印超过该深度（-1 = 不限）");
static KNOB<UINT64> KnobMaxCalls(KNOB_MODE_WRITEONCE, "pintool",
    "maxcalls", "0", "记录 N 次调用后停止（0 = 不限）");
static KNOB<string> KnobOn(KNOB_MODE_WRITEONCE, "pintool",
    "on", "", "进入名字包含该子串的例程时打印完整调用链");
static KNOB<BOOL>   KnobRet(KNOB_MODE_WRITEONCE, "pintool",
    "ret", "0", "额外打印例程返回");
static KNOB<BOOL>   KnobDemangle(KNOB_MODE_WRITEONCE, "pintool",
    "demangle", "0", "反修饰（demangle）C++ 符号名");
static KNOB<BOOL>   KnobVerbose(KNOB_MODE_WRITEONCE, "pintool",
    "verbose", "0", "每行额外打印返回地址");
static KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "calltrace.out", "输出文件");

static string filter;          // 运行期快照，避免热路径访问 KNOB
static int    maxDepth  = -1;
static UINT64 maxCalls  = 0;
static string onSubstr;
static bool   traceRets = false;
static bool   doDemangle = false;
static bool   verbose = false;
static string outName = "calltrace.out";

/* ------------------------------------------------------------------
 *  全局状态
 * ------------------------------------------------------------------ */
static PIN_LOCK outLock;                 // 保护输出文件与下面所有共享结构
static ofstream out;
static UINT64 totalCalls = 0;
static bool   done = false;              // maxCalls 达到后置位，停止输出
static string toolBase;                  // 工具自身的 so 文件名（用于跳过）

struct ROUTINE_INFO;        // 前向声明（定义在下方）

/* 影子栈帧：例程信息 + 该帧"期望"的返回地址 */
struct Frame {
    const ROUTINE_INFO *info;
    ADDRINT retIP;          // 例程入口处用 IARG_RETURN_IP 取得
};

/* 每线程影子调用栈；当前深度即 stack.size() */
struct ThreadState {
    vector<Frame> stack;
};
static map<THREADID, ThreadState> threads;

/* 每个被插桩例程的一条记录。用 RTN_Address 作为 std::map 的键；
 * std::map 在插入新元素时不会使已有元素的引用失效，
 * 因此把 &info 传给分析函数是安全的。 */
struct ROUTINE_INFO {
    string  name;        // 符号名
    string  demangled;   // 反修饰后的名字（-demangle 时）
    string  mod;         // 所属模块（文件名）
    ADDRINT addr;        // 例程入口地址
};
static map<ADDRINT, ROUTINE_INFO> rtnInfo;

/* 每个例程的调用次数，用于退出时打印热点汇总 */
static map<const ROUTINE_INFO*, UINT64> callCounts;

static string BaseName(const string &p) {
    size_t pos = p.find_last_of("/\\");
    return pos == string::npos ? p : p.substr(pos + 1);
}

/* 跳过 Pin 运行时与工具自身，避免追踪到插桩框架本身的调用 */
static bool SkipModule(const string &mod) {
    if (mod == toolBase) return true;                 // 工具自身
    if (mod == "pin" || mod.compare(0, 4, "pin-") == 0) return true;  // pin, pin-crt, ...
    if (mod.compare(0, 6, "libpin") == 0) return true; // libpin.so, libpindwarf.so, ...
    if (mod.find("pindio") != string::npos) return true;
    return false;
}

/* ------------------------------------------------------------------
 *  分析函数（运行在目标程序被插桩的指令之前，必须尽量轻量、不可重入）
 * ------------------------------------------------------------------ */

/* 例程入口：压影子栈 + 打印调用记录 */
static VOID RecordCall(ROUTINE_INFO *info, ADDRINT retIP, THREADID tid) {
    PIN_GetLock(&outLock, tid);

    ThreadState &s = threads[tid];
    Frame f;
    f.info  = info;
    f.retIP = retIP;
    s.stack.push_back(f);
    int d = (int)s.stack.size() - 1;   // 本帧深度

    if (!done) {
        /* -on：命中时打印当前完整活动调用链（含本函数） */
        if (!onSubstr.empty() && info->name.find(onSubstr) != string::npos) {
            out << "\n>>> call chain reaching " << info->name
                << " @" << std::hex << info->addr << std::dec << "\n";
            for (size_t i = 0; i < s.stack.size(); i++) {
                for (size_t j = 0; j < i; j++) out << "  ";
                out << i << " " << s.stack[i].info->name << " @" << std::hex
                    << s.stack[i].info->addr << std::dec << " [" << s.stack[i].info->mod << "]\n";
            }
            out << "---\n";
        }

        /* 常规调用记录：按深度缩进 */
        if (maxDepth < 0 || d < maxDepth) {
            for (int i = 0; i < d; i++) out << "  ";
            out << d << " " << info->name;
            if (!info->demangled.empty())
                out << "  (" << info->demangled << ")";
            out << " @" << std::hex << info->addr << std::dec << " [" << info->mod << "]";
            if (verbose)
                out << "  ret=" << std::hex << retIP << std::dec;
            out << "\n";
        }

        callCounts[info]++;
        ++totalCalls;
        if (maxCalls && totalCalls >= maxCalls) {
            done = true;
            out << "\n... trace stopped: maxcalls (" << maxCalls << ") reached\n";
        }
    }

    PIN_ReleaseLock(&outLock);
}

/* 例程返回：按 RET 实际目标弹栈；-ret 时打印返回记录。
 * 若栈顶帧的"期望返回地址"与 RET 实际跳转目标不匹配（尾调用泄漏、
 * setjmp/longjmp 造成帧残留），则从栈顶向下找匹配帧并一次性截断，
 * 从而修复深度漂移。 */
static VOID RecordRet(ROUTINE_INFO *info, ADDRINT retTarget, THREADID tid) {
    PIN_GetLock(&outLock, tid);

    ThreadState &s = threads[tid];
    if (!s.stack.empty()) {
        if (s.stack.back().retIP == retTarget) {
            s.stack.pop_back();                       // 正常返回
        } else {
            size_t i = s.stack.size();
            while (i > 0 && s.stack[i - 1].retIP != retTarget)
                i--;
            if (i > 0)
                s.stack.resize(i - 1);                // 弹出匹配帧及其上方的泄漏帧
            else if (!s.stack.empty())
                s.stack.pop_back();                   // 未找到匹配：尽力弹顶层
        }
    }

    if (!done && traceRets) {
        int d = (int)s.stack.size();
        if (maxDepth < 0 || d < maxDepth) {
            for (int i = 0; i < d; i++) out << "  ";
            out << d << " " << info->name << " ret\n";
        }
    }

    PIN_ReleaseLock(&outLock);
}

/* ------------------------------------------------------------------
 *  插桩回调：对每个例程插入入口 / RET 两处分析调用
 * ------------------------------------------------------------------ */
static VOID RtnInstrument(RTN rtn, VOID *v) {
    if (!RTN_Valid(rtn)) return;

    string name = RTN_Name(rtn);
    if (!filter.empty() && name.find(filter) == string::npos)
        return;

    string mod = BaseName(IMG_Name(SEC_Img(RTN_Sec(rtn))));
    if (SkipModule(mod)) return;

    RTN_Open(rtn);
    INS head = RTN_InsHead(rtn);
    if (!INS_Valid(head)) {              // 无指令可插桩（例如纯跳转桩）
        RTN_Close(rtn);
        return;
    }

    ADDRINT a = RTN_Address(rtn);
    ROUTINE_INFO &info = rtnInfo[a];
    info.name = name;
    info.mod  = mod;
    info.addr = a;
    if (doDemangle)
        info.demangled = PIN_UndecorateSymbolName(name, UNDECORATION_NAME_ONLY);

    /* 入口：每次进入该例程都触发 RecordCall */
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)RecordCall,
                   IARG_PTR, &info,
                   IARG_RETURN_IP,          // 调用返回地址（在调用者一侧）
                   IARG_THREAD_ID,
                   IARG_END);

    /* 例程内每一条 RET 指令前触发 RecordRet（传入 RET 实际跳转目标，
     * 用于按返回地址校验影子栈、修复尾调用导致的帧泄漏） */
    for (INS ins = head; INS_Valid(ins); ins = INS_Next(ins)) {
        if (INS_IsRet(ins)) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordRet,
                           IARG_PTR, &info,
                           IARG_BRANCH_TARGET_ADDR,
                           IARG_THREAD_ID,
                           IARG_END);
        }
    }

    RTN_Close(rtn);
}

/* ------------------------------------------------------------------
 *  退出时的汇总
 * ------------------------------------------------------------------ */
static bool CmpCount(const pair<const ROUTINE_INFO*, UINT64> &a,
                     const pair<const ROUTINE_INFO*, UINT64> &b) {
    return a.second > b.second;
}

static VOID Fini(INT32 code, VOID *v) {
    out << "\n===== end of trace: " << totalCalls << " calls recorded =====\n";

    if (!callCounts.empty()) {
        vector<pair<const ROUTINE_INFO*, UINT64> > counts(callCounts.begin(),
                                                          callCounts.end());
        sort(counts.begin(), counts.end(), CmpCount);
        out << "\n--- most-called functions (top 20) ---\n";
        size_t n = counts.size() < 20 ? counts.size() : 20;
        for (size_t i = 0; i < n; i++)
            out << i + 1 << ". " << counts[i].second << "x "
                << counts[i].first->name << " @" << std::hex
                << counts[i].first->addr << std::dec
                << " [" << counts[i].first->mod << "]\n";
    }

    out.flush();
    out.close();
    cerr << "calltrace: " << totalCalls << " calls written to '" << outName << "'\n";
}

/* ------------------------------------------------------------------
 *  入口
 * ------------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    PIN_InitSymbols();   // 必须先于 PIN_Init 调用，用于符号解析

    if (PIN_Init(argc, argv)) {
        cerr << "Usage: " << argv[0]
             << " [-filter <substr>] [-maxdepth <n>] [-maxcalls <n>]\n"
             << "          [-on <substr>] [-ret 0|1] [-demangle 0|1] [-verbose 0|1]\n"
             << "          [-o <file>]\n"
             << "Run as: pin -t <path>/calltrace.so [knobs] -- <app> [app-args]\n";
        return 1;
    }

    /* 从 KNOB 读取配置（KNOB.Value() 在 PIN_Init 之后才有效） */
    filter     = KnobFilter.Value();
    maxDepth   = KnobMaxDepth.Value();
    maxCalls   = KnobMaxCalls.Value();
    onSubstr   = KnobOn.Value();
    traceRets  = KnobRet.Value();
    doDemangle = KnobDemangle.Value();
    verbose    = KnobVerbose.Value();
    outName    = KnobOutputFile.Value();

    toolBase = BaseName(argv[0]);   /* 工具自身的 .so 文件名（用于跳过自身） */

    out.open(outName.c_str());
    if (!out.is_open()) {
        cerr << "calltrace: cannot open output file '" << outName << "'\n";
        return 1;
    }
    out << "===== call trace (depth-indented; depth shown at line start) =====\n";

    PIN_InitLock(&outLock);

    PIN_AddFiniFunction(Fini, 0);
    RTN_AddInstrumentFunction(RtnInstrument, 0);

    PIN_StartProgram();   /* 从此不再返回 */
    return 0;
}
