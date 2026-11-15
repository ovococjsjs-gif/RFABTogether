#pragma once

// What the client leaves behind for a playtest report, and where.
//
// Playtest 2026-11-15 (two players, several "crashes" and a black screen): both reports arrived
// without a client log and without a single record of the game ending. Three reasons, all here:
//
//  1. Where the files went. Started through Mod Organizer 2 the client finds itself under the
//     game's *virtual* Data folder, and a file created there lands in MO2's overwrite folder, not
//     in the mod folder the report collector reads. The game folder itself is not virtual: the
//     startup trace written there did arrive. So everything written for a report now goes to
//     <game>\RFAB_Online_Logs (the client folder is the fallback when that cannot be created).
//
//  2. Endings that raise no exception. A game that stops responding and is closed by the player,
//     std::terminate from a noexcept function, a pure virtual call, a CRT invalid-parameter abort:
//     none of them reaches an exception filter. Each now leaves its own line ([HANG], [TERMINATE],
//     [PURECALL], [INVALID_PARAMETER]) in RFAB_Online_Crash.log.
//
//  3. A crash writer that needed the heap. The writer built its file names with
//     std::filesystem::path while handling the fault; after heap corruption that is a second fault
//     and the record is lost. The names are now resolved once at start-up into static buffers.

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace RfabDiag
{
// Resolves every file name used later (also by the crash writer). The first call is made without a
// game folder (the launcher has not chosen one yet); it is called again when the folder is known.
void InitPaths(const std::filesystem::path& acGamePath) noexcept;

// <game>\RFAB_Online_Logs, or the client's own "logs" folder when that cannot be created.
// Created on demand. Never empty after InitPaths.
[[nodiscard]] std::filesystem::path GetLogDirectory() noexcept;
// The same folder as a zero-terminated string with static storage: usable inside a crash handler.
[[nodiscard]] const wchar_t* GetLogDirectoryW() noexcept;

// Appends one line (CR LF is added) and flushes. Allocation-free.
void AppendLine(const wchar_t* apPath, const char* apText, bool aReset) noexcept;
// RFAB_Online_Startup.log in the client folder and in the game folder. Allocation-free once
// InitPaths has run. aReset empties the client-folder copy (one start per file); the copy in the
// game folder is append-only, so the history of starts survives.
void AppendToStartupLogs(const char* apText, bool aReset) noexcept;
// RFAB_Online_Crash.log in the client folder and in the game folder. Append-only, allocation-free.
void AppendToCrashLogs(const char* apText) noexcept;

// Writes "[local date and time] " and apText into apOut. Returns the length written.
size_t StampLine(char* apOut, size_t aOutSize, const char* apText) noexcept;

// "Module.dll+0x1234 (0x7FF...)" for a code address; the layout Tools/rfab/symbolize_crash.py reads.
void FormatAddress(uintptr_t aAddress, char* apOut, size_t aOutSize) noexcept;
// The breadcrumb rings (common/Diag/Breadcrumbs.h) as continuation lines of a record. Lock-free and
// allocation-free. Returns the length written.
size_t FormatCrumbs(char* apOut, size_t aOutSize) noexcept;

// std::terminate, pure virtual call, CRT invalid parameter: each leaves a line in the crash logs
// before the process ends. Also reserves stack for the exception filter of the calling thread, so
// that a stack overflow on it can still be recorded. Idempotent.
void InstallProcessEndHandlers() noexcept;

// ---- hang watch (common/Diag/HangWatch.h decides, this writes) ----
// Starts the watching thread. Idempotent. From then on a main loop that never starts, or that
// stops turning, leaves a [HANG] record with the main thread's code addresses.
void StartHangWatch() noexcept;
// One turn of the game's main loop (HookMainLoop). The first call names the main thread.
void Beat() noexcept;
// The engine is inside its save loader: the main loop legitimately stands still.
void SetWorldLoadInProgress(bool aInProgress) noexcept;
// The game was told to quit: no hang is reported after this. Writes one [EXIT] line, once.
void NoteExitRequested(const char* apHow) noexcept;
} // namespace RfabDiag
