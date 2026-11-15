#pragma once

// When the server writes its "pulse" line and when it reports that it stood still (4.1.2).
//
// Playtest 2026-11-15: the server log of the host ended four lines after the start, although the
// session ran for half an hour after it. Nothing in the log said whether the server had been
// running, standing still or serving nobody. With a line every minute a silent log is itself an
// answer (the process was gone or blocked), and a server that stood still - a console window in
// "select" mode blocks every write to it, the machine went to sleep, the disk stalled - says so.
//
// Platform-free and without I/O: unit-tested in Code/tests/rfab_v412_hang_watch.cpp.

#include <cstdint>

namespace Diag
{
struct ServerPulsePolicy
{
    uint64_t BusyPeriodMs{60'000};  // with players connected
    uint64_t IdlePeriodMs{600'000}; // with nobody connected
    uint64_t StallMs{2'000};        // one turn of the server loop longer than this is reported at once
    uint64_t StallRepeatMs{10'000}; // but not more often than this
};

enum class ServerPulseKind : uint8_t
{
    kNone,
    kPulse, // the periodic line
    kStall, // one loop turn took StallMs or longer
};

struct ServerPulse
{
    ServerPulseKind Kind{ServerPulseKind::kNone};
    uint64_t LongestTurnMs{0}; // kPulse: since the last pulse; kStall: the turn that was just seen
    uint32_t Turns{0};         // kPulse: loop turns since the last pulse
    uint32_t Stalls{0};        // kPulse: turns of StallMs or longer since the last pulse
};

class ServerPulseState
{
public:
    // Once per turn of the server loop. aTurnMs is how long the previous turn took.
    [[nodiscard]] ServerPulse Evaluate(uint64_t aNowMs, uint64_t aTurnMs, bool aHasPlayers, const ServerPulsePolicy& acPolicy = {}) noexcept
    {
        ServerPulse out{};

        if (!m_started)
        {
            m_started = true;
            m_lastPulseMs = aNowMs;
            m_hadPlayers = aHasPlayers;
            return out;
        }

        ++m_turns;
        if (aTurnMs > m_longestTurnMs)
            m_longestTurnMs = aTurnMs;

        const bool stalled = aTurnMs >= acPolicy.StallMs;
        if (stalled)
            ++m_stalls;

        if (stalled && (!m_reportedStall || aNowMs - m_lastStallReportMs >= acPolicy.StallRepeatMs))
        {
            m_reportedStall = true;
            m_lastStallReportMs = aNowMs;
            out.Kind = ServerPulseKind::kStall;
            out.LongestTurnMs = aTurnMs;
            return out;
        }

        // The first player after an idle stretch: say so at once instead of up to ten minutes later.
        const bool playersArrived = aHasPlayers && !m_hadPlayers;
        m_hadPlayers = aHasPlayers;

        const uint64_t period = aHasPlayers ? acPolicy.BusyPeriodMs : acPolicy.IdlePeriodMs;
        if (!playersArrived && aNowMs - m_lastPulseMs < period)
            return out;

        out.Kind = ServerPulseKind::kPulse;
        out.LongestTurnMs = m_longestTurnMs;
        out.Turns = m_turns;
        out.Stalls = m_stalls;

        m_lastPulseMs = aNowMs;
        m_longestTurnMs = 0;
        m_turns = 0;
        m_stalls = 0;
        return out;
    }

private:
    bool m_started{false};
    bool m_hadPlayers{false};
    bool m_reportedStall{false};
    uint64_t m_lastPulseMs{0};
    uint64_t m_lastStallReportMs{0};
    uint64_t m_longestTurnMs{0};
    uint32_t m_turns{0};
    uint32_t m_stalls{0};
};
} // namespace Diag
