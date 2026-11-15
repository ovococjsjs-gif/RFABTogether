#include <TiltedOnlinePCH.h>

#include <Services/RunnerService.h>

#include <Events/UpdateEvent.h>
#include <World.h>

#include <common/Session/Features.h>

RunnerService::RunnerService(entt::dispatcher& aDispatcher, Session::MainThreadQueue& aQueue, const Session::Lifecycle& acLifecycle) noexcept
    : m_dispatcher(aDispatcher)
    , m_queue(aQueue)
    , m_lifecycle(acLifecycle)
{
}

void RunnerService::Queue(std::function<void()> aFunctor) noexcept
{
    if (!aFunctor)
        return;

    if (m_queue.Post(Session::Lane::kSimulation, m_lifecycle.GetTokenAnyThread(), m_lifecycle.IsWorldLoadingAnyThread(), std::move(aFunctor)))
        return;

    WarnOverflow();
}

void RunnerService::QueueForThisWorld(std::function<void()> aFunctor) noexcept
{
    if (!aFunctor)
        return;

    // One read of both values, in the order that is safe against a load beginning right now.
    const Session::PostStamp stamp = m_lifecycle.GetPostStampAnyThread();
    if (m_queue.Post(Session::Lane::kSimulation, stamp.PostedIn, stamp.DuringWorldLoad, std::move(aFunctor), Session::Scope::kWorld))
        return;

    WarnOverflow();
}

bool RunnerService::RejectIfSimulationLaneFull() noexcept
{
    if (!m_queue.TryRejectIfFull(Session::Lane::kSimulation, m_lifecycle.IsWorldLoadingAnyThread()))
        return false;

    WarnOverflow();
    return true;
}

void RunnerService::WarnOverflow() noexcept
{
    // Overflow means the simulation lane has not been drained for a long time; say so a few times.
    if (m_overflowWarnings.fetch_add(1, std::memory_order_relaxed) < 8)
    {
        spdlog::warn("[RFAB MainThread] simulation lane full ({} tasks): a task was dropped",
                     m_queue.GetCapacity(Session::Lane::kSimulation));
    }
}

void RunnerService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    (void)acUpdateEvent;

    // Gameplay:bEnableWorldScopedHookEvents on the server. Off: hook events live as long as their
    // session, as in 4.1.0 (an event posted before or during a coordinated reload runs after it).
    const bool worldScoped = World::IsFeatureEnabled(Session::Feature::kWorldScopedHookEvents);

    m_queue.Drain(
        Session::Lane::kSimulation,
        [this](Session::Token aPostedIn) noexcept { return m_lifecycle.IsStillCurrentSession(aPostedIn); },
        [this, worldScoped](const Session::PostStamp& acStamp) noexcept {
            return !worldScoped || m_lifecycle.IsWorldWorkStillValid(acStamp);
        });

    // One line per reload that left such events behind, so a playtest log shows how many there were.
    const uint64_t dropped = m_queue.GetStats(Session::Lane::kSimulation).DroppedStaleWorld;
    if (dropped != m_reportedStaleWorldDrops)
    {
        spdlog::info("[RFAB MainThread] dropped {} hook event(s) that were posted before or during the world load",
                     dropped - m_reportedStaleWorldDrops);
        m_reportedStaleWorldDrops = dropped;
    }
}
