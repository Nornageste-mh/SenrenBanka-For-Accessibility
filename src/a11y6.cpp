// a11y6.cpp -- Senren*Banka (千恋＊万花) 无障碍层，单进程版（krkrz V2Link 插件，32 位）
//
// 结构对齐用户的 A11yFramework（《无障碍补丁流水线》§2）：
//
//   L1 平台层（照抄 A11yFramework，只换语言；逐作不改）
//        Speech  后端链 Tolk → NVDA(nvdaControllerClient.dll) → SAPI(vtable + SPF_ASYNC)
//   L2 契约层
//        diag / [朗读] 诊断日志出口（= A11yHost.Diag / CfgDiagLog）
//   L3 逐作层
//        文本抓取：kag.historyLayer.currentInfo（本作实测）
//        界面导航：对 1920x1080 打网格问 getPrimaryLayerAt → 得到本作自己的
//                  可点击图层清单；选中后对该图层发真鼠标点击（见文件后半段）
//
// 与前几版的区别（用户指出的不优雅之处）：
//   * 没有宿主进程、没有命名管道、没有伴随窗口里的标准控件。
//     —— 文本**直接交给读屏 DLL**朗读（这就是三个仓库的做法）。
//   * 游戏进程里只留一个 message-only 隐藏窗口（不可见、不可聚焦、不在任务栏），
//     只用来跑 200 ms 定时器（抓文本 + 收开发用 REPL 命令）。
//   * SAPI 兜底必须置 SPF_ASYNC：SAPI 默认同步朗读，会阻塞主线程 —— 那才是
//     「游戏无响应」的真凶（A11yFramework/Sapi.cs 的注释里写着同一条事故）。
//
// 开发用通道（不进发布包）：D:\a11yb\sba11y_cmd.txt -> 执行 -> D:\a11yb\sba11y_res.txt
#include <windows.h>
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc（子类化游戏窗口用）
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "tjsCommHead.h"
#include "tjsInterface.h"
#include "tjsVariant.h"

// ---------------------------------------------------------------- exporter ---
struct iTVPFunctionExporter
{
	virtual bool TJS_INTF_METHOD QueryFunctions(const tjs_char **name, void **function, tjs_uint count) = 0;
	virtual bool TJS_INTF_METHOD QueryFunctionsByNarrowString(const char **name, void **function, tjs_uint count) = 0;
};

typedef void *(__stdcall *GetScriptDispatchFn)(void);
typedef void (__stdcall *ExecuteScriptFn)(const void *script, const void *name, tjs_int mode, tTJSVariant *result);
typedef tTJSVariantString *(__stdcall *AllocVariantStringFn)(const tjs_char *str);
typedef void (__cdecl *TryBlockFn)(void *);
typedef int  (__cdecl *CatchBlockFn)(void *, const void *);
typedef void (__stdcall *DoTryBlockFn)(void *, void *, void *, void *);

static GetScriptDispatchFn  g_getScriptDispatch = NULL;
static ExecuteScriptFn      g_executeScript = NULL;
static AllocVariantStringFn g_allocVariantString = NULL;
static DoTryBlockFn         g_doTryBlock = NULL;

// ---------------------------------------------------------------- POD types ---
struct TJSStrPOD { tTJSVariantString *Ptr; };

#pragma pack(push, 4)
typedef struct _VarPOD {
	union {
		struct { void *Object; void *ObjThis; } closure;
		__int64 integer;
		double real;
		tTJSVariantString *string;
		void *octet;
	} u;
	unsigned long vt;
} VarPOD;
#pragma pack(pop)

typedef struct _TryCtx {
	TJSStrPOD code;
	TJSStrPOD name;
	VarPOD result;
	int failed;
} TryCtx;

static void diag(const char *msg);
static void diagWN(const wchar_t *w, int len);
static void diagW(const wchar_t *w);
static char *g_lastCode = NULL;

static void __cdecl tryExecScript(void *p)
{
	TryCtx *c = (TryCtx *)p;
	if(g_executeScript) g_executeScript(&c->code, &c->name, 0, (tTJSVariant *)&c->result);
}

// 脚本抛异常时，把异常文本与失败的脚本一起写进诊断日志。
// 没有这一段，REPL 的失败只会表现为「结果为空」，排查全靠猜。
static int __cdecl catchExec(void *p, const void *desc)
{
	TryCtx *c = (TryCtx *)p;
	c->failed = 1;
	diag("\n[REPL] 脚本抛异常；脚本内容:\n");
	if(g_lastCode) { diag(g_lastCode); diag("\n"); }
	if(desc)
	{
		const VarPOD *fields = (const VarPOD *)desc;
		if(fields[1].vt == tvtString && fields[1].u.string)
		{
			tTJSVariantString *s = fields[1].u.string;
			const tjs_char *dp = s->LongString ? s->LongString : s->ShortString;
			if(dp && s->Length > 0 && s->Length < 8192)
			{
				diag("[REPL] 异常信息: ");
				diagWN(dp, s->Length);
				diag("\n");
			}
		}
	}
	return 0;
}

// ---------------------------------------------------------------- globals -----
static HINSTANCE g_hInst = NULL;
static HWND g_hWnd = NULL;
static volatile LONG g_quit = 0;
static wchar_t g_exeDir[MAX_PATH];

enum { TIMER_POLL = 1 };
#define POLL_MS      200   // 平时：抓文本 + 导航
#define SKIP_POLL_MS 100   // 快进时收紧：选项一出现要尽量早停手

#define CMD_PATH  "D:\\a11yb\\sba11y_cmd.txt"
#define RES_PATH  "D:\\a11yb\\sba11y_res.txt"
#define DIAG_PATH "D:\\a11yb\\a11y5_diag.txt"

static void diag(const char *msg)
{
	HANDLE h = CreateFileA(DIAG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(h == INVALID_HANDLE_VALUE) return;
	DWORD w = 0; WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL); CloseHandle(h);
}
static void diagf(const char *fmt, ...)
{
	char buf[1024]; va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap); diag(buf);
}

// 宽字符诊断：MSVC 的 %ls 在 C 区域下遇到中文会转换失败并把缓冲区清空
// （实测 [朗读] 那一行就是这样整行消失的），所以先自己转成 UTF-8 再写。
static void diagWN(const wchar_t *w, int len)
{
	if(!w || !w[0]) return;
	if(len < 0) len = (int)wcslen(w);
	if(len <= 0) return;
	int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
	if(n <= 0) return;
	char *u = (char *)malloc(n + 4);
	if(!u) return;
	WideCharToMultiByte(CP_UTF8, 0, w, len, u, n, NULL, NULL);
	u[n] = 0;
	diag(u);
	free(u);
}
static void diagW(const wchar_t *w) { diagWN(w, -1); diag("\n"); }

// ================================================================ L1 Speech ===
//
// 后端链与 A11yFramework/platform/Speech.cs 一致：
//   1. Tolk   若游戏目录里有 tolk.dll（一套接口覆盖 NVDA/JAWS/争渡/SuperNova…）
//   2. NVDA   nvdaControllerClient.dll（官方 x86 构建），实测可用
//   3. SAPI   系统语音，纯 vtable 调用，绝不走 COM 后期绑定
// 任一后端失败都只降级、绝不把异常抛回游戏主循环。

enum Backend { BK_NONE = 0, BK_TOLK, BK_NVDA, BK_SAPI };
static Backend g_backend = BK_NONE;
static char    g_backendName[128] = "无";
static char    g_verdict[512] = "";
static DWORD   g_nextRetry = 0;
static const DWORD RETRY_MS = 10000;

// ---------------- Tolk ----------------
typedef void (__cdecl *TolkLoadFn)(void);
typedef bool (__cdecl *TolkIsLoadedFn)(void);
typedef bool (__cdecl *TolkOutputFn)(const wchar_t *, bool);
typedef void (__cdecl *TolkSilenceFn)(void);
typedef void (__cdecl *TolkUnloadFn)(void);
typedef void (__cdecl *TolkTrySapiFn)(bool);
typedef wchar_t *(__cdecl *TolkDetectFn)(void);

static HMODULE         g_tolkDll = NULL;
static TolkLoadFn      g_tolkLoad = NULL;
static TolkIsLoadedFn  g_tolkIsLoaded = NULL;
static TolkOutputFn    g_tolkOutput = NULL;
static TolkSilenceFn   g_tolkSilence = NULL;
static TolkUnloadFn    g_tolkUnload = NULL;

static bool TryTolk(char *why, size_t cap)
{
	// 游戏目录（含 plugin\）→ 开发目录
	static const char *cands[] = { "tolk.dll", "plugin\\tolk.dll", "D:\\a11yb\\tolk.dll" };
	for(int i = 0; i < 3 && !g_tolkDll; i++) g_tolkDll = LoadLibraryA(cands[i]);
	if(!g_tolkDll)
	{
		// 再试 exe 同目录的绝对路径
		wchar_t p[MAX_PATH];
		_snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%stolk.dll", g_exeDir);
		g_tolkDll = LoadLibraryW(p);
	}
	if(!g_tolkDll) { _snprintf_s(why, cap, _TRUNCATE, "找不到 tolk.dll"); return false; }

	g_tolkLoad     = (TolkLoadFn)GetProcAddress(g_tolkDll, "Tolk_Load");
	g_tolkIsLoaded = (TolkIsLoadedFn)GetProcAddress(g_tolkDll, "Tolk_IsLoaded");
	g_tolkOutput   = (TolkOutputFn)GetProcAddress(g_tolkDll, "Tolk_Output");
	g_tolkSilence  = (TolkSilenceFn)GetProcAddress(g_tolkDll, "Tolk_Silence");
	g_tolkUnload   = (TolkUnloadFn)GetProcAddress(g_tolkDll, "Tolk_Unload");
	TolkTrySapiFn trySapi = (TolkTrySapiFn)GetProcAddress(g_tolkDll, "Tolk_TrySAPI");
	TolkDetectFn detect = (TolkDetectFn)GetProcAddress(g_tolkDll, "Tolk_DetectScreenReader");
	if(!g_tolkLoad || !g_tolkOutput) { _snprintf_s(why, cap, _TRUNCATE, "tolk.dll 缺导出"); return false; }

	if(trySapi) trySapi(true);
	g_tolkLoad();
	if(g_tolkIsLoaded && !g_tolkIsLoaded()) { _snprintf_s(why, cap, _TRUNCATE, "Tolk_Load 后仍未就绪"); return false; }

	wchar_t *reader = detect ? detect() : NULL;
	if(reader && reader[0]) _snprintf_s(g_backendName, sizeof(g_backendName), _TRUNCATE, "Tolk");
	else                    _snprintf_s(g_backendName, sizeof(g_backendName), _TRUNCATE, "Tolk (SAPI 兜底)");
	return true;
}

// ---------------- NVDA ----------------
// 官方 NVDA Controller Client（NV Access 稳定版 controllerClient.zip 的 x86 目录）。
// 返回 0 = 成功；非 0 为 Windows 错误码。
typedef int (__stdcall *NvdaTestFn)(void);
typedef int (__stdcall *NvdaSpeakFn)(const wchar_t *);
typedef int (__stdcall *NvdaCancelFn)(void);

static HMODULE    g_nvdaDll = NULL;
static NvdaTestFn g_nvdaTest = NULL;
static NvdaSpeakFn g_nvdaSpeak = NULL;
static NvdaCancelFn g_nvdaCancel = NULL;
static bool g_nvdaOk = false;
static DWORD g_nvdaNextProbe = 0;

static bool NvdaLoadDll(char *why, size_t cap)
{
	if(g_nvdaDll) return true;
	const wchar_t *rel[] = { L"nvdaControllerClient.dll", L"plugin\\nvdaControllerClient.dll" };
	wchar_t p[MAX_PATH];
	for(int i = 0; i < 2 && !g_nvdaDll; i++)
	{
		_snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s%s", g_exeDir, rel[i]);
		g_nvdaDll = LoadLibraryW(p);
	}
	if(!g_nvdaDll) g_nvdaDll = LoadLibraryW(L"D:\\Harness工作区\\nvda_dl\\x86\\nvdaControllerClient.dll");
	if(!g_nvdaDll) { _snprintf_s(why, cap, _TRUNCATE, "找不到 nvdaControllerClient.dll"); return false; }

	g_nvdaTest   = (NvdaTestFn)GetProcAddress(g_nvdaDll, "nvdaController_testIfRunning");
	g_nvdaSpeak  = (NvdaSpeakFn)GetProcAddress(g_nvdaDll, "nvdaController_speakText");
	g_nvdaCancel = (NvdaCancelFn)GetProcAddress(g_nvdaDll, "nvdaController_cancelSpeech");
	if(!g_nvdaTest || !g_nvdaSpeak) { _snprintf_s(why, cap, _TRUNCATE, "DLL 缺导出"); return false; }
	return true;
}

// NVDA 可能后启动，故 5 秒重试一次（对齐 Nvda.cs 的 Ready()）
static bool NvdaReady(void)
{
	if(g_nvdaOk) return true;
	DWORD now = GetTickCount();
	if(now < g_nvdaNextProbe) return false;
	g_nvdaNextProbe = now + 5000;
	if(!g_nvdaDll) return false;
	int rc = g_nvdaTest();
	if(rc == 0) { g_nvdaOk = true; return true; }
	return false;
}

// ---------------- SAPI ----------------
// vtable 槽位见 A11yFramework/Sapi.cs（ISpVoice : ISpEventSource : ISpNotifySource : IUnknown）
static const GUID CLSID_SpVoice =
	{ 0x96749377, 0x3391, 0x11d2, { 0x9e, 0xe3, 0x00, 0xc0, 0x4f, 0x79, 0x73, 0x96 } };
static const GUID IID_ISpVoice =
	{ 0x6c44df74, 0x72b9, 0x4992, { 0xa1, 0xec, 0xef, 0x99, 0x6e, 0x04, 0x22, 0xd4 } };

typedef HRESULT (__stdcall *CoInitializeExFn)(void *, DWORD);
typedef HRESULT (__stdcall *CoCreateInstanceFn)(const GUID &, void *, DWORD, const GUID &, void **);

typedef HRESULT (__stdcall *SpSpeakFn)(void *self, const wchar_t *pwcs, DWORD flags, ULONG *stream);
typedef ULONG   (__stdcall *SpReleaseFn)(void *self);
typedef HRESULT (__stdcall *SpGetRateFn)(void *self, long *rate);
typedef HRESULT (__stdcall *SpGetVolumeFn)(void *self, unsigned short *vol);

static void       *g_spVoice = NULL;
static SpSpeakFn   g_spSpeak = NULL;
static SpReleaseFn g_spRelease = NULL;
static CoInitializeExFn g_coInit = NULL;
static CoCreateInstanceFn g_coCreate = NULL;

// SPF_ASYNC 必须置位：SAPI 默认同步朗读 → 阻塞游戏主线程 → 表现为「游戏无响应」。
static const DWORD SPF_ASYNC = 1;
static const DWORD SPF_PURGEBEFORESPEAK = 2;

static bool TrySapi(char *why, size_t cap)
{
	if(g_spVoice) return true;

	HMODULE ole = LoadLibraryA("ole32.dll");
	if(!ole) { _snprintf_s(why, cap, _TRUNCATE, "ole32.dll 加载失败"); return false; }
	g_coInit   = (CoInitializeExFn)GetProcAddress(ole, "CoInitializeEx");
	g_coCreate = (CoCreateInstanceFn)GetProcAddress(ole, "CoCreateInstance");
	if(!g_coInit || !g_coCreate) { _snprintf_s(why, cap, _TRUNCATE, "ole32 缺导出"); return false; }

	// S_OK / S_FALSE / RPC_E_CHANGED_MODE 都不影响 SpVoice（threading(both)）
	g_coInit(NULL, 2 /*COINIT_APARTMENTTHREADED*/);

	void *p = NULL;
	HRESULT hr = g_coCreate(CLSID_SpVoice, NULL, 0x17 /*CLSCTX_ALL*/, IID_ISpVoice, &p);
	if(FAILED(hr) || !p) { _snprintf_s(why, cap, _TRUNCATE, "CoCreateInstance(SpVoice) 0x%08X", (unsigned)hr); return false; }

	void **vtbl = *(void ***)p;
	g_spSpeak   = (SpSpeakFn)vtbl[20];
	g_spRelease = (SpReleaseFn)vtbl[2];
	SpGetRateFn   getRate   = (SpGetRateFn)vtbl[29];
	SpGetVolumeFn getVolume = (SpGetVolumeFn)vtbl[31];

	// 先读两个属性：vtable 槽位取错会在这一行露馅，而不是等玩家需要朗读时静默失败
	long rate = 0; unsigned short vol = 0;
	HRESULT hrRate = getRate ? getRate(p, &rate) : E_FAIL;
	HRESULT hrVol  = getVolume ? getVolume(p, &vol) : E_FAIL;
	g_spVoice = p;
	diagf("SAPI: rate=%ld(0x%08X) volume=%u(0x%08X)\n", rate, (unsigned)hrRate, vol, (unsigned)hrVol);
	return true;
}

// ---------------- 对外接口 ----------------

static const char *BackendTag(void)
{
	switch(g_backend)
	{
	case BK_TOLK: return "Tolk";
	case BK_NVDA: return "NVDA";
	case BK_SAPI: return "SAPI";
	default:      return "无";
	}
}

static bool SpeechInit(void)
{
	char why[256];
	g_verdict[0] = 0;

	// 1. Tolk
	why[0] = 0;
	if(TryTolk(why, sizeof(why))) { g_backend = BK_TOLK; return true; }
	_snprintf_s(g_verdict + strlen(g_verdict), sizeof(g_verdict) - strlen(g_verdict), _TRUNCATE, "Tolk：%s；", why);

	// 2. NVDA
	why[0] = 0;
	if(NvdaLoadDll(why, sizeof(why)) && NvdaReady()) { g_backend = BK_NVDA; _snprintf_s(g_backendName, sizeof(g_backendName), _TRUNCATE, "NVDA"); return true; }
	if(!why[0]) _snprintf_s(why, sizeof(why), _TRUNCATE, "NVDA 未运行");
	_snprintf_s(g_verdict + strlen(g_verdict), sizeof(g_verdict) - strlen(g_verdict), _TRUNCATE, "NVDA：%s；", why);

	// 3. SAPI
	why[0] = 0;
	if(TrySapi(why, sizeof(why))) { g_backend = BK_SAPI; _snprintf_s(g_backendName, sizeof(g_backendName), _TRUNCATE, "SAPI"); return true; }
	_snprintf_s(g_verdict + strlen(g_verdict), sizeof(g_verdict) - strlen(g_verdict), _TRUNCATE, "SAPI：%s；", why);

	g_backend = BK_NONE;
	return false;
}

static bool SpeechReady(void)
{
	if(g_backend != BK_NONE) return true;
	DWORD now = GetTickCount();
	if(now < g_nextRetry) return false;
	g_nextRetry = now + RETRY_MS;
	if(SpeechInit()) { diagf("语音后端就绪: %s\n", g_backendName); return true; }
	return false;
}

static void SpeechSpeak(const wchar_t *text, bool interrupt)
{
	if(!text || !text[0]) return;
	if(g_backend == BK_NONE && !SpeechReady()) return;

	diag("[朗读] "); diagW(text);   // 诊断日志块：与三个仓库一致，逐条可核对

	if(g_backend == BK_TOLK)
	{
		if(g_tolkOutput && !g_tolkOutput(text, interrupt)) g_backend = BK_NONE;
	}
	else if(g_backend == BK_NVDA)
	{
		if(interrupt && g_nvdaCancel) g_nvdaCancel();
		int rc = g_nvdaSpeak(text);
		if(rc != 0) { g_nvdaOk = false; g_nvdaNextProbe = 0; }
	}
	else if(g_backend == BK_SAPI)
	{
		if(g_spSpeak)
		{
			DWORD flags = SPF_ASYNC | (interrupt ? SPF_PURGEBEFORESPEAK : 0);
			g_spSpeak(g_spVoice, text, flags, NULL);
		}
	}
}

static void SpeechStop(void)
{
	if(g_backend == BK_TOLK && g_tolkSilence) g_tolkSilence();
	else if(g_backend == BK_NVDA && g_nvdaCancel) g_nvdaCancel();
	else if(g_backend == BK_SAPI && g_spSpeak) g_spSpeak(g_spVoice, NULL, SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);
}

// ============================================================== TJS 执行入口 ===
static void ExecOnMainThread(const char *codeUtf8, char **outText)
{
	*outText = NULL;
	if(!g_executeScript || !g_allocVariantString || !g_doTryBlock) return;
	if(g_getScriptDispatch && g_getScriptDispatch() == NULL) return;

	int n = MultiByteToWideChar(CP_UTF8, 0, codeUtf8, -1, NULL, 0);
	if(n <= 0) return;
	wchar_t *codeW = (wchar_t *)malloc(n * 2);
	if(!codeW) return;
	MultiByteToWideChar(CP_UTF8, 0, codeUtf8, -1, codeW, n);

	TryCtx ctx; memset(&ctx, 0, sizeof(ctx));
	ctx.code.Ptr = g_allocVariantString(codeW);
	ctx.name.Ptr = g_allocVariantString(L"sba11y");
	free(codeW);
	if(!ctx.code.Ptr) return;
	if(g_lastCode) { free(g_lastCode); g_lastCode = NULL; }
	{
		size_t cl = strlen(codeUtf8);
		g_lastCode = (char *)malloc(cl + 1);
		if(g_lastCode) { memcpy(g_lastCode, codeUtf8, cl); g_lastCode[cl] = 0; }
	}
	g_doTryBlock((void *)tryExecScript, (void *)catchExec, NULL, &ctx);
	if(ctx.failed) return;

	VarPOD *r = &ctx.result;
	if(r->vt == tvtString && r->u.string)
	{
		tTJSVariantString *s = r->u.string;
		const tjs_char *dp = s->LongString ? s->LongString : s->ShortString;
		int len = s->Length; if(len < 0) len = 0;
		int un = WideCharToMultiByte(CP_UTF8, 0, dp, len, NULL, 0, NULL, NULL);
		if(un > 0)
		{
			char *u = (char *)malloc(un + 1);
			if(u)
			{
				WideCharToMultiByte(CP_UTF8, 0, dp, len, u, un, NULL, NULL);
				u[un] = 0;
				*outText = u;
			}
		}
	}
}

static void utf8ToW(const char *s, wchar_t *out, int cap)
{
	out[0] = 0;
	if(!s) return;
	MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
}

// ============================================================ L3 逐作：文本 ===
// 本作实测：剧情走 [sceneplay]（Emote PSB），KAG 的 onProcessCh 不触发；
// 唯一可靠来源是 kag.historyLayer.currentInfo:
//     .disp = 显示用说话人（中文）， .text = 显示用台词（中文）， .plaintext = 原文
static wchar_t g_lastName[256];
static wchar_t g_lastText[4096];
static wchar_t g_lastAnnounced[4600];
static volatile LONG g_pollBusy = 0;

// ------------------------------------------------ 选项（分歧） ----------------
//
// 实机实测（选项画面）：
//   kag.historyLayer.currentInfo.text      = 日文原文（形如「選択肢：…／…」）
//   kag.historyLayer.currentInfo.plaintext = 中文译文（形如「選項：…／…」）
//   kag.selectLayer.selects[i] = { name=「選択肢ボタン」+选项原文, text=选项原文,
//                                  target=*<场景号>_<序号><分支字母>,
//                                  left/top/width/height=按钮矩形（实测 1002x146）}
// 也就是说：**选项行与普通台词的字段是反的**（普通台词 .text 是译文、.plaintext 是原文），
// 所以不能写死用 .text，而是「挑没有假名的那个」（中文界面里正常不会出现假名）。
// 实测点选项按钮矩形的中心即可走进对应分支。
#define MAX_CHOICES 9
static char    g_choiceNames[MAX_CHOICES][80];
static wchar_t g_choiceLabels[MAX_CHOICES][128];
static int     g_choiceRect[MAX_CHOICES][4];
static int     g_choiceCount = 0;

// 探针实测（2026-09-25，非选项画面基准值）：
//   kag.selectLayer.name    = 選択肢レイヤ     kag.selectLayer.visible = 0
//   kag.selectLayer.selects.length = 0        kag.selectShowing       = 0   ← 存在，可靠
//   kag.currentLabel        = *envplay        kag.current.name        = 表メッセージレイヤ0
// 注意：kag.current 在**读档画面**上仍然是 表メッセージレイヤ0（陈旧值），
// 所以它不能用来判断「现在是不是在剧情里」；kag.selectShowing 可以。
static volatile LONG g_selectShowing = 0;

// ------------------------------------------------------------------ 快进 -----
//
// 本作**完全不收键盘**（真键盘的空格/回车/以及 PostMessage 送进去的空格/回车，
// 剧情都纹丝不动；只有鼠标点击能推进）。所以日式 AVG 惯例的「按住 Ctrl 快进」
// 在本作里根本触发不了 —— Ctrl 是死的。这里由我们自己做：按住 Ctrl 时按固定
// 节拍向消息区发合成点击，一旦 kag.selectShowing 变真就立刻停手，把选项让给
// PollDialogue 去念（数字键选择那套）。
//
// 安全阀：不在剧情里时绝不能一直点（读档画面的存档格正好落在推进点上，
// 连点可能变成双击 → 读档）。所以头几下是「试推」：每 600 ms 点一下（超过系统
// 双击间隔），一旦文本变了才算确认进入剧情；试推期间文本不变就立刻收手。
static bool  g_skip = false;
static DWORD g_skipNext = 0;
static DWORD g_skipBegan = 0;
static int   g_skipClicks = 0;
static int   g_skipStuck = 0;
static bool  g_skipConfirmed = false;
static wchar_t g_skipWatch[512];
#define SKIP_PROBE_MS    600    // 试推节拍（大于系统双击间隔）
#define SKIP_PROBE_TRIES 3      // 试推最多点几下
#define SKIP_RUN_MS      SKIP_POLL_MS   // 确认后：跟着收紧后的定时器走
#define SKIP_STUCK_LIMIT 6      // 确认后连续几拍文本没变就收手
#define SKIP_MAX_CLICKS  4000


// 假名（平假名/片假名）—— 中文译文里不该有
static bool HasKana(const wchar_t *s)
{
	if(!s) return false;
	for(int i = 0; s[i]; i++)
	{
		wchar_t c = s[i];
		if((c >= 0x3041 && c <= 0x309F) || (c >= 0x30A0 && c <= 0x30FF)) return true;
	}
	return false;
}

// 解析 kPoll 送回来的选项清单：每条 "层名,left,top,width,height"，条间用 \x01
static void ParseChoices(const char *s)
{
	g_choiceCount = 0;
	if(!s || !s[0]) return;
	const char *p = s;
	while(*p && g_choiceCount < MAX_CHOICES)
	{
		char item[256]; int k = 0;
		while(*p && *p != '\x01' && k < (int)sizeof(item) - 1) item[k++] = *p++;
		item[k] = 0;
		if(*p == '\x01') p++;
		if(!item[0]) continue;
		int l = 0, t = 0, w = 0, h = 0;
		char nm[80]; nm[0] = 0;
		if(sscanf_s(item, "%79[^,],%d,%d,%d,%d", nm, (unsigned)sizeof(nm), &l, &t, &w, &h) == 5)
		{
			strncpy_s(g_choiceNames[g_choiceCount], sizeof(g_choiceNames[0]), nm, _TRUNCATE);
			g_choiceRect[g_choiceCount][0] = l;
			g_choiceRect[g_choiceCount][1] = t;
			g_choiceRect[g_choiceCount][2] = w;
			g_choiceRect[g_choiceCount][3] = h;
			g_choiceCount++;
		}
	}
}

// 选项每一条的中文标签：从游戏自己给的本地化摘要里切出来
//   摘要形如「選擇項：說實話/敷衍過去」→ 跳过冒号，按 / 切开，顺序与画面上的按钮一致。
// 切不出正好对上条数时退回层名（宁可生硬，也不编）。
static void BuildChoiceLabels(const wchar_t *summary)
{
	if(g_choiceCount <= 0) return;
	for(int i = 0; i < g_choiceCount; i++) utf8ToW(g_choiceNames[i], g_choiceLabels[i], 128);

	const wchar_t *p = summary ? summary : L"";
	for(const wchar_t *q = p; *q; q++)
	{
		if(*q == L'：' || *q == L':') { p = q + 1; break; }
	}
	int idx = 0;
	const wchar_t *start = p;
	for(const wchar_t *q = p; ; q++)
	{
		if(*q == L'/' || *q == L'／' || *q == 0)
		{
			const wchar_t *s = start;
			size_t len = (size_t)(q - s);
			while(len > 0 && (s[0] == L' ' || s[0] == L'　')) { s++; len--; }
			while(len > 0 && (s[len-1] == L' ' || s[len-1] == L'　')) len--;
			if(idx < g_choiceCount && len > 0 && len < 127)
			{
				wcsncpy_s(g_choiceLabels[idx], 128, s, len);
				g_choiceLabels[idx][len] = 0;
			}
			idx++;
			start = q + 1;
			if(*q == 0) break;
		}
	}
	if(idx != g_choiceCount)
	{
		for(int i = 0; i < g_choiceCount; i++) utf8ToW(g_choiceNames[i], g_choiceLabels[i], 128);
	}
}

static void SpeakLine(const wchar_t *name, const wchar_t *text)
{
	if(name && name[0]) _snwprintf_s(g_lastAnnounced, 4600, _TRUNCATE, L"%s，%s", name, text);
	else                _snwprintf_s(g_lastAnnounced, 4600, _TRUNCATE, L"%s", text);
	SpeechSpeak(g_lastAnnounced, true);
}

static void PollDialogue(void)
{
	if(InterlockedExchange(&g_pollBusy, 1) == 1) return;

	// 开发用 REPL（发布时删掉这一段即可）
	FILE *cf = NULL; fopen_s(&cf, CMD_PATH, "rb");
	if(cf)
	{
		static char code[64 * 1024];
		size_t cn = fread(code, 1, sizeof(code) - 1, cf);
		fclose(cf);
		code[cn] = 0; DeleteFileA(CMD_PATH);
		char *p = code;
		if(cn >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
		char *res = NULL;
		ExecOnMainThread(p, &res);
		FILE *rf = NULL; fopen_s(&rf, RES_PATH, "wb");
		if(rf) { fprintf(rf, "OK %s", res ? res : ""); fclose(rf); }
		if(res) free(res);
	}

	static const char *kPoll =
		"var __r = \"\";"
		"try {"
		"  var __h = global.kag.historyLayer;"
		"  if (__h != void) {"
		"    var __i = __h.currentInfo;"
		"    if (__i != void) {"
		"      var __n = \"\"; var __t = \"\"; var __p = \"\";"
		"      try { __n = \"\" + __i.disp; } catch(e1) { __n = \"\"; }"
		"      try { __t = \"\" + __i.text; } catch(e2) { __t = \"\"; }"
		"      try { __p = \"\" + __i.plaintext; } catch(e3) { __p = \"\"; }"
		"      var __m = \"\";"
		"      try {"
		"        var __ren = global.kag.renderMsgwinPlugin.getRender(void, 1);"
		"        var __ct = __ren.currentText;"
		"        for (var __q = 0; __q < __ct.length; __q++) {"
		"          try { __m += \"[\" + __q + \"]\" + __ct[__q].text + \" \"; } catch(q1) {}"
		"        }"
		"      } catch(q2) {}"
		"      var __c = \"\";"
		"      try {"
		"        var __ss = global.kag.selectLayer.selects;"
		"        for (var __i2 = 0; __i2 < __ss.length && __i2 < 9; __i2++) {"
		"          var __o = __ss[__i2];"
		"          try { __c += (\"\" + __o.name) + \",\" + __o.left + \",\" + __o.top + \",\" +"
		"                       __o.width + \",\" + __o.height + \"\\x01\"; } catch(y1) {}"
		"        }"
		"      } catch(y2) {}"
		"      var __s = \"-1\";"
		"      try { __s = \"\" + global.kag.selectShowing; } catch(y3) { __s = \"-1\"; }"
		"      __r = __n + \"~|~\" + __t + \"~|~\" + __p + \"~|~\" + __m + \"~|~\" + __c + \"~|~\" + __s;"
		"    }"
		"  }"
		"} catch(e) { __r = \"\"; }"
		"return __r;";
	char *out = NULL;
	ExecOnMainThread(kPoll, &out);
	InterlockedExchange(&g_pollBusy, 0);
	if(!out) return;

	char *sep = strstr(out, "~|~");
	if(!sep) { free(out); return; }
	*sep = 0;
	char *nameU = out, *textU = sep + 3;
	char *plainU = strstr(textU, "~|~");
	if(plainU) { *plainU = 0; plainU += 3; }
	char *rendU = plainU ? strstr(plainU, "~|~") : NULL;
	if(rendU) { *rendU = 0; rendU += 3; }
	char *choiceU = rendU ? strstr(rendU, "~|~") : NULL;
	if(choiceU) { *choiceU = 0; choiceU += 3; }
	char *showU = choiceU ? strstr(choiceU, "~|~") : NULL;
	if(showU) { *showU = 0; showU += 3; }

	ParseChoices(choiceU);
	g_selectShowing = showU ? atoi(showU) : -1;

	// 消息窗渲染插件里的当前文本（原文，只作诊断）。
	// 只在内容变了时写进日志 —— 排查「选项朗读与画面不符」的一手证据。
	if(rendU && rendU[0])
	{
		static char lastRend[4096];
		if(strcmp(rendU, lastRend) != 0)
		{
			strncpy_s(lastRend, sizeof(lastRend), rendU, _TRUNCATE);
			diag("[文本] ");
			diag(rendU);
			diag("\n");
		}
	}

	wchar_t name[256], text[4096], plain[4096];
	utf8ToW(nameU, name, 256);
	utf8ToW(textU, text, 4096);
	utf8ToW(plainU, plain, 4096);
	free(out);

	// 选本地化的那一条：普通台词 .text 是译文、.plaintext 是原文；选项行两者相反。
	// 判据用「假名」，不依赖字段名 —— 含假名的判为原文，另一个就是译文。
	const wchar_t *chosen = text;
	if(plain[0])
	{
		bool tk = HasKana(text), pk = HasKana(plain);
		if(!text[0] || (tk && !pk)) chosen = plain;
	}
	if(!chosen[0]) return;
	if(wcscmp(chosen, g_lastText) == 0 && wcscmp(name, g_lastName) == 0) return;
	wcscpy_s(g_lastName, 256, name);
	wcscpy_s(g_lastText, 4096, chosen);

	wchar_t line[4096];
	wcsncpy_s(line, 4096, chosen, _TRUNCATE);
	size_t tl = wcslen(line);
	while(tl > 0 && (line[tl-1] == L'\n' || line[tl-1] == L'\r' || line[tl-1] == L' ')) line[--tl] = 0;
	if(!line[0]) return;

	if(g_choiceCount > 0)
	{
		// 选项场景：逐条报出中文选项，并说明数字键选择（框架的「数字键选择选项」）
		BuildChoiceLabels(line);
		wchar_t say[1024];
		int off = _snwprintf_s(say, 1024, _TRUNCATE, L"选项 %d 个。", g_choiceCount);
		for(int i = 0; i < g_choiceCount && off > 0 && off < 800; i++)
			off += _snwprintf_s(say + off, 1024 - off, _TRUNCATE, L"%d，%ls。", i + 1, g_choiceLabels[i]);
		if(off > 0 && off < 950) _snwprintf_s(say + off, 1024 - off, _TRUNCATE, L"按数字键选择。");
		wcsncpy_s(g_lastAnnounced, 4600, say, _TRUNCATE);
		SpeechSpeak(say, true);
	}
	else
	{
		// 快进时不朗读过路台词（免得语音队列把选项那一条淹了）；
		// g_lastText 上面已经更新，快进的「推得动推不动」判据不受影响。
		if(!g_skip) SpeakLine(name, line);
	}
}

// ====================================================== L3 逐作：界面导航 =====
//
// 与 A11yFramework/platform/UiNav.cs 同构，只是「控件」在本作里是 KAG 的**图层**：
//
//   控件发现  对逻辑分辨率 1920x1080 打 40 px 网格，用 kag.getPrimaryLayerAt(x,y)
//             问「这一点上是哪一层」，按层名去重 → 得到的就是**游戏自己的**可点击
//             图层清单。实测：标题菜单 start/load/continue/flowchart/system/exit；
//             游戏内底部工具栏 hold/custom/save/load/qsave/qload/option/prev/…/hide。
//   激活      对该图层一个「确实命中过」的点发真的鼠标点击
//             （WM_MOUSEMOVE + WM_LBUTTONDOWN/UP，走游戏自己的点击派发）。
//             实测：点 start 进入正片、点 log 打开 BACKLOG、点 start 层中间推进对话。
//   按键      Tab 进出导航；↑↓ 选择；回车/空格 激活；Home/End 首尾；
//             PgUp/PgDn 前后跳 5 项；Esc 退出导航；Backspace 重读上一句。
//
// 纪律（同流水线 §7）：层名一个都不许猜 —— 全部来自上面的网格探针转储；
// 别名表里没把握的层名就照念层名本身，绝不编一个中文词冒充。
#define NAV_MAX_ITEMS 128
#define NAV_LOGICAL_W 1920
#define NAV_LOGICAL_H 1080
#define NAV_GRID 40

struct NavItem {
	char    name[64];
	wchar_t label[128];
	int hx, hy;                    // 命中的逻辑坐标（点这里一定落在该图层上）
	int left, top, w, h;
	bool isChild;                  // 是不是容器层的子控件（存档槽这类）
};

static NavItem g_items[NAV_MAX_ITEMS];
static int     g_itemCount = 0;
static bool    g_nav = false;
static int     g_sel = 0;
static int     g_slidePct = 50;   // 滑条类控件当前用左/右键点到的百分比
static bool    g_scanPending = false;
static DWORD   g_scanAt = 0;
static DWORD   g_rescanAt = 0;
static int     g_scanTries = 0;
static bool    g_scanOnEnter = false;   // 这一次扫描是不是「刚进导航模式」
static HWND    g_gameWnd = NULL;
static bool    g_subclassed = false;
static char    g_namesBuf[8192];

static void EnsureGameHook(void);   // 定义在下面（子类化游戏窗口）

// 表/裏/トップ这类图层族与整屏容器不是控件（尺寸过滤已在 TJS 侧做，这里是兜底）
static bool Excluded(const char *name)
{
	if(!name[0]) return true;
	if(_stricmp(name, "touchUI") == 0) return true;
	if(strstr(name, "\xE3\x83\xAC\xE3\x82\xA4\xE3\x83\xA4")) return true;   // レイヤ
	if(strncmp(name, "\xE8\xA1\xA8", 3) == 0) return true;                  // 表
	if(strncmp(name, "\xE8\xA3\x8F", 3) == 0) return true;                  // 裏
	if(strncmp(name, "\xE3\x83\x88", 3) == 0) return true;                  // ト

	// 纯装饰层：不是控件，做成导航项只是噪声（设置界面实测）。
	// 依据：设置界面 40 项转储里这几个的名字与 rect ——
	//   chaphidetime_val(59x21) chap_num_bg(94x34) 是「章节标题显示时间」那一行的
	//   数值框与底图，真正的滑条是 chaphidetime(426x35)；helptext(747x61) 是左下角
	//   说明文字框，不是按钮。数值/滑条按左右方向键调整，不需要单独导航到框上。
	if(_stricmp(name, "chaphidetime_val") == 0) return true;
	if(_stricmp(name, "chap_num_bg") == 0) return true;
	if(_stricmp(name, "mushidetime_val") == 0) return true;
	if(_stricmp(name, "music_num_bg") == 0) return true;
	if(_stricmp(name, "helptext") == 0) return true;
	if(_stricmp(name, "helpbase") == 0) return true;   // 标题画面实测多出来的帮助底图
	return false;
}

// 网格探针：把「哪一层在哪一点上被命中」问出来，并把容器层的**子控件**也列出来。
//
// 依据（实机探针转储）：
//   · 存档/读档界面的 16 个存档槽不是独立图层，而是 ParentHackLayer 的 children：
//       ParentHackLayer rect=698,195,1104x720
//       children[0]=item00 0,0,252x228   children[1]=item01 284,0,252x228  …
//     子控件的坐标是**相对父层**的，所以绝对坐标 = 父层 left/top + 子控件 left/top。
//   · 尺寸过滤的依据：整屏图层（表-背景 1920x1440、消息层 1920x1080）与容器层
//     （touchUI 2763x282）都不是控件。
//   · 每一次属性访问都必须单独 try/catch：实测有些点上 getPrimaryLayerAt 返回的
//     不是 void，而是一个「半空」的包装对象，读 .name 就抛 "Accessing to null object"
//     —— 曾因此整段扫描静默失败（表现为「探针脚本没有返回」）。
// 控件发现：网格探针 + 容器子控件枚举。
//
// 【实测教训 · 设置界面】早先这里有一行 `if(w>=1900)continue;` 把整屏容器整层丢掉。
// 设置界面的选项**全是** `表メッセージレイヤ1`（宽 1928）的子控件，于是四十多个子控件
// 一起被丢，只剩下被 40px 网格恰好直接命中的那几个按钮层当「叶子」收进来 ——
// 结果每个选项只碰到一半：显示模式只念到「全屏」念不到「窗口」，总在最前只念到「开」
// 念不到「关」，老板键功能、显示说话人表情整行消失。
// 现在：容器照样收集，然后用它的 children 发精确 rect；剩下的才当叶子层。
static const char *kScanScript =
	"var r=\"\";var k=global.kag;"
	"var NM=new Array();var HX=new Array();var HY=new Array();"
	"var LL=new Array();var TT=new Array();var WW=new Array();var HH=new Array();"
	"var OB=new Array();var KC=new Array();var SN=new Array();"
	"for(var y=0;y<1080;y+=40){for(var x=0;x<1920;x+=40){"
	"var l=k.getPrimaryLayerAt(x,y);if(l==void)continue;"
	"var n=\"\";try{n=\"\"+l.name;}catch(g1){continue;}"
	"if(n==\"\")continue;"
	"var vis=1;var op=255;var w=0;var h=0;var lf=0;var tp=0;"
	"try{vis=l.visible;}catch(g2){}"
	"try{op=l.opacity;}catch(g3){}"
	"if(vis!=1||op<128)continue;"
	"try{w=l.width;}catch(g4){}"
	"try{h=l.height;}catch(g5){}"
	"try{lf=l.left;}catch(g6){}"
	"try{tp=l.top;}catch(g7){}"
	"var idx=-1;for(var i=0;i<NM.length;i++){if(NM[i]==n){idx=i;break;}}"
	"if(idx<0){NM.push(n);HX.push(x);HY.push(y);LL.push(lf);TT.push(tp);"
	"WW.push(w);HH.push(h);OB.push(l);KC.push(0);}"
	"}}"
	"for(var j=0;j<NM.length;j++){"
	"var lay=OB[j];var cn=0;try{cn=lay.children.length;}catch(c0){cn=0;}"
	"if(cn<2)continue;"
	"for(var ci=0;ci<cn&&ci<120;ci++){"
	"var c=void;try{c=lay.children[ci];}catch(c1){continue;}"
	"if(c==void)continue;"
	"var cnm=\"\";try{cnm=\"\"+c.name;}catch(c2){continue;}"
	"if(cnm==\"\")continue;"
	"var cv=1;try{cv=c.visible;}catch(c3){}"
	"if(cv!=1)continue;"
	"var cw=0;var chh=0;var cl=0;var ct=0;"
	"try{cw=c.width;}catch(c4){}"
	"try{chh=c.height;}catch(c5){}"
	"try{cl=c.left;}catch(c6){}"
	"try{ct=c.top;}catch(c7){}"
	"if(cw<20||chh<20)continue;"
	"if(cw>1600||chh>1000)continue;"
	"var dp=0;for(var d=0;d<SN.length;d++){if(SN[d]==cnm){dp=1;break;}}"
	"if(dp==1)continue;"
	"SN.push(cnm);KC[j]=1;"
	"r+=\"C\\t\"+cnm+\"\\t\"+(LL[j]+cl+cw/2)+\",\"+(TT[j]+ct+chh/2)+\",\"+cw+\",\"+chh+\",\"+NM[j]+\"|\";"
	"}}"
	"for(var m=0;m<NM.length;m++){"
	"if(KC[m]==1)continue;"
	"if(WW[m]>1600||HH[m]>1000||WW[m]<20||HH[m]<20)continue;"
	"var dp2=0;for(var e2=0;e2<SN.length;e2++){if(SN[e2]==NM[m]){dp2=1;break;}}"
	"if(dp2==1)continue;"
	"r+=\"L\\t\"+NM[m]+\"\\t\"+HX[m]+\",\"+HY[m]+\",\"+WW[m]+\",\"+HH[m]+\"|\";"
	"}"
	"return r;";

// 层名 → 中文标签。每条的依据都写在注释里（层名来自探针转储，中文来自界面截图核对）。
// 想改标签不必重新编译：把同名的行写进 plugin\a11y_labels.ini，ini 里的会覆盖这里。
struct NameAlias { const char *name; const wchar_t *label; };
static const NameAlias g_alias[] = {
	// ---- 标题菜单：网格探针实测出 6 个图层，rect 依次为
	//      start(64,355,219x53) load(65,470,218x55) continue(70,586,328x56)
	//      flowchart(63,705,334x54) system(63,820,269x56) exit(65,937,252x56)
	{ "start",     L"开始游戏" },
	{ "load",      L"读取存档" },
	{ "continue",  L"继续游戏" },
	{ "flowchart", L"流程图" },
	{ "system",    L"系统设置" },
	{ "exit",      L"退出游戏" },
	{ "language",  L"界面语言" },
	// ---- 游戏内底部工具栏：探针实测的一批 48x48 图层
	//      （鼠标不在底部时整条工具栏会滑走，所以进入导航前先把它唤醒）
	{ "hold",      L"保持" },
	{ "custom",    L"自定义" },
	{ "save",      L"存档" },
	{ "qsave",     L"快速存档" },
	{ "qload",     L"快速读档" },
	{ "option",    L"设置" },
	{ "log",       L"回想（已读文本）" },      // 实点验证：打开 BACKLOG 面板
	{ "auto",      L"自动播放" },
	{ "skip",      L"快进" },
	{ "hide",      L"隐藏界面" },
	{ "scnchart",  L"场景流程" },
	{ "volchg",    L"音量" },
	// ---- 读档/存档界面（tw 界面截图逐项核对；层名来自 20px 网格探针）
	{ "copy",      L"複製存檔" },
	{ "move",      L"替換存檔" },
	{ "edit",      L"編輯註釋" },
	{ "del",       L"刪除存檔" },
	{ "thumbview", L"顯示縮圖" },
	{ "edithold",  L"選中編輯按鈕" },
	{ "title",     L"回到標題界面" },
	{ "title2",    L"回到標題界面" },
	{ "to_quick",  L"快速讀檔" },
	{ "to_voice",  L"語音收藏夾" },
	{ "slider",    L"捲動軸" },
	{ "page_up1",  L"上一頁" },
	{ "page_up10", L"往前十頁" },
	{ "page_add",  L"下一頁" },
	{ "page_end",  L"最後一頁" },
	// ---- 设置界面（实机截图 shots\settings_map.png + 40 项探针转储逐项对照）
	//      每个选项是**左右两个独立子控件**，两半各有名字，所以两半各有标签。
	//      层名描述底层标志位、画面显示人话，两者有时相反：noeff_off（左，高亮）
	//      是「画面效果 开」，因为底层标志叫 noEffect（关掉特效 = noeffect=true）。
	{ "page0",     L"画面显示" },
	{ "page1",     L"游戏设置 1" },
	{ "page2",     L"游戏设置 2" },
	{ "page3",     L"文本" },
	{ "page4",     L"音频" },
	{ "page5",     L"确认" },
	{ "page6",     L"鼠标" },
	{ "page7",     L"键盘" },
	{ "window",    L"显示模式：窗口" },
	{ "fullscreen",L"显示模式：全屏" },
	{ "sqr_on",    L"画面比例：4:3" },
	{ "sqr_off",   L"画面比例：16:9" },
	{ "noeff_off", L"画面效果：开" },
	{ "noeff_on",  L"画面效果：关" },
	{ "bganim_on", L"动画效果：开" },
	{ "bganim_off",L"动画效果：关" },
	{ "esc_cancel",L"ESC 键功能：鼠标右键" },
	{ "esc_boss",  L"ESC 键功能：老板键" },
	{ "boss_min",  L"老板键功能：最小化" },
	{ "boss_img1", L"老板键功能：图片 1" },
	{ "boss_img2", L"老板键功能：图片 2" },
	{ "boss_user", L"老板键功能：用户指定" },
	{ "topmost_on",L"总在最前：开" },
	{ "topmost_off",L"总在最前：关" },
	{ "asicon",    L"功能区域：状态图标" },
	{ "showprog",  L"功能区域：进度条" },
	{ "showmenu",  L"功能区域：窗口菜单" },
	{ "touchmode", L"功能区域：触控按钮" },
	{ "chaphidetime", L"章节标题显示时间" },
	{ "mushidetime",  L"背景音乐曲名显示时间" },
	{ "facemode_on",  L"显示说话人表情：开" },
	{ "facemode_off", L"显示说话人表情：关" },
	{ "reset",     L"恢复默认设置" },
	// ---- 回想 / 画廊等面板（探针里以独立图层出现）
	{ "Return",    L"返回" },
	{ "Flowchart", L"流程图" },
	{ "back",      L"返回" },
};
static const int g_aliasCount = (int)(sizeof(g_alias) / sizeof(g_alias[0]));

// ---------------- 用户标签表（plugin\a11y_labels.ini）----------------
// 格式： 每行 `层名=中文标签`，# 或 ; 开头是注释。ini 里的条目优先于内置表，
// 于是「念出英文层名」这件事不需要改代码就能修。
struct UserLabel { char name[64]; wchar_t label[128]; };
static UserLabel g_userLabels[256];
static int g_userLabelCount = 0;

static void LoadUserLabels(void)
{
	g_userLabelCount = 0;
	const wchar_t *rel[] = { L"plugin\\a11y_labels.ini", L"a11y_labels.ini" };
	for(int i = 0; i < 2; i++)
	{
		wchar_t p[MAX_PATH];
		_snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s%s", g_exeDir, rel[i]);
		FILE *f = NULL; _wfopen_s(&f, p, L"rb");
		if(!f) continue;
		char buf[512];
		while(fgets(buf, sizeof(buf), f) && g_userLabelCount < 256)
		{
			char *s = buf;
			if((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
			while(*s == ' ' || *s == '\t') s++;
			if(*s == '#' || *s == ';' || *s == '\r' || *s == '\n' || *s == 0) continue;
			char *eq = strchr(s, '=');
			if(!eq) continue;
			*eq = 0;
			char *nm = s; char *vl = eq + 1;
			size_t nl = strlen(nm);
			while(nl > 0 && (nm[nl-1] == ' ' || nm[nl-1] == '\t')) nm[--nl] = 0;
			while(*vl == ' ' || *vl == '\t') vl++;
			size_t vlLen = strlen(vl);
			while(vlLen > 0 && (vl[vlLen-1] == '\r' || vl[vlLen-1] == '\n' || vl[vlLen-1] == ' ')) vl[--vlLen] = 0;
			if(nl == 0 || vlLen == 0) continue;
			UserLabel *u = &g_userLabels[g_userLabelCount];
			strncpy_s(u->name, sizeof(u->name), nm, _TRUNCATE);
			utf8ToW(vl, u->label, 128);
			if(u->label[0]) g_userLabelCount++;
		}
		fclose(f);
		diagf("a11y_labels.ini: %d 条  ", g_userLabelCount);
		diagW(p);
		break;
	}
}

// 没有把握的层名不做翻译，直接念层名本身 —— 宁可听起来生硬，也不编。
static void LabelOf(const char *name, wchar_t *out, int cap)
{
	for(int u = 0; u < g_userLabelCount; u++)
	{
		if(_stricmp(name, g_userLabels[u].name) == 0)
		{
			wcsncpy_s(out, cap, g_userLabels[u].label, _TRUNCATE);
			return;
		}
	}
	for(int i = 0; i < g_aliasCount; i++)
	{
		if(_stricmp(name, g_alias[i].name) == 0)
		{
			wcsncpy_s(out, cap, g_alias[i].label, _TRUNCATE);
			return;
		}
	}
	utf8ToW(name, out, cap);
}

static bool IsSaveOrLoadScreen(void)
{
	for(int i = 0; i < g_itemCount; i++)
	{
		const char *n = g_items[i].name;
		if(_stricmp(n, "copy") == 0 || _stricmp(n, "del") == 0 ||
		   _stricmp(n, "to_quick") == 0 || _stricmp(n, "thumbview") == 0) return true;
	}
	return false;
}

static void ScanItems(void)
{
	g_itemCount = 0;
	char *out = NULL;
	ExecOnMainThread(kScanScript, &out);
	if(!out) { diag("scan: 探针脚本没有返回\n"); return; }

	int slotSeq = 0;
	char *p = out;
	while(*p && g_itemCount < NAV_MAX_ITEMS)
	{
		char *bar = strchr(p, '|');
		if(bar) *bar = 0;
		// 形如  C\t<名>\t<x,y,w,h,父层名>   或   L\t<名>\t<x,y,w,h>
		if(p[0] && p[1] == '\t')
		{
			char kind = p[0];
			char *nm = p + 2;
			char *tab = strchr(nm, '\t');
			if(tab)
			{
				// 必须先把制表符打成字符串结尾：不这么做，nm 会连父层名一起带走
				// （"helpbase\t1384,99,…,表メッセージレイヤ0"），而 Excluded() 的规则是
				// 「名字里含 レイヤ 就当噪声」—— 父层名里的 レイヤ 被带进来，
				// **每一项都会被丢掉**，表现为扫描恒返回 0 项、导航里按方向键一片死寂。
				*tab = 0;
				// 坐标必须按**浮点**解析：TJS 侧发的是控件中心，宽高为奇数时就是
				// 167.5 这种半像素值。原先用 "%d,%d,%d,%d" —— %d 读到 '167' 后
				// 撞上 '.' 就整个中止，后面 y/w/h 全部留 0，排序键与点击点一起烂掉
				// （实测症状：标题菜单念成 开始→退出→流程图→系统设置→继续→语言→读取，
				//   以及激活控件时点到 (x,0) 即屏幕最顶端，看起来就是「点了没反应」）。
				double dx = 0, dy = 0, dw = 0, dh = 0;
				char parent[64]; parent[0] = 0;
				int got = sscanf_s(tab + 1, "%lf,%lf,%lf,%lf,%63s",
					&dx, &dy, &dw, &dh, parent, (unsigned)sizeof(parent));
				if(got >= 2)
				{
					int x = (int)(dx + 0.5), y = (int)(dy + 0.5);
					int w = (int)(dw + 0.5), h = (int)(dh + 0.5);
					if(!Excluded(nm))
					{
						NavItem *it = &g_items[g_itemCount];
						strncpy_s(it->name, sizeof(it->name), nm, _TRUNCATE);
						it->hx = x; it->hy = y;
						it->left = x - w / 2; it->top = y - h / 2;
						it->w = w; it->h = h;
						it->isChild = (kind == 'C');
						LabelOf(it->name, it->label, 128);
						g_itemCount++;
					}
				}
			}
		}
		if(!bar) break;
		p = bar + 1;
	}
	free(out);

	// 存档槽（层名 item00 / item01…）在画面上只有数字，读不到文字；
	// 用「存档位 N」这种位置说法，N 是这一屏子控件的顺次编号。
	if(IsSaveOrLoadScreen())
	{
		for(int i = 0; i < g_itemCount; i++)
		{
			if(g_items[i].isChild && _strnicmp(g_items[i].name, "item", 4) == 0 &&
			   isdigit((unsigned char)g_items[i].name[4]))
			{
				slotSeq++;
				_snwprintf_s(g_items[i].label, 128, _TRUNCATE, L"存档位 %d", slotSeq);
			}
		}
	}

	// 选项场景：选项按钮图层的层名是日文的（「選択肢ボタン」+ 选项原文），
	// 按层名换成游戏自己给的本地化选项文字（BuildChoiceLabels 的结果）。
	if(g_choiceCount > 0)
	{
		for(int i = 0; i < g_itemCount; i++)
		{
			for(int c = 0; c < g_choiceCount; c++)
			{
				if(_stricmp(g_items[i].name, g_choiceNames[c]) == 0)
				{
					wcsncpy_s(g_items[i].label, 128, g_choiceLabels[c], _TRUNCATE);
					break;
				}
			}
		}
	}
	// 按「先上后下、先左后右」排成阅读顺序。
	// 探针的发现顺序是按图层树的，念起来前后乱跳（设置界面实测：先念底部按钮，
	// 再念最上面的页签，中间夹着零散按钮）。位置排序后就是人看界面的顺序：
	// 页签 → 第一行左侧 → 第一行右侧 → 第二行 …… → 底部按钮。
	for(int a = 1; a < g_itemCount; a++)
	{
		NavItem key = g_items[a];
		int b = a - 1;
		while(b >= 0 && (g_items[b].top > key.top ||
		                 (g_items[b].top == key.top && g_items[b].left > key.left)))
		{
			g_items[b + 1] = g_items[b];
			b--;
		}
		g_items[b + 1] = key;
	}

	diagf("scan: %d 个控件\n", g_itemCount);
}

// 「点这个点一定命中该图层」→ 直接发鼠标点击，游戏自己派发
static void ClickLogical(int lx, int ly)
{
	if(!g_gameWnd) return;
	RECT rc; GetClientRect(g_gameWnd, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	if(cw <= 0 || ch <= 0) return;
	int cx = (int)((double)lx * cw / (double)NAV_LOGICAL_W);
	int cy = (int)((double)ly * ch / (double)NAV_LOGICAL_H);
	if(cx < 0) cx = 0; if(cx >= cw) cx = cw - 1;
	if(cy < 0) cy = 0; if(cy >= ch) cy = ch - 1;
	LPARAM lp = MAKELPARAM(cx, cy);
	PostMessageW(g_gameWnd, WM_MOUSEMOVE, 0, lp);
	PostMessageW(g_gameWnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
	PostMessageW(g_gameWnd, WM_LBUTTONUP, 0, lp);
	diagf("click logical (%d,%d) -> client (%d,%d)\n", lx, ly, cx, cy);
}

// 底部工具栏会自己滑走；进导航前先晃一下鼠标把它叫回来（探针实测有效）
static void WakeToolbar(void)
{
	if(!g_gameWnd) return;
	RECT rc; GetClientRect(g_gameWnd, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	if(cw <= 0 || ch <= 0) return;
	PostMessageW(g_gameWnd, WM_MOUSEMOVE, 0, MAKELPARAM(cw / 2, ch - 40));
}

static void NavAnnounce(const wchar_t *prefix)
{
	if(g_itemCount == 0) { SpeechSpeak(L"当前界面上没有可操作的项目。", true); return; }
	if(g_sel < 0) g_sel = 0;
	if(g_sel >= g_itemCount) g_sel = g_itemCount - 1;
	g_slidePct = 50;   // 换了控件，滑条百分比从头开始
	wchar_t say[512];
	_snwprintf_s(say, 512, _TRUNCATE, L"%ls%ls。%d / %d",
		prefix ? prefix : L"", g_items[g_sel].label, g_sel + 1, g_itemCount);
	SpeechSpeak(say, true);
}

static void NavExit(bool announce)
{
	diag("NavExit\n");
	g_nav = false;
	g_scanPending = false;
	g_itemCount = 0;
	if(announce) SpeechSpeak(L"已退出导航模式。", true);
}

static void NavEnter(void)
{
	if(!g_gameWnd) { SpeechSpeak(L"还没有找到游戏窗口，请稍后再试。", true); return; }
	diag("NavEnter\n");
	WakeToolbar();
	g_nav = true;
	g_sel = 0;
	g_itemCount = 0;
	g_scanTries = 0;
	g_scanOnEnter = true;
	g_scanPending = true;
	g_scanAt = GetTickCount() + 400;   // 等工具栏滑出来再扫
	SpeechSpeak(L"导航模式。", true);
}

static void NavActivate(void)
{
	if(g_itemCount == 0) { NavExit(false); return; }
	NavItem *it = &g_items[g_sel];
	wchar_t say[256];
	_snwprintf_s(say, 256, _TRUNCATE, L"已激活 %ls", it->label);
	SpeechSpeak(say, true);
	diag("activate "); diagW(it->label);
	ClickLogical(it->hx, it->hy);
	g_rescanAt = GetTickCount() + 600;
}

// 本作实测：**只有点击能推进剧情** —— 真键盘的空格/回车、以及 PostMessage 送进去的
// 空格/回车，剧情都纹丝不动（对比：同样方式点击消息区立刻出下一句）。
// 所以「空格/回车 = 推进剧情」这一条必须由我们自己发点击来实现。
#define ADVANCE_X 960
#define ADVANCE_Y 800

// ---------------------------------------------------------------- 快进 ------
static void SkipStop(const wchar_t *say)
{
	if(!g_skip) return;
	g_skip = false;
	if(g_hWnd) SetTimer(g_hWnd, TIMER_POLL, POLL_MS, NULL);   // 定时器恢复平时的节拍
	diagf("skip stop: clicks=%d confirmed=%d\n", g_skipClicks, g_skipConfirmed ? 1 : 0);
	if(say) SpeechSpeak(say, true);
}

static void SkipStart(void)
{
	if(g_skip) return;
	if(g_nav) { SpeechSpeak(L"导航模式下不启动快进。", true); return; }
	if(!g_lastText[0]) { SpeechSpeak(L"现在没有剧情文本，快进没有启动。", true); return; }
	g_skip = true;
	g_skipBegan = GetTickCount();
	g_skipNext = g_skipBegan + SKIP_PROBE_MS;
	g_skipClicks = 0;
	g_skipStuck = 0;
	g_skipConfirmed = false;
	wcsncpy_s(g_skipWatch, 512, g_lastText, _TRUNCATE);
	if(g_hWnd) SetTimer(g_hWnd, TIMER_POLL, SKIP_POLL_MS, NULL);
	diag("skip start\n");
	SpeechSpeak(L"快进开始，遇到选项会停下来。", true);
}

static void SkipTick(void)
{
	if(!g_skip) return;
	// 选项出现 → 立刻收手，让 PollDialogue 去念选项（数字键选择那套）
	if(g_selectShowing > 0 || g_choiceCount > 0) { SkipStop(NULL); return; }
	if(!GetAsyncKeyState(VK_CONTROL))
	{
		// 松手：播报结束，并把当前这一句读出来（快进期间过路台词是不念的，
		// 不补这一下，盲人松开 Ctrl 后会不知道自己停在哪）
		bool spoke = g_skipConfirmed;
		SkipStop(spoke ? L"快进结束。" : NULL);
		if(spoke && g_lastText[0]) SpeakLine(g_lastName, g_lastText);
		return;
	}

	DWORD now = GetTickCount();
	if((long)(now - g_skipNext) < 0) return;

	// 文本变了吗？快进期间不朗读，但 g_lastText 照常更新，用来看「推得动推不动」
	if(wcscmp(g_skipWatch, g_lastText) != 0)
	{
		wcsncpy_s(g_skipWatch, 512, g_lastText, _TRUNCATE);
		g_skipStuck = 0;
		if(!g_skipConfirmed) { g_skipConfirmed = true; diag("skip confirmed\n"); }
	}
	else if(++g_skipStuck >= (g_skipConfirmed ? SKIP_STUCK_LIMIT : SKIP_PROBE_TRIES))
	{
		// 试推期推不动 = 根本不在剧情里（读档画面的存档格正好压在推进点上，
		// 连点可能变成双击 → 读档），所以试推只允许点 SKIP_PROBE_TRIES 下就收手。
		SkipStop(g_skipConfirmed ? L"推进不动了，快进停止。" : L"现在好像不在剧情里，快进没有启动。");
		return;
	}
	if(g_skipClicks >= SKIP_MAX_CLICKS) { SkipStop(L"快进结束。"); return; }

	// 引擎自己把推进节流在 ~2.5 句/秒（实测：点 17.8 下/秒也只到 2.3 句/秒，
	// 而采样器是 5 Hz —— 引擎若真更快，每次采样都会看到新行）。所以**不要**多发点击，
	// 一拍一下刚好；多发只是让引擎丢消息，一点也不快。
	ClickLogical(ADVANCE_X, ADVANCE_Y);
	g_skipClicks++;
	g_skipNext = GetTickCount() + (g_skipConfirmed ? SKIP_RUN_MS : SKIP_PROBE_MS);
	(void)now;
}

// 设置页里实测到的两条滑条（层名来自探针转储）：点左右两端就是设值
static const char *g_sliderNames[] = { "chaphidetime", "mushidetime" };

static bool IsSlider(const char *n)
{
	for(int i = 0; i < (int)(sizeof(g_sliderNames) / sizeof(g_sliderNames[0])); i++)
		if(_stricmp(n, g_sliderNames[i]) == 0) return true;
	return false;
}

// 数字键选择选项：用游戏自己给的按钮矩形中心点击（矩形来自 selectLayer.selects[i]，
// 实测 459,244,1002x146，与画面上那两条一致，不是我们猜的布局）。
static void SelectChoice(int i)
{
	if(i < 0 || i >= g_choiceCount) return;
	wchar_t say[256];
	_snwprintf_s(say, 256, _TRUNCATE, L"已选择 %ls", g_choiceLabels[i]);
	wcsncpy_s(g_lastAnnounced, 4600, say, _TRUNCATE);
	SpeechSpeak(say, true);
	ClickLogical(g_choiceRect[i][0] + g_choiceRect[i][2] / 2,
	             g_choiceRect[i][1] + g_choiceRect[i][3] / 2);
	g_rescanAt = GetTickCount() + 600;
}

// 返回 true = 这一下按键由我们处理（游戏看不到它）
static bool NavKeyDown(WPARAM vk, bool repeat)
{
	if(repeat) return false;   // 长按重复交给系统；我们只在「新的一次按下」上动作

	// Ctrl = 快进（本作不收键盘，引擎自己的 Ctrl 快进是死的，只能我们自己连点）
	if(vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
	{
		SkipStart();
		return true;
	}
	if(g_skip && vk == VK_ESCAPE)
	{
		SkipStop(L"快进已停止。");
		return true;
	}
	if(vk == VK_TAB)
	{
		if(g_nav) NavExit(true); else NavEnter();
		return true;
	}
	if(vk == VK_BACK)
	{
		if(g_lastAnnounced[0]) SpeechSpeak(g_lastAnnounced, true);
		else SpeechSpeak(L"还没有朗读过内容。", true);
		return true;
	}
	// 选项场景：数字键 1..9 = 选择对应选项（用游戏自己的按钮矩形点击）
	if(vk >= '1' && vk <= '9' && g_choiceCount > 0)
	{
		int n = (int)(vk - '1');
		if(n < g_choiceCount) SelectChoice(n);
		return true;
	}
	// 非导航模式下，空格/回车 = 推进剧情。
	// 实测（真键盘、游戏窗口在前台）：按一下正好推进一句，不重复。游戏自己是收真键盘的
	// （真按 Ctrl 会快进），但走的是 DirectInput/键盘状态轮询，不经过窗口消息 ——
	// 所以我们 PostMessage 送进去的空格游戏看不见，这一下点击是给「我们送进去的键」用的。
	if(!g_nav && (vk == VK_SPACE || vk == VK_RETURN))
	{
		ClickLogical(ADVANCE_X, ADVANCE_Y);
		return true;
	}
	if(!g_nav) return false;

	switch(vk)
	{
	case VK_UP:
		if(g_itemCount == 0) return true;
		g_sel = (g_sel - 1 + g_itemCount) % g_itemCount;
		NavAnnounce(NULL);
		return true;
	case VK_DOWN:
		if(g_itemCount == 0) return true;
		g_sel = (g_sel + 1) % g_itemCount;
		NavAnnounce(NULL);
		return true;
	case VK_HOME:
		if(g_itemCount == 0) return true;
		g_sel = 0; NavAnnounce(NULL);
		return true;
	case VK_END:
		if(g_itemCount == 0) return true;
		g_sel = g_itemCount - 1; NavAnnounce(NULL);
		return true;
	case VK_PRIOR:
		if(g_itemCount == 0) return true;
		g_sel -= 5; if(g_sel < 0) g_sel = 0;
		NavAnnounce(NULL);
		return true;
	case VK_NEXT:
		if(g_itemCount == 0) return true;
		g_sel += 5; if(g_sel >= g_itemCount) g_sel = g_itemCount - 1;
		NavAnnounce(NULL);
		return true;
	case VK_RETURN:
	case VK_SPACE:
		NavActivate();
		return true;
	case VK_LEFT:
	case VK_RIGHT:
	{
		if(g_itemCount == 0) return true;
		NavItem *it = &g_items[g_sel];
		if(!IsSlider(it->name)) { SpeechSpeak(L"这一项不能用左右方向键调整。", true); return true; }
		g_slidePct += (vk == VK_RIGHT) ? 10 : -10;
		if(g_slidePct < 0) g_slidePct = 0;
		if(g_slidePct > 100) g_slidePct = 100;
		wchar_t say[64];
		_snwprintf_s(say, 64, _TRUNCATE, L"已调到百分之 %d", g_slidePct);
		SpeechSpeak(say, true);
		ClickLogical(it->left + it->w * g_slidePct / 100, it->top + it->h / 2);
		return true;
	}
	case VK_ESCAPE:
		NavExit(true);
		return true;
	}
	return false;
}

// 每 200 ms 一次的导航小状态机（跟着抓文本的定时器走，不额外起线程）
static void NavTick(void)
{
	if(!g_subclassed) EnsureGameHook();

	SkipTick();

	if(g_nav && g_rescanAt && (long)(GetTickCount() - g_rescanAt) >= 0)
	{
		g_rescanAt = 0;
		g_scanPending = true;
		g_scanAt = GetTickCount();
	}

	if(g_nav && g_scanPending && (long)(GetTickCount() - g_scanAt) >= 0)
	{
		g_scanPending = false;
		ScanItems();

		// 清单变了吗？（框架的做法：变了就播报「界面已更新」，否则不出声）
		char now[8192]; now[0] = 0;
		for(int i = 0; i < g_itemCount; i++)
		{
			strncat_s(now, sizeof(now), g_items[i].name, _TRUNCATE);
			strncat_s(now, sizeof(now), "|", _TRUNCATE);
		}
		bool changed = (strcmp(now, g_namesBuf) != 0);
		strcpy_s(g_namesBuf, now);

		if(g_itemCount == 0)
		{
			// 标题菜单是淡入的、面板也要滑出来 —— 一次没扫到不算「没有控件」。
			// 连扫几次都没东西才认输（框架里对应的就是 SceneSettleSeconds 那道门禁）。
			if(g_scanTries < 8)
			{
				g_scanTries++;
				g_scanPending = true;
				g_scanAt = GetTickCount() + 700;
				return;
			}
			SpeechSpeak(L"当前界面上没有可操作的项目。", true);
			NavExit(false);
			return;
		}
		g_scanTries = 0;
		if(g_sel >= g_itemCount) g_sel = g_itemCount - 1;

		// 播报规则与框架一致：
		//   刚进导航模式   → 「导航模式，共 N 项，<当前项>。i / N」
		//   激活后清单变了 → 「界面已更新，<当前项>。i / N」
		//   激活后没变化   → 不出声（免得每点一下都念一遍）
		if(g_scanOnEnter)
		{
			wchar_t pfx[64];
			_snwprintf_s(pfx, 64, _TRUNCATE, L"导航模式，共 %d 项，", g_itemCount);
			g_scanOnEnter = false;
			NavAnnounce(pfx);
		}
		else if(changed)
		{
			NavAnnounce(L"界面已更新，");
		}
	}
}

static BOOL CALLBACK EnumFindGameWnd(HWND h, LPARAM lp)
{
	DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
	if(pid != GetCurrentProcessId()) return TRUE;
	wchar_t cls[64];
	GetClassNameW(h, cls, 64);
	if(wcscmp(cls, L"TVPMainWindow") == 0) { *(HWND *)lp = h; return FALSE; }
	return TRUE;
}

static LRESULT CALLBACK GameSubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR id, DWORD_PTR ref)
{
	if(msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
	{
		bool repeat = (lp & (1 << 30)) != 0;
		if(NavKeyDown(wp, repeat)) return 0;
	}
	return DefSubclassProc(hWnd, msg, wp, lp);
}

static void EnsureGameHook(void)
{
	if(g_subclassed) return;
	HWND found = NULL;
	EnumWindows(EnumFindGameWnd, (LPARAM)&found);
	if(!found) return;
	if(SetWindowSubclass(found, GameSubclassProc, 1, 0))
	{
		g_gameWnd = found;
		g_subclassed = true;
		diagf("game window subclassed hwnd=%p\n", (void *)found);
	}
}

// ==================================================================== 窗口 ====
static LRESULT CALLBACK A11yWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch(msg)
	{
	case WM_TIMER:
		if(wp == TIMER_POLL) { PollDialogue(); NavTick(); return 0; }
		break;
	case WM_DESTROY:
		KillTimer(hWnd, TIMER_POLL);
		g_hWnd = NULL;
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wp, lp);
}

// message-only 窗口：不可见、不在任务栏、拿不到焦点 —— 只作为定时器与消息的落点。
static void EnsurePump(void)
{
	if(g_hWnd) return;
	WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = A11yWndProc;
	wc.hInstance = g_hInst;
	wc.lpszClassName = L"Sba11yMsgWindow";
	RegisterClassW(&wc);

	g_hWnd = CreateWindowExW(0, L"Sba11yMsgWindow", L"", 0,
		0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInst, NULL);
	if(!g_hWnd) { diagf("CreateWindow(HWND_MESSAGE) failed err=%lu\n", GetLastError()); return; }
	SetTimer(g_hWnd, TIMER_POLL, POLL_MS, NULL);
	diagf("message-only window ok hwnd=%p\n", (void *)g_hWnd);
}

// ================================================================ V2 exports ==
extern "C" __declspec(dllexport) HRESULT __stdcall V2Link(void *exporterptr)
{
	if(!g_hInst) g_hInst = GetModuleHandleW(NULL);
	DeleteFileA(DIAG_PATH);
	diag("=== V2Link (a11y5, 单进程) ===\n");

	// exe 所在目录（含尾部反斜杠）
	GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
	{
		wchar_t *s = wcsrchr(g_exeDir, L'\\');
		if(s) s[1] = 0;
	}
	diag("exeDir="); diagW(g_exeDir);
	diag("== a11y6 build: CtrlFastForward + selectShowing + digit-choice ==\n");
	LoadUserLabels();

	iTVPFunctionExporter *exp = (iTVPFunctionExporter *)exporterptr;
	if(!exp) { diag("no exporter\n"); return E_FAIL; }

	const char *names[5] = {
		"iTJSDispatch2 * ::TVPGetScriptDispatch()",
		"void ::TVPExecuteExpression(const ttstr &,tTJSVariant *)",
		"void ::TVPExecuteScript(const ttstr &,const ttstr &,tjs_int,tTJSVariant *)",
		"tTJSVariantString * ::TJSAllocVariantString(const tjs_char *)",
		"void ::TVPDoTryBlock(tTVPTryBlockFunction,tTVPCatchBlockFunction,tTVPFinallyBlockFunction,void *)",
	};
	void *funcs[5] = { NULL, NULL, NULL, NULL, NULL };
	exp->QueryFunctionsByNarrowString(names, funcs, 5);
	g_getScriptDispatch   = (GetScriptDispatchFn)funcs[0];
	g_executeScript       = (ExecuteScriptFn)funcs[2];
	g_allocVariantString  = (AllocVariantStringFn)funcs[3];
	g_doTryBlock          = (DoTryBlockFn)funcs[4];
	diagf("dispatch=%p execScript=%p allocStr=%p doTry=%p\n", funcs[0], funcs[2], funcs[3], funcs[4]);
	if(!g_executeScript || !g_allocVariantString || !g_doTryBlock) { diag("engine entry points missing\n"); return E_FAIL; }

	if(SpeechInit())
	{
		diagf("语音后端: %s\n", g_backendName);		// 启动提示：一句就能证明「文本直接交给读屏 DLL」这条路是通的
		{
			wchar_t hello[256];
			wchar_t wname[128];
			utf8ToW(g_backendName, wname, 128);
			_snwprintf_s(hello, 256, _TRUNCATE, L"无障碍模块已加载，语音后端 %s", wname);
			SpeechSpeak(hello, true);
		}
	}
	else
	{
		diagf("没有可用的语音后端，逐个结论: %s\n", g_verdict);
	}

	EnsurePump();
	return S_OK;
}

extern "C" __declspec(dllexport) HRESULT __stdcall V2Unlink(void)
{
	if(g_gameWnd && g_subclassed) { RemoveWindowSubclass(g_gameWnd, GameSubclassProc, 1); g_subclassed = false; g_gameWnd = NULL; }
	if(g_hWnd) { KillTimer(g_hWnd, TIMER_POLL); DestroyWindow(g_hWnd); g_hWnd = NULL; }
	SpeechStop();
	if(g_tolkUnload) g_tolkUnload();
	if(g_spRelease && g_spVoice) { g_spRelease(g_spVoice); g_spVoice = NULL; }
	diag("=== V2Unlink ===\n");
	return S_OK;
}

extern "C" __declspec(dllexport) ULONG __stdcall GetModuleThreadModel(void) { return 0; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH) g_hInst = hInst;
	return TRUE;
}
