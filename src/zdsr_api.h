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
//  【dll 从哪来】官方原文两条一起看：
//    · 「应用程序可以直接调用争渡读屏安装目录下的相应 dll 文件，或者直接把 dll
//       复制到指定的目录调用均可……另外**建议直接读取争渡读屏安装目录下的 dll 文件**，
//       尽量不要复制到自己程序目录」；
//    · 「争渡读屏 26.6 dll 文件有更新，在此之前编写的应用程序自带的 ZDSRAPI.dll /
//       ZDSRAPI_x64.dll 需要更新成新的 dll 文件」。
//  所以顺序是：**注册表登记的安装目录 → 默认安装目录 → 随包兜底 → 宿主目录**，
//  随包那份永远排在安装目录后面；而万一随包那份真的旧到报版本不匹配（InitTTS=1），
//  下面的 ZdsrOpen 会**自动跳过去试下一份**，不会把玩家卡死。详见 ZdsrOpen 的注释。
//
//  【注册表自动发现 · 为什么需要】很多用户找不到这个 dll（「比如两年前的我」），
//  而争渡的安装位置是登记在案的：卸载项里 `DisplayName = 争渡读屏`、
//  `InstallLocation = <安装目录>`。本机核实（2026-10-16）：
//      HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\
//          {301A3D3B-...}_is1   DisplayName=争渡读屏  InstallLocation=C:\Program Files (x86)\zdsr\zdsr\
//  于是「装在 D 盘、自定义目录」的机器也能自动找到，不用谁去教。
//  同一个注册表里还有一个 `ZDSR hook control`（InstallLocation 含 ZDSRHook，名字里有 zdsr，
//  但那里**没有** ZDSRAPI.dll）—— 所以「DisplayName 含『争渡』」的那条排前面，只含 zdsr 的排后面。
//
//  【哪些路径来自注册表是**每次重试**重新读的】不缓存「找不到」：争渡可能后装、后启动、
//  后授权，插件那边的重试只在「一个后端都没有」时发生（见 a11y6.cpp 的 TryZdsr）。
//
//  【本机验证状态】桩 DLL 场景（缺 dll / state 2 / 3 / 4 / Speak rc=2 / 版本不匹配换下一份 /
//  注册表候选顺序）与真 dll 的只读握手（InitTTS=0、GetSpeakState=4）见 tools\zdsr_selftest
//  的自检；**真实游戏进程里「争渡真的出声」这一条未验证**（本机不启动游戏、不做端到端朗读）。
// ============================================================================
#ifndef SBA11Y_ZDSR_API_H
#define SBA11Y_ZDSR_API_H

#include <windows.h>
#include <winreg.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

// 注册表用的 advapi32：winreg.h 自己**不带** #pragma comment(lib,...)，
// 所以这里显式带上 —— 免得 build.cmd 与自检的编译命令各写一遍、哪天漏一个。
#pragma comment(lib, "advapi32.lib")

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

// 候选 dll 的处数上限：注册表最多 8 处（4 条含「争渡」+ 4 条只含 zdsr，见 ZdsrRegistryDirs）
// + 默认安装目录 2 处 + 随包 plugin\ 1 处 + 宿主目录 1 处 = 12，取 16 留余量。
// 这只是一个**安全上限**（数组开多大），实际能凑出几条由机器上的注册表决定。
#define ZDSR_MAX_CANDIDATES 16

// 尾部候选的条数：默认安装目录 ×2 + 随包 plugin\ + 宿主目录。
// 这几条**任何情况下都要留位** —— 尤其随包的 plugin\ 那份，0.1.0.6 的随包兜底就靠它。
// 注册表理论上能凑出 8 条，真让它占满就会把这些从末尾挤掉，所以见 ZdsrCandidatePaths 里的留位。
#define ZDSR_TAIL_CANDIDATES 4

// 「这份候选不行」的原因串长度。要放得下**所有试过的路径**：玩家报「争渡没声音」
// 时，这一串就是唯一能判断「到底是没装、还是装了两份、还是自带那份过期」的依据。
#define ZDSR_WHY_CHARS 1024

// 注册表里找「争渡读屏」的那条路（HKLM/HKCU 各查一遍，两种视图各查一遍）。
#define ZDSR_UNINSTALL_SUBKEY \
	L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"

// 自检/排查用：把上面那条子键换掉（换成别的路径或一个不存在的路径）。
// **不设 = 走正常路径**；插件本身从不设置它，生产行为不受影响。
#define ZDSR_UNINSTALL_ENV L"ZDSR_UNINSTALL_KEY_OVERRIDE"

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
	wchar_t        why[ZDSR_WHY_CHARS]; // 不可用的原因（人话；调用方转 UTF-8 记日志）
	int            initRc;          // 最后一份候选 InitTTS 的原始返回值
	int            state;           // 最近一次 GetSpeakState 的原始返回值
	DWORD          stateTick;       // 状态缓存到期时刻（GetTickCount 体系）
};

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

// 【踩过的坑 · 别改回去】CRT 在 C++ 下给 _snwprintf_s 一类函数准备了「数组版」重载模板
// （corecrt.h，_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 宏组）：
//     template <size_t _Size> _FuncName(wchar_t (&_Dst)[_Size], _TType1 a1, _TType2 a2)
// 于是 `_snwprintf_s(buf, _TRUNCATE, fmt, x)` 会**静默**匹配到它 —— _TType1 收下 _TRUNCATE、
// _TType2 收下 fmt，后面的 x 直接丢掉，再按数组版去调 (buf, _Size, _TRUNCATE, fmt)。
// 编译通过、没有警告、结果是空串或没格式化的东西：0.1.0.6 写自检时就栽在这里，
// 所有多参数调试输出全是假的，白查了半天（还以为是被测代码的问题）。
// **凡是要格式化进数组，第一个参数一律显式转成指针**，把模板重载挡在门外。
#define ZDSR_FMT_BUF(p) ((wchar_t *)(p))

// 不区分大小写的子串查找（宽字符）。不用 wcsstr：那个区分大小写，
// 而卸载项里的写法五花八门（ZDSR / Zdsr / zdsr）。
static const wchar_t *ZdsrStrIStr(const wchar_t *hay, const wchar_t *needle)
{
	if(!hay || !needle || !needle[0]) return NULL;
	for(; *hay; hay++)
	{
		const wchar_t *h = hay, *nn = needle;
		while(*h && *nn && (towlower(*h) == towlower(*nn))) { h++; nn++; }
		if(!*nn) return hay;
	}
	return NULL;
}

// 把 msg 作为**整个** why 写进去，并返回它 —— 便于 `return ZdsrWhy(api, L"...")`。
static bool ZdsrWhy(ZdsrApi *api, const wchar_t *fmt, ...)
{
	if(api)
	{
		va_list ap;
		va_start(ap, fmt);
		_vsnwprintf_s(ZDSR_FMT_BUF(api->why), (size_t)ZDSR_WHY_CHARS, _TRUNCATE, fmt, ap);
		va_end(ap);
		api->why[ZDSR_WHY_CHARS - 1] = 0;
	}
	return false;
}

// 按优先级追加一个候选 dll 完整路径；**不检查文件是否存在**（交给 LoadLibrary 如实失败），
// 但去重：注册表那条常常与默认路径逐字相同，不去重就会对着同一份 dll 试两遍。
static void ZdsrAppendPath(wchar_t paths[][MAX_PATH], int *n, int maxPaths, const wchar_t *p)
{
	if(!p || !p[0] || *n >= maxPaths) return;
	if(wcslen(p) >= MAX_PATH) return;
	for(int i = 0; i < *n; i++) if(_wcsicmp(paths[i], p) == 0) return;
	wcsncpy_s(paths[*n], MAX_PATH, p, _TRUNCATE);
	(*n)++;
}

// 「<某目录>\ZDSRAPI.dll」拼出来再追加。
// 目录**有没有尾部反斜杠都行** —— 这里自己补。原因是一行真实的教训：
// 原来写的是 `L"%splugin"`，而 `%sp` 会被 printf 当成一个**非法转换**，反斜杠就这么没了，
// 拼出来是 `<appDir>plugin`。它不报错、不崩溃，只是安静地找错地方。
// 现在的写法是 `L"%s\\plugin"`，并且对已经带尾杠的目录去重。
// **位宽按本进程选**：32 位进程用 ZDSRAPI.dll，64 位进程用 ZDSRAPI_x64.dll。
// 这里不写死 —— 现在这份头文件只被 32 位目标编译，但把文件名钉死成 32 位那份，
// 哪天有人拿它编 64 位就会得到一个「看起来在找、其实永远装错」的 bug。
static void ZdsrAppendDir(wchar_t paths[][MAX_PATH], int *n, int maxPaths, const wchar_t *dir)
{
	wchar_t p[MAX_PATH];
#if defined(_WIN64)
	static const wchar_t *name = L"ZDSRAPI_x64.dll";
#else
	static const wchar_t *name = L"ZDSRAPI.dll";
#endif
	if(!dir || !dir[0]) return;
	size_t len = wcslen(dir);
	const wchar_t *sep = (dir[len - 1] == L'\\') ? L"" : L"\\";
	if(_snwprintf_s(ZDSR_FMT_BUF(p), (size_t)MAX_PATH, _TRUNCATE, L"%s%s%s", dir, sep, name) < 0) return;
	p[MAX_PATH - 1] = 0;
	ZdsrAppendPath(paths, n, maxPaths, p);
}

// ---------------------------------------------------------------------------
//  注册表：找 `争渡读屏 → InstallLocation`
//
//  全程「读不到就算了」：注册表不该成为故障源，任何一步失败都只是少一批候选。
//  枚举的是 HKLM + HKCU × {默认视图, 64 位视图}：
//    · 本构建是 32 位，默认视图本来就被重定向到 WOW6432Node（本机那条正在那里）；
//    · KEY_WOW64_64KEY 再看一遍 64 位视图（用 64 位安装器装的机器可能只登记在那边）。
//  重复的路径由 ZdsrAppendPath 去重，所以重叠的视图不会造成重复尝试。
// ---------------------------------------------------------------------------

// 读一个 REG_SZ 值（宽字符）。读不到（不存在 / 类型不符 / 空 / 出错）一律返回 false。
static bool ZdsrRegString(HKEY key, const wchar_t *name, wchar_t *out, DWORD cch)
{
	if(!key || !out || cch == 0) return false;
	out[0] = 0;

	DWORD type = 0, cb = 0;
	if(RegQueryValueExW(key, name, NULL, &type, NULL, &cb) != ERROR_SUCCESS) return false;
	// REG_SZ 与 REG_EXPAND_SZ 都收（有的安装器写后者）；其它类型不碰。
	if(type != REG_SZ && type != REG_EXPAND_SZ) return false;
	if(cb == 0 || cb > cch * sizeof(wchar_t)) return false;

	DWORD got = cb;
	if(RegQueryValueExW(key, name, NULL, &type, (LPBYTE)out, &got) != ERROR_SUCCESS) { out[0] = 0; return false; }
	out[cch - 1] = 0;

	// 尾部多余的 NUL 与空白一起剪掉（InstallLocation 常常以 '\' 结尾）。
	int len = (int)wcslen(out);
	while(len > 0 && (out[len - 1] == L'\r' || out[len - 1] == L'\n' ||
	                  out[len - 1] == L' '  || out[len - 1] == L'\t')) out[--len] = 0;
	while(len > 0 && out[len - 1] == L'\\') out[--len] = 0;
	return len > 0;
}

#define ZDSR_REG_NAME_CHARS 256
#define ZDSR_REG_VAL_CHARS  512

// 扫一个视图（access 里已经带了 or 不带宽视图标志）。
// exact = DisplayName 含「争渡」的（本体，官方卸载项就是「争渡读屏」）；
// loose = 只含 zdsr 的（可能是周边工具：本机那个 `ZDSR hook control v2.1.0` 的
//         InstallLocation 是 C:\Program Files\ZDSRHook，那里没有 ZDSRAPI.dll）。
// 两类分开收，合并时 exact 在前 —— 只是省掉一次注定失败的 LoadLibrary。
static void ZdsrScanUninstallView(HKEY hive, REGSAM access,
                                  wchar_t exact[][MAX_PATH], int *nExact, int maxExact,
                                  wchar_t loose[][MAX_PATH], int *nLoose, int maxLoose)
{
	wchar_t subkey[ZDSR_REG_NAME_CHARS];
	DWORD n = GetEnvironmentVariableW(ZDSR_UNINSTALL_ENV, subkey, ZDSR_REG_NAME_CHARS);
	if(n == 0 || n >= ZDSR_REG_NAME_CHARS)
		wcsncpy_s(subkey, ZDSR_REG_NAME_CHARS, ZDSR_UNINSTALL_SUBKEY, _TRUNCATE);
	subkey[ZDSR_REG_NAME_CHARS - 1] = 0;

	HKEY root = NULL;
	// KEY_READ 已经含 KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE；再 or 上视图标志。
	if(RegOpenKeyExW(hive, subkey, 0, KEY_READ | access, &root) != ERROR_SUCCESS || !root) return;

	wchar_t name[ZDSR_REG_NAME_CHARS];
	for(DWORD i = 0; i < 4096; i++)
	{
		DWORD cch = ZDSR_REG_NAME_CHARS;
		if(RegEnumKeyExW(root, i, name, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
		name[ZDSR_REG_NAME_CHARS - 1] = 0;

		wchar_t itemSub[ZDSR_REG_NAME_CHARS * 2];
		if(_snwprintf_s(ZDSR_FMT_BUF(itemSub), (size_t)(ZDSR_REG_NAME_CHARS * 2), _TRUNCATE, L"%s\\%s", subkey, name) < 0) continue;

		HKEY item = NULL;
		if(RegOpenKeyExW(hive, itemSub, 0, KEY_READ | access, &item) != ERROR_SUCCESS || !item) continue;

		// 先看 DisplayName：「争渡」= 本体；退一步只认 zdsr（不区分大小写）。
		// 两个都不沾的卸载项直接跳过 —— 别去读无关软件的 InstallLocation。
		wchar_t disp[ZDSR_REG_NAME_CHARS];
		if(ZdsrRegString(item, L"DisplayName", disp, ZDSR_REG_NAME_CHARS))
		{
			bool isProduct = (wcsstr(disp, L"争渡") != NULL);
			bool isLoose   = (ZdsrStrIStr(disp, L"zdsr") != NULL);
			if(isProduct || isLoose)
			{
				wchar_t loc[MAX_PATH];
				if(ZdsrRegString(item, L"InstallLocation", loc, MAX_PATH))
				{
					wchar_t (*dst)[MAX_PATH] = isProduct ? exact : loose;
					int *dstN = isProduct ? nExact : nLoose;
					int dstMax = isProduct ? maxExact : maxLoose;

					wchar_t dll[MAX_PATH];
					#if defined(_WIN64)
					static const wchar_t *dllName = L"ZDSRAPI_x64.dll";
					#else
					static const wchar_t *dllName = L"ZDSRAPI.dll";
					#endif
					if(_snwprintf_s(ZDSR_FMT_BUF(dll), (size_t)MAX_PATH, _TRUNCATE, L"%s\\%s", loc, dllName) >= 0)
					{
						dll[MAX_PATH - 1] = 0;
						bool dup = false;
						for(int k = 0; k < *dstN && !dup; k++) if(_wcsicmp(dst[k], dll) == 0) dup = true;
						if(!dup && *dstN < dstMax)
						{
							wcsncpy_s(dst[*dstN], MAX_PATH, dll, _TRUNCATE);
							(*dstN)++;
						}
					}
				}
			}
		}
		RegCloseKey(item);
	}
	RegCloseKey(root);
}

// 注册表里所有「争渡读屏」的安装目录（转成候选 dll 完整路径）。
// 返回条数；**失败一律是 0，不报错、不抛异常、不中止**。
static int ZdsrRegistryDirs(wchar_t out[][MAX_PATH], int maxOut)
{
	if(maxOut <= 0) return 0;

	wchar_t exact[4][MAX_PATH];
	wchar_t loose[4][MAX_PATH];
	int nExact = 0, nLoose = 0;

	ZdsrScanUninstallView(HKEY_LOCAL_MACHINE, 0, exact, &nExact, 4, loose, &nLoose, 4);
	ZdsrScanUninstallView(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, exact, &nExact, 4, loose, &nLoose, 4);
	ZdsrScanUninstallView(HKEY_CURRENT_USER, 0, exact, &nExact, 4, loose, &nLoose, 4);
	ZdsrScanUninstallView(HKEY_CURRENT_USER, KEY_WOW64_64KEY, exact, &nExact, 4, loose, &nLoose, 4);

	int n = 0;
	for(int i = 0; i < nExact && n < maxOut; i++) ZdsrAppendPath(out, &n, maxOut, exact[i]);
	for(int i = 0; i < nLoose && n < maxOut; i++) ZdsrAppendPath(out, &n, maxOut, loose[i]);
	return n;
}

// ---------------------------------------------------------------------------
//  候选路径
// ---------------------------------------------------------------------------

// 排查用：把候选**钉死**成这几处（`;` 分隔，允许空格与引号）。
// 设了它就**只试这些**，不再看注册表、默认目录和随包那份 —— 与 Framework 的
// A11Y_ZDSR_DIR 同义，给「明明装了争渡却找不到」的机器一个一句话就能验证的出口：
//     set A11Y_ZDSR_DIR=D:\zdsr\zdsr
// 不设 = 走正常路径，插件自己从不设置它。
#define ZDSR_PIN_ENV L"A11Y_ZDSR_DIR"

static int ZdsrPinnedDirs(wchar_t paths[][MAX_PATH], int maxPaths)
{
	wchar_t buf[1024];
	DWORD n = GetEnvironmentVariableW(ZDSR_PIN_ENV, buf, 1024);
	if(n == 0 || n >= 1024) return 0;   // 没设 / 太长：都当没设
	buf[1023] = 0;

	int count = 0;
	wchar_t *p = buf;
	while(*p && count < maxPaths)
	{
		wchar_t *semi = wcschr(p, L';');
		if(semi) *semi = 0;

		// 去首尾的空白与引号：玩家从别处复制路径时经常把引号一起带进来
		while(*p == L' ' || *p == L'\t' || *p == L'"') p++;
		size_t len = wcslen(p);
		while(len > 0 && (p[len - 1] == L' ' || p[len - 1] == L'\t' || p[len - 1] == L'"')) p[--len] = 0;
		if(len > 0) ZdsrAppendDir(paths, &count, maxPaths, p);

		if(!semi) break;
		p = semi + 1;
	}
	return count;
}

// 候选 dll 完整路径，顺序即优先级：
//   1) 注册表登记的安装目录（HKLM/HKCU × 默认/64 位视图；含「争渡」的排在只含 zdsr 的前面）
//   2) %ProgramFiles(x86)%\zdsr\zdsr\ZDSRAPI.dll
//   3) %ProgramFiles%\zdsr\zdsr\ZDSRAPI.dll
//   4) <appDir>plugin\ZDSRAPI.dll   （随包兜底的那一份就放这里）
//   5) <appDir>ZDSRAPI.dll
// appDir 要带尾部反斜杠（a11y6.cpp 的 g_exeDir 就是这个约定）。
// 环境变量是**调用时刻**读的 —— 自检程序靠临时改环境变量把候选指到桩目录去。
static int ZdsrCandidatePaths(const wchar_t *appDir, wchar_t paths[][MAX_PATH], int maxPaths)
{
	int n = 0;

	// 0) 钉死的候选（排查用）。设了 `A11Y_ZDSR_DIR` 就只认它 —— 「钉死」的字面意思：
	//    不去猜注册表、也不去看默认目录，试的就是玩家写的那几处。
	n += ZdsrPinnedDirs(paths, maxPaths);
	if(n > 0) return n;

	// 1) 注册表（最可靠：覆盖装在别的盘 / 自定义目录的用户）。
	//    **先给尾部 4 条留位**：注册表能凑出 8 条（4 exact + 4 loose），占满的话
	//    下面 2/3/4/5 就全被挤掉 —— 而随包兜底那份正在最后面。
	//    少试两条注册表条目只是「多试几次才找到」，漏掉随包那份则是「这个功能不存在」。
	{
		int room = maxPaths - n - ZDSR_TAIL_CANDIDATES;
		if(room > 0) n += ZdsrRegistryDirs(paths + n, room);
	}

	// 2/3) 默认安装位置。x86 优先：官方 32 位那份 ZDSRAPI.dll 通常就在 x86 目录下。
	//      （注意：这两条是**位宽无关**的目录，选哪份 dll 由 ZdsrAppendDir 按本进程位数决定）
	static const wchar_t *envs[2] = { L"ProgramFiles(x86)", L"ProgramFiles" };
	for(int e = 0; e < 2 && n < maxPaths; e++)
	{
		wchar_t pf[MAX_PATH];
		DWORD len = GetEnvironmentVariableW(envs[e], pf, MAX_PATH);
		if(len == 0 || len >= MAX_PATH) continue;
		wchar_t dir[MAX_PATH];
		if(_snwprintf_s(ZDSR_FMT_BUF(dir), (size_t)MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr", pf) < 0) continue;
		dir[MAX_PATH - 1] = 0;
		ZdsrAppendDir(paths, &n, maxPaths, dir);
	}

	// 4/5) 随包兜底的那份（plugin\）与宿主目录
	if(appDir && appDir[0])
	{
		// 注意这里是 `"%s\\plugin"` 而**不是** `"%splugin"`：后者里的 `%sp`
		// 是个非法转换，反斜杠会被吃掉（0.1.0.6 真踩过，见 ZdsrAppendDir 的注释）。
		// 好在 ZdsrAppendDir 自己会补反斜杠，两种写法现在都对；留着它是为了让意图显式。
		wchar_t dir[MAX_PATH];
		if(_snwprintf_s(ZDSR_FMT_BUF(dir), (size_t)MAX_PATH, _TRUNCATE, L"%s\\plugin", appDir) >= 0)
		{
			dir[MAX_PATH - 1] = 0;
			ZdsrAppendDir(paths, &n, maxPaths, dir);
		}
		ZdsrAppendDir(paths, &n, maxPaths, appDir);
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
	ZdsrWhy(api,
		L"GetSpeakState=%d（只有 3/4 算可用；2=争渡没有运行或未授权，8=没送到，9=通道未就绪）",
		api->state);
	return false;
}

// 从第 startIdx 份候选开始，找一份**能加载且导出齐全**的 dll。
// **每次都重新找**（调用方不要缓存「找不到」）：争渡可能后装、后启动、后授权，
// 而插件那边的重试只在「一个后端都没有」时发生。
// 只解析四个必需导出；Braille（1.0.0.5+ 才有）本插件不接盲文输出，所以不解析、不调用。
//
// 失败时为什么里带上「试了 N 处、每一处分别怎么失败的」—— 玩家报「争渡没声音」时，
// 这一串就是唯一能分清「没装 / 装了两份 / 自带那份过期」的依据。
static bool ZdsrLoadDll(const wchar_t *appDir, ZdsrApi *api, int startIdx = 0)
{
	memset(api, 0, sizeof(*api));

	wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
	int n = ZdsrCandidatePaths(appDir, paths, ZDSR_MAX_CANDIDATES);

	if(startIdx < 0) startIdx = 0;
	if(startIdx >= n) startIdx = n;   // 没有更多候选了：循环直接不跑，走下面的失败分支

	wchar_t tried[ZDSR_WHY_CHARS];
	tried[0] = 0;
	int triedChars = 0;
	for(int i = startIdx; i < n; i++)
	{
		// LOAD_WITH_ALTERED_SEARCH_PATH：按 dll 自己所在目录去找它的依赖。
		// 装错位宽时的表现就是这里失败、err=193（ERROR_BAD_EXE_FORMAT）。
		HMODULE dll = LoadLibraryExW(paths[i], NULL, 0x00000008);
		if(!dll)
		{
			DWORD err = GetLastError();
			triedChars += _snwprintf_s(ZDSR_FMT_BUF(tried + triedChars), (size_t)(ZDSR_WHY_CHARS - triedChars), _TRUNCATE,
				L"%s（LoadLibrary 失败 err=%lu）；", paths[i], (unsigned long)err);
			if(triedChars < 0) triedChars = (int)wcslen(tried);   // 截断：别再往后写
			continue;
		}

		api->initTts  = (ZdsrInitTtsFn) GetProcAddress(dll, "InitTTS");
		api->speak    = (ZdsrSpeakFn)   GetProcAddress(dll, "Speak");
		api->getState = (ZdsrGetStateFn)GetProcAddress(dll, "GetSpeakState");
		api->stop     = (ZdsrStopFn)    GetProcAddress(dll, "StopSpeak");
		if(!api->initTts || !api->speak || !api->getState)
		{
			// 导出不全：不是争渡的 ZDSRAPI（例如某些挂钩工具的 dll 就叫这个名字）。
			// 句柄不保留、dll 本体也不 FreeLibrary（与 Tolk/NVDA 一样：加载了就留着），
			// 继续找下一份 —— 「名字对但东西不对」不该让整条链停在这里。
			triedChars += _snwprintf_s(ZDSR_FMT_BUF(tried + triedChars), (size_t)(ZDSR_WHY_CHARS - triedChars), _TRUNCATE,
				L"%s（不是争渡的 dll：缺导出 InitTTS/Speak/GetSpeakState）；", paths[i]);
			if(triedChars < 0) triedChars = (int)wcslen(tried);
			continue;
		}

		api->dll = dll;
		wcsncpy_s(api->path, MAX_PATH, paths[i], _TRUNCATE);
		api->why[0] = 0;
		return true;
	}

	if(n == 0)
	{
		return ZdsrWhy(api, L"找不到可加载的 ZDSRAPI.dll（一个候选路径都没有）");
	}
	return ZdsrWhy(api, L"没有一份 ZDSRAPI.dll 能用（试了 %d 处：%s）", n - startIdx, tried);
}

// 握手 + 可用性判定：InitTTS(0, NULL, FALSE) 然后 GetSpeakState ∈ {3,4}。
//   type=0           读屏通道（官方：type=0 时 channelName 忽略，传 NULL）
//   keyDownInterrupt=FALSE  按键不打断 —— 台词是排队念的，若允许按键打断，
//                           玩家每次推进都会掐掉正在念的那句
// 官方原文：InitTTS「可被重复调用」，所以每次重试都重新握手是允许的。
//
// 【版本不匹配（rc=1）→ 自动换下一份候选】这一条是**随包兜底那份 dll 安全的前提**：
//   · 正常情况下安装目录那份排在前面，随包那份根本轮不到（官方也是这个建议）；
//   · 万一随包那份太旧、而它偏偏被用上了，rc=1 就会跳过它去试下一份，
//     而不是判「争渡后端不可用」把玩家卡在一份旧 dll 上；
//   · 反过来，安装目录那份自己就是坏的（比如降级安装留下的旧 dll），也会被跳过，
//     于是**排在它后面的随包那份还能被够到** —— 两头都留了活路。
// 只有 rc==1 才换下一份：2（没运行/未授权）、8（没送到）、9（独立通道未就绪）
// 都是「现在不能用」，对每一份 dll 都一样，换一份毫无意义，直接如实判不可用。
// 收工时给出**人话**的理由，并列出试过的路径（玩家照着这一串就能自己判断该重装什么）。
static bool ZdsrOpen(const wchar_t *appDir, ZdsrApi *api)
{
	int start = 0;
	wchar_t examined[ZDSR_WHY_CHARS];
	examined[0] = 0;
	int examinedChars = 0;
	bool sawMismatch = false;

	for(;;)
	{
		if(!ZdsrLoadDll(appDir, api, start))
		{
			// 一份都没加载成功。若前面曾因版本不匹配跳过（典型：随包那份旧了），
			// 两种原因都要写进理由里 —— 否则玩家只看到「找不到 dll」，去重装也未必对。
			if(sawMismatch)
			{
				// 注意：这次的 why 是「往后没得试了」的原因，先自己留一份再拼总原因。
				wchar_t tail[ZDSR_WHY_CHARS];
				wcsncpy_s(tail, ZDSR_WHY_CHARS, api->why, _TRUNCATE);
				return ZdsrWhy(api,
					L"这些 dll 都报版本不匹配（InitTTS=1）：%s再往后也没有可用的 dll：%s",
					examined, tail);
			}
			return false;
		}

		api->initRc = api->initTts(0, NULL, FALSE);
		if(api->initRc == ZDSR_RC_OK) return ZdsrCheckState(api, true);

		// 同一份 dll 会被反复试到：start++ 之后每一轮都从它开头重新枚举，
		// 它又是唯一能加载的那份，于是同一条路径会被追加 N 遍。这段理由是**念给玩家听**
		// 的（争渡会把它读出来），同一个路径念 9 遍纯属折磨，只留第一条。
		if(wcsstr(examined, api->path) == NULL)
		{
			examinedChars += _snwprintf_s(ZDSR_FMT_BUF(examined + examinedChars), (size_t)(ZDSR_WHY_CHARS - examinedChars), _TRUNCATE,
				L"%s（InitTTS=%d）；", api->path, api->initRc);
			if(examinedChars < 0) examinedChars = (int)wcslen(examined);
		}

		if(api->initRc != ZDSR_RC_VERSION_MISMATCH)
		{
			// 不是版本不匹配：换一份 dll 也还是这个结果，如实判不可用。
			if(api->initRc == ZDSR_RC_NOT_RUNNING)
				return ZdsrWhy(api, L"InitTTS 返回 2：争渡读屏没有运行或没有授权（dll=%s）", api->path);
			return ZdsrWhy(api, L"InitTTS 返回 %d，这一份 dll 现在不能用（dll=%s）", api->initRc, api->path);
		}

		// rc == 1：这一份与当前争渡对不上（多半是随包的旧 dll）→ 试下一份。
		sawMismatch = true;
		start++;
		if(start >= ZDSR_MAX_CANDIDATES)
			return ZdsrWhy(api, L"试过的每一份 ZDSRAPI.dll 都报版本不匹配（InitTTS=1）：%s", examined);
	}
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
		ZdsrWhy(api, L"Speak 返回 %d（2=没有运行或没有授权，8=没送到，9=通道未就绪）", rc);
	return rc;
}
static void ZdsrSilence(ZdsrApi *api) { if(api->stop) api->stop(); }

#endif // SBA11Y_ZDSR_API_H
