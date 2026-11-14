// Work plan 4.1: gameplay work posted by engine hooks dies with the world it was read in.
//
// A hook event names actors and objects by form id. Form ids are only meaningful inside one loaded
// world: after a save is loaded the engine hands the temporary ids (0xFFxxxxxx) out again, and what
// the engine does while it restores a save looks like gameplay to every hook. Before 4.1.1 such work
// only died with its *session*, so an event posted just before or during a coordinated reload ran
// after it, against the new world. These tests drive the real Lifecycle and MainThreadQueue.

#include <catch2/catch.hpp>

#include <common/Session/Features.h>
#include <common/Session/Lifecycle.h>
#include <common/Session/MainThreadQueue.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace Session;

namespace
{
// The client's runner, without the client: Trigger/QueueForThisWorld post world-scoped work with the
// stamp, Queue posts session-scoped work, OnUpdate drains with both rules.
struct Runner
{
    Lifecycle Cycle;
    MainThreadQueue Queue;
    std::vector<std::string> Ran;
    bool WorldScopedEnabled{true};  // the server switch Gameplay:bEnableWorldScopedHookEvents

    void Online(uint32_t aPlayerId = 7)
    {
        Cycle.OnTransportUp();
        Cycle.OnAuthenticated(aPlayerId);
    }

    void HookEvent(std::string aName)
    {
        const PostStamp stamp = Cycle.GetPostStampAnyThread();
        Queue.Post(Lane::kSimulation, stamp.PostedIn, stamp.DuringWorldLoad, [this, name = std::move(aName)]() { Ran.push_back(name); },
                   Scope::kWorld);
    }

    void SessionWork(std::string aName)
    {
        Queue.Post(Lane::kSimulation, Cycle.GetTokenAnyThread(), Cycle.IsWorldLoadingAnyThread(),
                   [this, name = std::move(aName)]() { Ran.push_back(name); });
    }

    void Frame()
    {
        Queue.Drain(
            Lane::kSimulation, [this](Token aPostedIn) noexcept { return Cycle.IsStillCurrentSession(aPostedIn); },
            [this](const PostStamp& acStamp) noexcept { return !WorldScopedEnabled || Cycle.IsWorldWorkStillValid(acStamp); });
    }

    [[nodiscard]] LaneStats Stats() const { return Queue.GetStats(Lane::kSimulation); }
};

using Names = std::vector<std::string>;
} // namespace

TEST_CASE("v4.1.1 world-scoped work: the rule", "[rfab][v411][worldscope]")
{
    const Token inWorld5{3, 5};

    SECTION("posted in the world that is still loaded, outside a load: valid")
    {
        CHECK(IsWorldWorkStillValid(PostStamp{inWorld5, false}, true, 5));
    }

    SECTION("a save was loaded since: stale, whatever else is true")
    {
        CHECK_FALSE(IsWorldWorkStillValid(PostStamp{inWorld5, false}, true, 6));
        CHECK_FALSE(IsWorldWorkStillValid(PostStamp{inWorld5, false}, true, 4));  // never "newer than now", but never trusted either
    }

    SECTION("posted while a save was loading: stale even though the generation matches")
    {
        CHECK_FALSE(IsWorldWorkStillValid(PostStamp{inWorld5, true}, true, 5));
    }

    SECTION("the session is over: stale")
    {
        CHECK_FALSE(IsWorldWorkStillValid(PostStamp{inWorld5, false}, false, 5));
    }
}

TEST_CASE("v4.1.1 world-scoped work: a hook event does not survive a coordinated reload", "[rfab][v411][worldscope]")
{
    Runner r;
    r.Online();

    SECTION("posted before the reload was committed: dropped, counted, session work untouched")
    {
        r.HookEvent("hit on FF000A31");      // a temporary form id of the old world
        r.SessionWork("ui command");
        r.Cycle.OnWorldLoadBegin(true);       // the reload barrier commits
        r.Cycle.OnWorldLoadEnd(true);
        r.Frame();

        CHECK(r.Ran == Names{"ui command"});
        CHECK(r.Stats().DroppedStaleWorld == 1);
        CHECK(r.Stats().DroppedStale == 0);
        CHECK(r.Stats().Executed == 1);
    }

    SECTION("the frames between the commit and the engine starting the load still simulate: the old event is already stale there")
    {
        r.HookEvent("health change");
        r.Cycle.OnWorldLoadBegin(true);
        r.Frame();  // World::Update still runs for a few frames; every participant has torn its world down
        CHECK(r.Ran.empty());
        CHECK(r.Stats().DroppedStaleWorld == 1);
    }

    SECTION("posted between the commit and the engine's own load begin: dropped although the generation never changes again")
    {
        r.Cycle.OnWorldLoadBegin(true);       // committed: generation W+1, loading
        r.HookEvent("equip in the dying world");
        r.Cycle.OnWorldLoadBegin(true);       // the engine starts the committed load (Repeated)
        r.Cycle.OnWorldLoadEnd(true);
        r.Frame();

        CHECK(r.Ran.empty());
        CHECK(r.Stats().DroppedStaleWorld == 1);
    }

    SECTION("what the engine does while it restores the save is the new baseline, not a change to it")
    {
        r.Cycle.OnWorldLoadBegin(true);
        r.Cycle.OnWorldLoadBegin(true);
        for (int i = 0; i < 500; ++i)
            r.HookEvent("inventory add during load");
        r.SessionWork("save control during load");
        r.Cycle.OnWorldLoadEnd(true);
        r.Frame();

        CHECK(r.Ran == Names{"save control during load"});
        CHECK(r.Stats().DroppedStaleWorld == 500);
        CHECK(r.Stats().PostedDuringWorldLoad == 501);
    }

    SECTION("posted after the load finished: runs")
    {
        r.Cycle.OnWorldLoadBegin(true);
        r.Cycle.OnWorldLoadEnd(true);
        r.HookEvent("first hit in the new world");
        r.Frame();

        CHECK(r.Ran == Names{"first hit in the new world"});
        CHECK(r.Stats().DroppedStaleWorld == 0);
    }

    SECTION("order is kept around the boundary: only the stale ones are removed")
    {
        r.HookEvent("old 1");
        r.SessionWork("session 1");
        r.Cycle.OnWorldLoadBegin(true);
        r.HookEvent("loading 1");
        r.SessionWork("session 2");
        r.Cycle.OnWorldLoadEnd(true);
        r.HookEvent("new 1");
        r.SessionWork("session 3");
        r.HookEvent("new 2");
        r.Frame();

        CHECK(r.Ran == Names{"session 1", "session 2", "new 1", "session 3", "new 2"});
        CHECK(r.Stats().DroppedStaleWorld == 2);
    }

    SECTION("two reloads in a row")
    {
        r.HookEvent("world 1");
        r.Cycle.OnWorldLoadBegin(true);
        r.Cycle.OnWorldLoadEnd(true);
        r.HookEvent("world 2");
        r.Cycle.OnWorldLoadBegin(true);
        r.Cycle.OnWorldLoadEnd(true);
        r.HookEvent("world 3");
        r.Frame();

        CHECK(r.Ran == Names{"world 3"});
        CHECK(r.Stats().DroppedStaleWorld == 2);
    }
}

TEST_CASE("v4.1.1 world-scoped work: a load that never happens still invalidates what was posted around it", "[rfab][v411][worldscope]")
{
    Runner r;
    r.Online();

    // Every participant tore its world state down at the commit and rebuilds it from discovery after
    // the cancel: a delta from in between has no baseline on either side.
    r.HookEvent("before the commit");
    r.Cycle.OnWorldLoadBegin(true);
    r.HookEvent("between commit and cancel");
    r.Cycle.OnWorldLoadCancel();
    r.HookEvent("after the cancel");
    r.Frame();

    CHECK(r.Ran == Names{"after the cancel"});
    CHECK(r.Stats().DroppedStaleWorld == 2);
}

TEST_CASE("v4.1.1 world-scoped work: sessions and offline play", "[rfab][v411][worldscope]")
{
    Runner r;

    SECTION("offline work is never judged: no session, nothing to be stale against")
    {
        r.HookEvent("offline hit");
        r.Cycle.OnWorldLoadBegin(false);
        r.HookEvent("offline, during a single-player load");
        r.Cycle.OnWorldLoadEnd(true);
        r.HookEvent("offline, after the load");
        r.Frame();

        CHECK(r.Ran.size() == 3);
        CHECK(r.Stats().DroppedStaleWorld == 0);
        CHECK(r.Stats().DroppedStale == 0);
    }

    SECTION("the session ending is counted as a stale session, not as a stale world")
    {
        r.Online();
        r.HookEvent("hit in session 1");
        r.Cycle.OnTransportDown();
        r.Frame();

        CHECK(r.Ran.empty());
        CHECK(r.Stats().DroppedStale == 1);
        CHECK(r.Stats().DroppedStaleWorld == 0);
    }

    SECTION("an uncoordinated load closes the session: everything posted in it is gone")
    {
        r.Online();
        r.HookEvent("hit");
        r.SessionWork("ui");
        r.Cycle.OnWorldLoadBegin(false);  // not a coordinated reload: the session will close
        r.Cycle.OnTransportDown();
        r.Cycle.OnWorldLoadEnd(true);
        r.Frame();

        CHECK(r.Ran.empty());
        CHECK(r.Stats().DroppedStale == 2);
    }

    SECTION("a world generation of an earlier session never validates work in a later one")
    {
        r.Online();
        r.HookEvent("session 1 event");
        r.Cycle.OnTransportDown();
        r.Online(8);
        r.HookEvent("session 2 event");
        r.Frame();

        CHECK(r.Ran == Names{"session 2 event"});
    }
}

TEST_CASE("v4.1.1 world-scoped work: the server switch restores 4.1.0", "[rfab][v411][worldscope]")
{
    SECTION("switched off: hook events live as long as their session")
    {
        Runner r;
        r.WorldScopedEnabled = false;
        r.Online();
        r.HookEvent("before");
        r.Cycle.OnWorldLoadBegin(true);
        r.HookEvent("during");
        r.Cycle.OnWorldLoadEnd(true);
        r.HookEvent("after");
        r.Frame();

        CHECK(r.Ran == Names{"before", "during", "after"});
        CHECK(r.Stats().DroppedStaleWorld == 0);
    }

    SECTION("switched off, the session rule still applies")
    {
        Runner r;
        r.WorldScopedEnabled = false;
        r.Online();
        r.HookEvent("before");
        r.Cycle.OnTransportDown();
        r.Frame();
        CHECK(r.Ran.empty());
        CHECK(r.Stats().DroppedStale == 1);
    }

    SECTION("the one-rule drain treats world-scoped work as session work")
    {
        Runner r;
        r.Online();
        r.HookEvent("before");
        r.Cycle.OnWorldLoadBegin(true);
        r.Cycle.OnWorldLoadEnd(true);
        r.Queue.Drain(Lane::kSimulation, [&r](Token aPostedIn) noexcept { return r.Cycle.IsStillCurrentSession(aPostedIn); });
        CHECK(r.Ran == Names{"before"});
    }

    SECTION("the switch is a feature bit that is on unless the server turns it off")
    {
        CHECK(NameOf(Feature::kWorldScopedHookEvents) == "WorldScopedHookEvents");
        CHECK((kAllKnownFeatureBits & Bit(Feature::kWorldScopedHookEvents)) != 0);
        CHECK(IsEnabledIn(0u, Feature::kWorldScopedHookEvents));
        CHECK_FALSE(IsEnabledIn(Bit(Feature::kWorldScopedHookEvents), Feature::kWorldScopedHookEvents));
    }
}

TEST_CASE("v4.1.1 world-scoped work: a post racing with a load begin never looks like work of the new world",
          "[rfab][v411][worldscope][threads]")
{
    // The main thread begins and ends loads; a game thread stamps posts as fast as it can. The stamp
    // may be of the old world (stale by generation) or of the load (stale by flag). What must never
    // be observed is "new generation, not loading" before the load has started to end: that stamp
    // would let an event of the dying world run in the new one.
    Lifecycle cycle;
    cycle.OnTransportUp();
    cycle.OnAuthenticated(7);

    constexpr uint32_t kLoads = 4000;
    std::atomic<uint32_t> endsStarted{0};   // number of loads whose end has been started
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> violations{0};
    std::atomic<uint64_t> stamps{0};
    std::atomic<uint64_t> sawLoading{0};

    const uint32_t firstGeneration = cycle.GetMachine().GetWorldGeneration();

    std::thread poster([&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            const PostStamp stamp = cycle.GetPostStampAnyThread();
            const uint32_t ended = endsStarted.load(std::memory_order_seq_cst);
            stamps.fetch_add(1, std::memory_order_relaxed);
            if (stamp.DuringWorldLoad)
            {
                sawLoading.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Generation firstGeneration + k is the world of load number k (1-based). "Not loading"
            // in it is only possible once the end of load k has been started.
            const uint32_t load = stamp.PostedIn.World - firstGeneration;
            if (load > ended)
                violations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (uint32_t i = 0; i < kLoads; ++i)
    {
        cycle.OnWorldLoadBegin(true);
        endsStarted.store(i + 1, std::memory_order_seq_cst);
        cycle.OnWorldLoadEnd(true);
    }
    stop.store(true, std::memory_order_release);
    poster.join();

    CHECK(violations.load() == 0);
    CHECK(stamps.load() > 0);
    CHECK(cycle.GetMachine().GetWorldGeneration() == firstGeneration + kLoads);
}

TEST_CASE("v4.1.1 world-scoped work: many game threads post through reloads, nothing posted inside a load ever runs",
          "[rfab][v411][worldscope][threads]")
{
    Lifecycle cycle;
    MainThreadQueue queue;
    cycle.OnTransportUp();
    cycle.OnAuthenticated(7);

    // The test's own truth about "a load is in progress": odd while the main thread is strictly
    // between a completed begin and the start of the end. A hook that reads the same odd value
    // before and after stamping knows its event was produced inside that load.
    std::atomic<uint32_t> loadWindow{0};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ran{0};
    std::atomic<uint64_t> ranFromInsideLoad{0};
    std::atomic<uint64_t> postedInsideLoad{0};
    const Lifecycle* pCycle = &cycle;

    std::vector<std::thread> hooks;
    for (int t = 0; t < 4; ++t)
    {
        hooks.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire))
            {
                const uint32_t before = loadWindow.load(std::memory_order_seq_cst);
                const PostStamp stamp = pCycle->GetPostStampAnyThread();
                const uint32_t after = loadWindow.load(std::memory_order_seq_cst);
                const bool insideLoad = before == after && (before & 1u) != 0;
                if (insideLoad)
                    postedInsideLoad.fetch_add(1, std::memory_order_relaxed);
                queue.Post(
                    Lane::kSimulation, stamp.PostedIn, stamp.DuringWorldLoad,
                    [&ran, &ranFromInsideLoad, insideLoad]() {
                        ran.fetch_add(1, std::memory_order_relaxed);
                        if (insideLoad)
                            ranFromInsideLoad.fetch_add(1, std::memory_order_relaxed);
                    },
                    Scope::kWorld);
            }
        });
    }

    const auto frame = [&]() {
        queue.Drain(
            Lane::kSimulation, [&](Token aPostedIn) noexcept { return cycle.IsStillCurrentSession(aPostedIn); },
            [&](const PostStamp& acStamp) noexcept { return cycle.IsWorldWorkStillValid(acStamp); });
    };

    for (int i = 0; i < 300; ++i)
    {
        frame();
        cycle.OnWorldLoadBegin(true);
        loadWindow.fetch_add(1, std::memory_order_seq_cst);  // odd: inside
        frame();                                             // a frame between the commit and the engine load
        cycle.OnWorldLoadBegin(true);
        std::this_thread::yield();
        loadWindow.fetch_add(1, std::memory_order_seq_cst);  // even: about to end
        cycle.OnWorldLoadEnd(true);
        frame();
    }
    stop.store(true, std::memory_order_release);
    for (auto& hook : hooks)
        hook.join();
    frame();

    const LaneStats stats = queue.GetStats(Lane::kSimulation);
    CHECK(ranFromInsideLoad.load() == 0);
    CHECK(stats.Executed == ran.load());
    CHECK(stats.Posted == stats.Executed + stats.DroppedStaleWorld + stats.DroppedOverflow);
    CHECK(stats.DroppedStaleWorld >= postedInsideLoad.load());
    CHECK(stats.DroppedStale == 0);
    CHECK(stats.Failed == 0);
}
