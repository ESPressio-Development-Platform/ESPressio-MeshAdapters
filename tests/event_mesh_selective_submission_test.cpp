#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <ESPressio_EventMeshSelectiveSubmission.hpp>

using namespace ESPressio;

namespace {

System::DeviceIdentifier Device(std::uint8_t value) {
    System::DeviceIdentifier::Storage bytes{};
    bytes.back() = value;
    return System::DeviceIdentifier{bytes};
}

Mesh::MembershipIncarnation Incarnation(std::uint8_t value) {
    Mesh::MembershipIncarnation::Storage bytes{};
    bytes.back() = value;
    return Mesh::MembershipIncarnation{bytes};
}

Mesh::GroupIdentifier Group(std::uint8_t value) {
    Mesh::GroupIdentifier::Storage bytes{};
    bytes.back() = value;
    return Mesh::GroupIdentifier{bytes};
}

Mesh::CanonicalName Name(char value) {
    Mesh::CanonicalName result;
    const std::array<char, 2> bytes{{'N', value}};
    assert(Mesh::CanonicalName::TryCreate(bytes.data(), bytes.size(), result));
    return result;
}

Mesh::MeshNodeProfile Profile(
    char name,
    Mesh::MeshNodeAlias alias,
    Mesh::CapabilityMask capabilities,
    const Mesh::GroupIdentifier& group
) {
    Mesh::MeshNodeProfile result;
    assert(Mesh::MeshNodeProfile::TryCreate(
        Name(name), alias, capabilities, 1U, &group, 1U, result));
    return result;
}

class Contexts final : public MeshAdapters::IEventMeshSelectiveTransmissionContextProvider {
public:
    MeshAdapters::EventMeshSelectiveTransmissionContext Context{};
    Event::EventMessageId Observed{};
    bool Available{true};

    bool TryResolve(
        Event::EventMessageId messageId,
        MeshAdapters::EventMeshSelectiveTransmissionContext& context
    ) noexcept override {
        Observed = messageId;
        if (!Available) return false;
        context = Context;
        return true;
    }
};

class LocalDispatcher final : public MeshAdapters::IEventMeshLocalPacketDispatcher {
public:
    Event::EventProtocolVersion Version{0U};
    Event::EventTransportPacket Packet{};
    std::size_t Calls{0U};
    bool Accept{true};

    bool DispatchLocal(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept override {
        Version = version;
        Packet = std::move(packet);
        ++Calls;
        return Accept;
    }
};

Event::EventTransportPacket Packet(std::uint8_t value, std::uint64_t messageId) {
    Event::EventTransportBuffer bytes{value, static_cast<std::uint8_t>(value + 1U)};
    return Event::EventTransportPacket(std::move(bytes), Event::EventMessageId(messageId));
}

} // namespace

int main() {
    const auto groupA = Group(1U);
    const auto groupB = Group(2U);
    const auto device1 = Device(1U);
    const auto device2 = Device(2U);
    const auto device3 = Device(3U);
    const auto incarnation1 = Incarnation(1U);
    const auto incarnation2 = Incarnation(2U);
    const auto incarnation3 = Incarnation(3U);
    Mesh::AuthenticatedMembershipTable<3> memberships;
    assert(memberships.UpsertAuthenticated(
        device2, incarnation2, Mesh::MembershipState::Active) ==
        Mesh::AuthenticatedMembershipInsertResult::Inserted);
    assert(memberships.UpsertAuthenticated(
        device3, incarnation3, Mesh::MembershipState::Joining) ==
        Mesh::AuthenticatedMembershipInsertResult::Inserted);
    assert(memberships.UpsertAuthenticated(
        device1, incarnation1, Mesh::MembershipState::Active) ==
        Mesh::AuthenticatedMembershipInsertResult::Inserted);
    assert(memberships.ApplyAuthenticatedProfile(
        device1, incarnation1, Profile('1', 1U, 0x03U, groupA)) ==
        Mesh::AuthenticatedProfileUpdateResult::Applied);
    assert(memberships.ApplyAuthenticatedProfile(
        device2, incarnation2, Profile('2', 2U, 0x01U, groupA)) ==
        Mesh::AuthenticatedProfileUpdateResult::Applied);
    assert(memberships.ApplyAuthenticatedProfile(
        device3, incarnation3, Profile('3', 3U, 0x02U, groupA)) ==
        Mesh::AuthenticatedProfileUpdateResult::Applied);

    Mesh::MeshDestinationResolver<3, 2> resolver(memberships);
    const auto localProfile = Profile('L', 9U, 0x02U, groupA);
    Mesh::ApplicationTransmissionTable<2, 2> table;
    Mesh::DefaultMeshTrafficGovernor traffic;
    Mesh::ApplicationTransmissionCoordinator<2, 2> transmissions(table, traffic);
    Mesh::MeshMessageIdGenerator messageIds;
    Contexts contexts;
    LocalDispatcher local;
    contexts.Context = {
        MeshAdapters::EventMeshSelectorKind::Group, groupA, 0U, 100U, 500U};
    MeshAdapters::EventMeshSelectiveSubmission<2, 2, 3> submission(
        transmissions, messageIds, resolver, localProfile, contexts, local);

    auto packet = Packet(0x10U, 91U);
    const auto backing = packet.Buffer();
    auto result = submission.SubmitSelective(2U, std::move(packet));
    assert(result.Disposition == MeshAdapters::EventMeshSelectiveSubmissionDisposition::Accepted);
    assert(result.RemoteRecipients == 2U && result.LocalSelected && result.LocalAccepted);
    assert(result.Transmission && submission.Size() == 1U && messageIds.LastIssued() == 2U);
    assert(contexts.Observed == Event::EventMessageId(91U));
    assert(local.Calls == 1U && local.Version == 2U && local.Packet.Buffer() == backing);
    assert(transmissions.Payload(result.Transmission)->StableData() == backing->data());
    Mesh::ApplicationTransmissionRecipient recipient{};
    Mesh::ApplicationRecipientOutcome outcome{};
    assert(transmissions.TryGetRecipient(result.Transmission, 0U, recipient, outcome));
    assert(recipient.Device == device1 && recipient.Incarnation == incarnation1 && recipient.MessageId == 1U);
    assert(transmissions.TryGetRecipient(result.Transmission, 1U, recipient, outcome));
    assert(recipient.Device == device2 && recipient.Incarnation == incarnation2 && recipient.MessageId == 2U);
    assert(table.SetOutcome(result.Transmission, 1U, Mesh::ApplicationRecipientOutcome::Delivered) ==
        Mesh::ApplicationTransmissionUpdateResult::Updated);
    assert(table.SetOutcome(result.Transmission, 2U, Mesh::ApplicationRecipientOutcome::Delivered) ==
        Mesh::ApplicationTransmissionUpdateResult::Updated);
    assert(submission.ReleaseTerminal(result.Transmission));

    // CapabilitySelector requires all bits and retains the same local/remote independence.
    contexts.Context = {
        MeshAdapters::EventMeshSelectorKind::CapabilitySelector, {}, 0x02U, 110U, 500U};
    result = submission.SubmitSelective(2U, Packet(0x20U, 92U));
    assert(result.Disposition == MeshAdapters::EventMeshSelectiveSubmissionDisposition::Accepted);
    assert(result.RemoteRecipients == 1U && result.LocalSelected && result.LocalAccepted);
    assert(messageIds.LastIssued() == 3U && local.Calls == 2U);
    assert(transmissions.TryGetRecipient(result.Transmission, 0U, recipient, outcome));
    assert(recipient.Device == device1 && recipient.MessageId == 3U);
    assert(table.SetOutcome(result.Transmission, 3U, Mesh::ApplicationRecipientOutcome::Delivered) ==
        Mesh::ApplicationTransmissionUpdateResult::Updated);
    assert(submission.ReleaseTerminal(result.Transmission));

    // More exact recipients than the aggregate capacity rejects the whole resolution without IDs or local dispatch.
    assert(memberships.SetMembershipState(device3, incarnation3, Mesh::MembershipState::Active));
    contexts.Context = {
        MeshAdapters::EventMeshSelectorKind::Group, groupA, 0U, 120U, 500U};
    result = submission.SubmitSelective(2U, Packet(0x30U, 93U));
    assert(result.Disposition == MeshAdapters::EventMeshSelectiveSubmissionDisposition::ResolutionUnavailable);
    assert(messageIds.LastIssued() == 3U && local.Calls == 2U && submission.Size() == 0U);

    contexts.Context = {
        MeshAdapters::EventMeshSelectorKind::Group, groupB, 0U, 130U, 500U};
    result = submission.SubmitSelective(2U, Packet(0x40U, 94U));
    assert(result.Disposition == MeshAdapters::EventMeshSelectiveSubmissionDisposition::NoRecipients);
    assert(messageIds.LastIssued() == 3U && local.Calls == 2U);

    return 0;
}
