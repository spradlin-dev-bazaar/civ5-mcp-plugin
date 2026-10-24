#include "CvGameCoreDLLPCH.h"
#include "McpCrash.h"
#include "FireWorks/FILogFile.h"
#include <windows.h>
#include <dbghelp.h>
#include <stdarg.h>

// Writes AICrash.log for exceptions raised in this DLL or with it on the call
// stack: the exception, the tool that was running, and the stack as module+offset,
// which the linker map resolves. The game's own code raises and handles many
// exceptions that never involve this DLL; those are skipped.

namespace
{
	char s_context[128] = "";
	char s_line[256] = "";
	volatile LONG s_busy = 0;
	PVOID s_handler = NULL;
	const int MAX_FRAMES = 64;
	DWORD64 s_frames[MAX_FRAMES];

	PVOID ModuleOf(DWORD64 pc)
	{
		MEMORY_BASIC_INFORMATION mbi;
		return VirtualQuery((LPCVOID)(DWORD_PTR)pc, &mbi, sizeof(mbi)) ? mbi.AllocationBase : NULL;
	}

	bool InThisDll(DWORD64 pc)
	{
		PVOID self = ModuleOf((DWORD64)(DWORD_PTR)&ModuleOf);
		return self != NULL && ModuleOf(pc) == self;
	}

	void Line(FILogFile* pLog, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		_vsnprintf_s(s_line, sizeof(s_line), _TRUNCATE, fmt, ap);
		va_end(ap);
		pLog->Msg("%s", s_line);
	}

	typedef BOOL (WINAPI *StackWalkFn)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
	typedef PVOID (WINAPI *FunctionTableAccessFn)(HANDLE, DWORD64);
	typedef DWORD64 (WINAPI *GetModuleBaseFn)(HANDLE, DWORD64);
	typedef BOOL (WINAPI *SymInitializeFn)(HANDLE, PCSTR, BOOL);

	void Describe(DWORD64 pc, char* out, size_t cap)
	{
		MEMORY_BASIC_INFORMATION mbi;
		char path[MAX_PATH] = "";
		if(VirtualQuery((LPCVOID)(DWORD_PTR)pc, &mbi, sizeof(mbi)) && mbi.AllocationBase
		   && GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH))
		{
			const char* name = strrchr(path, '\\');
			name = name ? name + 1 : path;
			_snprintf_s(out, cap, _TRUNCATE, "%s+0x%08X", name, (unsigned)(pc - (DWORD64)(DWORD_PTR)mbi.AllocationBase));
		}
		else _snprintf_s(out, cap, _TRUNCATE, "0x%08X", (unsigned)pc);
	}

	LONG WINAPI OnException(EXCEPTION_POINTERS* info)
	{
		DWORD code = info->ExceptionRecord->ExceptionCode;
		bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
		          || code == EXCEPTION_PRIV_INSTRUCTION || code == 0xC0000409;
		if(!fatal || InterlockedCompareExchange(&s_busy, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

		int iFrames = 0;
		bool bOurs = InThisDll((DWORD64)(DWORD_PTR)info->ExceptionRecord->ExceptionAddress);
		HMODULE hDbg = LoadLibraryA("dbghelp.dll");
		StackWalkFn pWalk = hDbg ? (StackWalkFn)GetProcAddress(hDbg, "StackWalk64") : NULL;
		FunctionTableAccessFn pFta = hDbg ? (FunctionTableAccessFn)GetProcAddress(hDbg, "SymFunctionTableAccess64") : NULL;
		GetModuleBaseFn pGmb = hDbg ? (GetModuleBaseFn)GetProcAddress(hDbg, "SymGetModuleBase64") : NULL;
		SymInitializeFn pInit = hDbg ? (SymInitializeFn)GetProcAddress(hDbg, "SymInitialize") : NULL;
		if(pWalk && pFta && pGmb)
		{
			if(pInit) pInit(GetCurrentProcess(), NULL, TRUE);
			CONTEXT ctx = *info->ContextRecord;
			STACKFRAME64 frame;
			memset(&frame, 0, sizeof(frame));
			frame.AddrPC.Offset = ctx.Eip;    frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Offset = ctx.Ebp; frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Offset = ctx.Esp; frame.AddrStack.Mode = AddrModeFlat;
			while(iFrames < MAX_FRAMES && pWalk(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx, NULL, pFta, pGmb, NULL) && frame.AddrPC.Offset != 0)
			{
				s_frames[iFrames++] = frame.AddrPC.Offset;
				bOurs = bOurs || InThisDll(frame.AddrPC.Offset);
			}
		}

		FILogFile* pLog = bOurs ? LOGFILEMGR.GetLog("AICrash.log", FILogFile::kDontTimeStamp) : NULL;
		if(pLog)
		{
			char where[160];
			Describe((DWORD64)(DWORD_PTR)info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
			Line(pLog, "---- exception (fatal only if the game died right after) ----");
			Line(pLog, "exception 0x%08X at %s", (unsigned)code, where);
			if(code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
				Line(pLog, "access violation: %s address 0x%08X", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (unsigned)info->ExceptionRecord->ExceptionInformation[1]);
			Line(pLog, "running: %s", s_context[0] ? s_context : "(no tool)");

			for(int i = 0; i < iFrames; ++i)
			{
				Describe(s_frames[i], where, sizeof(where));
				Line(pLog, "  #%02d %s", i, where);
			}
			if(iFrames == 0) Line(pLog, "  (no stack: dbghelp not available or the walk failed)");
			Line(pLog, "---- end ----");
		}
		s_busy = 0;
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

void McpInstallCrashHandler()
{
	if(s_handler == NULL) s_handler = AddVectoredExceptionHandler(1, OnException);
}

void McpUninstallCrashHandler()
{
	if(s_handler != NULL) RemoveVectoredExceptionHandler(s_handler);
	s_handler = NULL;
}

void McpCrashSetContext(const char* text)
{
	strncpy_s(s_context, sizeof(s_context), text ? text : "", _TRUNCATE);
}
