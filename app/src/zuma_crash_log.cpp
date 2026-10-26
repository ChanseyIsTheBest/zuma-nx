// Windows: logs fatal host exceptions that the SDK does not report (illegal instruction from a __builtin_trap in the
// generated code, divide by zero, stack overflow, ...) with the module and offset, so the crash can be symbolized
// afterwards: llvm-symbolizer --obj=zuma.exe 0x<0x140000000 + offset>. Access violations are left alone: the SDK
// handles those itself (guest memory, MMIO) and reports the unhandled ones.

#if defined(_WIN32)

#include <windows.h>

#include <cstring>

#include <rex/logging.h>
#include <rex/logging/api.h>

namespace {

LONG CALLBACK LogFatalException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_STACK_BUFFER_OVERRUN:
    // zuma-nx (2026-10-26): the first boot ended with 0xC000008F (x87 inexact result) and no log line: floating-point
    // traps, which only happen when something unmasked them in the control word (x87) or MXCSR (SSE).
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case STATUS_FLOAT_MULTIPLE_FAULTS:
    case STATUS_FLOAT_MULTIPLE_TRAPS:
    // rex::debug::Break() (__debugbreak): the SDK's RtlRaiseException ends a guest C++ throw this way
    // (zuma_throw_log.cpp logs the thrown type just before).
    case EXCEPTION_BREAKPOINT:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) && module) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  const char* name = std::strrchr(path, '\\');
  REXLOG_CRITICAL("[crash] host exception {:08X} at {:016X} ({}+0x{:X}) on thread {}", uint32_t(code), pc,
                  name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module), GetCurrentThreadId());
  if (info->ContextRecord) {
    // The x87 control word and MXCSR at the fault (a trap needs its mask bit clear: x87 bits 0-5, MXCSR bits 7-12).
    const auto& f = info->ContextRecord->FltSave;
    REXLOG_CRITICAL("[crash] x87 control word {:04X}, MXCSR {:08X}", uint32_t(f.ControlWord),
                    uint32_t(info->ContextRecord->MxCsr));
  }
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

// Installed before main, first in the chain; it only logs and never handles.
const PVOID g_handler = AddVectoredExceptionHandler(1, LogFatalException);

}  // namespace

#endif
