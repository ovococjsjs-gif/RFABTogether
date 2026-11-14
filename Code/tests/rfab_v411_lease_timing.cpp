// Work plan 5.1: every time constant of NPC ownership is a named field of a policy, and the
// decisions that read them are pure functions.
//
// What this suite found worth pinning: the lease of a boss was granted for 15 s and renewed for
// 8 s. The claim asked "is this fight locked?" and the renewal - one call per packet from the
// owner - did not, so the longer window lasted until the owner's next packet. The cases below
// replay that sequence through the same functions the server calls.

#include <Authority/Lease.h>

#include <catch2/catch.hpp>

using namespace Authority;

namespace
{
constexpr uint32_t kOwner = 1u;
constexpr uint32_t kPeer = 2u;
constexpr uint64_t kT0 = 1'000'000ull;

EntityTraits WorldNpc()
{
    return EntityTraits{};
}

EntityTraits LockedBoss()
{
    EntityTraits traits{};
    traits.IsBossEncounterLocked = true;
    return traits;
}

PeerActivity ActiveAt(uint64_t aAtMs)
{
    PeerActivity activity{};
    activity.Known = true;
    activity.LastAtMs = aAtMs;
    return activity;
}

// What the server does for one entity: the claim grants, each packet from the owner renews.
Lease GrantTo(uint32_t aPlayerId, uint64_t aNowMs, const EntityTraits& acTraits)
{
    Candidate challenger{};
    challenger.PlayerId = aPlayerId;
    const auto decision =
        Claim(Lease{}, Subject{}, OwnerState{false, false}, challenger, aNowMs, SelectLeasePolicy(acTraits));
    REQUIRE(decision.Result == Outcome::kGranted);
    return decision.Next;
}

Lease RenewedAt(const Lease& acLease, uint64_t aNowMs, const EntityTraits& acTraits)
{
    const auto decision = RefreshUnrevoked(acLease, acLease.OwnerId, aNowMs, SelectLeasePolicy(acTraits));
    return decision.Changed ? decision.Next : acLease;
}
} // namespace

TEST_CASE("5.1: one function picks the lease policy of an entity", "[rfab][authority][lease][timing]")
{
    SECTION("an ordinary world NPC lapses after 8 s without its owner")
    {
        const Policy policy = SelectLeasePolicy(WorldNpc());
        CHECK(policy.LeaseDurationMs == kWorldNpcLeaseDurationMs);
        CHECK(policy.LeaseDurationMs == 8000ull);
        CHECK(policy.MinHoldMs == 1500ull);
    }

    SECTION("a locked boss fight and a dragon in combat get the boss window")
    {
        const Policy boss = SelectLeasePolicy(LockedBoss());
        CHECK(boss.LeaseDurationMs == kBossEncounterLeaseDurationMs);
        CHECK(boss.MinHoldMs == kBossEncounterMinHoldMs);

        EntityTraits dragon{};
        dragon.IsDragonInCombat = true;
        CHECK(SelectLeasePolicy(dragon).LeaseDurationMs == kBossEncounterLeaseDurationMs);

        // The boss window is longer than the ordinary one: that is its whole point.
        CHECK(boss.LeaseDurationMs > SelectLeasePolicy(WorldNpc()).LeaseDurationMs);
    }

    SECTION("avatars, summons and mounts never lapse - a boss lock does not change that")
    {
        for (int kind = 0; kind < 3; ++kind)
        {
            EntityTraits traits = LockedBoss();
            traits.IsDragonInCombat = true;
            traits.IsPlayerAvatar = kind == 0;
            traits.IsPlayerSummon = kind == 1;
            traits.IsMount = kind == 2;
            CHECK(SelectLeasePolicy(traits).LeaseDurationMs == 0ull);
        }
    }
}

TEST_CASE("5.1: the boss window survives the owner's next packet", "[rfab][authority][lease][timing]")
{
    // The owner claims the boss at T0 and its client keeps sending for two seconds, then the
    // player opens a menu that pauses his game for ten seconds. Another player is fighting next
    // to him the whole time.
    const Lease granted = GrantTo(kOwner, kT0, LockedBoss());
    REQUIRE(granted.ExpiresAtMs == kT0 + kBossEncounterLeaseDurationMs);

    const uint64_t lastPacket = kT0 + 2000ull;
    const uint64_t menuFor10s = lastPacket + 10'000ull;

    SECTION("renewed with the same facts as the claim, the boss is still its owner's after 10 s")
    {
        const Lease renewed = RenewedAt(granted, lastPacket, LockedBoss());
        CHECK(renewed.Epoch == granted.Epoch); // a renewal is not a transition
        CHECK(renewed.ExpiresAtMs == lastPacket + kBossEncounterLeaseDurationMs);
        CHECK(EvaluateLapsedLease(renewed, ActiveAt(lastPacket), menuFor10s, SelectLeasePolicy(LockedBoss())) ==
              LapsedLeaseAction::kNothing);

        // ...and it does lapse once the boss window itself is over.
        CHECK(EvaluateLapsedLease(renewed, ActiveAt(lastPacket), lastPacket + kBossEncounterLeaseDurationMs,
                                  SelectLeasePolicy(LockedBoss())) == LapsedLeaseAction::kOfferToActivePeers);
    }

    SECTION("renewed without the lock (before 4.1.1, and with the switch off) the window shrinks to 8 s")
    {
        const Lease renewed = RenewedAt(granted, lastPacket, WorldNpc());
        CHECK(renewed.ExpiresAtMs == lastPacket + kWorldNpcLeaseDurationMs);
        CHECK(EvaluateLapsedLease(renewed, ActiveAt(lastPacket), menuFor10s, SelectLeasePolicy(WorldNpc())) ==
              LapsedLeaseAction::kOfferToActivePeers);
    }
}

TEST_CASE("5.1: what the sweep does with one lease", "[rfab][authority][lease][timing]")
{
    const Policy policy = SelectLeasePolicy(WorldNpc());
    const Lease lease = GrantTo(kOwner, kT0, WorldNpc());
    const uint64_t expiry = lease.ExpiresAtMs;

    SECTION("nothing while the lease is live, to the millisecond")
    {
        CHECK(EvaluateLapsedLease(lease, ActiveAt(kT0), expiry - 1, policy) == LapsedLeaseAction::kNothing);
        CHECK(EvaluateLapsedLease(lease, ActiveAt(kT0), expiry, policy) == LapsedLeaseAction::kOfferToActivePeers);
    }

    SECTION("nothing for an entity nobody owns, however old its lease is")
    {
        const auto released = Revoke(lease, expiry);
        REQUIRE(released.Changed);
        CHECK(EvaluateLapsedLease(released.Next, PeerActivity{}, expiry + 60'000ull, policy) ==
              LapsedLeaseAction::kNothing);
    }

    SECTION("a lease that never expires is never offered")
    {
        EntityTraits mount{};
        mount.IsMount = true;
        const Lease pinned = GrantTo(kOwner, kT0, mount);
        CHECK(pinned.ExpiresAtMs == 0ull);
        CHECK(EvaluateLapsedLease(pinned, PeerActivity{}, kT0 + 3'600'000ull, SelectLeasePolicy(mount)) ==
              LapsedLeaseAction::kNothing);
    }

    SECTION("lapsed on paper while the owner is sending: renewed, not handed over")
    {
        // The owner's last packet is one second old; the lease expired because that packet did
        // not reach the renewal (it never should, and the sweep is the net under it).
        CHECK(EvaluateLapsedLease(lease, ActiveAt(expiry - 1000ull), expiry, policy) ==
              LapsedLeaseAction::kRefreshForActiveOwner);
    }

    SECTION("the owner's grace is the lease window, strictly")
    {
        const uint64_t now = expiry + 20'000ull;
        CHECK(EvaluateLapsedLease(lease, ActiveAt(now - policy.LeaseDurationMs + 1), now, policy) ==
              LapsedLeaseAction::kRefreshForActiveOwner);
        CHECK(EvaluateLapsedLease(lease, ActiveAt(now - policy.LeaseDurationMs), now, policy) ==
              LapsedLeaseAction::kOfferToActivePeers);
    }

    SECTION("an owner the server never heard from, or heard from 'in the future', gets no grace")
    {
        CHECK(EvaluateLapsedLease(lease, PeerActivity{}, expiry, policy) == LapsedLeaseAction::kOfferToActivePeers);
        CHECK(EvaluateLapsedLease(lease, ActiveAt(expiry + 5000ull), expiry, policy) ==
              LapsedLeaseAction::kOfferToActivePeers);
    }
}

TEST_CASE("5.1: who may take over, and how often the server looks", "[rfab][authority][lease][timing]")
{
    const SweepPolicy sweep{};

    SECTION("the constants that used to be literals at the loops")
    {
        CHECK(sweep.ExpiredLeaseSweepIntervalMs == 500ull);
        CHECK(sweep.DormantWakeSweepIntervalMs == 1000ull);
        CHECK(sweep.ActivePeerWindowMs == kActivePeerSimulationWindowMs);
        // A peer must look away for less time than an owner to stop counting: otherwise a
        // lapsed NPC could go to a client that is itself about to lapse.
        CHECK(sweep.ActivePeerWindowMs < kWorldNpcLeaseDurationMs);
    }

    SECTION("only a peer that is simulating right now may take over")
    {
        const uint64_t now = kT0 + 100'000ull;
        CHECK_FALSE(IsPeerActivelySimulating(PeerActivity{}, now, sweep));               // never sent anything
        CHECK(IsPeerActivelySimulating(ActiveAt(now), now, sweep));
        CHECK(IsPeerActivelySimulating(ActiveAt(now - sweep.ActivePeerWindowMs), now, sweep));        // inclusive
        CHECK_FALSE(IsPeerActivelySimulating(ActiveAt(now - sweep.ActivePeerWindowMs - 1), now, sweep));
        CHECK_FALSE(IsPeerActivelySimulating(ActiveAt(now + 1), now, sweep));             // clock went backwards
    }

    SECTION("a sweep runs at once the first time, then once per interval")
    {
        CHECK(IsSweepDue(0, kT0, 500));
        CHECK_FALSE(IsSweepDue(kT0, kT0 + 1, 500));
        CHECK_FALSE(IsSweepDue(kT0, kT0 + 499, 500));
        CHECK(IsSweepDue(kT0, kT0 + 500, 500));
        CHECK(IsSweepDue(kT0, kT0 + 60'000, 500));
    }

    SECTION("a clock that stands still or goes backwards does not stop the sweep")
    {
        CHECK(IsSweepDue(kT0, kT0, 500));
        CHECK(IsSweepDue(kT0, kT0 - 1, 500));
    }

    SECTION("the whole sequence: owner stalls, an active peer is offered the NPC, a stalled one is not")
    {
        const Policy policy = SelectLeasePolicy(WorldNpc());
        Lease lease = GrantTo(kOwner, kT0, WorldNpc());
        lease = RenewedAt(lease, kT0 + 2000ull, WorldNpc());
        const uint64_t now = kT0 + 2000ull + policy.LeaseDurationMs + 300ull;

        REQUIRE(EvaluateLapsedLease(lease, ActiveAt(kT0 + 2000ull), now, policy) ==
                LapsedLeaseAction::kOfferToActivePeers);

        Candidate peer{};
        peer.PlayerId = kPeer;
        peer.InRange = true;

        // The peer sent a snapshot half a second ago: it is offered the NPC and takes it.
        REQUIRE(IsPeerActivelySimulating(ActiveAt(now - 500ull), now, sweep));
        const auto handed = Reassign(lease, Subject{}, {peer}, {}, now, policy);
        CHECK(handed.Result == Outcome::kGranted);
        CHECK(handed.Next.OwnerId == kPeer);
        CHECK(handed.Next.Epoch == lease.Epoch + 1);
        CHECK(handed.Next.ExpiresAtMs == now + policy.LeaseDurationMs);

        // The same peer, silent for six seconds, is not a candidate at all.
        CHECK_FALSE(IsPeerActivelySimulating(ActiveAt(now - 6000ull), now, sweep));
    }
}
