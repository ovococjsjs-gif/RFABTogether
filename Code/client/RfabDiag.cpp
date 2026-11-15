// See RfabDiag.h. Everything here must work when the rest of the client does not: no heap after
// InitPaths, no spdlog, no game types. Only the Windows API, the C runtime and the two headers of
// common/Diag.

#include <RfabDiag.h>

#include <common/Diag/Breadcrumbs.h>
#include <common/Diag/HangWatch.h>

#include <BuildInfo.h>

#include <Windows.h>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>

extern "C" const wchar_t* GetTiltedClientDirW() noexcept;

namespace
{
constexpr size_t kPathCapacity = 1024;

// File names. Written by InitPaths (twice, both times before the game runs), read by everything
// else, including the crash writer on any thread.
wchar_t s_clientDir[kPathCapacity]{};
wchar_t s_gameDir[kPathCapacity]{};
wchar_t s_logDir[kPathCapacity]{};
wchar_t s_startupClient[kPathCapacity]{};
wchar_t s_startupGame[kPathCapacity]{};
wchar_t s_crashClient[kPathCapacity]{};
wchar_t s_crashGame[kPathCapacity]{};

bool CopyPath(wchar_t* apOut, const wchar_t* apText) noexcept
{
    apOut[0] = L'\0';
    if (!apText)
        return false;
    const size_t length = std::wcslen(apText);
    if (length == 0 || length >= kPathCapacity)
        return false;
    std::memcpy(apOut, apText, (length + 1) * sizeof(wchar_t));
    // No trailing separator, so that Join never produces "x\\\\y".
    size_t end = length;
    while (end > 3 && (apOut[end - 1] == L'\\' || apOut[end - 1] == L'/'))
        apOut[--end] = L'\0';
    return true;
}

bool Join(wchar_t* apOut, const wchar_t* apDir, const wchar_t* apLeaf) noexcept
{
    apOut[0] = L'\0';
    if (!apDir || !*apDir || !apLeaf)
        return false;
    const size_t dirLength = std::wcslen(apDir);
    const size_t leafLength = std::wcslen(apLeaf);
    if (dirLength + 1 + leafLength >= kPathCapacity)
        return false;
    std::memcpy(apOut, apDir, dirLength * sizeof(wchar_t));
    apOut[dirLength] = L'\\';
    std::memcpy(apOut + dirLength + 1, apLeaf, (leafLength + 1) * sizeof(wchar_t));
    return true;
}

bool SamePath(const wchar_t* apLeft, const wchar_t* apRight) noexcept
{
    return CompareStringOrdinal(apLeft, -1, apRight, -1, TRUE) == CSTR_EQUAL;
}

bool EnsureDirectory(const wchar_t* apPath) noexcept
{
    if (!apPath || !*apPath)
        return false;
    if (CreateDirectoryW(apPath, nullptr))
        return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD attributes = GetFileAttributesW(apPath);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// A folder that exists is not yet a folder that can be written to (a game under Program Files).
bool CanWriteInto(const wchar_t* apDir) noexcept
{
    wchar_t probe[kPathCapacity];
    if (!Join(probe, apDir, L"rfab_write_probe.tmp"))
        return false;
    const HANDLE file = CreateFileW(probe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(file);
    return true;
}

size_t AppendText(char* apOut, size_t aOutSize, size_t aPosition, const char* apFormat, ...) noexcept
{
    if (aPosition >= aOutSize)
        return aPosition;
    va_list arguments;
    va_start(arguments, apFormat);
    const int written = std::vsnprintf(apOut + aPosition, aOutSize - aPosition, apFormat, arguments);
    va_end(arguments);
    if (written <= 0)
        return aPosition;
    const size_t left = aOutSize - aPosition - 1;
    return aPosition + (static_cast<size_t>(written) < left ? static_cast<size_t>(written) : left);
}

// One record at a time: the buffers below are static (a stack overflow leaves no stack to spare).
std::atomic<bool> s_recordBusy{false};
char s_recordText[16384];

std::atomic<bool> s_endRecorded{false};  // one of TERMINATE / PURECALL / INVALID_PARAMETER / ABORT is on disk
std::terminate_handler s_previousTerminate = nullptr;
_purecall_handler s_previousPurecall = nullptr;
_invalid_parameter_handler s_previousInvalidParameter = nullptr;

// The calling thread's own stack, as "[unwind #n]" lines.
size_t AppendOwnStack(char* apOut, size_t aOutSize, size_t aPosition, ULONG aSkip) noexcept
{
    void* frames[32]{};
    const USHORT count = CaptureStackBackTrace(aSkip, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
    {
        char frame[192]{};
        RfabDiag::FormatAddress(reinterpret_cast<uintptr_t>(frames[i]), frame, sizeof(frame));
        aPosition = AppendText(apOut, aOutSize, aPosition, "\r\n    [unwind #%u] %s", static_cast<unsigned>(i), frame);
    }
    return aPosition;
}

// "[stamp] [TAG] TID=n text", the caller's stack and the breadcrumbs, into both crash logs and (the
// first line only) both startup logs.
void WriteEndRecord(const char* apTag, const char* apText) noexcept
{
    if (s_recordBusy.exchange(true))
        return;

    char head[1024];
    std::snprintf(head, sizeof(head), "[%s] TID=%lu %s", apTag, static_cast<unsigned long>(GetCurrentThreadId()), apText);

    size_t position = RfabDiag::StampLine(s_recordText, sizeof(s_recordText), head);
    RfabDiag::AppendToStartupLogs(s_recordText, false);
    position = AppendOwnStack(s_recordText, sizeof(s_recordText), position, 2);
    if (position + 2 < sizeof(s_recordText))
    {
        s_recordText[position++] = '\r';
        s_recordText[position++] = '\n';
        position += RfabDiag::FormatCrumbs(s_recordText + position, sizeof(s_recordText) - position);
    }
    RfabDiag::AppendToCrashLogs(s_recordText);

    s_recordBusy.store(false);
}

[[noreturn]] void OnTerminate() noexcept
{
    if (!s_endRecorded.exchange(true))
    {
        char what[512] = "no exception is active";
        if (const std::exception_ptr current = std::current_exception())
        {
            try
            {
                std::rethrow_exception(current);
            }
            catch (const std::exception& acError)
            {
                std::snprintf(what, sizeof(what), "%s", acError.what());
            }
            catch (...)
            {
                std::snprintf(what, sizeof(what), "an exception that is not derived from std::exception");
            }
        }

        char text[1024];
        std::snprintf(text, sizeof(text),
                      "the game was stopped by std::terminate: an exception left a thread or a noexcept function "
                      "(the stack below names it); what()=[\"%s\"]",
                      what);
        WriteEndRecord("TERMINATE", text);
    }

    if (s_previousTerminate)
        s_previousTerminate();
    std::abort();
}

void OnPureCall() noexcept
{
    if (!s_endRecorded.exchange(true))
        WriteEndRecord("PURECALL", "a pure virtual function was called: an object was used while it was being built or after it was destroyed");

    if (s_previousPurecall)
        s_previousPurecall();
    RaiseFailFastException(nullptr, nullptr, 0);
}

void OnInvalidParameter(const wchar_t* apExpression, const wchar_t* apFunction, const wchar_t* apFile, unsigned int aLine, uintptr_t aReserved) noexcept
{
    if (!s_endRecorded.exchange(true))
    {
        // Release runtimes pass null for all three; the stack is what names the caller.
        char function[192] = "?";
        if (apFunction && *apFunction)
            WideCharToMultiByte(CP_UTF8, 0, apFunction, -1, function, sizeof(function) - 1, nullptr, nullptr);
        char text[512];
        std::snprintf(text, sizeof(text), "the C runtime rejected an argument and ends the process (function %s)", function);
        WriteEndRecord("INVALID_PARAMETER", text);
    }

    if (s_previousInvalidParameter)
        s_previousInvalidParameter(apExpression, apFunction, apFile, aLine, aReserved);
    RaiseFailFastException(nullptr, nullptr, 0);
}

void OnAbortSignal(int) noexcept
{
    if (!s_endRecorded.exchange(true))
        WriteEndRecord("ABORT", "abort() was called");
    // Returning lets abort() finish the process the usual way.
    std::signal(SIGABRT, SIG_DFL);
}

// ---- hang watch ----

std::atomic<uint64_t> s_beatCount{0};
std::atomic<uint64_t> s_lastBeatMs{0};
std::atomic<uint64_t> s_startedAtMs{0};
std::atomic<bool> s_worldLoad{false};
std::atomic<bool> s_exiting{false};
std::atomic<bool> s_watchStarted{false};
std::atomic<DWORD> s_mainThreadId{0};
std::atomic<HANDLE> s_mainThread{nullptr};

void AdoptCurrentThreadAsMain() noexcept
{
    const DWORD threadId = GetCurrentThreadId();
    if (s_mainThreadId.load(std::memory_order_acquire) == threadId)
        return;

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &duplicate,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        duplicate = nullptr;

    const HANDLE previous = s_mainThread.exchange(duplicate, std::memory_order_acq_rel);
    s_mainThreadId.store(threadId, std::memory_order_release);
    if (previous)
        CloseHandle(previous);

    // Room for the exception filter after a stack overflow on this thread.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

struct WindowSearch
{
    DWORD ProcessId{0};
    DWORD ThreadId{0};
    HWND Found{nullptr};
};

BOOL CALLBACK FindGameWindow(HWND aWindow, LPARAM aParam) noexcept
{
    auto* pSearch = reinterpret_cast<WindowSearch*>(aParam);
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(aWindow, &processId);
    if (processId != pSearch->ProcessId || threadId != pSearch->ThreadId)
        return TRUE;
    if (!IsWindowVisible(aWindow) || GetWindow(aWindow, GW_OWNER) != nullptr)
        return TRUE;
    pSearch->Found = aWindow;
    return FALSE;
}

enum class WindowState
{
    kNone,
    kAnswers,
    kHung,
};

WindowState QueryGameWindow() noexcept
{
    WindowSearch search{};
    search.ProcessId = GetCurrentProcessId();
    search.ThreadId = s_mainThreadId.load(std::memory_order_acquire);
    if (search.ThreadId == 0)
        return WindowState::kNone;
    EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.Found)
        return WindowState::kNone;
    return IsHungAppWindow(search.Found) ? WindowState::kHung : WindowState::kAnswers;
}

const char* ToString(WindowState aState) noexcept
{
    switch (aState)
    {
    case WindowState::kNone: return "no game window";
    case WindowState::kAnswers: return "window answers";
    case WindowState::kHung: return "window does not answer";
    }
    return "?";
}

// What the main thread was doing, read while it is suspended. Owned by the watching thread.
struct ThreadCapture
{
    CONTEXT Context;
    uintptr_t Words[4096];  // 32 KB of stack above the stack pointer
    size_t WordCount;
    bool Valid;
};
ThreadCapture s_capture{};

// Between SuspendThread and ResumeThread nothing may take a lock the suspended thread could hold
// (the heap, the loader): only system calls. The stack is copied and read after the thread runs again.
bool CaptureMainThread() noexcept
{
    s_capture.Valid = false;
    s_capture.WordCount = 0;

    const HANDLE thread = s_mainThread.load(std::memory_order_acquire);
    if (!thread || s_mainThreadId.load(std::memory_order_acquire) == GetCurrentThreadId())
        return false;

    if (SuspendThread(thread) == static_cast<DWORD>(-1))
        return false;

    s_capture.Context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(thread, &s_capture.Context))
    {
        s_capture.Valid = true;
        const uintptr_t start = static_cast<uintptr_t>(s_capture.Context.Rsp) & ~static_cast<uintptr_t>(7);
        constexpr size_t kChunkWords = 512;  // one page: the copy stops cleanly at the top of the stack
        while (start >= 0x10000 && s_capture.WordCount + kChunkWords <= sizeof(s_capture.Words) / sizeof(s_capture.Words[0]))
        {
            SIZE_T read = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(start + s_capture.WordCount * sizeof(uintptr_t)),
                                   s_capture.Words + s_capture.WordCount, kChunkWords * sizeof(uintptr_t), &read) ||
                read != kChunkWords * sizeof(uintptr_t))
            {
                s_capture.WordCount += read / sizeof(uintptr_t);
                break;
            }
            s_capture.WordCount += kChunkWords;
        }
    }

    ResumeThread(thread);
    return s_capture.Valid;
}

// Is aAddress the instruction after a call? Filters the stack copy down to return addresses.
bool LooksLikeReturnAddress(uintptr_t aAddress) noexcept
{
    if (aAddress < 0x10000 + 8)
        return false;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(aAddress), &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & kExecutable) == 0 || (info.Protect & PAGE_GUARD) != 0)
        return false;

    uint8_t before[8]{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(aAddress - 7), before, 7, &read) || read != 7)
        return false;

    // before[6] is the byte just in front of aAddress.
    if (before[2] == 0xE8)  // call rel32
        return true;
    if (before[1] == 0xFF && (before[2] == 0x15 || (before[2] & 0xF8) == 0x90))  // call [rip+d32] / call [reg+d32]
        return true;
    if (before[0] == 0xFF && before[1] == 0x94)  // call [sib+d32]
        return true;
    if (before[5] == 0xFF && ((before[6] & 0xF8) == 0xD0 || (before[6] & 0xF8) == 0x10))  // call reg / call [reg]
        return true;
    if (before[4] == 0xFF && ((before[5] & 0xF8) == 0x50 || before[5] == 0x14))  // call [reg+d8] / call [sib]
        return true;
    if (before[3] == 0xFF && before[4] == 0x54)  // call [sib+d8]
        return true;
    return false;
}

size_t AppendCapturedStack(char* apOut, size_t aOutSize, size_t aPosition) noexcept
{
    if (!s_capture.Valid)
        return AppendText(apOut, aOutSize, aPosition, "\r\n    (the main thread's stack could not be read)");

    char frame[192]{};
    RfabDiag::FormatAddress(static_cast<uintptr_t>(s_capture.Context.Rip), frame, sizeof(frame));
    aPosition = AppendText(apOut, aOutSize, aPosition, "\r\n    [unwind #0] %s", frame);

    // Not an unwind: every value on the stack that is the address after a call instruction. A few
    // are leftovers of earlier calls; the order (innermost first) is right.
    unsigned found = 0;
    for (size_t i = 0; i < s_capture.WordCount && found < 40 && aPosition + 200 < aOutSize; ++i)
    {
        const uintptr_t candidate = s_capture.Words[i];
        if (!LooksLikeReturnAddress(candidate))
            continue;
        RfabDiag::FormatAddress(candidate, frame, sizeof(frame));
        aPosition = AppendText(apOut, aOutSize, aPosition, "\r\n    [stack #%u] %s", ++found, frame);
    }
    return aPosition;
}

// ---- the dump of a hang ----
//
// The text record names where the main thread stood. Why it stood there is usually on ANOTHER
// thread (the one holding the lock), and only a dump has every thread's stack. dbghelp.dll is loaded
// when the watch starts: loading a library from the watch thread, while the stuck thread may hold
// the loader lock, would hang the watch too.
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
MiniDumpWriteDumpFn s_pWriteDump = nullptr;
std::atomic<uint32_t> s_hangDumpsWritten{0};
constexpr uint32_t kMaxHangDumpsPerRun = 2;
constexpr uint32_t kMaxHangDumpsKept = 3;

// Removes the oldest hang_*.dmp files so that at most aKeep remain. The names sort by time.
void PruneHangDumps(uint32_t aKeep) noexcept
{
    wchar_t pattern[kPathCapacity];
    if (!Join(pattern, s_logDir, L"hang_*.dmp"))
        return;

    for (uint32_t pass = 0; pass < 16; ++pass)
    {
        WIN32_FIND_DATAW found{};
        const HANDLE search = FindFirstFileW(pattern, &found);
        if (search == INVALID_HANDLE_VALUE)
            return;

        uint32_t count = 0;
        wchar_t oldest[MAX_PATH]{};
        do
        {
            ++count;
            if (oldest[0] == L'\0' || _wcsicmp(found.cFileName, oldest) < 0)
                wcsncpy_s(oldest, found.cFileName, _TRUNCATE);
        } while (FindNextFileW(search, &found));
        FindClose(search);

        if (count <= aKeep || oldest[0] == L'\0')
            return;
        wchar_t path[kPathCapacity];
        if (!Join(path, s_logDir, oldest) || !DeleteFileW(path))
            return;
    }
}

// Writes hang_<date>_<time>.dmp into the log folder. Returns the file name, or nullptr.
const wchar_t* WriteHangDump() noexcept
{
    static wchar_t s_name[64];

    if (!s_pWriteDump || s_logDir[0] == L'\0')
        return nullptr;
    if (s_hangDumpsWritten.load() >= kMaxHangDumpsPerRun)
        return nullptr;

    PruneHangDumps(kMaxHangDumpsKept - 1);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    swprintf_s(s_name, L"hang_%04u-%02u-%02u_%02u-%02u-%02u.dmp", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    wchar_t path[kPathCapacity];
    if (!Join(path, s_logDir, s_name))
        return nullptr;

    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;

    // MiniDumpNormal | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo: every thread's stack and
    // registers, no heap. A few megabytes for the game.
    constexpr int kDumpType = 0x00000000 | 0x00000020 | 0x00001000;
    const BOOL written = s_pWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (!written)
    {
        DeleteFileW(path);
        return nullptr;
    }
    s_hangDumpsWritten.fetch_add(1);
    return s_name;
}

void WriteHangRecord(const Diag::HangVerdict& acVerdict, Diag::HangPhase aPhase, WindowState aWindow) noexcept
{
    const bool captured = CaptureMainThread();
    (void)captured;

    if (s_recordBusy.exchange(true))
        return;

    char head[768];
    if (acVerdict.Kind == Diag::HangVerdictKind::kNeverStarted)
    {
        std::snprintf(head, sizeof(head),
                      "[HANG] the game was started %llu s ago and its main loop has not turned once (%s; main thread TID=%lu; record %u). "
                      "A black screen at start that is closed by hand ends here, without a crash record.",
                      static_cast<unsigned long long>(acVerdict.StalledForMs / 1000), ToString(aWindow),
                      static_cast<unsigned long>(s_mainThreadId.load()), static_cast<unsigned>(acVerdict.ReportIndex));
    }
    else
    {
        std::snprintf(head, sizeof(head),
                      "[HANG] the game's main loop has not turned for %llu s (%s; %s; main thread TID=%lu; %llu turns before; record %u). "
                      "If the game was closed after this line, this is the reason and there is no crash record.",
                      static_cast<unsigned long long>(acVerdict.StalledForMs / 1000), Diag::ToString(aPhase), ToString(aWindow),
                      static_cast<unsigned long>(s_mainThreadId.load()), static_cast<unsigned long long>(s_beatCount.load()),
                      static_cast<unsigned>(acVerdict.ReportIndex));
    }

    size_t position = RfabDiag::StampLine(s_recordText, sizeof(s_recordText), head);
    RfabDiag::AppendToStartupLogs(s_recordText, false);
    position = AppendCapturedStack(s_recordText, sizeof(s_recordText), position);
    if (position + 2 < sizeof(s_recordText))
    {
        s_recordText[position++] = '\r';
        s_recordText[position++] = '\n';
        position += RfabDiag::FormatCrumbs(s_recordText + position, sizeof(s_recordText) - position);
    }
    RfabDiag::AppendToCrashLogs(s_recordText);

    s_recordBusy.store(false);

    // After the text is on disk: should the dump itself get stuck, the record is already there.
    // One dump per stall (its first record), two per run.
    if (acVerdict.ReportIndex == 1)
    {
        if (const wchar_t* pName = WriteHangDump())
        {
            char note[256];
            std::snprintf(note, sizeof(note), "[HANG] every thread's call stack was saved to RFAB_Online_Logs\\%ls (the report collector takes it)", pName);
            char line[320];
            RfabDiag::StampLine(line, sizeof(line), note);
            RfabDiag::AppendToStartupLogs(line, false);
        }
    }
}

void WriteHangEnd(const Diag::HangVerdict& acVerdict) noexcept
{
    char head[256];
    std::snprintf(head, sizeof(head), "[HANG-END] the main loop turns again; it stood for %llu s",
                  static_cast<unsigned long long>(acVerdict.StalledForMs / 1000));
    char line[320];
    RfabDiag::StampLine(line, sizeof(line), head);
    RfabDiag::AppendToStartupLogs(line, false);
    RfabDiag::AppendToCrashLogs(line);
}

DWORD WINAPI HangWatchThread(LPVOID) noexcept
{
    Diag::HangWatchState state{};
    const Diag::HangWatchPolicy policy{};

    for (;;)
    {
        Sleep(1000);

        const uint64_t now = GetTickCount64();
        const uint64_t beats = s_beatCount.load(std::memory_order_acquire);
        const uint64_t lastBeat = s_lastBeatMs.load(std::memory_order_acquire);
        const uint64_t startedAt = s_startedAtMs.load(std::memory_order_acquire);

        Diag::HangPhase phase = Diag::HangPhase::kRunning;
        if (s_exiting.load(std::memory_order_acquire))
            phase = Diag::HangPhase::kExiting;
        else if (beats == 0)
            phase = Diag::HangPhase::kStarting;
        else if (s_worldLoad.load(std::memory_order_acquire))
            phase = Diag::HangPhase::kWorldLoad;

        // Asking Windows about the window costs a walk over all top-level windows: only when the
        // loop has been silent for a while.
        WindowState window = WindowState::kNone;
        const uint64_t silentSince = beats == 0 ? startedAt : lastBeat;
        if (now >= silentSince && now - silentSince >= 5000)
            window = QueryGameWindow();

        const Diag::HangVerdict verdict = state.Evaluate(now, startedAt, lastBeat, beats, phase, window == WindowState::kAnswers, policy);
        switch (verdict.Kind)
        {
        case Diag::HangVerdictKind::kNothing: break;
        case Diag::HangVerdictKind::kNeverStarted:
        case Diag::HangVerdictKind::kStalled: WriteHangRecord(verdict, phase, window); break;
        case Diag::HangVerdictKind::kResumed: WriteHangEnd(verdict); break;
        }
    }
}

void OnProcessExit() noexcept
{
    s_exiting.store(true, std::memory_order_release);
    char line[256];
    RfabDiag::StampLine(line, sizeof(line), "[EXIT] the process is ending the normal way");
    RfabDiag::AppendToStartupLogs(line, false);
}
} // namespace

namespace RfabDiag
{
void InitPaths(const std::filesystem::path& acGamePath) noexcept
{
    // The client folder: where the launcher lives. Asked once, before the launcher renames the
    // process's main module to the game's executable.
    if (s_clientDir[0] == L'\0')
    {
        const wchar_t* pClientDir = GetTiltedClientDirW();
        if (!pClientDir || !*pClientDir || !CopyPath(s_clientDir, pClientDir))
        {
            wchar_t module[kPathCapacity]{};
            const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(kPathCapacity - 1));
            if (length > 0 && length < kPathCapacity - 1)
            {
                if (wchar_t* pSlash = std::wcsrchr(module, L'\\'))
                    *pSlash = L'\0';
                CopyPath(s_clientDir, module);
            }
        }
        Join(s_startupClient, s_clientDir, L"RFAB_Online_Startup.log");
        Join(s_crashClient, s_clientDir, L"RFAB_Online_Crash.log");
    }

    if (s_gameDir[0] == L'\0' && !acGamePath.empty())
    {
        // The only place of this file that may allocate (path::c_str does not, but stay honest:
        // this runs at start-up, never inside a handler).
        wchar_t game[kPathCapacity]{};
        if (CopyPath(game, acGamePath.c_str()) && !SamePath(game, s_clientDir))
        {
            Join(s_startupGame, game, L"RFAB_Online_Startup.log");
            Join(s_crashGame, game, L"RFAB_Online_Crash.log");
            CopyPath(s_gameDir, game);
        }
    }

    // The log folder. The game folder is preferred: it is never virtual (see RfabDiag.h).
    wchar_t candidate[kPathCapacity]{};
    if (s_gameDir[0] != L'\0' && Join(candidate, s_gameDir, L"RFAB_Online_Logs") && EnsureDirectory(candidate) && CanWriteInto(candidate))
    {
        CopyPath(s_logDir, candidate);
        return;
    }
    if (s_clientDir[0] != L'\0' && Join(candidate, s_clientDir, L"logs") && EnsureDirectory(candidate))
        CopyPath(s_logDir, candidate);
}

std::filesystem::path GetLogDirectory() noexcept
{
    if (s_logDir[0] == L'\0')
        InitPaths({});
    try
    {
        return std::filesystem::path(s_logDir);
    }
    catch (...)
    {
        return {};
    }
}

const wchar_t* GetLogDirectoryW() noexcept
{
    return s_logDir;
}

void AppendLine(const wchar_t* apPath, const char* apText, bool aReset) noexcept
{
    if (!apPath || !*apPath || !apText)
        return;

    const HANDLE file = CreateFileW(apPath, aReset ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    aReset ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, apText, static_cast<DWORD>(std::strlen(apText)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void AppendToStartupLogs(const char* apText, bool aReset) noexcept
{
    if (s_startupClient[0] == L'\0')
        InitPaths({});
    AppendLine(s_startupClient, apText, aReset);
    AppendLine(s_startupGame, apText, false);
}

void AppendToCrashLogs(const char* apText) noexcept
{
    if (s_crashClient[0] == L'\0')
        InitPaths({});
    AppendLine(s_crashClient, apText, false);
    AppendLine(s_crashGame, apText, false);
}

size_t StampLine(char* apOut, size_t aOutSize, const char* apText) noexcept
{
    if (!apOut || aOutSize == 0)
        return 0;
    SYSTEMTIME local{};
    GetLocalTime(&local);
    const int written = std::snprintf(apOut, aOutSize, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s", static_cast<unsigned>(local.wYear),
                                      static_cast<unsigned>(local.wMonth), static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
                                      static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond),
                                      static_cast<unsigned>(local.wMilliseconds), apText ? apText : "");
    if (written <= 0)
    {
        apOut[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written) < aOutSize ? static_cast<size_t>(written) : aOutSize - 1;
}

void FormatAddress(uintptr_t aAddress, char* apOut, size_t aOutSize) noexcept
{
    if (!apOut || aOutSize == 0)
        return;
    apOut[0] = '\0';

    HMODULE module = nullptr;
    char modulePath[MAX_PATH]{};
    if (aAddress >= 0x10000 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(aAddress),
                           &module) &&
        module != nullptr)
    {
        GetModuleFileNameA(module, modulePath, MAX_PATH - 1);
        const char* pSlash = std::strrchr(modulePath, '\\');
        const char* pShortName = pSlash ? (pSlash + 1) : modulePath;
        const uintptr_t rva = aAddress - reinterpret_cast<uintptr_t>(module);
        // SkyrimTogether.exe reserves ~1.05 GB above 0x140000000 for RipAllocator hook trampolines;
        // RVAs >= 64 MB are TiltedHooks trampolines rather than SkyrimSE.exe's ~55 MB image.
        if (module == GetModuleHandleA(nullptr) && rva >= 0x04000000ULL)
        {
            std::snprintf(apOut, aOutSize, "%s[TiltedTrampoline]+0x%llX (0x%llX)", pShortName, static_cast<unsigned long long>(rva),
                          static_cast<unsigned long long>(aAddress));
            return;
        }
        std::snprintf(apOut, aOutSize, "%s+0x%llX (0x%llX)", pShortName, static_cast<unsigned long long>(rva),
                      static_cast<unsigned long long>(aAddress));
        return;
    }

    std::snprintf(apOut, aOutSize, "0x%llX", static_cast<unsigned long long>(aAddress));
}

size_t FormatCrumbs(char* apOut, size_t aOutSize) noexcept
{
    if (!apOut || aOutSize < 256)
        return 0;

    const uint64_t now = GetTickCount64();
    const int head = std::snprintf(apOut, aOutSize, "    [crumbs] events=%llu packets=%llu hooks=%llu dropped=%llu (newest last; a/b: see the RFAB_*_CRUMB call sites)",
                                   static_cast<unsigned long long>(Diag::g_eventCrumbs.Total()),
                                   static_cast<unsigned long long>(Diag::g_packetCrumbs.Total()),
                                   static_cast<unsigned long long>(Diag::g_hookCrumbs.Total()),
                                   static_cast<unsigned long long>(Diag::g_eventCrumbs.Dropped() + Diag::g_packetCrumbs.Dropped() +
                                                                   Diag::g_hookCrumbs.Dropped()));
    if (head <= 0 || static_cast<size_t>(head) >= aOutSize)
        return 0;

    size_t length = static_cast<size_t>(head);
    // Each ring gets its own share of the buffer, so a full one cannot crowd out the next.
    const auto share = [&length, aOutSize](size_t aWanted) noexcept {
        const size_t left = aOutSize - length;
        return aWanted < left ? aWanted : left;
    };
    length += Diag::g_eventCrumbs.Format(apOut + length, share(3584), "event", now, 40);
    length += Diag::g_packetCrumbs.Format(apOut + length, share(2560), "packet", now, 28);
    length += Diag::g_hookCrumbs.Format(apOut + length, share(aOutSize), "hook", now, 28);
    return length;
}

void InstallProcessEndHandlers() noexcept
{
    static std::atomic<bool> s_installed{false};
    if (s_installed.exchange(true))
        return;

    s_previousTerminate = std::set_terminate(&OnTerminate);
    s_previousPurecall = _set_purecall_handler(&OnPureCall);
    s_previousInvalidParameter = _set_invalid_parameter_handler(&OnInvalidParameter);
    std::signal(SIGABRT, &OnAbortSignal);
    std::atexit(&OnProcessExit);

    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

void StartHangWatch() noexcept
{
    if (s_watchStarted.exchange(true))
        return;

    s_startedAtMs.store(GetTickCount64(), std::memory_order_release);
    // The launcher's main thread becomes the game's: it calls the game's entry point directly.
    AdoptCurrentThreadAsMain();

    // For the dump of a hang; loaded now, while nothing is stuck (see WriteHangDump).
    if (const HMODULE dbgHelp = LoadLibraryW(L"dbghelp.dll"))
        s_pWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbgHelp, "MiniDumpWriteDump")));

    const HANDLE thread = CreateThread(nullptr, 256 * 1024, &HangWatchThread, nullptr, 0, nullptr);
    if (thread)
    {
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(thread);
    }
    else
    {
        char line[160];
        StampLine(line, sizeof(line), "[BOOT] WARNING: the hang watch thread could not be started; a hang will leave no record");
        AppendToStartupLogs(line, false);
    }
}

void Beat() noexcept
{
    const uint64_t now = GetTickCount64();
    const uint64_t count = s_beatCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_lastBeatMs.store(now, std::memory_order_release);

    if (count == 1)
    {
        AdoptCurrentThreadAsMain();
        const uint64_t startedAt = s_startedAtMs.load(std::memory_order_acquire);
        char text[160];
        std::snprintf(text, sizeof(text), "[BOOT] the game's main loop is turning (first turn %llu s after start, TID=%lu)",
                      static_cast<unsigned long long>(startedAt && now >= startedAt ? (now - startedAt) / 1000 : 0),
                      static_cast<unsigned long>(GetCurrentThreadId()));
        char line[224];
        StampLine(line, sizeof(line), text);
        AppendToStartupLogs(line, false);
    }
}

void SetWorldLoadInProgress(bool aInProgress) noexcept
{
    s_worldLoad.store(aInProgress, std::memory_order_release);
    // The loop did not turn while the engine loaded; the time until the next turn counts from now.
    if (!aInProgress && s_beatCount.load(std::memory_order_acquire) != 0)
        s_lastBeatMs.store(GetTickCount64(), std::memory_order_release);
}

void NoteExitRequested(const char* apHow) noexcept
{
    if (s_exiting.exchange(true))
        return;

    const uint64_t now = GetTickCount64();
    const uint64_t startedAt = s_startedAtMs.load(std::memory_order_acquire);
    char text[256];
    std::snprintf(text, sizeof(text), "[EXIT] %s (ran for %llu s, %llu main-loop turns)", apHow ? apHow : "exit requested",
                  static_cast<unsigned long long>(startedAt && now >= startedAt ? (now - startedAt) / 1000 : 0),
                  static_cast<unsigned long long>(s_beatCount.load(std::memory_order_acquire)));
    char line[320];
    StampLine(line, sizeof(line), text);
    AppendToStartupLogs(line, false);
}
} // namespace RfabDiag
