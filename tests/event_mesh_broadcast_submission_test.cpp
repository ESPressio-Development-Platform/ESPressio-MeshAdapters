#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <ESPressio_EventMeshBroadcastSubmission.hpp>

using namespace ESPressio;

namespace {

class Contexts final : public MeshAdapters::IEventMeshBroadcastTransmissionContextProvider {
public:
    Event::EventMessageId Observed{};
    bool Available{true};

    bool TryResolve(
        Event::EventMessageId messageId,
        MeshAdapters::EventMeshBroadcastTransmissionContext& context
    ) noexcept override {
        Observed = messageId;
        if (!Available) return false;
        context = {100U, 500U, 4U};
        return true;
    }
};

struct Plan final {};

class Broadcast final {
public:
    Mesh::ApplicationPrimitiveDescriptor Primitive{};
    const std::uint8_t* Payload{nullptr};
    std::size_t PayloadBytes{0U};
    std::uint64_t Now{0U};
    std::uint64_t Deadline{0U};
    Mesh::RemainingHopLimit HopLimit{0U};
    const Plan* ObservedPlan{nullptr};
    Mesh::MeshV1BroadcastDisposition Next{Mesh::MeshV1BroadcastDisposition::Completed};

    Mesh::MeshV1BroadcastResult Submit(
        Mesh::ApplicationPrimitiveDescriptor primitive,
        const Mesh::ApplicationPayload& payload,
        std::uint64_t now,
        std::uint64_t deadline,
        Mesh::RemainingHopLimit hopLimit,
        const Plan& plan
    ) noexcept {
        Primitive = primitive;
        Payload = payload.StableData();
        PayloadBytes = payload.Size();
        Now = now;
        Deadline = deadline;
        HopLimit = hopLimit;
        ObservedPlan = &plan;
        return {Next, 77U};
    }
};

} // namespace

int main() {
    Broadcast broadcast;
    Plan plan;
    Contexts contexts;
    MeshAdapters::EventMeshBroadcastSubmission<Broadcast, Plan> submission(
        broadcast, plan, contexts);

    Event::EventTransportBuffer bytes{0x11U, 0x22U, 0x33U};
    Event::EventTransportPacket packet(std::move(bytes), Event::EventMessageId(91U));
    const auto backing = packet.Buffer();
    assert(submission.Submit(2U, std::move(packet)));
    assert(contexts.Observed == Event::EventMessageId(91U));
    assert(broadcast.Primitive.Family == Primitive::FamilyIds::Event &&
           broadcast.Primitive.Version == 2U);
    assert(broadcast.Payload == backing->data() && broadcast.PayloadBytes == backing->size());
    assert(broadcast.Now == 100U && broadcast.Deadline == 500U && broadcast.HopLimit == 4U);
    assert(broadcast.ObservedPlan == &plan);
    assert(submission.LastResult().Disposition == Mesh::MeshV1BroadcastDisposition::Completed);
    assert(submission.LastResult().MessageId == 77U);

    broadcast.Next = Mesh::MeshV1BroadcastDisposition::ResourceUnavailable;
    assert(!submission.Submit(2U, Event::EventTransportPacket(
        Event::EventTransportBuffer{0x44U}, Event::EventMessageId(92U))));
    assert(submission.LastResult().Disposition == Mesh::MeshV1BroadcastDisposition::ResourceUnavailable);

    contexts.Available = false;
    assert(!submission.Submit(2U, Event::EventTransportPacket(
        Event::EventTransportBuffer{0x55U}, Event::EventMessageId(93U))));
    assert(submission.LastResult().Disposition == Mesh::MeshV1BroadcastDisposition::Invalid);

    return 0;
}
