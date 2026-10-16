// ============================================================================
//  zdsr_selftest.cpp —— ZDSR（争渡读屏）后端的机械自检（32 位，MSVC）
//
//  为什么要有它：争渡的「装没装 / 跑没跑 / 授没授权」在开发机上无法稳定复现
//  （同工作区先例 Framework\platform\Zdsr.cs 的既有结论），所以选路与失败路径
//  只能靠**桩 DLL** 机械验证。本程序 include 的 src\zdsr_api.h 就是插件本体用的
//  那一份 —— 断言过的逻辑 = 插件里真正执行的逻辑，不是照抄一遍。
//
//  验的东西：
//    S1  dll 不存在          → 判不可用，原因可读（不崩、不假装成功）
//    S2  GetSpeakState=2     → 判不可用，**且 Speak 根本没被调用**
//                             （可用性判据绝不能是 Speak 的返回值：未授权时它照样返回 0）
//    S3  GetSpeakState=3     → 判可用（正在朗读）
//    S4  GetSpeakState=4     → 判可用（空闲）
//    S5  Speak 返回 2        → ZdsrSay 返回非 0，调用方据此放弃这一级
//    S6  文本与打断标记       → 桩日志里的 UTF-16 码元与传入**逐码元相同**，interrupt 原样透传
//    S7  中途掉线            → 状态变 2 后 ZdsrSay 不再调用 Speak（只认状态，不看返回值）
//    S8  真 dll 只读握手      → InitTTS(0,NULL,FALSE)=0、GetSpeakState ∈ {3,4}
//                             （**只读**：不调用 Speak，不会替玩家说话）
//    S9  dll 在但缺导出       → 判不可用并说明缺导出
//    S10 StopSpeak           → ZdsrSilence 真的走到了官方那个导出
//    S11 注册表自动发现与候选顺序 → 「争渡读屏 → InstallLocation」排第一；只含 zdsr 的挂钩工具
//                             排在「争渡」后面；默认目录 → <appDir>plugin\ → <appDir> 依次在后
//    S12 注册表读不到         → 候选表照样成立（注册表不该成为故障源）
//    S13 InitTTS=1 自动换下一份 → 第 0 份报版本不匹配时**自动**跳到随包兜底那份并成功；
//                             坏的那份不会再被问第二次，也不会去问它的 GetSpeakState/Speak
//    S14 安装目录那份是坏的    → 随包兜底那份仍然被够到（这就是「带一份旧 dll 也安全」）
//    S15 所有候选都版本不匹配  → 如实判不可用，理由里是人话 + 试过哪些路径（不假装成功）
//    S16 注册表塞满 8 条       → 尾部四条（%ProgramFiles(x86)% / %ProgramFiles% /
//                             <appDir>plugin\ / <appDir>）一条不少、顺序不变
//    S17 A11Y_ZDSR_DIR 钉死     → 设了就**只试**写的这几处（空格/引号会被剪掉），
//                             默认目录与随包那份都不再被试；清掉后恢复正常候选表
//
//  它**不**验证（本机不启动游戏）：插件里 SpeechInit 的插入顺序、F9 循环切换与播报、
//  a11y_speech.ini 的读写、以及「争渡真的出声」—— 那几条只能靠代码审查 + 实机。
//
//  用法：zdsr_selftest.exe [报告文件路径]（UTF-8；不给就写到 %TEMP%）
//  退出码：0 = 全部通过；1 = 有 FAIL；2 = 环境不齐（桩 dll 没编出来）
// ============================================================================
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "zdsr_api.h"

static FILE   *g_report = NULL;
static int     g_pass = 0, g_fail = 0;
static char    g_u8[8192];
static wchar_t g_root[MAX_PATH];      // 沙箱根（带尾部反斜杠）
static wchar_t g_emptyDir[MAX_PATH];  // 空目录：既没有 dll、也不当 appDir
static wchar_t g_stubRoot[MAX_PATH];  // 里面有 <g_stubDir>\ZDSRAPI.dll（桩）
static wchar_t g_noStateRoot[MAX_PATH];
static wchar_t g_stubDir[MAX_PATH];     // <stubRoot>\zdsr\zdsr（冒充争渡安装目录的形状）
static wchar_t g_noStateDir[MAX_PATH];
static wchar_t g_emptySub[MAX_PATH];    // <emptyDir>\zdsr\zdsr
static wchar_t g_staleDir[MAX_PATH];    // <staleRoot>\zdsr\zdsr（注册表那条指向这里）
static wchar_t g_mixDir[MAX_PATH];      // <mixRoot>\zdsr\zdsr
static wchar_t g_appDir[MAX_PATH];    // 冒充「游戏目录」：plugin\ 里有随包兜底那份桩
static wchar_t g_appEmptyDir[MAX_PATH]; // 真正空空如也的「游戏目录」（S1/S9 用）
static wchar_t g_noStateAppDir[MAX_PATH]; // 只有 plugin\，且里面是缺导出的桩（S9 用）
static wchar_t g_badAppDir[MAX_PATH];     // 只有 plugin\，且里面是「永远版本不匹配」的桩（S15 用）
static wchar_t g_logPath[MAX_PATH];
static wchar_t g_selfDir[MAX_PATH];
static wchar_t g_staleRoot[MAX_PATH];   // 冒充「升级前留下的旧安装目录」：里面是 mismatch 桩
static wchar_t g_mixRoot[MAX_PATH];     // 注册表那条的安装位置（同样放 mismatch 桩）
static wchar_t g_pluginRoot[MAX_PATH];  // 冒充「随包兜底」的 <appDir>plugin\ 目录
static wchar_t g_regRoot[MAX_PATH];     // 自检自己的注册表子树（HKCU\Software\...）
static wchar_t g_regNoKey[MAX_PATH];    // 指过去一定是「没有这个子键」的路径
static wchar_t g_regSave[MAX_PATH];     // 环境变量 ZDSR_UNINSTALL_KEY_OVERRIDE 的原值
static bool    g_regHad = false;
static wchar_t g_pf86Save[MAX_PATH], g_pfSave[MAX_PATH];
static bool    g_pf86Had = false, g_pfHad = false;
static wchar_t g_pinSave[MAX_PATH];       // 环境变量 A11Y_ZDSR_DIR 的原值（S17 用）
static bool    g_pinHad = false;

// 【坑】格式化到数组时第一个参数必须显式转成指针：C++ 下 CRT 的「数组版」重载模板
// `_FuncName(wchar_t (&)[_Size], _TType1, _TType2)` 会把 `(buf, _TRUNCATE, fmt, a, b)`
// 静默解释成 `(buf, _TRUNCATE, fmt)` —— 多余的参数直接丢掉，编译还不报错。
// 理由与出处见 src\zdsr_api.h 里的 ZDSR_FMT_BUF。
static void out(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s((char *)buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	if(g_report) { fputs(buf, g_report); fflush(g_report); }
}

static void check(bool ok, const char *label)
{
	if(ok) { g_pass++; out("PASS  %s\n", label); }
	else   { g_fail++; out("FAIL  %s\n", label); }
}

static void checkf(bool ok, const char *fmt, ...)
{
	char label[4096];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s((char *)label, sizeof(label), _TRUNCATE, fmt, ap);
	va_end(ap);
	check(ok, label);
}

static const char *u8(const wchar_t *w)
{
	g_u8[0] = 0;
	if(!w) { _snprintf_s(g_u8, sizeof(g_u8), _TRUNCATE, "(null)"); return g_u8; }
	WideCharToMultiByte(CP_UTF8, 0, w, -1, g_u8, (int)sizeof(g_u8), NULL, NULL);
	return g_u8;
}

static void setEnvW(const wchar_t *n, const wchar_t *v)
{
	SetEnvironmentVariableW(n, v);
}

// 在 UTF-8 报告里做子串判断的辅助：UTF-8 是自同步编码，
// 一个完整的 UTF-8 串不可能出现在另一个完整串的「半个字符」中间。
static bool hasU8(const wchar_t *hay, const char *needleUtf8)
{
	// 【坑】u8() 用的是**同一个**静态缓冲。调用方写成 hasU8(x, u8(y)) 时，
	// 下面这行 u8(hay) 会把 needle 指着的那块内存**覆盖成 hay 自己**，
	// 于是 strstr(buf, buf) 恒真 —— 所有「理由里应该没有 X」的**否定断言**全部假通过，
	// 正例也变成白过（S15「理由里列出了试过的路径」一条就是这么白过了很久）。
	// S16/S17 加进来之后才被撞出来：先抄一份 needle 再转换 hay。
	if(!hay || !needleUtf8) return false;
	char needle[1024];
	strncpy_s(needle, sizeof(needle), needleUtf8, _TRUNCATE);
	return strstr(u8(hay), needle) != NULL;
}

// 数一个子串出现几次（同上，先抄 needle）。
// 用来盯「同一份 dll 在给玩家念的理由里被重复列出」这个回退 —— 争渡是**念**这段的。
static int countU8(const wchar_t *hay, const char *needleUtf8)
{
	if(!hay || !needleUtf8 || !*needleUtf8) return 0;
	char needle[1024];
	strncpy_s(needle, sizeof(needle), needleUtf8, _TRUNCATE);
	size_t len = strlen(needle);
	int n = 0;
	const char *p = u8(hay);
	while((p = strstr(p, needle)) != NULL) { n++; p += len; }
	return n;
}

static void saveEnv(const wchar_t *n, wchar_t *dst, bool *had)
{
	DWORD len = GetEnvironmentVariableW(n, dst, MAX_PATH);
	*had = (len > 0 && len < MAX_PATH);
	if(!*had) dst[0] = 0;
}

static bool mkdirs(const wchar_t *path)
{
	wchar_t tmp[MAX_PATH];
	wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
	for(wchar_t *p = tmp + 3; *p; p++)
	{
		if(*p == L'\\') { *p = 0; CreateDirectoryW(tmp, NULL); *p = L'\\'; }
	}
	CreateDirectoryW(tmp, NULL);
	return GetFileAttributesW(tmp) != INVALID_FILE_ATTRIBUTES;
}

static void resetLog(void) { DeleteFileW(g_logPath); }

static int readLog(char *buf, int cap)
{
	buf[0] = 0;
	FILE *f = NULL;
	_wfopen_s(&f, g_logPath, L"rb");
	if(!f) return 0;
	int n = (int)fread(buf, 1, (size_t)cap - 1, f);
	buf[n < 0 ? 0 : n] = 0;
	fclose(f);
	return n;
}

// 读**任意**路径的日志（桩有两个：正常的那个与「永远版本不匹配」的那个）。
static int readLog2(const wchar_t *path, char *buf, int cap)
{
	buf[0] = 0;
	FILE *f = NULL;
	_wfopen_s(&f, path, L"rb");
	if(!f) return 0;
	int n = (int)fread(buf, 1, (size_t)cap - 1, f);
	buf[n < 0 ? 0 : n] = 0;
	fclose(f);
	return n;
}

static int countOccur(const char *hay, const char *needle)
{
	int n = 0;
	const char *p = hay;
	while((p = strstr(p, needle)) != NULL) { n++; p++; }
	return n;
}

// 取「最后一条以 prefix 开头的日志行」（日志里同一行可能被调用多次）
static bool lastLogLine(const char *buf, const char *prefix, char *line, int cap)
{
	line[0] = 0;
	size_t plen = strlen(prefix);
	const char *p = buf;
	const char *found = NULL;
	while(*p)
	{
		if(strncmp(p, prefix, plen) == 0) found = p;
		const char *nl = strchr(p, '\n');
		if(!nl) break;
		p = nl + 1;
	}
	if(!found) return false;
	const char *nl = strchr(found, '\n');
	int n = nl ? (int)(nl - found) : (int)strlen(found);
	if(n >= cap) n = cap - 1;
	memcpy(line, found, n);
	line[n] = 0;
	return true;
}

// UTF-16 码元十六进制（与桩 DLL 日志里的 text_utf16hex 同格式）
static void utf16Hex(const wchar_t *w, char *dst, int cap)
{
	int o = 0;
	dst[0] = 0;
	for(int i = 0; w && w[i] && o + 6 < cap; i++)
		o += _snprintf_s(dst + o, cap - o, _TRUNCATE, "%04X ", (unsigned)w[i]);
	if(o > 0) dst[o - 1] = 0;
}

// ---------------------------------------------------------------------------
//  注册表：自检用的读写
//
//  被测代码（src\zdsr_api.h 的 ZdsrRegistryDirs）在**真实**的卸载项里找
//  「争渡读屏 → InstallLocation」。自检不能往真实卸载项里塞东西（那是往玩家的
//  机器上写垃圾），所以被测代码留了一个口子：环境变量 ZDSR_UNINSTALL_KEY_OVERRIDE
//  可以换掉「去哪儿找」。下面这几个函数就在自检自己的子树里造出**长得和真卸载项
//  一模一样**的条目（DisplayName / InstallLocation），让那条路径被真正跑到。
//
//  默认 HKCU 写、KEY_WOW64_32KEY 读（父键 HKCU\Software 两边都有），
//  这样 32 位进程的默认视图也能看见 —— 不碰任何系统键、跑完删干净。
// ---------------------------------------------------------------------------

// 「<dir>\ZDSRAPI.dll」——按**被测代码的拼法**生成期望值（ZdsrAppendDir 自己会补反斜杠，
// 所以这里也允许 dir 带/不带尾杠）。测试里所有「期望路径」都走它，
// 免得再出现「测试自己拼错、却报成实现错了」。
static void expectDll(const wchar_t *dir, wchar_t *out, int cap)
{
	size_t len = dir ? wcslen(dir) : 0;
	const wchar_t *sep = (len > 0 && dir[len - 1] == L'\\') ? L"" : L"\\";
	_snwprintf_s((wchar_t *)out, (size_t)cap, _TRUNCATE, L"%s%sZDSRAPI.dll", dir, sep);
	out[cap - 1] = 0;
}

// 自检的注册表沙箱：**整个子树**建在 HKCU\Software\SBA11yZdsrSelftest\<pid>\ 下。
//
// g_regParent 直接写成「HKCU\Software」这一层，是因为 regCreateEntry 传进来的子键
// 是**相对于打开的那个句柄**的：句柄停在 Software 上、子键又写一遍 "Software\..."，
// 就会多套一层（HKCU\Software\Software\...）。0.1.0.6 的自检第一版正是这么写错的 ——
// 表现是「写进去的条目自己读不回来」，而不是任何被测代码的问题。
static const wchar_t *g_regParent = L"Software\\SBA11yZdsrSelftest";

// 建一个项，并在里面写两个 REG_SZ 值。失败返回 false（自检会直接报错，不静默跳过）。
// sub 是**相对于 parent 句柄**的路径（parent 停在 HKCU\Software\SBA11yZdsrSelftest 上）。
static bool regCreateEntry(HKEY hive, HKEY parent, REGSAM access,
                           const wchar_t *sub, const wchar_t *display, const wchar_t *loc)
{
	HKEY k = NULL;
	int rc = RegCreateKeyExW(parent, sub, 0, NULL, 0, KEY_ALL_ACCESS | access,
	                         NULL, &k, NULL);
	if(rc != ERROR_SUCCESS || !k)
	{
		out("      [reg] RegCreateKeyExW(%s) rc=%d\n", u8(sub), rc);
		return false;
	}
	if(display)
	{
		rc = RegSetValueExW(k, L"DisplayName", 0, REG_SZ, (const BYTE *)display,
		                    (DWORD)((wcslen(display) + 1) * sizeof(wchar_t)));
		if(rc != ERROR_SUCCESS) { out("      [reg] RegSetValueExW(DisplayName) rc=%d\n", rc); RegCloseKey(k); return false; }
	}
	if(loc)
	{
		rc = RegSetValueExW(k, L"InstallLocation", 0, REG_SZ, (const BYTE *)loc,
		                    (DWORD)((wcslen(loc) + 1) * sizeof(wchar_t)));
		if(rc != ERROR_SUCCESS) { out("      [reg] RegSetValueExW(InstallLocation) rc=%d\n", rc); RegCloseKey(k); return false; }
	}
	RegCloseKey(k);
	(void)hive;
	return true;
}

// 打开（必要时创建）自检注册表沙箱的**父键**：HKCU\Software\SBA11yZdsrSelftest\<pid>\Uninstall
// 的上两层。返回的句柄就是 regCreateEntry 的 parent。
static HKEY regOpenSandbox(void)
{
	HKEY k = NULL;
	wchar_t path[MAX_PATH];
	_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%lu", g_regParent, (unsigned long)GetCurrentProcessId());
	if(RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0,
	                   KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &k, NULL) != ERROR_SUCCESS)
		return NULL;
	return k;
}

// 删掉自检的整棵子树（含子项）。本来就不存在也算成功。
static void regCleanup(void)
{
	HKEY k = NULL;
	if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS || !k)
		return;
	RegDeleteTreeW(k, L"SBA11yZdsrSelftest");
	RegCloseKey(k);
}

int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);

	char reportPathA[MAX_PATH] = "";
	if(argc > 1) { _snprintf_s(reportPathA, sizeof(reportPathA), _TRUNCATE, "%s", argv[1]); }
	else
	{
		wchar_t t[MAX_PATH];
		DWORD n = GetTempPathW(MAX_PATH, t);
		if(n == 0 || n >= MAX_PATH) { printf("GetTempPath failed\n"); return 2; }
		_snprintf_s(reportPathA, sizeof(reportPathA), _TRUNCATE, "%lszdsr_selftest_report.txt", t);
	}
	{
		wchar_t wp[MAX_PATH];
		MultiByteToWideChar(CP_ACP, 0, reportPathA, -1, wp, MAX_PATH);
		_wfopen_s(&g_report, wp, L"wb");
	}

	// ---- 沙箱：%TEMP%\a11y_zdsr_selftest\<pid>\
	//
	// 【命名纪律】这一节里所有「目录」常量**都不带尾部反斜杠**，拼路径时再显式加。
	// 第一版把它们写成带尾杠的（<root>stub\），再按被测代码的拼法去拼，
	// 就拼出 `...\stub\\zdsr\zdsr\ZDSRAPI.dll` 这种双斜杠 —— 功能上没问题，
	// 但断言里两个字符串对不上，测试会报成「实现错了」。少一个变量就少一处这种坑。
	wchar_t tmp[MAX_PATH];
	if(GetTempPathW(MAX_PATH, tmp) == 0) { out("GetTempPath failed\n"); return 2; }

	wchar_t exeDir[MAX_PATH], stubDir[MAX_PATH], noStateDir[MAX_PATH], staleSub[MAX_PATH], mixSub[MAX_PATH];
	wchar_t pluginDir[MAX_PATH], noStatePlugin[MAX_PATH], badPlugin[MAX_PATH];
	_snwprintf_s(g_root, MAX_PATH, _TRUNCATE, L"%sa11y_zdsr_selftest\\%lu", tmp, (unsigned long)GetCurrentProcessId());
	_snwprintf_s(g_emptyDir, MAX_PATH, _TRUNCATE, L"%s\\empty", g_root);
	_snwprintf_s(g_stubRoot, MAX_PATH, _TRUNCATE, L"%s\\stub", g_root);
	_snwprintf_s(g_noStateRoot, MAX_PATH, _TRUNCATE, L"%s\\nostate", g_root);
	_snwprintf_s(g_appDir, MAX_PATH, _TRUNCATE, L"%s\\app", g_root);
	_snwprintf_s(g_logPath, MAX_PATH, _TRUNCATE, L"%s\\stub_log.txt", g_root);
	_snwprintf_s(g_staleRoot, MAX_PATH, _TRUNCATE, L"%s\\stale", g_root);
	_snwprintf_s(g_mixRoot, MAX_PATH, _TRUNCATE, L"%s\\mix", g_root);
	_snwprintf_s(g_pluginRoot, MAX_PATH, _TRUNCATE, L"%s", g_root);
	wcscat_s(g_pluginRoot, MAX_PATH, L"\\app\\plugin");
	_snwprintf_s(g_appEmptyDir, MAX_PATH, _TRUNCATE, L"%s\\appempty", g_root);
	_snwprintf_s(g_noStateAppDir, MAX_PATH, _TRUNCATE, L"%s\\appnostate", g_root);
	_snwprintf_s(g_badAppDir, MAX_PATH, _TRUNCATE, L"%s\\appbad", g_root);
	_snwprintf_s(g_regRoot, MAX_PATH, _TRUNCATE,
		L"Software\\SBA11yZdsrSelftest\\%lu\\Uninstall", (unsigned long)GetCurrentProcessId());
	_snwprintf_s(g_regNoKey, MAX_PATH, _TRUNCATE,
		L"Software\\SBA11yZdsrSelftest\\%lu\\NotThere", (unsigned long)GetCurrentProcessId());

	// 安装目录的形状是 <x>\zdsr\zdsr（官方默认位置就是 %ProgramFiles(x86)%\zdsr\zdsr）
	_snwprintf_s(stubDir, MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr", g_stubRoot);
	_snwprintf_s(noStateDir, MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr", g_noStateRoot);
	_snwprintf_s(staleSub, MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr", g_staleRoot);
	_snwprintf_s(mixSub, MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr", g_mixRoot);
	_snwprintf_s(noStatePlugin, MAX_PATH, _TRUNCATE, L"%s", g_noStateAppDir);
	wcscat_s(noStatePlugin, MAX_PATH, L"\\plugin");
	_snwprintf_s(badPlugin, MAX_PATH, _TRUNCATE, L"%s", g_badAppDir);
	wcscat_s(badPlugin, MAX_PATH, L"\\plugin");

	// 给后面的断言留一份（它们要按被测代码的拼法生成期望路径）
	wcscpy_s(g_stubDir, MAX_PATH, stubDir);
	wcscpy_s(g_noStateDir, MAX_PATH, noStateDir);
	wcscpy_s(g_staleDir, MAX_PATH, staleSub);
	wcscpy_s(g_mixDir, MAX_PATH, mixSub);
	_snwprintf_s(g_emptySub, MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr", g_emptyDir);

	mkdirs(g_root);
	mkdirs(g_emptyDir); mkdirs(g_appDir); mkdirs(g_stubRoot); mkdirs(g_noStateRoot);
	mkdirs(g_staleRoot); mkdirs(g_mixRoot); mkdirs(g_pluginRoot);
	mkdirs(g_appEmptyDir); mkdirs(g_noStateAppDir);
	mkdirs(stubDir); mkdirs(noStateDir); mkdirs(staleSub); mkdirs(mixSub);
	mkdirs(noStatePlugin); mkdirs(badPlugin);
	mkdirs(g_badAppDir);

	// 本程序目录 = 桩 dll 的来源（build_and_run.cmd 把它们一起编在这里）
	GetModuleFileNameW(NULL, g_selfDir, MAX_PATH);
	{
		wchar_t *s = wcsrchr(g_selfDir, L'\\');
		if(s) *s = 0;
	}
	wchar_t srcStub[MAX_PATH], srcNoState[MAX_PATH], srcMismatch[MAX_PATH];
	wchar_t dstStub[MAX_PATH], dstNoState[MAX_PATH], dstStale[MAX_PATH], dstMix[MAX_PATH], dstPlugin[MAX_PATH];
	wchar_t dstNoStatePlugin[MAX_PATH], dstBadPlugin[MAX_PATH];
	_snwprintf_s(srcStub, MAX_PATH, _TRUNCATE, L"%s\\zdsr_stub.dll", g_selfDir);
	_snwprintf_s(srcNoState, MAX_PATH, _TRUNCATE, L"%s\\zdsr_stub_noGetState.dll", g_selfDir);
	_snwprintf_s(srcMismatch, MAX_PATH, _TRUNCATE, L"%s\\zdsr_stub_mismatch.dll", g_selfDir);
	_snwprintf_s(dstStub, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", stubDir);
	_snwprintf_s(dstNoState, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", noStateDir);
	_snwprintf_s(dstStale, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", staleSub);
	_snwprintf_s(dstMix, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", mixSub);
	_snwprintf_s(dstPlugin, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", g_pluginRoot);
	_snwprintf_s(dstNoStatePlugin, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", noStatePlugin);
	_snwprintf_s(dstBadPlugin, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", badPlugin);
	if(!CopyFileW(srcStub, dstStub, FALSE) ||
	   !CopyFileW(srcNoState, dstNoState, FALSE) ||
	   !CopyFileW(srcMismatch, dstStale, FALSE) ||
	   !CopyFileW(srcMismatch, dstMix, FALSE) ||
	   !CopyFileW(srcStub, dstPlugin, FALSE) ||
	   !CopyFileW(srcNoState, dstNoStatePlugin, FALSE) ||
	   !CopyFileW(srcMismatch, dstBadPlugin, FALSE))
	{
		out("setup: cannot copy stub dlls (%s / %s / %s) - build them first\n",
			u8(srcStub), u8(srcNoState), u8(srcMismatch));
		out("setup: GetLastError=%lu\n", (unsigned long)GetLastError());
		if(g_report) fclose(g_report);
		return 2;
	}

	saveEnv(L"ProgramFiles(x86)", g_pf86Save, &g_pf86Had);
	saveEnv(L"ProgramFiles", g_pfSave, &g_pfHad);
	saveEnv(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regSave, &g_regHad);
	saveEnv(L"A11Y_ZDSR_DIR", g_pinSave, &g_pinHad);

	// 注册表沙箱。**S1–S10 与 S8 的语义都要求「那条路一个候选都不给」**，
	// 所以整个测试期间把 override 指到一个**不存在**的子键：注册表候选恒为空，
	// 既不受本机真实安装影响（真装了两份、装在别的盘都不影响断言），
	// 也不会去读玩家的真实卸载项。只有 S11–S15 会显式改掉它。
	regCleanup();
	setEnvW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regNoKey);
	{
		HKEY probe = NULL;
		bool absent = (RegOpenKeyExW(HKEY_CURRENT_USER, g_regNoKey, 0,
		                             KEY_READ | KEY_WOW64_32KEY, &probe) != ERROR_SUCCESS);
		if(probe) RegCloseKey(probe);
		checkf(absent, "setup: the registry sandbox path really does not exist (%s)", u8(g_regNoKey));
	}

	out("=== ZDSR backend self-test (32-bit, shares src\\zdsr_api.h with the plugin) ===\n");
	out("sandbox=%s\n", u8(g_root));
	out("stub=%s\n\n", u8(dstStub));

	// ---------------- S1：dll 不存在 ----------------
	{
		setEnvW(L"ProgramFiles(x86)", g_emptyDir);
		setEnvW(L"ProgramFiles", g_emptyDir);
		setEnvW(L"ZDSR_STUB_LOG", g_logPath);
		resetLog();

		ZdsrApi api;
		// 注意 appDir：这里**必须**用一个真正空空如也的目录。用 g_appDir 的话，
		// 它的 plugin\ 里躺着「随包兜底」那份桩 dll，S1 就不再是「哪儿都没有」了
		// （0.1.0.6 加进随包副本时，这条断言正是这么被自己的测试夹具绊倒的）。
		bool ok = ZdsrOpen(g_appEmptyDir, &api);
		checkf(!ok, "S1 missing dll -> judged unavailable");
		checkf(api.dll == NULL, "S1 no dll handle kept");
		checkf(wcsstr(api.why, L"没有一份 ZDSRAPI.dll 能用") != NULL,
			"S1 reason readable: %s", u8(api.why));
		checkf(hasU8(api.why, u8(g_appEmptyDir)),
			"S1 reason lists the caller-side paths it tried");
		out("      S1 why = %s\n", u8(api.why));
	}

	// ---------------- S2：state=2（未运行/未授权）----------------
	{
		setEnvW(L"ProgramFiles(x86)", g_stubRoot);
		setEnvW(L"ProgramFiles", g_emptyDir);
		setEnvW(L"ZDSR_STUB_STATE", L"2");
		setEnvW(L"ZDSR_STUB_INITRC", L"0");
		setEnvW(L"ZDSR_STUB_SPEAKRC", L"0");
		resetLog();

		ZdsrApi api;
		bool ok = ZdsrOpen(g_appDir, &api);
		char log[65536];
		readLog(log, sizeof(log));

		checkf(!ok, "S2 GetSpeakState=2 -> judged unavailable");
		checkf(wcsstr(api.why, L"GetSpeakState=2") != NULL, "S2 reason names state 2: %s", u8(api.why));
		checkf(countOccur(log, "InitTTS:") == 1, "S2 InitTTS called exactly once (got %d)", countOccur(log, "InitTTS:"));
		checkf(countOccur(log, "Speak:interrupt=") == 0,
			"S2 Speak was NOT called although Speak() would return 0 (got %d Speak calls) - availability is judged by GetSpeakState",
			countOccur(log, "Speak:interrupt="));
		out("      S2 why = %s\n", u8(api.why));
		out("      S2 stub log:\n");
		out("%s\n", log);
	}

	// ---------------- S3 / S4：state=3 / state=4 ----------------
	{
		const wchar_t *states[2] = { L"3", L"4" };
		for(int i = 0; i < 2; i++)
		{
			setEnvW(L"ZDSR_STUB_STATE", states[i]);
			resetLog();
			ZdsrApi api;
			bool ok = ZdsrOpen(g_appDir, &api);
			checkf(ok, "S%d GetSpeakState=%ls -> judged available", 3 + i, states[i]);
			checkf(ok && api.state == (3 + i), "S%d raw state echoed (%d)", 3 + i, api.state);
			checkf(ok && wcsstr(api.path, L"ZDSRAPI.dll") != NULL, "S%d loaded path recorded: %s", 3 + i, u8(api.path));
			out("      S%d path = %s\n", 3 + i, u8(api.path));
		}
	}

	// ---------------- S5 / S6：朗读、返回码、文本与打断标记 ----------------
	{
		setEnvW(L"ZDSR_STUB_STATE", L"4");
		setEnvW(L"ZDSR_STUB_SPEAKRC", L"2");
		resetLog();

		ZdsrApi api;
		bool ok = ZdsrOpen(g_appDir, &api);
		checkf(ok, "S5 open (state=4) ok");

		int rc = ZdsrSay(&api, L"probe", TRUE);
		char log[65536];
		readLog(log, sizeof(log));
		checkf(rc == 2, "S5 Speak rc=2 -> ZdsrSay returns non-zero (got %d)", rc);
		checkf(countOccur(log, "Speak:interrupt=") == 1, "S5 Speak was actually attempted (got %d)", countOccur(log, "Speak:interrupt="));

		// 打断标记 + 精确文本（UTF-16 逐码元比对）
		setEnvW(L"ZDSR_STUB_SPEAKRC", L"0");
		resetLog();
		const wchar_t *text = L"语音后端：争渡读屏";
		int rc1 = ZdsrSay(&api, text, TRUE);
		int rc2 = ZdsrSay(&api, text, FALSE);
		readLog(log, sizeof(log));

		char wantHex[1024], gotHex[2048], line[4096];
		utf16Hex(text, wantHex, sizeof(wantHex));
		checkf(rc1 == 0 && rc2 == 0, "S6 both speaks accepted (rc=%d,%d)", rc1, rc2);
		checkf(countOccur(log, "Speak:interrupt=") == 2, "S6 two Speak calls logged (got %d)", countOccur(log, "Speak:interrupt="));

		bool gotLine = lastLogLine(log, "Speak:text_utf16hex=", line, sizeof(line));
		checkf(gotLine, "S6 stub echoed the UTF-16 text");
		if(gotLine)
		{
			_snprintf_s(gotHex, sizeof(gotHex), _TRUNCATE, "%s", line + strlen("Speak:text_utf16hex="));
			checkf(strcmp(gotHex, wantHex) == 0, "S6 UTF-16 code units arrive unchanged");
			out("      S6 sent     = %s\n", wantHex);
			out("      S6 received = %s\n", gotHex);
		}
		bool gotInt = lastLogLine(log, "Speak:interrupt=", line, sizeof(line));
		checkf(gotInt && strcmp(line, "Speak:interrupt=0") == 0, "S6 interrupt flag passed through (%s)", gotInt ? line : "no line");
		{
			// 第一条 Speak 必须是 interrupt=1
			const char *p = strstr(log, "Speak:interrupt=");
			checkf(p && strncmp(p, "Speak:interrupt=1", 17) == 0, "S6 first call had interrupt=1 (%s)",
				p ? "ok" : "no Speak line");
		}
		out("      S6 stub log:\n%s\n", log);
	}

	// ---------------- S7：中途掉线（状态变 2 之后不许再 Speak）----------------
	{
		setEnvW(L"ZDSR_STUB_STATE", L"4");
		setEnvW(L"ZDSR_STUB_SPEAKRC", L"0");
		resetLog();

		ZdsrApi api;
		bool ok = ZdsrOpen(g_appDir, &api);
		checkf(ok, "S7 open ok");

		char log[65536];
		int rcFirst = ZdsrSay(&api, L"before the reader goes away", TRUE);
		readLog(log, sizeof(log));
		int before = countOccur(log, "Speak:interrupt=");
		checkf(rcFirst == 0 && before == 1, "S7 speak works before the drop (rc=%d calls=%d)", rcFirst, before);

		setEnvW(L"ZDSR_STUB_STATE", L"2");        // 读屏被关掉/掉授权
		bool fresh = ZdsrCheckState(&api, true);  // 强制重新问一次状态
		checkf(!fresh, "S7 state dropped to 2 -> ZdsrCheckState(force) false");

		int rc = ZdsrSay(&api, L"should not be spoken", TRUE);
		readLog(log, sizeof(log));
		int after = countOccur(log, "Speak:interrupt=");
		checkf(rc != 0, "S7 ZdsrSay returns non-zero after drop (got %d)", rc);
		checkf(after == before, "S7 ZdsrSay did NOT call Speak after drop (before=%d after=%d)", before, after);
	}

	// ---------------- S10：停止朗读（SpeechStop 走的就是这个）----------------
	{
		setEnvW(L"ZDSR_STUB_STATE", L"4");
		setEnvW(L"ZDSR_STUB_SPEAKRC", L"0");
		resetLog();

		ZdsrApi api;
		bool ok = ZdsrOpen(g_appDir, &api);
		checkf(ok, "S10 open ok");

		ZdsrSilence(&api);
		char log[65536];
		readLog(log, sizeof(log));
		checkf(strstr(log, "StopSpeak") != NULL, "S10 ZdsrSilence reaches the official StopSpeak export");
	}

	// ---------------- S9：dll 在但缺 GetSpeakState 导出 ----------------
	{
		// 同理：注册表那条路关掉、ProgramFiles 指到空目录，只剩 <appDir>plugin\ 里
		// 那一份「名字对、导出不对」的 dll —— 它必须被判掉，并且原因要说清缺什么。
		setEnvW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regNoKey);
		setEnvW(L"ProgramFiles(x86)", g_emptyDir);
		setEnvW(L"ProgramFiles", g_emptyDir);
		resetLog();
		ZdsrApi api;
		bool ok = ZdsrOpen(g_noStateAppDir, &api);
		checkf(!ok, "S9 dll without GetSpeakState -> judged unavailable");
		checkf(wcsstr(api.why, L"不是争渡的 dll") != NULL,
			"S9 reason names the missing export: %s", u8(api.why));
		out("      S9 why = %s\n", u8(api.why));
	}

	// ---------------- S8：真 dll 的只读握手（放在最后，且不 Speak）----------------
	{
		if(g_pf86Had) setEnvW(L"ProgramFiles(x86)", g_pf86Save); else SetEnvironmentVariableW(L"ProgramFiles(x86)", NULL);
		if(g_pfHad)   setEnvW(L"ProgramFiles", g_pfSave);       else SetEnvironmentVariableW(L"ProgramFiles", NULL);
		resetLog();

		ZdsrApi api;
		bool ok = ZdsrOpen(g_appDir, &api);      // appDir 里没有 dll → 只能命中真安装目录
		char log[65536];
		readLog(log, sizeof(log));

		checkf(ok, "S8 real ZDSRAPI.dll: InitTTS(0,NULL,FALSE)+state check ok");
		checkf(ok && api.initRc == 0, "S8 real InitTTS returned 0 (got %d)", api.initRc);
		checkf(ok && (api.state == ZDSR_STATE_SPEAKING || api.state == ZDSR_STATE_IDLE),
			"S8 real GetSpeakState in {3,4} (got %d)", api.state);
		checkf(countOccur(log, "InitTTS:") == 0, "S8 the stub did NOT answer (log is empty) -> the real DLL was loaded");
		out("      S8 path  = %s\n", u8(api.path));
		out("      S8 InitTTS=%d GetSpeakState=%d (%s)\n", api.initRc, api.state,
			api.state == ZDSR_STATE_IDLE ? "4=idle" : (api.state == ZDSR_STATE_SPEAKING ? "3=speaking" : "?"));
		out("      S8 read-only: Speak() was deliberately NOT called against the real reader\n");
		if(!ok) out("      S8 why = %s\n", u8(api.why));
	}

	// ---------------- S11–S15：注册表自动发现 + 候选顺序 + 版本不匹配自动跳下一份 ----------------
	//
	//  这一段要证明的三件事（对应发布包里那份兜底 dll 的安全前提）：
	//    1. 装在哪儿能被自动找到（注册表登记的非默认安装目录排在最前）；
	//    2. 「DisplayName 含争渡」的排在只含 zdsr 的前面（本机那个 ZDSR hook 工具就是后者，
	//       它的 InstallLocation 里**没有** ZDSRAPI.dll）；
	//    3. 一份 dll 报 InitTTS=1（版本不匹配）时自动换下一份，而不是判死 ——
	//       于是随包那份旧 dll 会被跳过，而安装目录那份坏掉时随包那份还能被够到。
	{
		HKEY parent = regOpenSandbox();
		bool regOk = (parent != NULL);
		checkf(regOk, "S11 setup: registry sandbox created under HKCU\\%s\\<pid>", u8(g_regParent));

		if(parent)
		{
			// 形状与真实卸载项一致：A_ 那条 = 「争渡读屏 → InstallLocation」（旧安装目录：坏 dll）；
			// B_ = 那个挂钩工具（名字里有 zdsr，但目录里没有 ZDSRAPI.dll）；
			// C_ = 另一份「争渡读屏」记录（InstallLocation 与默认路径重合，用来验去重）。
			bool made =
				regCreateEntry(HKEY_CURRENT_USER, parent, KEY_WOW64_32KEY, L"Uninstall\\A_SBA11yRegLookup",
				               L"争渡读屏", staleSub) &&
				regCreateEntry(HKEY_CURRENT_USER, parent, KEY_WOW64_32KEY, L"Uninstall\\B_SBA11yHookTool",
				               L"ZDSR hook control (selftest)", g_emptyDir) &&
				regCreateEntry(HKEY_CURRENT_USER, parent, KEY_WOW64_32KEY, L"Uninstall\\C_SBA11yZdsrPlain",
				               L"争渡读屏", mixSub);
			checkf(made, "S11 setup: uninstall-style entries written (DisplayName / InstallLocation)");
			RegCloseKey(parent);
			parent = NULL;
		}
	{
		HKEY v = NULL;
		LONG vrc = RegOpenKeyExW(HKEY_CURRENT_USER, g_regRoot, 0, KEY_READ | KEY_WOW64_32KEY, &v);
		wchar_t nm[256]; DWORD cch = 256; int seen = 0;
		if(v)
		{
			while(RegEnumKeyExW(v, (DWORD)seen, nm, &cch, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
			{ seen++; cch = 256; }
			RegCloseKey(v);
		}
		checkf(vrc == ERROR_SUCCESS && seen == 3,
			"S11 setup: read-back sees the 3 entries I just wrote (open rc=%ld, saw %d)", (long)vrc, seen);
	}

		setEnvW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regRoot);

		// ---- S11：候选顺序 ----
		setEnvW(L"ProgramFiles(x86)", g_stubRoot);     // 冒充「默认安装目录」里也有一份
		setEnvW(L"ProgramFiles", g_emptyDir);
		{
			wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
			int n = ZdsrCandidatePaths(g_appDir, paths, ZDSR_MAX_CANDIDATES);
			checkf(n >= 5, "S11 registry entry lands in the candidate list at all (got %d)", n);
			{
				char order[8192];
				order[0] = 0;
				int o = 0;
				for(int i = 0; i < n; i++)
					o += _snprintf_s(order + o, sizeof(order) - o, _TRUNCATE, "[%d] %s  ", i, u8(paths[i]));
				out("      S11 candidates (%d):\n        %s\n", n, order);
			}
			// 顺序：注册表（含「争渡」在前、只含 zdsr 的在后）→ %ProgramFiles(x86)% → %ProgramFiles% → <appDir>plugin → <appDir>
			wchar_t want[7][MAX_PATH];
			{
				expectDll(staleSub, want[0], MAX_PATH);      // 注册表 · 争渡读屏
				expectDll(mixSub, want[1], MAX_PATH);        // 注册表 · 争渡读屏（第二条）
				expectDll(g_emptyDir, want[2], MAX_PATH);    // 注册表 · 只含 zdsr 的挂钩工具
				expectDll(g_stubDir, want[3], MAX_PATH);     // %ProgramFiles(x86)%\zdsr\zdsr
				expectDll(g_emptySub, want[4], MAX_PATH);    // %ProgramFiles%\zdsr\zdsr
				expectDll(g_pluginRoot, want[5], MAX_PATH);  // 随包兜底那份
				expectDll(g_appDir, want[6], MAX_PATH);      // <appDir>
			}
			{
				int idx[7];
				for(int i = 0; i < 7; i++) idx[i] = -1;
				for(int i = 0; i < n; i++)
					for(int k = 0; k < 7; k++)
						if(idx[k] < 0 && _wcsicmp(paths[i], want[k]) == 0) idx[k] = i;

				checkf(idx[0] == 0, "S11 the registry InstallLocation is candidate #0 (got idx=%d)", idx[0]);
				checkf(idx[1] == 1, "S11 a 2nd 争渡读屏 entry follows it (got idx=%d)", idx[1]);
				checkf(idx[2] == 2, "S11 a DisplayName with only \"zdsr\" in it comes after every 争渡读屏 (got idx=%d)", idx[2]);
				checkf(idx[3] == 3, "S11 %%ProgramFiles(x86)%%\\zdsr\\zdsr comes after the registry (got idx=%d)", idx[3]);
				checkf(idx[4] == 4, "S11 %%ProgramFiles%%\\zdsr\\zdsr follows it (got idx=%d)", idx[4]);
				checkf(idx[5] == 5, "S11 <appDir>plugin\\ (the bundled copy) comes after every install dir (got idx=%d)", idx[5]);
				checkf(idx[6] == 6, "S11 <appDir> itself is last (got idx=%d)", idx[6]);
				if(idx[5] >= 0) out("      S11 bundled copy would be tried as candidate #%d\n", idx[5]);
			}
		}

		// ---- S16：注册表塞满到上限时，尾部四条（尤其随包那份）不能被挤掉 ----
		//
		//  注册表那一侧最多能凑出 8 条（4 条含「争渡」+ 4 条只含 zdsr，见 ZdsrRegistryDirs）。
		//  若没人管这件事，它就会把 %ProgramFiles(x86)% / %ProgramFiles% / <appDir>plugin /
		//  <appDir> 全挤出候选表 —— 症状是「随包兜底那份从来没被试过」，
		//  而候选表自己看上去完全正常（S11 只有 3 条注册表记录，离上限还远，盖不到这个洞）。
		//  这条用例就是把 8 条写满，要求尾部四条一条不少、顺序不变。
		{
			HKEY parent = regOpenSandbox();
			bool ok = (parent != NULL);
			if(parent)
			{
				for(int i = 0; i < 4; i++)
				{
					wchar_t sub[128], loc[MAX_PATH];
					_snwprintf_s(sub, 128, _TRUNCATE, L"Uninstall\\S16_Prod%d", i);
					_snwprintf_s(loc, MAX_PATH, _TRUNCATE, L"%s\\s16prod%d", g_emptyDir, i);
					ok = regCreateEntry(HKEY_CURRENT_USER, parent, KEY_WOW64_32KEY, sub,
					                    L"争渡读屏", loc) && ok;
					_snwprintf_s(sub, 128, _TRUNCATE, L"Uninstall\\S16_Loose%d", i);
					_snwprintf_s(loc, MAX_PATH, _TRUNCATE, L"%s\\s16loose%d", g_emptyDir, i);
					ok = regCreateEntry(HKEY_CURRENT_USER, parent, KEY_WOW64_32KEY, sub,
					                    L"ZDSR hook control (selftest)", loc) && ok;
				}
				RegCloseKey(parent);
				parent = NULL;
			}
			// 连同 S11 那 3 条，卸载项里已经有 11 条 —— 足够把注册表那 8 个位置顶满
			checkf(ok, "S16 setup: registry over-filled (S11's 3 + 4 争渡读屏 + 4 zdsr-only)");

			setEnvW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regRoot);
			setEnvW(L"ProgramFiles(x86)", g_stubRoot);
			setEnvW(L"ProgramFiles", g_emptyDir);
			{
				wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
				int n = ZdsrCandidatePaths(g_appDir, paths, ZDSR_MAX_CANDIDATES);
				if(n > 0) out("      S16 last candidate = %s\n", u8(paths[n - 1]));

				wchar_t want[4][MAX_PATH];
				expectDll(g_stubDir, want[0], MAX_PATH);     // %ProgramFiles(x86)%\zdsr\zdsr
				expectDll(g_emptySub, want[1], MAX_PATH);    // %ProgramFiles%\zdsr\zdsr
				expectDll(g_pluginRoot, want[2], MAX_PATH);  // 随包兜底那份
				expectDll(g_appDir, want[3], MAX_PATH);      // <appDir>
				int idx[4] = { -1, -1, -1, -1 };
				for(int i = 0; i < n; i++)
					for(int k = 0; k < 4; k++)
						if(idx[k] < 0 && _wcsicmp(paths[i], want[k]) == 0) idx[k] = i;

				// 注册表给满 8 条 + 尾部 4 条，正好是这张表的全部
				checkf(n == 12, "S16 a full registry (8) still leaves room for the 4 tail candidates (got %d)", n);
				checkf(idx[2] >= 0,
					"S16 REGRESSION: the bundled copy is still reachable with a full registry (idx=%d of %d)",
					idx[2], n);
				checkf(idx[0] == n - 4 && idx[1] == n - 3 && idx[2] == n - 2 && idx[3] == n - 1,
					"S16 the 4 tail candidates keep their order at the end (idx=%d,%d,%d,%d of %d)",
					idx[0], idx[1], idx[2], idx[3], n);
			}
		}

		// ---- S13：一份加载成功但 InitTTS=1（版本不匹配）→ 自动换下一份，最后成功 ----
		// 场景就是发布包要面对的那个：注册表指向的安装目录里是升级前的旧 dll，
		// 而随包兜底的那份是好的。旧的那份被跳过，玩家照样有声音。
		{
			setEnvW(L"ProgramFiles(x86)", g_emptyDir);   // 只有注册表 + 随包那份
			setEnvW(L"ProgramFiles", g_emptyDir);
			setEnvW(L"ZDSR_STUB_STATE", L"4");
			setEnvW(L"ZDSR_STUB_INITRC", L"0");
			setEnvW(L"ZDSR_STUB_LOG", g_logPath);
			resetLog();

			wchar_t mismatchLog[MAX_PATH];
			_snwprintf_s(mismatchLog, MAX_PATH, _TRUNCATE, L"%s", g_root);
			wcscat_s(mismatchLog, MAX_PATH, L"\\mismatch_log.txt");
			DeleteFileW(mismatchLog);
			setEnvW(L"ZDSR_STUB_MISMATCH_LOG", mismatchLog);

			wchar_t header[ZDSR_MAX_CANDIDATES][MAX_PATH];
			int nHeader = ZdsrCandidatePaths(g_appDir, header, ZDSR_MAX_CANDIDATES);
			checkf(nHeader > 0 && _wcsicmp(header[0], dstStale) == 0,
				"S13 precondition: the stale dll is tried first (%s)", nHeader > 0 ? u8(header[0]) : "no candidates");

			// 用「调用次数的增量」断言，而不是绝对次数：日志是被同一只桩反复追加的，
			// 数绝对值等于把「哪个用例先跑」也写进断言里（那是夹具耦合，不是被测行为）。
			char mlog[65536];
			readLog2(mismatchLog, mlog, sizeof(mlog));
			int staleBefore = countOccur(mlog, "InitTTS:");

			ZdsrApi api;
			bool ok = ZdsrOpen(g_appDir, &api);
			checkf(ok, "S13 InitTTS=1 on candidate #0 -> falls through to the next candidate and succeeds");
			checkf(ok && _wcsicmp(api.path, dstPlugin) == 0,
				"S13 the bundled copy (plugin\\) is what got loaded: %s", u8(api.path));
			checkf(ok && api.initRc == 0, "S13 final InitTTS rc is 0 (got %d)", api.initRc);
			checkf(ok && api.state == ZDSR_STATE_IDLE, "S13 final state is 4 (got %d)", api.state);

			char slog[65536];
			readLog2(mismatchLog, mlog, sizeof(mlog));
			readLog(slog, sizeof(slog));
			checkf(countOccur(mlog, "InitTTS:") - staleBefore == 2,
				"S13 both broken install-dir copies were asked once each (got %d)",
				countOccur(mlog, "InitTTS:") - staleBefore);
			checkf(countOccur(slog, "InitTTS:") == 1,
				"S13 the good dll was asked exactly once (got %d)", countOccur(slog, "InitTTS:"));
			checkf(countOccur(mlog, "GetSpeakState") == 0,
				"S13 the stale dll's GetSpeakState was never consulted (%d)", countOccur(mlog, "GetSpeakState"));
			checkf(countOccur(mlog, "Speak:") == 0,
				"S13 the stale dll was never asked to speak (%d)", countOccur(mlog, "Speak:"));
			out("      S13 stale-dll log: %s\n", mlog[0] ? "see report" : "(empty)");
			out("      S13 loaded        = %s\n", u8(api.path));
		}

		// ---- S14：注册表里那两处都坏（InitTTS=1）→ 随包兜底那份仍然被够到 ----
		{
			HKEY parent2 = regOpenSandbox();
			if(parent2)
			{
				regCreateEntry(HKEY_CURRENT_USER, parent2, KEY_WOW64_32KEY, L"Uninstall\\C_SBA11yZdsrPlain",
				               L"争渡读屏", mixSub);
				RegCloseKey(parent2);
			}

			setEnvW(L"ProgramFiles(x86)", g_emptyDir);
			setEnvW(L"ProgramFiles", g_emptyDir);
			resetLog();

			wchar_t mismatchLog[MAX_PATH];
			_snwprintf_s(mismatchLog, MAX_PATH, _TRUNCATE, L"%s", g_root);
			wcscat_s(mismatchLog, MAX_PATH, L"\\mismatch_log.txt");
			DeleteFileW(mismatchLog);

			ZdsrApi api;
			bool ok = ZdsrOpen(g_appDir, &api);
			checkf(ok, "S14 a broken install dir (InitTTS=1) no longer blocks the bundled copy");
			checkf(ok && _wcsicmp(api.path, dstPlugin) == 0,
				"S14 loaded from plugin\\ (the bundled fallback): %s", u8(api.path));

			char mlog[65536];
			readLog2(mismatchLog, mlog, sizeof(mlog));
			checkf(countOccur(mlog, "InitTTS:") >= 2,
				"S14 both broken install dirs were tried (got %d InitTTS calls)", countOccur(mlog, "InitTTS:"));
		}

		// ---- S15：所有候选都报版本不匹配 → 如实判不可用，理由里带上试过哪些路径 ----
		{
			HKEY parent3 = regOpenSandbox();
			if(parent3)
			{
				// 把注册表那条也改指到坏的那份，然后**换一个 appDir**：它的 plugin\ 里
				// 躺着的也是那份「永远版本不匹配」的桩。于是从注册表到 plugin\ 一路全坏，
				// 正好是「随包那份也是旧的」这种最坏情况。
				regCreateEntry(HKEY_CURRENT_USER, parent3, KEY_WOW64_32KEY, L"Uninstall\\A_SBA11yRegLookup",
				               L"争渡读屏", staleSub);
				RegCloseKey(parent3);
			}
			setEnvW(L"ProgramFiles(x86)", g_staleRoot);
			setEnvW(L"ProgramFiles", g_staleRoot);

			wchar_t mismatchLog[MAX_PATH];
			_snwprintf_s(mismatchLog, MAX_PATH, _TRUNCATE, L"%s", g_root);
			wcscat_s(mismatchLog, MAX_PATH, L"\\mismatch_log.txt");
			DeleteFileW(mismatchLog);
			resetLog();

			ZdsrApi api;
			bool ok = ZdsrOpen(g_badAppDir, &api);
			checkf(!ok, "S15 every candidate reports InitTTS=1 -> judged unavailable (no fake success)");
			checkf(hasU8(api.why, "InitTTS=1"), "S15 reason names the return code: %s", u8(api.why));
			checkf(hasU8(api.why, "版本不匹配"), "S15 reason says 版本不匹配 in plain words");
			checkf(hasU8(api.why, u8(dstStale)), "S15 reason lists the paths that were tried (the stale dll)");
			checkf(countU8(api.why, u8(dstStale)) == 1,
				"REGRESSION S15 a path is listed exactly once in the spoken reason (got %d)", countU8(api.why, u8(dstStale)));
			checkf(!hasU8(api.why, "GetSpeakState=1"), "S15 reason is not misreported as a state problem");
			{
				char mlog[65536];
				readLog2(mismatchLog, mlog, sizeof(mlog));
				checkf(countOccur(mlog, "InitTTS:") >= 2, "S15 the mismatch stub really was asked (%d)", countOccur(mlog, "InitTTS:"));
			}
			out("      S15 why = %s\n", u8(api.why));
		}
	}

	// ---------------- S12：没有注册表可选时，候选表照样成立 ----------------
	{
		setEnvW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regNoKey);
		setEnvW(L"ProgramFiles(x86)", g_stubRoot);
		setEnvW(L"ProgramFiles", g_emptyDir);

		wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
		int n = ZdsrCandidatePaths(g_appDir, paths, ZDSR_MAX_CANDIDATES);
		checkf(n == 4, "S12 registry miss -> the default + caller-side candidates still come out (got %d)", n);
		checkf(n > 0 && _wcsicmp(paths[0], dstStub) == 0,
			"S12 registry miss -> %%ProgramFiles(x86)%% leads: %s", n > 0 ? u8(paths[0]) : "none");
		{
			// 期望串按被测代码的拼法生成（目录 + \\ + ZDSRAPI.dll）
			wchar_t wantPf[MAX_PATH], wantPlugin[MAX_PATH], wantApp[MAX_PATH];
			_snwprintf_s(wantPf, MAX_PATH, _TRUNCATE, L"%s\\zdsr\\zdsr\\ZDSRAPI.dll", g_emptyDir);
			_snwprintf_s(wantPlugin, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", g_pluginRoot);
			_snwprintf_s(wantApp, MAX_PATH, _TRUNCATE, L"%s\\ZDSRAPI.dll", g_appDir);
			checkf(n > 1 && _wcsicmp(paths[1], wantPf) == 0,
				"S12 %%ProgramFiles%%\\zdsr\\zdsr is next (idx1=%s)", n > 1 ? u8(paths[1]) : "none");
			checkf(n > 2 && _wcsicmp(paths[2], wantPlugin) == 0,
				"S12 then <appDir>plugin\\ (idx2=%s)", n > 2 ? u8(paths[2]) : "none");
			checkf(n > 3 && _wcsicmp(paths[3], wantApp) == 0,
				"S12 and <appDir> is last (idx3=%s)", n > 3 ? u8(paths[3]) : "none");
		}
		{
			ZdsrApi api;
			bool ok = ZdsrOpen(g_appDir, &api);
			checkf(ok, "S12 open still works with an empty registry result");
		}
	}

	// ---------------- S17：A11Y_ZDSR_DIR 把候选**钉死** ----------------
	//
	//  文档（README 排查那节）承诺了这个开关：设了就只试写的那几处。
	//  它也是「随包那份到底有没有被试过」的现场判据 —— 钉到两处坏候选上，
	//  失败理由里必须**只有那两处**：没有默认安装目录，也没有随包的 plugin\。
	{
		wchar_t pin[3 * MAX_PATH];
		// 故意写成「带空格、带引号、两条」—— 这就是玩家从别处复制粘贴时的一般长相
		_snwprintf_s(pin, 3 * MAX_PATH, _TRUNCATE, L"  \"%s\" ; %s  ", g_emptyDir, g_staleDir);
		setEnvW(L"A11Y_ZDSR_DIR", pin);
		{
			wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
			int n = ZdsrCandidatePaths(g_appDir, paths, ZDSR_MAX_CANDIDATES);
			checkf(n == 2,
				"S17 a pinned A11Y_ZDSR_DIR (2 dirs, spaces + quotes) yields exactly 2 candidates (got %d)", n);

			wchar_t want[2][MAX_PATH];
			expectDll(g_emptyDir, want[0], MAX_PATH);
			expectDll(g_staleDir, want[1], MAX_PATH);
			checkf(n > 0 && _wcsicmp(paths[0], want[0]) == 0,
				"S17 the 1st pinned dir leads (got %s)", n > 0 ? u8(paths[0]) : "none");
			checkf(n > 1 && _wcsicmp(paths[1], want[1]) == 0,
				"S17 spaces and quotes around the 2nd are trimmed (got %s)", n > 1 ? u8(paths[1]) : "none");
		}
		{
			// 两处都是坏的（一处没 dll、一处版本不匹配）→ 如实判不可用
			ZdsrApi api;
			bool ok = ZdsrOpen(g_appDir, &api);
			checkf(!ok, "S17 pinned candidates are both unusable -> judged unavailable (no fake success)");
			checkf(hasU8(api.why, u8(g_staleDir)),
				"S17 the reason names the pinned dir (%s)", hasU8(api.why, u8(g_staleDir)) ? "yes" : "NO");
			checkf(!hasU8(api.why, u8(g_pluginRoot)),
				"S17 PINNING REPLACES the search: the bundled plugin\\ copy was never tried");
			checkf(!hasU8(api.why, u8(g_stubDir)),
				"S17 and neither was the %%ProgramFiles(x86)%% default dir");
		}
		// 立刻清掉：后面收尾与复查都要走正常候选
		SetEnvironmentVariableW(ZDSR_PIN_ENV, NULL);
		{
			wchar_t paths[ZDSR_MAX_CANDIDATES][MAX_PATH];
			int n = ZdsrCandidatePaths(g_appDir, paths, ZDSR_MAX_CANDIDATES);
			checkf(n == 4, "S17 clearing it restores the normal candidate list (got %d)", n);
		}
	}

	// ---------------- 收尾：清掉自检自己的注册表子树，恢复环境变量 ----------------
	regCleanup();
	if(g_regHad) setEnvW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", g_regSave);
	else         SetEnvironmentVariableW(L"ZDSR_UNINSTALL_KEY_OVERRIDE", NULL);
	if(g_pf86Had) setEnvW(L"ProgramFiles(x86)", g_pf86Save); else SetEnvironmentVariableW(L"ProgramFiles(x86)", NULL);
	if(g_pfHad)   setEnvW(L"ProgramFiles", g_pfSave);       else SetEnvironmentVariableW(L"ProgramFiles", NULL);
	if(g_pinHad)  setEnvW(L"A11Y_ZDSR_DIR", g_pinSave);     else SetEnvironmentVariableW(L"A11Y_ZDSR_DIR", NULL);

	out("\n=== RESULT: %d PASS, %d FAIL ===\n", g_pass, g_fail);
	if(g_report)
	{
		fprintf(g_report, "\n=== RESULT: %d PASS, %d FAIL ===\n", g_pass, g_fail);
		fclose(g_report);
	}
	return g_fail ? 1 : 0;
}
