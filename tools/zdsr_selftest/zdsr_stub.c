/*
 * 争渡读屏 API（ZDSRAPI）的**桩 DLL** —— 只用于 SenrenBanka-For-Accessibility 的机械自检。
 *
 * 它按官方签名（ZDSRAPI.txt）实现五个导出，把每次调用**原样记进一个日志文件**，
 * 返回值由环境变量决定。这样自检程序就能在「没有争渡、也没有授权」的机器上断言：
 *   · 我们的委托签名/字符集/调用约定有没有把参数送错（文本、NULL、BOOL 都回显）
 *   · 可用性判据是不是 GetSpeakState（而不是 Speak 的返回值）
 *   · 失败之后是不是如实判不可用（而不是假装成功）
 *
 * 构建（32 位 MSVC；必须 x86 —— 目标进程是 32 位）：
 *     cl /nologo /c /O2 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE zdsr_stub.c
 *     link /nologo /DLL /MACHINE:X86 /DEF:zdsr_stub.def /OUT:zdsr_stub.dll zdsr_stub.obj
 * 定义 ZDSR_STUB_NO_GETSTATE 并改用 zdsr_stub_noGetState.def，可编出**没有
 * GetSpeakState 导出**的旧版（用来验证「缺导出 → 判不可用」这条路径）。
 *
 * 环境变量：
 *     ZDSR_STUB_LOG        日志文件路径（UTF-8，每行一次调用）；未设则不记
 *     ZDSR_STUB_STATE      GetSpeakState 的返回值，默认 4
 *     ZDSR_STUB_INITRC     InitTTS 的返回值，默认 0
 *     ZDSR_STUB_SPEAKRC    Speak 的返回值，默认 0
 *     ZDSR_STUB_BRAILLERC  Braille 的返回值，默认 0
 *
 * 【注意】这不是争渡读屏的一部分，也**不得**随任何发布包分发。
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stub_log(const char *fmt, ...)
{
	wchar_t wpath[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"ZDSR_STUB_LOG", wpath, MAX_PATH);
	if(n == 0 || n >= MAX_PATH) return;

	FILE *f = NULL;
	_wfopen_s(&f, wpath, L"ab");
	if(!f) return;

	char line[4096];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
	va_end(ap);
	fputs(line, f);
	fputc('\n', f);
	fclose(f);
}

static int env_int(const char *name, int dflt)
{
	char buf[32];
	DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
	if(n == 0 || n >= sizeof(buf)) return dflt;
	return atoi(buf);
}

/* 把 UTF-16 文本原样回显两种形式：
 *   text_utf8        —— 给人看（日志是 UTF-8）
 *   text_utf16hex    —— 给断言逐码元比对（不受任何编码转换影响）
 */
static void log_text(const char *tag, const wchar_t *w)
{
	if(w == NULL) { stub_log("%s=<NULL>", tag); return; }

	int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if(need > 0)
	{
		char *buf = (char *)malloc((size_t)need + 1);
		if(buf)
		{
			WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, need, NULL, NULL);
			stub_log("%s_utf8=%s", tag, buf);
			free(buf);
		}
	}

	char hex[4096];
	hex[0] = 0;
	size_t o = 0;
	int units = 0;
	for(; w[units] && units < 240; units++)
	{
		if(o + 6 >= sizeof(hex)) break;
		o += (size_t)sprintf_s(hex + o, sizeof(hex) - o, "%04X ", (unsigned)w[units]);
	}
	hex[o ? o - 1 : 0] = 0;   /* 去掉最后一个空格 */
	stub_log("%s_utf16hex=%s%s", tag, hex, (units >= 240) ? " <TRUNCATED>" : "");
}

int __stdcall InitTTS(int type, const wchar_t *channelName, int bKeyDownInterrupt)
{
	stub_log("InitTTS:type=%d keyDownInterrupt=%d channel=%s",
		type, bKeyDownInterrupt ? 1 : 0,
		channelName == NULL ? "<NULL>" : "<NOT-NULL>");
	if(channelName) log_text("InitTTS:channel", channelName);
	return env_int("ZDSR_STUB_INITRC", 0);
}

int __stdcall Speak(const wchar_t *text, int bInterrupt)
{
	stub_log("Speak:interrupt=%d", bInterrupt ? 1 : 0);
	log_text("Speak:text", text);
	return env_int("ZDSR_STUB_SPEAKRC", 0);
}

#ifndef ZDSR_STUB_NO_GETSTATE
int __stdcall GetSpeakState(void)
{
	int st = env_int("ZDSR_STUB_STATE", 4);
	stub_log("GetSpeakState:ret=%d", st);
	return st;
}
#endif

void __stdcall StopSpeak(void)
{
	stub_log("StopSpeak");
}

int __stdcall Braille(const wchar_t *text, int bFlashMessage)
{
	stub_log("Braille:flash=%d", bFlashMessage ? 1 : 0);
	if(text) log_text("Braille:text", text);
	return env_int("ZDSR_STUB_BRAILLERC", 0);
}
