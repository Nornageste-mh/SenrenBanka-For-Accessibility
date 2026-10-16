/*
 * 桩 DLL 的第二个变体：**InitTTS 永远返回 1（版本不匹配）**。
 *
 * 只用于 SenrenBanka-For-Accessibility 的机械自检。为什么不能拿 zdsr_stub.dll 设一个
 * 环境变量凑合：自检要证明的是「安装目录那份是坏的（26.6 之前留的旧 dll）时，
 * 会自动跳到下一份候选」，这条路径必须**与任何环境变量无关**地稳定成立 ——
 * 靠环境变量控制的桩，一旦哪个用例忘了设，就会静默地测了个别的东西。
 *
 * 行为刻意做到最小：InitTTS 恒返回 1，GetSpeakState 恒返回 1（官方语义：1=版本不匹配），
 * Speak/StopSpeak/Braille 什么都不做。真 dll 在这种状态下的表现也应当如此。
 *
 * 构建（32 位 MSVC；必须 x86 —— 目标进程是 32 位）：
 *     cl /nologo /c /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE zdsr_stub_mismatch.c
 *     link /nologo /DLL /MACHINE:X86 /DEF:zdsr_stub_mismatch.def /OUT:zdsr_stub_mismatch.dll zdsr_stub_mismatch.obj
 *
 * 环境变量：
 *     ZDSR_STUB_MISMATCH_LOG   日志文件路径（UTF-8，每行一次调用）；未设则不记
 *
 * 【注意】这不是争渡读屏的一部分，也**不得**随任何发布包分发。
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

/* 注意：源码刻意**只用 ASCII** —— 自检的构建脚本不依赖 /utf-8 也能编对。 */
static void mlog(const char *fmt, ...)
{
	wchar_t wpath[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"ZDSR_STUB_MISMATCH_LOG", wpath, MAX_PATH);
	if(n == 0 || n >= MAX_PATH) return;

	FILE *f = NULL;
	_wfopen_s(&f, wpath, L"ab");
	if(!f) return;

	char line[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
	va_end(ap);
	fputs(line, f);
	fputc('\n', f);
	fclose(f);
}

int __stdcall InitTTS(int type, const wchar_t *channelName, int bKeyDownInterrupt)
{
	mlog("InitTTS:type=%d keyDownInterrupt=%d -> rc=1 (version mismatch, always)",
		type, bKeyDownInterrupt ? 1 : 0);
	(void)channelName;
	return 1;
}

int __stdcall Speak(const wchar_t *text, int bInterrupt)
{
	mlog("Speak:interrupt=%d -> rc=1 (must never be reached when InitTTS already failed)",
		bInterrupt ? 1 : 0);
	(void)text;
	return 1;
}

int __stdcall GetSpeakState(void)
{
	mlog("GetSpeakState -> 1 (version mismatch)");
	return 1;
}

void __stdcall StopSpeak(void)
{
	mlog("StopSpeak");
}

int __stdcall Braille(const wchar_t *text, int bFlashMessage)
{
	mlog("Braille:flash=%d -> rc=1", bFlashMessage ? 1 : 0);
	(void)text;
	return 1;
}
