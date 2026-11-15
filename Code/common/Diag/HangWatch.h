#pragma once

/**
 * Hang watch: when is a main loop that has stopped turning worth a record?
 *
 * Playtest 2026-11-15: "black screen" and "the game closed" several times, and not one record in
 * any log. A game that stops responding and is then closed by the player (or by Windows) raises no
 * exception, so the crash handler never runs. The only witness is a second thread that notices the
 * main loop has stopped and writes down where it stands.
 *
 * This header is the decision only (tested on Linux, Code/tests/rfab_v412_hang_watch.cpp):
 * given the time, the time of the last main-loop turn and what the game is doing, say whether to
 * write a record now. The Windows part - the thread, reading the stuck thread's stack, the file -
 * is Code/client/RfabDiag.cpp.
 *
 * Rules:
 *  - Before the first turn of the main loop the game is starting (plugins, SKSE, the main menu).
 *    That takes long with a large load order, so the deadline is generous; when it passes, a
 *    start that never reached the main loop is reported ("black screen at launch").
 *  - While a save is being loaded the main thread is inside the engine's loader and does not turn
 *    the loop at all. That is normal for minutes; only a much longer silence is reported.
 *  - Otherwise a loop that has not turned for `StallMs` is reported once, then again every
 *    `RepeatMs` while it lasts, at most `MaxReports` times (a stuck game must not fill the disk).
 *  - A game window that still answers Windows is not a hang, whatever the loop does: the main
 *    thread reads its messages, so the game is waiting (window in the background, a dialog), not
 *    stuck. A hang is a silent loop AND a window that does not answer (or no window at all).
 *  - When the loop turns again after a report, that is reported too, with how long it stood:
 *    it tells a long stall (the game came back) from a hang (it never did).
 *  - Nothing is reported once the game was told to quit: a slow exit is not a hang.
 */

#include <cstdint>

namespace Diag
{

enum class HangPhase : uint32_t
{
    kStarting = 0,   // the main loop has not turned yet
    kRunning = 1,    // the main loop turns (menus, gameplay)
    kWorldLoad = 2,  // the engine is loading a save: the loop legitimately stands still
    kExiting = 3,    // the game was told to quit
};

struct HangWatchPolicy
{
    uint64_t StartDeadlineMs{300'000};  // launch to first main-loop turn
    uint64_t StallMs{20'000};           // main loop silent during play or in a menu
    uint64_t WorldLoadStallMs{240'000}; // main loop silent while a save is loading
    uint64_t RepeatMs{60'000};          // between two records of the same stall
    uint32_t MaxReports{5};             // records per stall
};

enum class HangVerdictKind : uint32_t
{
    kNothing = 0,
    kNeverStarted,  // StartDeadlineMs passed without a single main-loop turn
    kStalled,       // the main loop has not turned for StalledForMs
    kResumed,       // the main loop turns again; it stood for StalledForMs
};

struct HangVerdict
{
    HangVerdictKind Kind{HangVerdictKind::kNothing};
    uint64_t StalledForMs{0};
    uint32_t ReportIndex{0};  // 1 for the first record of a stall
};

// Owned by the watching thread. Times are milliseconds of one monotonic clock.
class HangWatchState
{
public:
    /**
     * @param aNowMs        the time now
     * @param aStartedAtMs  when the watch started (the game was launched)
     * @param aLastBeatMs   the time of the last main-loop turn, 0 while there was none
     * @param aBeatCount    how many turns there were so far (tells two turns in the same millisecond apart)
     * @param aPhase        what the game is doing, as the main thread last reported it
     * @param aWindowAnswers the game has a window and Windows does not consider it hung
     */
    [[nodiscard]] HangVerdict Evaluate(uint64_t aNowMs, uint64_t aStartedAtMs, uint64_t aLastBeatMs, uint64_t aBeatCount,
                                       HangPhase aPhase, bool aWindowAnswers,
                                       const HangWatchPolicy& acPolicy = HangWatchPolicy{}) noexcept
    {
        HangVerdict verdict{};

        // The loop turned since the stall was reported: the stall is over.
        if (m_inStall && aBeatCount != m_stallBeatCount)
        {
            verdict.Kind = HangVerdictKind::kResumed;
            verdict.StalledForMs = aLastBeatMs >= m_stallSinceMs ? aLastBeatMs - m_stallSinceMs : 0;
            verdict.ReportIndex = m_reports;
            m_inStall = false;
            m_reports = 0;
            return verdict;
        }

        if (aPhase == HangPhase::kExiting)
            return verdict;

        const bool neverTurned = aBeatCount == 0;
        const uint64_t since = neverTurned ? aStartedAtMs : aLastBeatMs;
        const uint64_t silentFor = aNowMs >= since ? aNowMs - since : 0;

        uint64_t limit = acPolicy.StallMs;
        if (neverTurned)
            limit = acPolicy.StartDeadlineMs;
        else if (aPhase == HangPhase::kWorldLoad)
            limit = acPolicy.WorldLoadStallMs;

        if (silentFor < limit)
            return verdict;

        if (!m_inStall)
        {
            // Waiting, not stuck. A stall that was already reported stays open until the loop turns.
            if (aWindowAnswers && !neverTurned)
                return verdict;

            m_inStall = true;
            m_stallBeatCount = aBeatCount;
            m_stallSinceMs = since;
            m_reports = 0;
        }
        else
        {
            if (m_reports >= acPolicy.MaxReports)
                return verdict;
            if (aNowMs < m_lastReportMs || aNowMs - m_lastReportMs < acPolicy.RepeatMs)
                return verdict;
        }

        ++m_reports;
        m_lastReportMs = aNowMs;
        verdict.Kind = neverTurned ? HangVerdictKind::kNeverStarted : HangVerdictKind::kStalled;
        verdict.StalledForMs = silentFor;
        verdict.ReportIndex = m_reports;
        return verdict;
    }

    [[nodiscard]] bool IsInStall() const noexcept { return m_inStall; }

private:
    bool m_inStall{false};
    uint64_t m_stallBeatCount{0};
    uint64_t m_stallSinceMs{0};
    uint64_t m_lastReportMs{0};
    uint32_t m_reports{0};
};

/**
 * The other "black screen": the main loop turns, but the player sees a loading screen or a faded
 * (black) screen that never ends. Nothing is stuck as far as the processor is concerned, so the hang
 * watch above stays silent. This one is fed from the main loop itself, once per turn.
 *
 * Reports when the same blocking screen has been up for `FirstMs`, then every `RepeatMs`, and once
 * more when it ends (with how long it lasted), so a log shows both "it never ended" and "it took
 * four minutes".
 */
enum class BlockingScreen : uint32_t
{
    kNone = 0,
    kLoading = 1,  // the loading menu is open
    kFaded = 2,    // the fader menu is open (the screen is black or fading)
};

struct ScreenWatchPolicy
{
    uint64_t FirstMs{60'000};
    uint64_t RepeatMs{60'000};
    uint32_t MaxReports{10};
};

struct ScreenVerdict
{
    bool Report{false};
    bool Ended{false};          // the screen is gone; ForMs is how long it lasted
    BlockingScreen Screen{BlockingScreen::kNone};
    uint64_t ForMs{0};
};

class ScreenWatchState
{
public:
    [[nodiscard]] ScreenVerdict Evaluate(uint64_t aNowMs, BlockingScreen aScreen, const ScreenWatchPolicy& acPolicy = ScreenWatchPolicy{}) noexcept
    {
        ScreenVerdict verdict{};

        if (aScreen != m_screen)
        {
            if (m_screen != BlockingScreen::kNone && m_reports > 0)
            {
                verdict.Report = true;
                verdict.Ended = true;
                verdict.Screen = m_screen;
                verdict.ForMs = aNowMs >= m_sinceMs ? aNowMs - m_sinceMs : 0;
            }
            m_screen = aScreen;
            m_sinceMs = aNowMs;
            m_reports = 0;
            return verdict;
        }

        if (aScreen == BlockingScreen::kNone || m_reports >= acPolicy.MaxReports)
            return verdict;

        const uint64_t upFor = aNowMs >= m_sinceMs ? aNowMs - m_sinceMs : 0;
        const uint64_t due = acPolicy.FirstMs + static_cast<uint64_t>(m_reports) * acPolicy.RepeatMs;
        if (upFor < due)
            return verdict;

        ++m_reports;
        verdict.Report = true;
        verdict.Screen = aScreen;
        verdict.ForMs = upFor;
        return verdict;
    }

private:
    BlockingScreen m_screen{BlockingScreen::kNone};
    uint64_t m_sinceMs{0};
    uint32_t m_reports{0};
};

[[nodiscard]] constexpr const char* ToString(BlockingScreen aScreen) noexcept
{
    switch (aScreen)
    {
    case BlockingScreen::kNone: return "none";
    case BlockingScreen::kLoading: return "loading screen";
    case BlockingScreen::kFaded: return "faded (black) screen";
    }
    return "?";
}

[[nodiscard]] constexpr const char* ToString(HangPhase aPhase) noexcept
{
    switch (aPhase)
    {
    case HangPhase::kStarting: return "starting";
    case HangPhase::kRunning: return "running";
    case HangPhase::kWorldLoad: return "loading a save";
    case HangPhase::kExiting: return "exiting";
    }
    return "?";
}

} // namespace Diag
