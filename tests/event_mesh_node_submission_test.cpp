#include <cassert>
#include <cstdint>
#include <utility>

#include <ESPressio_EventMeshNodeSubmission.hpp>

using namespace ESPressio;

namespace {

System::DeviceIdentifier Device(std::uint8_t value) {
    System::DeviceIdentifier::Storage bytes{};
    bytes[15] = value;
    return System::DeviceIdentifier(bytes);
}

Mesh::MembershipIncarnation Incarnation(std::uint8_t value) {
    Mesh::MembershipIncarnation::Storage bytes{};
    bytes[15] = value;
    return Mesh::MembershipIncarnation(bytes);
}


class Contexts final : public MeshAdapters::IEventMeshNodeTransmissionContextProvider {
public:
    bool Available{true};
    Event::EventMessageId Observed{};

    bool TryResolve(
        Event::EventMessageId messageId,
        MeshAdapters::EventMeshNodeTransmissionContext& context
    ) noexcept override {
        Observed = messageId;
        if (!Available) return false;
        context = {Device(7), Incarnation(17), 100, 500};
        return true;
    }
};

} // namespace

int main() {
    Mesh::ApplicationTransmissionTable<2, 1> table;
    Mesh::DefaultMeshTrafficGovernor traffic;
    Mesh::ApplicationTransmissionCoordinator<2, 1> transmissions(table, traffic);
    Mesh::MeshMessageIdGenerator meshMessageIds;
    Contexts contexts;
    MeshAdapters::EventMeshNodeSubmission<2, 1> submission(
        transmissions, meshMessageIds, contexts
    );

    Event::EventTransportBuffer bytes{0x10, 0x20, 0x30};
    Event::EventTransportPacket packet(std::move(bytes), Event::EventMessageId(91));
    const auto backing = packet.Buffer();
    assert(submission.Submit(2, std::move(packet)));
    assert(contexts.Observed == Event::EventMessageId(91));
    assert(submission.Size() == 1U);
    assert(meshMessageIds.LastIssued() == 1U);
    assert(traffic.Active(Mesh::MeshTrafficClass::Application) == 1U);

    Mesh::ApplicationTransmissionHandle handle;
    std::size_t visits = 0;
    submission.ForEachActive([&](Mesh::ApplicationTransmissionHandle current) {
        handle = current;
        ++visits;
    });
    assert(visits == 1U && handle);
    const auto* primitive = transmissions.PrimitiveDescriptor(handle);
    assert(primitive != nullptr && primitive->Family == Primitive::FamilyIds::Event);
    assert(primitive->Version == 2U);
    const auto* payload = transmissions.Payload(handle);
    assert(payload != nullptr && payload->StableData() == backing->data());

    assert(table.SetOutcome(handle, 1, Mesh::ApplicationRecipientOutcome::Delivered) ==
           Mesh::ApplicationTransmissionUpdateResult::Updated);
    assert(submission.ReleaseTerminal(handle));
    assert(submission.Size() == 0U);
    assert(traffic.Active(Mesh::MeshTrafficClass::Application) == 0U);

    Event::EventTransportBuffer invalidBytes{0x40};
    Event::EventTransportPacket invalid(std::move(invalidBytes));
    assert(!submission.Submit(2, std::move(invalid)));
    assert(meshMessageIds.LastIssued() == 1U);

    contexts.Available = false;
    Event::EventTransportBuffer rejectedBytes{0x50};
    Event::EventTransportPacket rejected(
        std::move(rejectedBytes), Event::EventMessageId(92)
    );
    assert(!submission.Submit(2, std::move(rejected)));
    assert(meshMessageIds.LastIssued() == 1U);
    return 0;
}
