#include "CrashHandler.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

// Helper to get a timestamp for the crash log filename
static std::string GetCrashTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t time_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_time;
    localtime_s(&local_time, &time_now);

    std::stringstream ss;
    ss << std::put_time(&local_time, "%Y%m%d_%H%M%S");
    return ss.str();
}

// Convert common Windows Exception Codes to human-readable strings
static const char* ExceptionCodeToString(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION (Invalid memory read/write)";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "INTEGER_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW (Stack exhaustion)";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        default:                                 return "UNKNOWN_EXCEPTION";
    }
}

// The callback function Windows executes when a crash happens
LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* exceptionInfo)
{
    std::string crashLogName = "crash_" + GetCrashTimestamp() + ".log";
    std::ofstream logFile(crashLogName, std::ios::out);

    if (logFile.is_open())
    {
        PEXCEPTION_RECORD record = exceptionInfo->ExceptionRecord;
        PCONTEXT context = exceptionInfo->ContextRecord;

        logFile << "==================================================\n";
        logFile << "           APPLICATION CRASH DETECTED             \n";
        logFile << "==================================================\n\n";

        logFile << "Exception Code:    0x" << std::hex << std::uppercase << record->ExceptionCode
            << " -> " << ExceptionCodeToString(record->ExceptionCode) << "\n";
        logFile << "Fault Address:     0x" << std::hex << record->ExceptionAddress << "\n";
        logFile << "Flags:             0x" << std::hex << record->ExceptionFlags << "\n\n";

        // If it's an Access Violation, determine if it was a Read or Write fault
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            ULONG_PTR isWrite = record->ExceptionInformation[0];
            ULONG_PTR targetAddr = record->ExceptionInformation[1];
            logFile << "Thread attempted to " << (isWrite ? "WRITE to" : "READ from")
                << " inaccessible memory address: 0x" << std::hex << targetAddr << "\n\n";
        }

        // Log CPU Register State (x64 specific)
#if defined(_M_X64) || defined(__x64__ )
        logFile << "--- CPU Register State (x64) ---\n";
        logFile << "RIP: 0x" << std::hex << context->Rip << "\n";
        logFile << "RSP: 0x" << std::hex << context->Rsp << "  RBP: 0x" << context->Rbp << "\n";
        logFile << "RAX: 0x" << std::hex << context->Rax << "  RBX: 0x" << context->Rbx << "\n";
        logFile << "RCX: 0x" << std::hex << context->Rcx << "  RDX: 0x" << context->Rdx << "\n";
        logFile << "R8:  0x" << std::hex << context->R8 << "  R9:  0x" << context->R9 << "\n";
#endif

        logFile << "\n==================================================\n";
        logFile.close();
    }

    // Optional: Inform the user via a clean native dialog right before dying
    MessageBoxA(nullptr,
        "The application encountered a fatal error and needs to close.\nA crash log file has been saved to the directory.",
        "Fatal Error", MB_ICONERROR | MB_OK);

    // EXCEPTION_EXECUTE_HANDLER instructs Windows to terminate the app immediately 
    // and suppresses the default "Application has stopped working" OS dialog.
    return EXCEPTION_EXECUTE_HANDLER;
}

void CrashHandler::Initialize()
{
    // Pass our function pointer directly to the OS exception router
    SetUnhandledExceptionFilter(UnhandledCrashFilter);
}