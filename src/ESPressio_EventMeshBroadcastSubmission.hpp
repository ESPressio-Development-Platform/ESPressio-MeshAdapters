#pragma once

#include <cstdint>
#include <utility>

#include <ESPressio_ApplicationPayload.hpp>
#include <ESPressio_ApplicationTransmissionTable.hpp>
#include <ESPressio_MeshV1BroadcastCoordinator.hpp>

#include "ESPressio_EventMeshTransport.hpp"

namespace ESPressio::MeshAdapters {

/**
 * ESPressio Memory Audit
 * Members:
 * - NowMilliseconds (std::uint64_t): 8 bytes [0 bytes dynamic allocation]
 * - AbsoluteDeadlineMilliseconds (std::uint64_t): 8 bytes [0 bytes dynamic allocation]
 * - HopLimit (Mesh::RemainingHopLimit): 1 bytes [0 bytes dynamic allocation]
 * Total Memory: 20 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
struct EventMeshBroadcastTransmissionContext final {
    std::uint64_t NowMilliseconds{0U};
    std::uint64_t AbsoluteDeadlineMilliseconds{0U};
    Mesh::RemainingHopLimit HopLimit{0U};

    constexpr bool IsValid() const noexcept {
        return AbsoluteDeadlineMilliseconds != 0U && NowMilliseconds < AbsoluteDeadlineMilliseconds &&
               HopLimit != 0U;
    }
};

/**
 * ESPressio Memory Audit
 * Members: none; polymorphic/virtual-base object metadata is included in the total.
 * Total Memory: 4 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
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
/**
 * ESPressio Memory Audit
 * Inherited Memory Total: 4 bytes [0 bytes dynamic allocation]
 * Members:
 * - _broadcast (TBroadcastCoordinator&): 4 bytes [0 bytes dynamic allocation]
 * - _plan (TFanoutPlan&): 4 bytes [0 bytes dynamic allocation]
 * - _contexts (IEventMeshBroadcastTransmissionContextProvider&): 4 bytes [0 bytes dynamic allocation]
 * - _last (Mesh::MeshV1BroadcastResult): 16 bytes [0 bytes dynamic allocation]
 * Total Memory: 32 bytes [0 bytes dynamic allocation]
 * Basis: ESP32/Xtensa ILP32 reference ABI (4-byte pointers/size_t); ESPressio stateful allocators/deleters included; ABI-sensitive STL/platform internals are identified explicitly.
 * End ESPressio Memory Audit
 */
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
