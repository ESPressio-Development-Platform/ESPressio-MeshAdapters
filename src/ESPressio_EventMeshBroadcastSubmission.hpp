#pragma once

#include <cstdint>
#include <utility>

#include <ESPressio_ApplicationPayload.hpp>
#include <ESPressio_ApplicationTransmissionTable.hpp>
#include <ESPressio_MeshV1BroadcastCoordinator.hpp>

#include "ESPressio_EventMeshTransport.hpp"

namespace ESPressio::MeshAdapters {

struct EventMeshBroadcastTransmissionContext final {
    std::uint64_t NowMilliseconds{0U};
    std::uint64_t AbsoluteDeadlineMilliseconds{0U};
    Mesh::RemainingHopLimit HopLimit{0U};

    constexpr bool IsValid() const noexcept {
        return AbsoluteDeadlineMilliseconds != 0U && NowMilliseconds < AbsoluteDeadlineMilliseconds &&
               HopLimit != 0U;
    }
};

class IEventMeshBroadcastTransmissionContextProvider {
public:
    virtual ~IEventMeshBroadcastTransmissionContextProvider() = default;
    virtual bool TryResolve(
        Event::EventMessageId messageId,
        EventMeshBroadcastTransmissionContext& context
    ) noexcept = 0;
};

/// <summary>Synchronous Event-family bridge into a configured Mesh v1 best-effort Broadcast coordinator.</summary>
/// <remarks>
/// The Event packet already owns immutable shared bytes. Mesh copies those bytes into its explicit frame workspace before
/// this call returns, so the adapter retains no second packet or hidden byte capacity. Deadline/hop policy and the
/// bounded one-binding-per-neighbour plan are composition-owned. Broadcast success means processing/fan-out attempts
/// completed; it does not promise any recipient delivery and creates no Event or Mesh acknowledgement state. EventManager
/// has already dispatched an originating Event locally, so this adapter explicitly suppresses Mesh's optional origin-side
/// primitive dispatch. Authenticated remote Event payloads still enter EventTransportManager through EventMeshTransport.
/// </remarks>
template<typename TBroadcastCoordinator, typename TFanoutPlan>
class EventMeshBroadcastSubmission final : public IEventMeshOutboundSubmission {
    TBroadcastCoordinator& _broadcast;
    const TFanoutPlan& _plan;
    IEventMeshBroadcastTransmissionContextProvider& _contexts;
    Mesh::MeshV1BroadcastResult _last{};

public:
    EventMeshBroadcastSubmission(
        TBroadcastCoordinator& broadcast,
        const TFanoutPlan& plan,
        IEventMeshBroadcastTransmissionContextProvider& contexts
    ) noexcept : _broadcast(broadcast), _plan(plan), _contexts(contexts) {}

    bool Submit(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept override {
        _last = {};
        if (!packet || !packet.MessageID()) return false;
        EventMeshBroadcastTransmissionContext context{};
        if (!_contexts.TryResolve(packet.MessageID(), context) || !context.IsValid()) return false;
        _last = _broadcast.Submit(
            {Primitive::FamilyIds::Event, version},
            Mesh::ApplicationPayload::Borrowed(packet.Data(), packet.Size()),
            context.NowMilliseconds, context.AbsoluteDeadlineMilliseconds,
            context.HopLimit, _plan, Mesh::MeshBroadcastLocalDispatch::Exclude);
        return _last.Disposition == Mesh::MeshV1BroadcastDisposition::Completed;
    }

    constexpr const Mesh::MeshV1BroadcastResult& LastResult() const noexcept { return _last; }
};

} // namespace ESPressio::MeshAdapters
