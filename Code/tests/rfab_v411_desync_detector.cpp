// Work plan 3.3: the desync detector (common/Session/EntityDigest.h, Messages/ClientEntityDigest.h).
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientEntityDigest.h>
#include <Messages/ClientMessageFactory.h>

#include <common/Session/EntityDigest.h>
#include <common/Session/Features.h>

#include <algorithm>
#include <limits>
#include <vector>

using namespace Session::EntityDigest;

namespace
{
Facts Row(uint32_t aServerId, uint8_t aFlags = 0, uint8_t aHealth = kNoHealth, uint32_t aEquipment = 0)
{
    Facts facts{};
    facts.ServerId = aServerId;
    facts.Flags = aFlags;
    facts.HealthPct = aHealth;
    facts.EquipmentHash = aEquipment;
    return facts;
}

const Finding* Find(const std::vector<Finding>& acFindings, uint32_t aServerId, Field aField)
{
    const auto it = std::find_if(acFindings.begin(), acFindings.end(),
                                 [&](const Finding& f) { return f.ServerId == aServerId && f.What == aField; });
    return it == acFindings.end() ? nullptr : &*it;
}

ClientEntityDigest RoundTrip(const ClientEntityDigest& acSent)
{
    TiltedPhoques::Buffer buffer(8192);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    acSent.Serialize(writer);

    TiltedPhoques::Buffer::Reader reader(&buffer);
    const ClientMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == kClientEntityDigest);
    return *static_cast<ClientEntityDigest*>(pMessage.get());
}
} // namespace

TEST_CASE("Desync detector: a difference counts only when a later report repeats it", "[rfab][v411][desync]")
{
    Comparer comparer;
    std::vector<Finding> findings;

    const std::vector<Facts> server{Row(10), Row(11, kFlagDead)};
    const std::vector<Facts> clientWrong{Row(10, kFlagLive), Row(11, kFlagLive)};  // 11 walks around here
    const std::vector<Facts> clientRight{Row(10, kFlagLive), Row(11, kFlagLive | kFlagDead)};

    SECTION("seen once: a suspicion, nothing is said")
    {
        comparer.Compare(clientWrong, server, false, 1000, findings);
        CHECK(findings.empty());
        CHECK(comparer.GetStats().Suspected == 1);
    }

    SECTION("gone in the next report: it was state in flight")
    {
        comparer.Compare(clientWrong, server, false, 1000, findings);
        comparer.Compare(clientRight, server, false, 6000, findings);
        comparer.Compare(clientRight, server, false, 11000, findings);
        CHECK(findings.empty());
        CHECK(comparer.GetStats().Cleared == 1);
        CHECK(comparer.TrackedCount() == 0);
    }

    SECTION("repeated too early: still a suspicion")
    {
        comparer.Compare(clientWrong, server, false, 1000, findings);
        comparer.Compare(clientWrong, server, false, 3000, findings);
        CHECK(findings.empty());
    }

    SECTION("repeated after the confirmation delay: one finding, logged and repairable")
    {
        comparer.Compare(clientWrong, server, false, 1000, findings);
        comparer.Compare(clientWrong, server, false, 6000, findings);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].ServerId == 11);
        CHECK(findings[0].What == Field::kLifeState);
        CHECK(findings[0].ClientValue == 0);
        CHECK(findings[0].ServerValue == 1);
        CHECK(findings[0].AgeMs == 5000);
        CHECK(findings[0].Log);
        CHECK(findings[0].Repair);
        CHECK_FALSE(findings[0].GaveUp);
        CHECK(comparer.GetStats().Confirmed[static_cast<uint8_t>(Field::kLifeState)] == 1);
    }

    SECTION("an unrepaired difference is logged once a minute, not every report")
    {
        comparer.Compare(clientWrong, server, false, 1000, findings);
        size_t logged = 0;
        for (uint64_t now = 6000; now <= 126000; now += 5000)
        {
            findings.clear();
            comparer.Compare(clientWrong, server, false, now, findings);
            for (const auto& finding : findings)
                logged += finding.Log ? 1 : 0;
        }
        CHECK(logged == 3);  // at 6 s, 66 s and 126 s
    }
}

TEST_CASE("Desync detector: what is compared, and for whom", "[rfab][v411][desync]")
{
    Comparer comparer;
    std::vector<Finding> first;
    std::vector<Finding> findings;

    const auto confirm = [&](const std::vector<Facts>& acClient, const std::vector<Facts>& acServer, bool aTruncated = false) {
        comparer.Compare(acClient, acServer, aTruncated, 1000, first);
        comparer.Compare(acClient, acServer, aTruncated, 6000, findings);
    };

    SECTION("a character the server delivered and the client does not have")
    {
        confirm({Row(1, kFlagLive)}, {Row(1), Row(2)});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].ServerId == 2);
        CHECK(findings[0].What == Field::kMissingOnClient);
        CHECK(findings[0].Repair);
    }

    SECTION("a report that did not fit proves nothing about absence")
    {
        confirm({Row(1, kFlagLive)}, {Row(1), Row(2)}, true);
        CHECK(findings.empty());
    }

    SECTION("a character the client still shows and the server does not expect there")
    {
        confirm({Row(1, kFlagLive), Row(7, kFlagLive)}, {Row(1)});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].ServerId == 7);
        CHECK(findings[0].What == Field::kUnknownToServer);
        CHECK(findings[0].Repair);
    }

    SECTION("the two ends disagree on who simulates it: logged, never repaired, nothing else compared")
    {
        confirm({Row(3, kFlagOwnedByReporter)}, {Row(3, kFlagDead, 0)});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].What == Field::kOwner);
        CHECK(findings[0].ClientValue == 1);
        CHECK(findings[0].ServerValue == 0);
        CHECK(findings[0].Log);
        CHECK_FALSE(findings[0].Repair);
    }

    SECTION("the reporter simulates it: it is the source, so its state is not compared")
    {
        confirm({Row(4, kFlagOwnedByReporter)}, {Row(4, kFlagOwnedByReporter | kFlagDead, 10)});
        CHECK(findings.empty());
    }

    SECTION("known here but without an actor: nothing on this screen can be wrong")
    {
        confirm({Row(5)}, {Row(5, kFlagDead, 0)});
        CHECK(findings.empty());
    }

    SECTION("health inside the tolerance is the same health")
    {
        confirm({Row(6, kFlagLive, 80)}, {Row(6, 0, 60)});
        CHECK(findings.empty());
    }

    SECTION("health beyond the tolerance is logged and not repaired (the health authority owns it)")
    {
        confirm({Row(6, kFlagLive, 100)}, {Row(6, 0, 40)});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].What == Field::kHealth);
        CHECK(findings[0].ClientValue == 100);
        CHECK(findings[0].ServerValue == 40);
        CHECK(findings[0].Log);
        CHECK_FALSE(findings[0].Repair);
    }

    SECTION("health unknown at either end is not compared")
    {
        confirm({Row(6, kFlagLive, kNoHealth)}, {Row(6, 0, 40)});
        CHECK(findings.empty());
    }

    SECTION("a body: only the life state is reported, not its health as well")
    {
        confirm({Row(8, kFlagLive, 100)}, {Row(8, kFlagDead, 0)});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].What == Field::kLifeState);
    }

    SECTION("equipment is compared only when both ends report it")
    {
        confirm({Row(9, kFlagLive | kFlagHasEquipment, 100, 0xAAAA)}, {Row(9, 0, 100, 0xBBBB)});
        CHECK(findings.empty());
    }

    SECTION("a player's equipment differs: logged, never repaired")
    {
        confirm({Row(9, kFlagLive | kFlagHasEquipment, 100, 0xAAAA)}, {Row(9, kFlagHasEquipment, 100, 0xBBBB)});
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].What == Field::kEquipment);
        CHECK(findings[0].ClientValue == 0xAAAA);
        CHECK(findings[0].ServerValue == 0xBBBB);
        CHECK_FALSE(findings[0].Repair);
    }

    SECTION("a server id listed twice counts once")
    {
        confirm({Row(1, kFlagLive), Row(1, kFlagLive | kFlagDead)}, {Row(1)});
        CHECK(findings.empty());
    }

    CHECK(IsRepairable(Field::kMissingOnClient));
    CHECK(IsRepairable(Field::kUnknownToServer));
    CHECK(IsRepairable(Field::kLifeState));
    CHECK_FALSE(IsRepairable(Field::kOwner));
    CHECK_FALSE(IsRepairable(Field::kHealth));
    CHECK_FALSE(IsRepairable(Field::kEquipment));
}

TEST_CASE("Desync detector: a resend is bounded, and its result is said", "[rfab][v411][desync]")
{
    Comparer comparer;
    std::vector<Finding> findings;

    const std::vector<Facts> server{Row(11, kFlagDead)};
    const std::vector<Facts> clientWrong{Row(11, kFlagLive)};
    const std::vector<Facts> clientRight{Row(11, kFlagLive | kFlagDead)};

    comparer.Compare(clientWrong, server, false, 1000, findings);
    comparer.Compare(clientWrong, server, false, 6000, findings);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].Repair);
    comparer.NoteRepaired(11, Field::kLifeState, 6000);

    SECTION("the resend worked: the next report says so, once")
    {
        findings.clear();
        comparer.Compare(clientRight, server, false, 11000, findings);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].Healed);
        CHECK(findings[0].Log);
        CHECK_FALSE(findings[0].Repair);
        CHECK(findings[0].ServerId == 11);
        CHECK(findings[0].What == Field::kLifeState);
        CHECK(comparer.GetStats().Healed == 1);

        findings.clear();
        comparer.Compare(clientRight, server, false, 16000, findings);
        CHECK(findings.empty());
    }

    SECTION("right after a resend the difference needs a fresh confirmation period")
    {
        findings.clear();
        comparer.Compare(clientWrong, server, false, 8000, findings);
        CHECK(findings.empty());
    }

    SECTION("a resend that changes nothing is tried twice per window, then given up - said once")
    {
        findings.clear();
        comparer.Compare(clientWrong, server, false, 11000, findings);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].Repair);
        comparer.NoteRepaired(11, Field::kLifeState, 11000);

        findings.clear();
        comparer.Compare(clientWrong, server, false, 16000, findings);
        REQUIRE(findings.size() == 1);
        CHECK_FALSE(findings[0].Repair);
        CHECK(findings[0].GaveUp);
        CHECK(findings[0].Log);

        // No more resends and no more "gave up" lines for the rest of the window.
        size_t repairs = 0;
        size_t gaveUp = 0;
        for (uint64_t now = 21000; now < 6000 + 120000; now += 5000)
        {
            findings.clear();
            comparer.Compare(clientWrong, server, false, now, findings);
            for (const auto& finding : findings)
            {
                repairs += finding.Repair ? 1 : 0;
                gaveUp += finding.GaveUp ? 1 : 0;
            }
        }
        CHECK(repairs == 0);
        CHECK(gaveUp == 0);

        // A new window: it may be tried again.
        findings.clear();
        comparer.Compare(clientWrong, server, false, 6000 + 120000 + 5000, findings);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].Repair);
    }

    SECTION("a difference that comes back after every resend does not get unlimited resends")
    {
        // Resend 1 worked for one report, then the difference is back.
        findings.clear();
        comparer.Compare(clientRight, server, false, 11000, findings);  // healed
        findings.clear();
        comparer.Compare(clientWrong, server, false, 16000, findings);  // back: a new suspicion
        CHECK(findings.empty());
        comparer.Compare(clientWrong, server, false, 21000, findings);
        REQUIRE(findings.size() == 1);
        REQUIRE(findings[0].Repair);
        comparer.NoteRepaired(11, Field::kLifeState, 21000);  // resend 2

        findings.clear();
        comparer.Compare(clientRight, server, false, 26000, findings);  // healed again
        findings.clear();
        comparer.Compare(clientWrong, server, false, 31000, findings);  // and back again
        comparer.Compare(clientWrong, server, false, 36000, findings);
        REQUIRE(findings.size() == 1);
        CHECK_FALSE(findings[0].Repair);
        CHECK(findings[0].GaveUp);
    }

    SECTION("the bookkeeping is bounded")
    {
        Policy policy{};
        policy.MaxTracked = 8;
        Comparer bounded(policy);
        std::vector<Facts> manyServer;
        for (uint32_t id = 0; id < 100; ++id)
            manyServer.push_back(Row(id));
        findings.clear();
        bounded.Compare({}, manyServer, false, 1000, findings);
        CHECK(bounded.TrackedCount() == 8);
    }
}

TEST_CASE("Desync detector: health percentage and equipment hash are the same at both ends", "[rfab][v411][desync]")
{
    CHECK(HealthPercent(50.0f, 100.0f) == 50);
    CHECK(HealthPercent(0.0f, 100.0f) == 0);
    CHECK(HealthPercent(-20.0f, 100.0f) == 0);
    CHECK(HealthPercent(250.0f, 100.0f) == 100);
    CHECK(HealthPercent(1.0f, 3.0f) == 33);
    CHECK(HealthPercent(10.0f, 0.0f) == kNoHealth);
    CHECK(HealthPercent(10.0f, -5.0f) == kNoHealth);
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    CHECK(HealthPercent(notANumber, 100.0f) == kNoHealth);
    CHECK(HealthPercent(10.0f, notANumber) == kNoHealth);

    EquipmentHasher forward;
    forward.Add(1, 0x12EB7);
    forward.Add(1, 0x13952);
    forward.Add(5, 0x800);

    EquipmentHasher backward;
    backward.Add(5, 0x800);
    backward.Add(1, 0x13952);
    backward.Add(1, 0x12EB7);
    CHECK(forward.Finish() == backward.Finish());

    // The same item listed twice (two stacks, both hands) is one worn item.
    EquipmentHasher twice;
    twice.Add(1, 0x12EB7);
    twice.Add(5, 0x800);
    twice.Add(1, 0x13952);
    twice.Add(1, 0x12EB7);
    CHECK(twice.Finish() == forward.Finish());

    EquipmentHasher other;
    other.Add(1, 0x12EB7);
    other.Add(1, 0x13952);
    CHECK(other.Finish() != forward.Finish());

    // Same base id in another plugin is another item.
    EquipmentHasher otherMod;
    otherMod.Add(2, 0x12EB7);
    otherMod.Add(1, 0x13952);
    otherMod.Add(5, 0x800);
    CHECK(otherMod.Finish() != forward.Finish());

    // Naked is a statement too, and 0 stays free to mean "not reported".
    EquipmentHasher naked;
    CHECK(naked.Finish() != 0);
    CHECK(naked.Finish() != other.Finish());
}

TEST_CASE("Desync detector: the report on the wire", "[rfab][v411][desync]")
{
    SECTION("round trip")
    {
        ClientEntityDigest sent{};
        sent.Sequence = 77;
        sent.Entries.push_back(Row(0, kFlagLive, 100));  // server id 0 is a valid id
        sent.Entries.push_back(Row(0x1A3, kFlagLive | kFlagDead, 0));
        sent.Entries.push_back(Row(0xFFFFF, kFlagOwnedByReporter));
        sent.Entries.push_back(Row(42, kFlagLive | kFlagPlayer | kFlagHasEquipment, 63, 0xDEADBEEF));

        const ClientEntityDigest received = RoundTrip(sent);
        CHECK(received == sent);
        CHECK_FALSE(received.Truncated);
    }

    SECTION("more rows than the cap: the first ones travel and the report says it is cut")
    {
        ClientEntityDigest sent{};
        for (uint32_t id = 0; id < kMaxEntries + 20; ++id)
            sent.Entries.push_back(Row(id, kFlagLive, 50));

        const ClientEntityDigest received = RoundTrip(sent);
        CHECK(received.Entries.size() == kMaxEntries);
        CHECK(received.Truncated);
    }

    SECTION("garbage in a row maps to harmless values")
    {
        TiltedPhoques::Buffer buffer(256);
        TiltedPhoques::Buffer::Writer writer(&buffer);
        writer.WriteBits(static_cast<uint8_t>(kClientEntityDigest), 8);
        Serialization::WriteVarInt(writer, 1);   // sequence
        Serialization::WriteBool(writer, false); // truncated
        Serialization::WriteVarInt(writer, 1);   // one row
        Serialization::WriteVarInt(writer, 9);   // server id
        writer.WriteBits(0xE7, 8);               // unknown flag bits set, equipment flag clear
        writer.WriteBits(200, 8);                // not a percentage

        TiltedPhoques::Buffer::Reader reader(&buffer);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        const auto& report = *static_cast<ClientEntityDigest*>(pMessage.get());
        REQUIRE(report.Entries.size() == 1);
        CHECK(report.Entries[0].ServerId == 9);
        CHECK(report.Entries[0].Flags == (0xE7 & kKnownFlags));
        CHECK(report.Entries[0].HealthPct == kNoHealth);
        CHECK(report.Entries[0].EquipmentHash == 0);
    }

    SECTION("a declared count beyond the cap never allocates or reads past it")
    {
        TiltedPhoques::Buffer buffer(64);
        TiltedPhoques::Buffer::Writer writer(&buffer);
        writer.WriteBits(static_cast<uint8_t>(kClientEntityDigest), 8);
        Serialization::WriteVarInt(writer, 1);
        Serialization::WriteBool(writer, false);
        Serialization::WriteVarInt(writer, 0xFFFFFFFFull);

        TiltedPhoques::Buffer::Reader reader(&buffer);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        const auto& report = *static_cast<ClientEntityDigest*>(pMessage.get());
        CHECK(report.Entries.size() <= kMaxEntries);
        CHECK(report.Truncated);
    }

    SECTION("the feature has its own switch")
    {
        CHECK(Session::NameOf(Session::Feature::kDesyncDetector) == "DesyncDetector");
        CHECK((Session::kAllKnownFeatureBits & Session::Bit(Session::Feature::kDesyncDetector)) != 0);
        CHECK(Session::IsEnabledIn(0u, Session::Feature::kDesyncDetector));
        CHECK_FALSE(Session::IsEnabledIn(Session::Bit(Session::Feature::kDesyncDetector), Session::Feature::kDesyncDetector));
    }
}
