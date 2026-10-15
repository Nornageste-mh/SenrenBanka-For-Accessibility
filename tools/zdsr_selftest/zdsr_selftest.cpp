// ============================================================================
//  zdsr_selftest.cpp —— ZDSR（争渡读屏）后端的机械自检（32 位，MSVC）
//
//  为什么要有它：争渡的「装没装 / 跑没跑 / 授没授权」在开发机上无法稳定复现
//  （同工作区先例 Framework\platform\Zdsr.cs 的既有结论），所以选路与失败路径
//  只能靠**桩 DLL** 机械验证。本程序 include 的 src\zdsr_api.h 就是插件本体用的
//  那一份 —— 断言过的逻辑 = 插件里真正执行的逻辑，不是照抄一遍。
//
//  验的东西：
//    S1 dll 不存在          → 判不可用，原因可读（不崩、不假装成功）
//    S2 GetSpeakState=2     → 判不可用，**且 Speak 根本没被调用**
//                             （可用性判据绝不能是 Speak 的返回值：未授权时它照样返回 0）
//    S3 GetSpeakState=3     → 判可用（正在朗读）
//    S4 GetSpeakState=4     → 判可用（空闲）
//    S5 Speak 返回 2        → ZdsrSay 返回非 0，调用方据此放弃这一级
//    S6 文本与打断标记       → 桩日志里的 UTF-16 码元与传入**逐码元相同**，interrupt 原样透传
//    S7 中途掉线            → 状态变 2 后 ZdsrSay 不再调用 Speak（只认状态，不看返回值）
//    S8 真 dll 只读握手      → InitTTS(0,NULL,FALSE)=0、GetSpeakState ∈ {3,4}
//                             （**只读**：不调用 Speak，不会替玩家说话）
//    S9 dll 在但缺导出       → 判不可用并说明缺导出
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
static wchar_t g_stubRoot[MAX_PATH];  // 里面有 zdsr\zdsr\ZDSRAPI.dll（桩）
static wchar_t g_noStateRoot[MAX_PATH];
static wchar_t g_appDir[MAX_PATH];    // 冒充「游戏目录」：里面什么都没有
static wchar_t g_logPath[MAX_PATH];
static wchar_t g_selfDir[MAX_PATH];
static wchar_t g_pf86Save[MAX_PATH], g_pfSave[MAX_PATH];
static bool    g_pf86Had = false, g_pfHad = false;

static void out(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
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
	_vsnprintf_s(label, sizeof(label), _TRUNCATE, fmt, ap);
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

	// ---- 沙箱：%TEMP%\a11y_zdsr_selftest\<pid>\ ----
	wchar_t tmp[MAX_PATH];
	if(GetTempPathW(MAX_PATH, tmp) == 0) { out("GetTempPath failed\n"); return 2; }
	_snwprintf_s(g_root, MAX_PATH, _TRUNCATE, L"%sa11y_zdsr_selftest\\%lu\\", tmp, (unsigned long)GetCurrentProcessId());
	_snwprintf_s(g_emptyDir, MAX_PATH, _TRUNCATE, L"%sempty\\", g_root);
	_snwprintf_s(g_stubRoot, MAX_PATH, _TRUNCATE, L"%sstub\\", g_root);
	_snwprintf_s(g_noStateRoot, MAX_PATH, _TRUNCATE, L"%snostate\\", g_root);
	_snwprintf_s(g_appDir, MAX_PATH, _TRUNCATE, L"%sapp\\", g_root);
	_snwprintf_s(g_logPath, MAX_PATH, _TRUNCATE, L"%sstub_log.txt", g_root);
	mkdirs(g_emptyDir); mkdirs(g_appDir); mkdirs(g_stubRoot); mkdirs(g_noStateRoot);

	wchar_t stubSub[MAX_PATH], noStateSub[MAX_PATH];
	_snwprintf_s(stubSub, MAX_PATH, _TRUNCATE, L"%szdsr\\zdsr\\", g_stubRoot);
	_snwprintf_s(noStateSub, MAX_PATH, _TRUNCATE, L"%szdsr\\zdsr\\", g_noStateRoot);
	mkdirs(stubSub); mkdirs(noStateSub);

	// 本程序目录 = 桩 dll 的来源（build_and_run.cmd 把它们一起编在这里）
	GetModuleFileNameW(NULL, g_selfDir, MAX_PATH);
	{
		wchar_t *s = wcsrchr(g_selfDir, L'\\');
		if(s) s[1] = 0;
	}
	wchar_t srcStub[MAX_PATH], srcNoState[MAX_PATH], dstStub[MAX_PATH], dstNoState[MAX_PATH];
	_snwprintf_s(srcStub, MAX_PATH, _TRUNCATE, L"%szdsr_stub.dll", g_selfDir);
	_snwprintf_s(srcNoState, MAX_PATH, _TRUNCATE, L"%szdsr_stub_noGetState.dll", g_selfDir);
	_snwprintf_s(dstStub, MAX_PATH, _TRUNCATE, L"%sZDSRAPI.dll", stubSub);
	_snwprintf_s(dstNoState, MAX_PATH, _TRUNCATE, L"%sZDSRAPI.dll", noStateSub);
	if(!CopyFileW(srcStub, dstStub, FALSE) || !CopyFileW(srcNoState, dstNoState, FALSE))
	{
		out("setup: cannot copy stub dlls (%s / %s) - build them first\n", u8(srcStub), u8(srcNoState));
		out("setup: GetLastError=%lu\n", (unsigned long)GetLastError());
		if(g_report) fclose(g_report);
		return 2;
	}

	saveEnv(L"ProgramFiles(x86)", g_pf86Save, &g_pf86Had);
	saveEnv(L"ProgramFiles", g_pfSave, &g_pfHad);

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
		bool ok = ZdsrOpen(g_appDir, &api);
		checkf(!ok, "S1 missing dll -> judged unavailable");
		checkf(api.dll == NULL, "S1 no dll handle kept");
		checkf(wcsstr(api.why, L"找不到可加载的 ZDSRAPI.dll") != NULL, "S1 reason readable: %s", u8(api.why));
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
		setEnvW(L"ProgramFiles(x86)", g_noStateRoot);
		resetLog();
		ZdsrApi api;
		bool ok = ZdsrOpen(g_appDir, &api);
		checkf(!ok, "S9 dll without GetSpeakState -> judged unavailable");
		checkf(wcsstr(api.why, L"缺导出") != NULL, "S9 reason names the missing export: %s", u8(api.why));
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

	out("\n=== RESULT: %d PASS, %d FAIL ===\n", g_pass, g_fail);
	if(g_report)
	{
		fprintf(g_report, "\n=== RESULT: %d PASS, %d FAIL ===\n", g_pass, g_fail);
		fclose(g_report);
	}
	return g_fail ? 1 : 0;
}
