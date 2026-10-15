// ============================================================================
//  zdsr_api.h —— 争渡读屏（ZDSR）ZDSRAPI.dll 的最小封装
//
//  这份头文件被**两处**包含，这是刻意的：
//    · src\a11y6.cpp                         —— 插件本体（KiriKiri Z 进程内 V2Link 插件）
//    · tools\zdsr_selftest\zdsr_selftest.cpp —— 32 位独立自检程序（可跑桩 DLL）
//  于是「自检里断言的那份逻辑」就是插件里真正在跑的那份，而不是照抄一遍。
//
//  官方接口（本机 C:\Program Files (x86)\zdsr\zdsr\ZDSRAPI.txt 原文核对）——
//  全部 __stdcall（WINAPI）：
//    int  WINAPI InitTTS(int type, const WCHAR *channelName, BOOL bKeyDownInterrupt)
//    int  WINAPI Speak(const WCHAR *text, BOOL bInterrupt)
//    int  WINAPI GetSpeakState()
//    void WINAPI StopSpeak()
//    int  WINAPI Braille(const WCHAR *text, BOOL bFlashMessage)   // 1.0.0.5+
//  注意是 __stdcall（与 a11y6.cpp 里 NVDA 那一组一致）；Tolk 那一组是 __cdecl，
//  别抄错。文本在插件里本来就已经是 UTF-16LE（const wchar_t*），原样透传、不做转换。
//
//  【可用性判据 · 本后端的头号坑】不能拿 Speak() 的返回值判可用：
//  未授权时 Speak() 照样返回 0（官方文档 + 同工作区先例
//  Framework\platform\Zdsr.cs 的实测注释：公益档下 GetSpeakState() 恒为 2），
//  只有 GetSpeakState() ∈ {3=正在朗读, 4=空闲} 才说明这条通道真的能用。
//  判错的后果就是「插件自称一切正常、玩家一片安静」，而且永远不降级到别的后端。
//
//  【不随包分发】官方原文：「建议直接读取争渡读屏安装目录下的 dll 文件，
//  尽量不要复制到自己程序目录」——26.6 起 dll 有更新，自带旧 dll 会踩
//  「版本不匹配（InitTTS=1）」。所以这里只按绝对路径找（争渡目录不在 PATH 里），
//  不复制、不打包任何 ZDSR 的 dll。
//
//  【本机验证状态】桩 DLL 场景（缺 dll / state 2 / 3 / 4 / Speak rc=2）与真 dll 的
//  只读握手（InitTTS=0、GetSpeakState=4）见 tools\zdsr_selftest 的自检；
//  **真实游戏进程里「争渡真的出声」这一条未验证**（本机不启动游戏、不做端到端朗读）。
// ============================================================================
#ifndef SBA11Y_ZDSR_API_H
#define SBA11Y_ZDSR_API_H

#include <windows.h>
#include <string.h>

// 官方返回码 / 状态码（ZDSRAPI.txt 原文）
#define ZDSR_RC_OK                0   // 成功
#define ZDSR_RC_VERSION_MISMATCH  1   // 版本不匹配
#define ZDSR_RC_NOT_RUNNING       2   // 争渡读屏没有运行或没有授权
#define ZDSR_STATE_SPEAKING       3   // 正在朗读
#define ZDSR_STATE_IDLE           4   // 没有朗读
#define ZDSR_RC_NOT_DELIVERED     8   // 命令没有成功送达
#define ZDSR_RC_CHANNEL_NOT_READY 9   // （独立通道）尚未就绪

// 状态检查的缓存时长。GetSpeakState 是一次跨进程调用（C# 先例实测 ~0.4 ms 本地、
// ~21 ms 走 IPC），而每一句台词都会走到朗读；但也不能永远不查 —— 读屏中途被关掉时
// Speak 仍返回 0，只看返回值就会「一直以为自己在念」。5 秒是这两件事的折中，
// 与 Framework\platform\Zdsr.cs 的 StateCheckMs 取同一个值。
#define ZDSR_STATE_TTL_MS 5000

// 候选 dll 的处数：%ProgramFiles(x86)% / %ProgramFiles% 各一处 + 调用方目录两处
#define ZDSR_MAX_CANDIDATES 4

typedef int  (__stdcall *ZdsrInitTtsFn)(int channelType, const wchar_t *channelName, BOOL keyDownInterrupt);
typedef int  (__stdcall *ZdsrSpeakFn)(const wchar_t *text, BOOL interrupt);
typedef int  (__stdcall *ZdsrGetStateFn)(void);
typedef void (__stdcall *ZdsrStopFn)(void);

struct ZdsrApi
{
	HMODULE        dll;             // 成功加载的那一份（不 FreeLibrary，与 Tolk/NVDA 一致）
	ZdsrInitTtsFn  initTts;
	ZdsrSpeakFn    speak;
	ZdsrGetStateFn getState;
	ZdsrStopFn     stop;
	wchar_t        path[MAX_PATH];  // 实际加载到的那份 dll 的完整路径（诊断日志用）
	wchar_t        why[320];        // 不可用的原因（人话；调用方转 UTF-8 记日志）
	int            initRc;          // InitTTS 的原始返回值
	int            state;           // 最近一次 GetSpeakState 的原始返回值
	DWORD          stateTick;       // 状态缓存到期时刻（GetTickCount 体系）
};

// 候选 dll 完整路径，顺序即优先级：争渡安装目录优先（官方要求），其次调用方目录。
//   1) %ProgramFiles(x86)%\zdsr\zdsr\ZDSRAPI.dll
//   2) %ProgramFiles%\zdsr\zdsr\ZDSRAPI.dll
//   3) <appDir>plugin\ZDSRAPI.dll   （用户手动放一份；官方也允许「复制到指定目录调用」）
//   4) <appDir>ZDSRAPI.dll
// appDir 要带尾部反斜杠（a11y6.cpp 的 g_exeDir 就是这个约定）。
// 环境变量是**调用时刻**读的 —— 自检程序靠临时改环境变量把候选指到桩目录去。
static int ZdsrCandidatePaths(const wchar_t *appDir, wchar_t paths[][MAX_PATH], int maxPaths)
{
	static const wchar_t *envs[2] = { L"ProgramFiles(x86)", L"ProgramFiles" };
	int n = 0;
	for(int e = 0; e < 2 && n < maxPaths; e++)
	{
		wchar_t pf[MAX_PATH];
		DWORD len = GetEnvironmentVariableW(envs[e], pf, MAX_PATH);
		if(len == 0 || len >= MAX_PATH) continue;
		_snwprintf_s(paths[n], MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr\\ZDSRAPI.dll", pf);
		paths[n][MAX_PATH - 1] = 0;
		if(paths[n][0]) n++;
	}
	if(appDir && appDir[0])
	{
		if(n < maxPaths) { _snwprintf_s(paths[n], MAX_PATH, _TRUNCATE, L"%splugin\\ZDSRAPI.dll", appDir); n++; }
		if(n < maxPaths) { _snwprintf_s(paths[n], MAX_PATH, _TRUNCATE, L"%sZDSRAPI.dll", appDir); n++; }
	}
	return n;
}

// 状态检查：**本后端唯一的可用性判据**（见文件头）。结果缓存 ZDSR_STATE_TTL_MS。
// force=true 时无条件重新问一次（握手时用）。
static bool ZdsrCheckState(ZdsrApi *api, bool force)
{
	if(!api->getState) return false;
	DWORD now = GetTickCount();
	if(!force && (long)(now - api->stateTick) < 0)
		return (api->state == ZDSR_STATE_SPEAKING || api->state == ZDSR_STATE_IDLE);

	api->stateTick = now + ZDSR_STATE_TTL_MS;
	api->state = api->getState();
	if(api->state == ZDSR_STATE_SPEAKING || api->state == ZDSR_STATE_IDLE)
	{
		api->why[0] = 0;
		return true;
	}
	_snwprintf_s(api->why, 320, _TRUNCATE,
		L"GetSpeakState=%d（只有 3/4 算可用；2=争渡没有运行或未授权，8=没送到，9=通道未就绪）",
		api->state);
	return false;
}

// 找 dll 并解析导出。**每次都重新找**（调用方不要缓存「找不到」）：争渡可能后装、
// 后启动、后授权，而插件那边的重试只在「一个后端都没有」时发生。
// 只解析四个必需导出；Braille（1.0.0.5+ 才有）本插件不接盲文输出，所以不解析、不调用。
static bool ZdsrLoadDll(const wchar_t *appDir, ZdsrApi *api)
{
	memset(api, 0, sizeof(*api));

	wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
	int n = ZdsrCandidatePaths(appDir, paths, ZDSR_MAX_CANDIDATES);
	DWORD lastErr = 0;
	const wchar_t *lastPath = L"(没有候选路径)";
	for(int i = 0; i < n; i++)
	{
		// LOAD_WITH_ALTERED_SEARCH_PATH：按 dll 自己所在目录去找它的依赖。
		// 装错位宽时的表现就是这里失败、err=193（ERROR_BAD_EXE_FORMAT）。
		HMODULE dll = LoadLibraryExW(paths[i], NULL, 0x00000008);
		if(!dll) { lastErr = GetLastError(); lastPath = paths[i]; continue; }
		api->dll = dll;
		wcsncpy_s(api->path, MAX_PATH, paths[i], _TRUNCATE);
		break;
	}
	if(!api->dll)
	{
		_snwprintf_s(api->why, 320, _TRUNCATE,
			L"找不到可加载的 ZDSRAPI.dll（试了 %d 处；最后一处 %s 失败，err=%lu）",
			n, lastPath, (unsigned long)lastErr);
		return false;
	}

	api->initTts  = (ZdsrInitTtsFn) GetProcAddress(api->dll, "InitTTS");
	api->speak    = (ZdsrSpeakFn)   GetProcAddress(api->dll, "Speak");
	api->getState = (ZdsrGetStateFn)GetProcAddress(api->dll, "GetSpeakState");
	api->stop     = (ZdsrStopFn)    GetProcAddress(api->dll, "StopSpeak");
	if(!api->initTts || !api->speak || !api->getState)
	{
		_snwprintf_s(api->why, 320, _TRUNCATE,
			L"%s 缺导出（需要 InitTTS/Speak/GetSpeakState）", api->path);
		api->dll = NULL;   // 句柄不保留；dll 本体不 FreeLibrary（与 Tolk/NVDA 一样加载了就留着）
		return false;
	}
	return true;
}

// 握手 + 可用性判定：InitTTS(0, NULL, FALSE) 然后 GetSpeakState ∈ {3,4}。
//   type=0           读屏通道（官方：type=0 时 channelName 忽略，传 NULL）
//   keyDownInterrupt=FALSE  按键不打断 —— 台词是排队念的，若允许按键打断，
//                           玩家每次推进都会掐掉正在念的那句
// 官方原文：InitTTS「可被重复调用」，所以每次重试都重新握手是允许的。
static bool ZdsrOpen(const wchar_t *appDir, ZdsrApi *api)
{
	if(!ZdsrLoadDll(appDir, api)) return false;

	api->initRc = api->initTts(0, NULL, FALSE);
	if(api->initRc != ZDSR_RC_OK)
	{
		_snwprintf_s(api->why, 320, _TRUNCATE,
			L"InitTTS 返回 %d（1=版本不匹配；多半是程序自带的 dll 比读屏旧）", api->initRc);
		return false;
	}
	return ZdsrCheckState(api, true);
}

// 朗读一句。返回 0 = 已交给争渡；非 0 = 现在不能用（状态不对，或 Speak 的返回码），
// 调用方应当**放弃这个后端并重新挑一个**，而不是继续假装在念。
// interrupt 原样透传给官方 Speak：TRUE=清空排队立刻打断，FALSE=等空闲。
static int ZdsrSay(ZdsrApi *api, const wchar_t *text, BOOL interrupt)
{
	if(!api->speak) return -1;
	// 先看状态：未授权/读屏被关掉时下面那次 Speak 也会返回 0，不看状态就会「假装成功」
	if(!ZdsrCheckState(api, false)) return api->state ? api->state : -1;

	int rc = api->speak(text, interrupt);
	if(rc != ZDSR_RC_OK)
		_snwprintf_s(api->why, 320, _TRUNCATE,
			L"Speak 返回 %d（2=没有运行或没有授权，8=没送到，9=通道未就绪）", rc);
	return rc;
}

static void ZdsrSilence(ZdsrApi *api) { if(api->stop) api->stop(); }

#endif // SBA11Y_ZDSR_API_H
