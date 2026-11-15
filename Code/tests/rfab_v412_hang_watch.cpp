// 4.1.2: the diagnostics added after the playtest of 2026-11-15.
//
// That session ended several times with "the game closed" and "a black screen", and the enemies did
// not fight. Neither report contained a client log, a crash record or anything else about it. These
// tests cover the decisions of the three watches that now leave a record:
//
//   Diag::HangWatchState    the main loop stopped turning (common/Diag/HangWatch.h)
//   Diag::ScreenWatchState  a loading or black screen stays up while the loop turns
//   Diag::NpcTally / NpcReportPacer   enemies stand idle next to a player (common/Diag/NpcWatch.h)
//   Diag::ServerPulseState  the server's line per minute and its "stood still" report (ServerPulse.h)
//
// The Windows parts (the watching thread, reading the stuck thread's stack, the engine queries) are
// Code/client/RfabDiag.cpp, SkyrimVM64.cpp and CombatService.cpp and are compiled by CI only.

#include <catch2/catch.hpp>

#include <common/Diag/HangWatch.h>
#include <common/Diag/NpcWatch.h>
#include <common/Diag/ServerPulse.h>

#include <vector>

using namespace Diag;

namespace
{
constexpr uint64_t kStart = 1'000'000;  // a tick count, not zero

// The watching thread: once a second, with what the main thread last published.
struct Watch
{
    HangWatchState State;
    HangWatchPolicy Policy;
    uint64_t Now{kStart};
    uint64_t LastBeat{0};
    uint64_t Beats{0};
    HangPhase Phase{HangPhase::kStarting};
    bool WindowAnswers{false};

    void Beat()
    {
        ++Beats;
        LastBeat = Now;
        if (Phase == HangPhase::kStarting)
            Phase = HangPhase::kRunning;
    }

    HangVerdict Tick(uint64_t aAdvanceMs = 1000)
    {
        Now += aAdvanceMs;
        return State.Evaluate(Now, kStart, LastBeat, Beats, Phase, WindowAnswers, Policy);
    }

    // Runs for aMs with the loop turning every second. Returns how many records were written.
    uint32_t RunTurning(uint64_t aMs)
    {
        uint32_t records = 0;
        for (uint64_t t = 0; t < aMs; t += 1000)
        {
            Now += 1000;
            Beat();
            if (State.Evaluate(Now, kStart, LastBeat, Beats, Phase, WindowAnswers, Policy).Kind != HangVerdictKind::kNothing)
                ++records;
        }
        return records;
    }

    // Runs for aMs with the loop standing still. Returns the verdicts that were not "nothing".
    std::vector<HangVerdict> RunSilent(uint64_t aMs)
    {
        std::vector<HangVerdict> out;
        for (uint64_t t = 0; t < aMs; t += 1000)
        {
            const HangVerdict verdict = Tick();
            if (verdict.Kind != HangVerdictKind::kNothing)
                out.push_back(verdict);
        }
        return out;
    }
};
} // namespace

TEST_CASE("4.1.2 hang watch: a game that runs leaves no record", "[rfab][v412][diag][hang]")
{
    Watch watch;
    REQUIRE(watch.RunSilent(60'000).empty());  // one minute of start-up: plugins, the main menu
    REQUIRE(watch.RunTurning(600'000) == 0);   // ten minutes of play
    REQUIRE_FALSE(watch.State.IsInStall());
}

TEST_CASE("4.1.2 hang watch: a start that never reaches the main loop is reported", "[rfab][v412][diag][hang]")
{
    Watch watch;

    SECTION("not before the deadline: a large load order starts slowly")
    {
        REQUIRE(watch.RunSilent(watch.Policy.StartDeadlineMs - 1000).empty());
    }

    SECTION("at the deadline, with or without a window")
    {
        watch.WindowAnswers = GENERATE(false, true);
        const auto records = watch.RunSilent(watch.Policy.StartDeadlineMs);
        REQUIRE(records.size() == 1);
        REQUIRE(records[0].Kind == HangVerdictKind::kNeverStarted);
        REQUIRE(records[0].StalledForMs == watch.Policy.StartDeadlineMs);
        REQUIRE(records[0].ReportIndex == 1);
    }

    SECTION("a start that was merely slow says so when the loop turns")
    {
        REQUIRE(watch.RunSilent(watch.Policy.StartDeadlineMs).size() == 1);
        watch.Now += 5000;
        watch.Beat();
        const HangVerdict resumed = watch.Tick();
        REQUIRE(resumed.Kind == HangVerdictKind::kResumed);
        REQUIRE(watch.RunTurning(30'000) == 0);
    }
}

TEST_CASE("4.1.2 hang watch: a main loop that stops is reported once, then at intervals, then never again", "[rfab][v412][diag][hang]")
{
    Watch watch;
    watch.RunTurning(120'000);

    const auto records = watch.RunSilent(3'600'000);  // the player went to bed with a hung game
    REQUIRE(records.size() == watch.Policy.MaxReports);
    for (size_t i = 0; i < records.size(); ++i)
    {
        REQUIRE(records[i].Kind == HangVerdictKind::kStalled);
        REQUIRE(records[i].ReportIndex == i + 1);
    }
    // The first record comes when the stall is StallMs old, not later: the player closes a hung
    // game within a minute, and a record that comes after that is never written.
    REQUIRE(records[0].StalledForMs == watch.Policy.StallMs);
    REQUIRE(records[1].StalledForMs == watch.Policy.StallMs + watch.Policy.RepeatMs);
}

TEST_CASE("4.1.2 hang watch: a stall that ends is told apart from a hang", "[rfab][v412][diag][hang]")
{
    Watch watch;
    watch.RunTurning(60'000);

    REQUIRE(watch.RunSilent(25'000).size() == 1);

    watch.Now += 6000;  // 31 s after the last turn
    watch.Beat();
    const HangVerdict resumed = watch.Tick();
    REQUIRE(resumed.Kind == HangVerdictKind::kResumed);
    REQUIRE(resumed.StalledForMs == 31'000);

    // And the next stall is a new one, counted from one.
    watch.RunTurning(10'000);
    const auto again = watch.RunSilent(watch.Policy.StallMs);
    REQUIRE(again.size() == 1);
    REQUIRE(again[0].ReportIndex == 1);
}

TEST_CASE("4.1.2 hang watch: short pauses are not stalls", "[rfab][v412][diag][hang]")
{
    Watch watch;
    watch.RunTurning(60'000);
    for (int i = 0; i < 20; ++i)
    {
        REQUIRE(watch.RunSilent(watch.Policy.StallMs - 1000).empty());  // a cell loads
        REQUIRE(watch.RunTurning(2000) == 0);
    }
}

TEST_CASE("4.1.2 hang watch: loading a save may keep the loop still for minutes", "[rfab][v412][diag][hang]")
{
    Watch watch;
    watch.RunTurning(60'000);

    watch.Phase = HangPhase::kWorldLoad;
    REQUIRE(watch.RunSilent(watch.Policy.WorldLoadStallMs - 1000).empty());

    SECTION("the load finishes: nothing was reported")
    {
        watch.Phase = HangPhase::kRunning;
        watch.LastBeat = watch.Now;  // RfabDiag::SetWorldLoadInProgress(false) restarts the count
        REQUIRE(watch.RunSilent(watch.Policy.StallMs - 1000).empty());
        REQUIRE(watch.RunTurning(10'000) == 0);
    }

    SECTION("an endless loading screen is reported")
    {
        const auto records = watch.RunSilent(2000);
        REQUIRE(records.size() == 1);
        REQUIRE(records[0].Kind == HangVerdictKind::kStalled);
    }
}

TEST_CASE("4.1.2 hang watch: a window that answers Windows is waiting, not hung", "[rfab][v412][diag][hang]")
{
    Watch watch;
    watch.RunTurning(60'000);

    // The game window is in the background and the engine stops turning its loop, but its thread
    // still reads window messages.
    watch.WindowAnswers = true;
    REQUIRE(watch.RunSilent(600'000).empty());

    // The same silence with a window that stopped answering is a hang, counted from the last turn.
    watch.WindowAnswers = false;
    const auto records = watch.RunSilent(1000);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].StalledForMs == 601'000);

    // Once reported, the stall stays open until the loop turns, whatever the window does.
    watch.WindowAnswers = true;
    REQUIRE(watch.RunSilent(watch.Policy.RepeatMs).size() == 1);
}

TEST_CASE("4.1.2 hang watch: a slow exit is not a hang", "[rfab][v412][diag][hang]")
{
    Watch watch;
    watch.RunTurning(60'000);
    watch.Phase = HangPhase::kExiting;
    REQUIRE(watch.RunSilent(600'000).empty());
}

TEST_CASE("4.1.2 hang watch: a clock that jumps back reports nothing", "[rfab][v412][diag][hang]")
{
    HangWatchState state;
    // Last turn "in the future": the difference must not wrap around into an enormous stall.
    const HangVerdict verdict = state.Evaluate(5000, 1000, 9000, 3, HangPhase::kRunning, false);
    REQUIRE(verdict.Kind == HangVerdictKind::kNothing);
}

TEST_CASE("4.1.2 screen watch: a loading or black screen that stays up is reported while it lasts and when it ends", "[rfab][v412][diag][screen]")
{
    ScreenWatchState watch;
    const ScreenWatchPolicy policy;
    uint64_t now = kStart;

    const auto run = [&](BlockingScreen aScreen, uint64_t aMs) {
        std::vector<ScreenVerdict> out;
        for (uint64_t t = 0; t < aMs; t += 1000)
        {
            now += 1000;
            const ScreenVerdict verdict = watch.Evaluate(now, aScreen, policy);
            if (verdict.Report)
                out.push_back(verdict);
        }
        return out;
    };

    SECTION("an ordinary loading screen says nothing, also many in a row")
    {
        for (int i = 0; i < 10; ++i)
        {
            REQUIRE(run(BlockingScreen::kLoading, policy.FirstMs - 2000).empty());
            REQUIRE(run(BlockingScreen::kNone, 5000).empty());
        }
    }

    SECTION("a black screen that never ends: once a minute, bounded")
    {
        REQUIRE(run(BlockingScreen::kNone, 5000).empty());
        const auto reports = run(BlockingScreen::kFaded, 3'600'000);
        REQUIRE(reports.size() == policy.MaxReports);
        REQUIRE(reports[0].Screen == BlockingScreen::kFaded);
        REQUIRE_FALSE(reports[0].Ended);
        REQUIRE(reports[0].ForMs >= policy.FirstMs);
        REQUIRE(reports[0].ForMs < policy.FirstMs + 2000);
        REQUIRE(reports[1].ForMs >= policy.FirstMs + policy.RepeatMs);
    }

    SECTION("a long loading screen that ends says how long it took")
    {
        REQUIRE(run(BlockingScreen::kNone, 2000).empty());
        REQUIRE(run(BlockingScreen::kLoading, 200'000).size() == 3);
        const auto ended = run(BlockingScreen::kNone, 1000);
        REQUIRE(ended.size() == 1);
        REQUIRE(ended[0].Ended);
        REQUIRE(ended[0].Screen == BlockingScreen::kLoading);
        REQUIRE(ended[0].ForMs >= 200'000);
        // And it is over: nothing more.
        REQUIRE(run(BlockingScreen::kNone, 300'000).empty());
    }

    SECTION("a fade that follows a loading screen is timed on its own")
    {
        REQUIRE(run(BlockingScreen::kNone, 1000).empty());
        REQUIRE(run(BlockingScreen::kLoading, policy.FirstMs - 5000).empty());
        REQUIRE(run(BlockingScreen::kFaded, policy.FirstMs - 5000).empty());
        REQUIRE(run(BlockingScreen::kNone, 5000).empty());
    }
}

TEST_CASE("4.1.2 NPC watch: the three ways an enemy can stand idle are counted apart", "[rfab][v412][diag][npc]")
{
    const auto npc = [](NpcOwner aOwner, bool aHostile, bool aInCombat, bool aWeaponDrawn, bool aPlayerNear) {
        NpcObservation out{};
        out.Owner = aOwner;
        out.HostileToAPlayer = aHostile;
        out.InCombat = aInCombat;
        out.WeaponDrawn = aWeaponDrawn;
        out.PlayerInPlainSightRange = aPlayerNear;
        return out;
    };

    SECTION("a fight that works")
    {
        NpcTally tally;
        REQUIRE_FALSE(tally.Add(npc(NpcOwner::kHere, true, true, true, true)));
        REQUIRE_FALSE(tally.Add(npc(NpcOwner::kElsewhere, true, false, true, true)));  // remote: only the weapon shows
        REQUIRE(tally.Total == 2);
        REQUIRE(tally.Hostile == 2);
        REQUIRE(tally.OwnedHereInCombat == 1);
        REQUIRE(tally.OwnedElsewhereArmed == 1);
        REQUIRE(tally.Anomalies() == 0);
    }

    SECTION("a town")
    {
        NpcTally tally;
        for (int i = 0; i < 30; ++i)
            REQUIRE_FALSE(tally.Add(npc(i % 2 ? NpcOwner::kHere : NpcOwner::kElsewhere, false, false, false, true)));
        REQUIRE_FALSE(tally.Add(npc(NpcOwner::kNobody, false, false, false, true)));
        REQUIRE(tally.Total == 31);
        REQUIRE(tally.Hostile == 0);
        REQUIRE(tally.Anomalies() == 0);
    }

    SECTION("enemies that have not noticed anyone yet")
    {
        NpcTally tally;
        REQUIRE_FALSE(tally.Add(npc(NpcOwner::kHere, true, false, false, false)));
        REQUIRE_FALSE(tally.Add(npc(NpcOwner::kElsewhere, true, false, false, false)));
        REQUIRE_FALSE(tally.Add(npc(NpcOwner::kNobody, true, false, false, false)));
        REQUIRE(tally.Hostile == 3);
        REQUIRE(tally.Anomalies() == 0);
    }

    SECTION("the playtest picture, from each side")
    {
        NpcTally tally;
        REQUIRE(tally.Add(npc(NpcOwner::kHere, true, false, false, true)));
        REQUIRE(tally.Add(npc(NpcOwner::kHere, true, false, true, true)));       // weapon out but no fight is still idle
        REQUIRE(tally.Add(npc(NpcOwner::kElsewhere, true, false, false, true)));
        REQUIRE(tally.Add(npc(NpcOwner::kNobody, true, false, false, true)));
        REQUIRE(tally.Add(npc(NpcOwner::kNobody, true, true, true, true)));      // fights, but only on this screen
        REQUIRE(tally.IdleHostileHere == 2);
        REQUIRE(tally.IdleHostileElsewhere == 1);
        REQUIRE(tally.HostileUnowned == 2);
        REQUIRE(tally.Anomalies() == 5);
    }
}

TEST_CASE("4.1.2 NPC watch: one line a minute, and a warning only for an anomaly that lasts", "[rfab][v412][diag][npc]")
{
    NpcReportPacer pacer;
    const NpcReportPolicy policy;
    uint64_t now = kStart;

    const auto run = [&](uint32_t aTotal, uint32_t aAnomalies, uint64_t aMs) {
        std::vector<NpcReport> out;
        for (uint64_t t = 0; t < aMs; t += 4000)  // the client scans every four seconds
        {
            now += 4000;
            const NpcReport report = pacer.Evaluate(now, aTotal, aAnomalies, policy);
            if (report != NpcReport::kNone)
                out.push_back(report);
        }
        return out;
    };

    SECTION("nobody around: silence")
    {
        REQUIRE(run(0, 0, 600'000).empty());
    }

    SECTION("NPCs around, all well: a summary a minute")
    {
        const auto reports = run(12, 0, 600'000);
        REQUIRE(reports.size() == 10);
        for (const NpcReport report : reports)
            REQUIRE(report == NpcReport::kSummary);
    }

    SECTION("an enemy that takes a moment to notice the player is not reported")
    {
        run(5, 0, 4000);  // the first summary
        for (int i = 0; i < 5; ++i)
        {
            for (const NpcReport report : run(5, 1, 4000))
                REQUIRE(report != NpcReport::kAnomaly);
            for (const NpcReport report : run(5, 0, 4000))
                REQUIRE(report != NpcReport::kAnomaly);
        }
    }

    SECTION("enemies that stay idle: a warning after eight seconds, then every thirty")
    {
        run(5, 0, 4000);
        const auto reports = run(5, 3, 128'000);
        uint32_t anomalies = 0;
        for (const NpcReport report : reports)
            anomalies += report == NpcReport::kAnomaly ? 1 : 0;
        // At 8 s, then at 40 s, 72 s, 104 s (the scan is every 4 s, the repeat is 30 s).
        REQUIRE(anomalies == 4);
        // The warning carries the summary: no separate summary lines in between.
        REQUIRE(reports.size() == anomalies);
    }

    SECTION("a new session starts from nothing")
    {
        run(5, 3, 60'000);
        pacer.Reset();
        const auto reports = run(5, 3, 12'000);
        // A summary at once, the warning only after the anomaly lasted again (third scan: 8 s after the first).
        REQUIRE(reports.size() == 2);
        REQUIRE(reports[0] == NpcReport::kSummary);
        REQUIRE(reports[1] == NpcReport::kAnomaly);
    }
}

TEST_CASE("v4.1.2 server pulse: a line per minute with players, a report when the loop stood still", "[rfab][v412][diag]")
{
    ServerPulseState state;
    uint64_t now = kStart;

    // Runs the loop at 20 turns a second for aForMs and returns what was reported.
    const auto run = [&](uint64_t aForMs, bool aHasPlayers) {
        std::vector<ServerPulse> out;
        for (uint64_t elapsed = 0; elapsed < aForMs; elapsed += 50)
        {
            now += 50;
            const ServerPulse pulse = state.Evaluate(now, 50, aHasPlayers);
            if (pulse.Kind != ServerPulseKind::kNone)
                out.push_back(pulse);
        }
        return out;
    };

    SECTION("an empty server: one line every ten minutes")
    {
        const auto reports = run(31 * 60'000, false);
        REQUIRE(reports.size() == 3);
        for (const ServerPulse& pulse : reports)
        {
            REQUIRE(pulse.Kind == ServerPulseKind::kPulse);
            REQUIRE(pulse.Stalls == 0);
            REQUIRE(pulse.LongestTurnMs == 50);
        }
    }

    SECTION("with players: at once when the first arrives, then every minute")
    {
        REQUIRE(run(90'000, false).empty());
        const auto reports = run(5 * 60'000, true);
        REQUIRE(reports.size() == 5);
        REQUIRE(reports[0].Kind == ServerPulseKind::kPulse);
        // Every turn of a minute is counted in the line that closes it.
        REQUIRE(reports[1].Turns == 1200);
    }

    SECTION("a turn of two seconds or more is reported at once, and counted in the next line")
    {
        (void)run(10'000, true);
        now += 7000;
        const ServerPulse stall = state.Evaluate(now, 7000, true);
        REQUIRE(stall.Kind == ServerPulseKind::kStall);
        REQUIRE(stall.LongestTurnMs == 7000);

        const auto reports = run(60'000, true);
        REQUIRE(reports.size() == 1);
        REQUIRE(reports[0].Kind == ServerPulseKind::kPulse);
        REQUIRE(reports[0].Stalls == 1);
        REQUIRE(reports[0].LongestTurnMs == 7000);
    }

    SECTION("a server that keeps stalling does not flood the log")
    {
        (void)run(1000, true);
        uint32_t stalls = 0;
        for (int i = 0; i < 20; ++i)
        {
            now += 2500;
            stalls += state.Evaluate(now, 2500, true).Kind == ServerPulseKind::kStall ? 1 : 0;
        }
        // 50 seconds of stalled turns: the first, then one every ten seconds.
        REQUIRE(stalls == 5);
    }

    SECTION("a turn just under the limit is not a stall")
    {
        (void)run(1000, true);
        now += 1999;
        REQUIRE(state.Evaluate(now, 1999, true).Kind == ServerPulseKind::kNone);
    }
}
