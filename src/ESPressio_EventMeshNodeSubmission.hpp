#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <ESPressio_ApplicationTransmissionCoordinator.hpp>
#include <ESPressio_MeshMessageIdGenerator.hpp>

#include "ESPressio_EventMeshTransport.hpp"

namespace ESPressio::MeshAdapters {

/// <summary>Resolved sender-local Node delivery context for one outbound Event occurrence.</summary>
struct EventMeshNodeTransmissionContext final {
    System::DeviceIdentifier Destination{};
    Mesh::MembershipIncarnation DestinationIncarnation{};
    std::uint64_t NowMilliseconds{0};
    std::uint64_t AbsoluteDeadlineMilliseconds{0};

    constexpr bool IsValid() const noexcept {
        return static_cast<bool>(Destination) && static_cast<bool>(DestinationIncarnation) &&
               AbsoluteDeadlineMilliseconds != 0U && NowMilliseconds < AbsoluteDeadlineMilliseconds;
    }
};

/// <summary>Composition-owned resolver for the Node target and immutable delivery deadline.</summary>
class IEventMeshNodeTransmissionContextProvider {
public:
    virtual ~IEventMeshNodeTransmissionContextProvider() = default;
    virtual bool TryResolve(
        Event::EventMessageId messageId,
        EventMeshNodeTransmissionContext& context
    ) noexcept = 0;
};

/// <summary>
/// Bounded zero-copy bridge from one Event transport route into Mesh application aggregate admission.
/// </summary>
/// <remarks>
/// Calls are serialized by the owning Event transport execution path. One retained shared Event packet backs one
/// single-recipient Mesh aggregate until the aggregate is terminal and ReleaseTerminal succeeds. MeshMessageId is fresh
/// per Node delivery while the Event packet's ConceptualMessageId remains unchanged. This coordinator does not route,
/// frame, transmit or infer Group/Broadcast semantics.
/// </remarks>
template<std::size_t TransmissionCapacity = Mesh::Limits::MaxActiveApplicationTransmissions,
         std::size_t RecipientCapacity = Mesh::Limits::MaxRecipientsPerTransmission>
class EventMeshNodeSubmission final : public IEventMeshOutboundSubmission {
    struct Record final {
        bool Used{false};
        Mesh::ApplicationTransmissionHandle Transmission{};
        Event::EventTransportPacket Packet{};
    };

    Mesh::ApplicationTransmissionCoordinator<TransmissionCapacity, RecipientCapacity>& _transmissions;
    Mesh::MeshMessageIdGenerator& _messageIds;
    IEventMeshNodeTransmissionContextProvider& _contexts;
    std::array<Record, TransmissionCapacity> _records{};

    Record* Find(Mesh::ApplicationTransmissionHandle handle) noexcept {
        for (auto& record : _records) {
            if (record.Used && record.Transmission == handle) return &record;
        }
        return nullptr;
    }

public:
    EventMeshNodeSubmission(
        Mesh::ApplicationTransmissionCoordinator<TransmissionCapacity, RecipientCapacity>& transmissions,
        Mesh::MeshMessageIdGenerator& messageIds,
        IEventMeshNodeTransmissionContextProvider& contexts
    ) noexcept : _transmissions(transmissions), _messageIds(messageIds), _contexts(contexts) {}

    bool Submit(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept override {
        if (!packet || !packet.MessageID()) return false;

        EventMeshNodeTransmissionContext context;
        if (!_contexts.TryResolve(packet.MessageID(), context) || !context.IsValid()) return false;

        Record* available = nullptr;
        for (auto& record : _records) {
            if (!record.Used) {
                available = &record;
                break;
            }
        }
        if (available == nullptr) return false;

        Mesh::MeshMessageId meshMessageId = 0;
        if (!_messageIds.TryIssue(meshMessageId)) return false;

        available->Used = true;
        available->Packet = std::move(packet);
        const Mesh::ApplicationTransmissionRecipient recipient{
            context.Destination,
            context.DestinationIncarnation,
            meshMessageId
        };
        const Mesh::ApplicationPrimitiveDescriptor primitive{
            Primitive::FamilyIds::Event,
            version
        };
        const auto payload = Mesh::ApplicationPayload::Borrowed(
            available->Packet.Data(), available->Packet.Size()
        );

        Mesh::ApplicationTransmissionHandle handle;
        const auto result = _transmissions.Begin(
            &recipient, 1U, primitive, payload,
            context.NowMilliseconds, context.AbsoluteDeadlineMilliseconds,
            handle
        );
        if (result != Mesh::ApplicationTransmissionAdmissionResult::Begun) {
            *available = {};
            return false;
        }
        available->Transmission = handle;
        return true;
    }

    /// <summary>Enumerates admitted aggregates synchronously for composition-owned route/delivery startup.</summary>
    template<typename TVisitor>
    void ForEachActive(TVisitor&& visitor) const {
        for (const auto& record : _records) {
            if (record.Used) visitor(record.Transmission);
        }
    }

    /// <summary>Releases a terminal Mesh aggregate and its final shared Event packet reference.</summary>
    bool ReleaseTerminal(Mesh::ApplicationTransmissionHandle handle) noexcept {
        auto* record = Find(handle);
        if (record == nullptr || !_transmissions.Release(handle)) return false;
        *record = {};
        return true;
    }

    /// <summary>
    /// Releases retained packet references after composition has completed controlled aggregate teardown.
    /// </summary>
    void ResetOwnedPacketsAfterControlledMeshTeardown() noexcept {
        for (auto& record : _records) record = {};
    }

    std::size_t Size() const noexcept {
        std::size_t count = 0;
        for (const auto& record : _records) if (record.Used) ++count;
        return count;
    }
};

} // namespace ESPressio::MeshAdapters
