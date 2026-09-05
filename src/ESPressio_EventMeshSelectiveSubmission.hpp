#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <ESPressio_ApplicationTransmissionCoordinator.hpp>
#include <ESPressio_MeshDestinationResolver.hpp>
#include <ESPressio_MeshMessageIdGenerator.hpp>

#include "ESPressio_EventMeshTransport.hpp"

namespace ESPressio::MeshAdapters {

enum class EventMeshSelectorKind : std::uint8_t {
    Group,
    CapabilitySelector
};

struct EventMeshSelectiveTransmissionContext final {
    EventMeshSelectorKind Kind{EventMeshSelectorKind::Group};
    Mesh::GroupIdentifier Group{};
    Mesh::CapabilityMask RequiredCapabilities{0U};
    std::uint64_t NowMilliseconds{0U};
    std::uint64_t AbsoluteDeadlineMilliseconds{0U};

    constexpr bool IsValid() const noexcept {
        const bool selectorValid = Kind == EventMeshSelectorKind::Group
            ? static_cast<bool>(Group)
            : Kind == EventMeshSelectorKind::CapabilitySelector && RequiredCapabilities != 0U;
        return selectorValid && AbsoluteDeadlineMilliseconds != 0U &&
               NowMilliseconds < AbsoluteDeadlineMilliseconds;
    }
};

class IEventMeshSelectiveTransmissionContextProvider {
public:
    virtual ~IEventMeshSelectiveTransmissionContextProvider() = default;
    virtual bool TryResolve(
        Event::EventMessageId messageId,
        EventMeshSelectiveTransmissionContext& context
    ) noexcept = 0;
};

/// <summary>Composition-owned local Event dispatch for a selector which includes the sending node.</summary>
class IEventMeshLocalPacketDispatcher {
public:
    virtual ~IEventMeshLocalPacketDispatcher() = default;
    virtual bool DispatchLocal(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept = 0;
};

enum class EventMeshSelectiveSubmissionDisposition : std::uint8_t {
    Accepted,
    AcceptedWithLocalRejection,
    LocalRejected,
    NoRecipients,
    ResolutionUnavailable,
    ResourceUnavailable,
    MessageIdExhausted,
    Invalid
};

struct EventMeshSelectiveSubmissionResult final {
    EventMeshSelectiveSubmissionDisposition Disposition{
        EventMeshSelectiveSubmissionDisposition::Invalid};
    Mesh::ApplicationTransmissionHandle Transmission{};
    std::size_t RemoteRecipients{0U};
    bool LocalSelected{false};
    bool LocalAccepted{false};

    constexpr explicit operator bool() const noexcept {
        return Disposition == EventMeshSelectiveSubmissionDisposition::Accepted ||
               Disposition == EventMeshSelectiveSubmissionDisposition::AcceptedWithLocalRejection;
    }
};

/// <summary>Bounded Event Group/Capability bridge into one frozen multi-recipient Mesh aggregate.</summary>
/// <remarks>
/// Remote membership is resolved exactly once. Each exact recipient receives a fresh independent MeshMessageId while
/// every delivery shares the packet's immutable backing and deadline. A matching local profile is dispatched through
/// an injected local Event boundary and never fabricated as an authenticated remote Node delivery. Remote aggregate
/// admission happens before local dispatch, so capacity failure is all-or-nothing. Once admitted, local rejection is an
/// independent outcome and cannot roll back already accepted remote deliveries.
/// </remarks>
template<std::size_t TransmissionCapacity = Mesh::Limits::MaxActiveApplicationTransmissions,
         std::size_t RecipientCapacity = Mesh::Limits::MaxRecipientsPerTransmission,
         std::size_t MembershipCapacity = Mesh::Limits::MaxMeshNodes>
class EventMeshSelectiveSubmission final : public IEventMeshOutboundSubmission {
    struct Record final {
        bool Used{false};
        Mesh::ApplicationTransmissionHandle Transmission{};
        Event::EventTransportPacket Packet{};
    };

    Mesh::ApplicationTransmissionCoordinator<TransmissionCapacity, RecipientCapacity>& _transmissions;
    Mesh::MeshMessageIdGenerator& _messageIds;
    const Mesh::MeshDestinationResolver<MembershipCapacity, RecipientCapacity>& _resolver;
    const Mesh::MeshNodeProfile& _localProfile;
    IEventMeshSelectiveTransmissionContextProvider& _contexts;
    IEventMeshLocalPacketDispatcher& _localDispatcher;
    std::array<Record, TransmissionCapacity> _records{};

    Record* Find(Mesh::ApplicationTransmissionHandle handle) noexcept {
        for (auto& record : _records) {
            if (record.Used && record.Transmission == handle) return &record;
        }
        return nullptr;
    }

    bool LocalSelected(const EventMeshSelectiveTransmissionContext& context) const noexcept {
        if (!_localProfile) return false;
        return context.Kind == EventMeshSelectorKind::Group
            ? _localProfile.HasGroup(context.Group)
            : _localProfile.SupportsAll(context.RequiredCapabilities);
    }

    Mesh::MeshDestinationResolutionDisposition Resolve(
        const EventMeshSelectiveTransmissionContext& context,
        Mesh::FrozenMeshRecipientSet<RecipientCapacity>& recipients
    ) const noexcept {
        return context.Kind == EventMeshSelectorKind::Group
            ? _resolver.ResolveGroup(context.Group, recipients)
            : _resolver.ResolveCapabilitySelector(context.RequiredCapabilities, recipients);
    }

public:
    EventMeshSelectiveSubmission(
        Mesh::ApplicationTransmissionCoordinator<TransmissionCapacity, RecipientCapacity>& transmissions,
        Mesh::MeshMessageIdGenerator& messageIds,
        const Mesh::MeshDestinationResolver<MembershipCapacity, RecipientCapacity>& resolver,
        const Mesh::MeshNodeProfile& localProfile,
        IEventMeshSelectiveTransmissionContextProvider& contexts,
        IEventMeshLocalPacketDispatcher& localDispatcher
    ) noexcept :
        _transmissions(transmissions), _messageIds(messageIds), _resolver(resolver),
        _localProfile(localProfile), _contexts(contexts), _localDispatcher(localDispatcher) {}

    EventMeshSelectiveSubmissionResult SubmitSelective(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept {
        EventMeshSelectiveSubmissionResult result{};
        if (!packet || !packet.MessageID()) return result;
        EventMeshSelectiveTransmissionContext context{};
        if (!_contexts.TryResolve(packet.MessageID(), context) || !context.IsValid()) return result;

        Mesh::FrozenMeshRecipientSet<RecipientCapacity> frozen;
        const auto resolution = Resolve(context, frozen);
        if (resolution == Mesh::MeshDestinationResolutionDisposition::Invalid) return result;
        if (resolution == Mesh::MeshDestinationResolutionDisposition::ResourceUnavailable) {
            result.Disposition = EventMeshSelectiveSubmissionDisposition::ResolutionUnavailable;
            return result;
        }
        result.LocalSelected = LocalSelected(context);
        result.RemoteRecipients = frozen.Size();
        if (frozen.Empty() && !result.LocalSelected) {
            result.Disposition = EventMeshSelectiveSubmissionDisposition::NoRecipients;
            return result;
        }

        Record* available = nullptr;
        if (!frozen.Empty()) {
            for (auto& record : _records) {
                if (!record.Used) {
                    available = &record;
                    break;
                }
            }
            if (available == nullptr) {
                result.Disposition = EventMeshSelectiveSubmissionDisposition::ResourceUnavailable;
                return result;
            }
        }

        std::array<Mesh::ApplicationTransmissionRecipient, RecipientCapacity> recipients{};
        for (std::size_t index = 0U; index < frozen.Size(); ++index) {
            Mesh::MeshMessageId messageId{0U};
            if (!_messageIds.TryIssue(messageId)) {
                result.Disposition = EventMeshSelectiveSubmissionDisposition::MessageIdExhausted;
                return result;
            }
            const auto* recipient = frozen.At(index);
            if (recipient == nullptr) return result;
            recipients[index] = {recipient->Device, recipient->Incarnation, messageId};
        }

        const auto localPacket = packet;
        if (!frozen.Empty()) {
            available->Used = true;
            available->Packet = std::move(packet);
            const Mesh::ApplicationPrimitiveDescriptor primitive{Primitive::FamilyIds::Event, version};
            const auto payload = Mesh::ApplicationPayload::Borrowed(
                available->Packet.Data(), available->Packet.Size());
            const auto admission = _transmissions.Begin(
                recipients.data(), frozen.Size(), primitive, payload,
                context.NowMilliseconds, context.AbsoluteDeadlineMilliseconds, result.Transmission);
            if (admission != Mesh::ApplicationTransmissionAdmissionResult::Begun) {
                *available = {};
                result.Transmission = {};
                result.Disposition = EventMeshSelectiveSubmissionDisposition::ResourceUnavailable;
                return result;
            }
            available->Transmission = result.Transmission;
        }

        if (result.LocalSelected) {
            result.LocalAccepted = _localDispatcher.DispatchLocal(version, localPacket);
            if (!result.LocalAccepted) {
                result.Disposition = frozen.Empty()
                    ? EventMeshSelectiveSubmissionDisposition::LocalRejected
                    : EventMeshSelectiveSubmissionDisposition::AcceptedWithLocalRejection;
                return result;
            }
        }
        result.Disposition = EventMeshSelectiveSubmissionDisposition::Accepted;
        return result;
    }

    bool Submit(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept override {
        return static_cast<bool>(SubmitSelective(version, std::move(packet)));
    }

    template<typename TVisitor>
    void ForEachActive(TVisitor&& visitor) const {
        for (const auto& record : _records) if (record.Used) visitor(record.Transmission);
    }

    bool ReleaseTerminal(Mesh::ApplicationTransmissionHandle handle) noexcept {
        auto* record = Find(handle);
        if (record == nullptr || !_transmissions.Release(handle)) return false;
        *record = {};
        return true;
    }

    void ResetOwnedPacketsAfterControlledMeshTeardown() noexcept {
        for (auto& record : _records) record = {};
    }

    std::size_t Size() const noexcept {
        std::size_t count = 0U;
        for (const auto& record : _records) if (record.Used) ++count;
        return count;
    }
};

} // namespace ESPressio::MeshAdapters
