#pragma once

#include <ThreadAffinity.h>
#include <common/Session/MainThreadQueue.h>

struct UpdateEvent;

/**
 * @brief Gameplay work for the next simulated frame: the simulation lane of World's MainThreadQueue.
 *
 * Callable from any thread (engine hooks post their events here). Every task carries the session it
 * was posted in and is dropped if that session ended first; tasks run outside any lock, so posting
 * never waits for the main thread and a task may post again (it runs on the next frame).
 * See common/Session/MainThreadQueue.h for the contract.
 */
struct RunnerService
{
    RunnerService(entt::dispatcher& aDispatcher, Session::MainThreadQueue& aQueue, const Session::Lifecycle& acLifecycle) noexcept;
    ~RunnerService() noexcept = default;

    TP_NOCOPYMOVE(RunnerService);

    /**
     * @brief Executes the work queued before this frame (World::Update, simulated frames only).
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;

    /**
     * @brief Queues a gameplay event to be dispatched on the main thread during the next simulated frame.
     *
     * World-scoped (common/Session/MainThreadQueue.h): the event names actors and objects by form id,
     * so it is dropped when a save was loaded, or was being loaded, between this call and the frame
     * that would dispatch it. This is the call every engine hook uses.
     */
    template <class T> void Trigger(T acEvent)
    {
        if (RejectIfSimulationLaneFull())
            return;
        QueueForThisWorld([event = std::move(acEvent), this]() { m_dispatcher.trigger(std::move(event)); });
    }

    /**
     * @brief Like Trigger, for an event that is not about the world (an error for the UI): it only
     * dies with the session it was posted in.
     */
    template <class T> void TriggerInSession(T acEvent)
    {
        if (RejectIfSimulationLaneFull())
            return;
        Queue([event = std::move(acEvent), this]() { m_dispatcher.trigger(std::move(event)); });
    }

    /**
     * @brief Queues a lambda to run on the main thread during the next simulated frame. It is dropped
     * only when the session it was posted in is over by then.
     */
    void Queue(std::function<void()> aFunctor) noexcept;

    /**
     * @brief Queue for work that carries form ids or anything else read from the world: also dropped
     * when a save was loaded, or was being loaded, since it was posted (see Trigger).
     */
    void QueueForThisWorld(std::function<void()> aFunctor) noexcept;

private:
    RFAB_ANY_THREAD [[nodiscard]] bool RejectIfSimulationLaneFull() noexcept;
    RFAB_ANY_THREAD void WarnOverflow() noexcept;

    entt::dispatcher& m_dispatcher;
    Session::MainThreadQueue& m_queue;
    const Session::Lifecycle& m_lifecycle;
    std::atomic<uint32_t> m_overflowWarnings{0};
    uint64_t m_reportedStaleWorldDrops{0};  // main thread (OnUpdate)
};
