#pragma once

#include <cstdint>
#include <utility>

#include <ESPressio_EventTypes.hpp>
#include <ESPressio_IEventTransport.hpp>
#include <ESPressio_PrimitiveReceiverRegistry.hpp>

namespace ESPressio::MeshAdapters {

/// <summary>Result of transferring borrowed Mesh receive bytes into bounded Event-owned packet storage.</summary>
enum class EventMeshInboundOwnershipResult : std::uint8_t {
    Owned,
    Malformed,
    ResourceUnavailable
};

/// <summary>
/// Injected bounded ownership boundary for one inbound Event-family Mesh payload.
/// </summary>
/// <remarks>
/// Mesh delivers borrowed bytes synchronously, while Event may process the packet asynchronously. The implementation
/// therefore owns the concrete byte capacity and storage policy; MeshAdapters defines no implicit payload limit.
/// </remarks>
class IEventMeshInboundPacketOwner {
public:
    virtual ~IEventMeshInboundPacketOwner() = default;

    virtual EventMeshInboundOwnershipResult TryOwn(
        Mesh::PrimitivePayloadView payload,
        Event::EventTransportPacket& packet
    ) noexcept = 0;
};

/// <summary>Injected outbound Mesh submission path for an ownership-bearing serialized Event packet.</summary>
/// <remarks>
/// The implementation chooses an already-authorized Node or selective destination, deadline, MeshMessageId issuance and
/// routing composition. Acceptance transfers shared immutable packet ownership; no Event bytes need to be copied per
/// Mesh recipient.
/// </remarks>
class IEventMeshOutboundSubmission {
public:
    virtual ~IEventMeshOutboundSubmission() = default;

    virtual bool Submit(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept = 0;
};

/// <summary>
/// Bidirectional Event transport adapter at Mesh's external primitive-family receiver boundary.
/// </summary>
/// <remarks>
/// The adapter owns neither destination selection nor packet storage capacity. Inbound authenticated Mesh provenance is
/// automatically handed to the registered Event transport receiver. In normal composition that receiver is
/// EventTransportManager, which deserializes the registered Event type and submits it to the local EventManager with
/// EventOrigin::Remote; applications do not wire a remote-to-local handoff per Event type. Type registration remains
/// necessary to define identity, schema, serialization and direction. Concrete Mesh framing and security remain owned by
/// Mesh composition, and authenticated Mesh provenance is deliberately not written into Event object state.
/// </remarks>
class EventMeshTransport final :
    public Event::IEventTransport,
    public Mesh::IPrimitiveReceiver {
    Mesh::PrimitiveReceiverDescriptor _descriptor{};
    Event::EventProtocolVersion _outboundVersion{0};
    IEventMeshInboundPacketOwner& _inboundOwner;
    IEventMeshOutboundSubmission& _outbound;
    Event::IEventTransportReceiver* _receiver{nullptr};

public:
    EventMeshTransport(
        Primitive::PrimitiveProtocolVersionRange supportedVersions,
        Primitive::ContractFingerprint fingerprint,
        Event::EventProtocolVersion outboundVersion,
        IEventMeshInboundPacketOwner& inboundOwner,
        IEventMeshOutboundSubmission& outbound,
        Mesh::PrimitiveReceiverExposure exposure = Mesh::PrimitiveReceiverExposure::Advertised
    ) noexcept :
        _descriptor{Primitive::FamilyIds::Event, supportedVersions, fingerprint, exposure},
        _outboundVersion(outboundVersion),
        _inboundOwner(inboundOwner),
        _outbound(outbound) {}

    /// <summary>Reports whether the configured Event family/version contract is internally consistent.</summary>
    constexpr bool IsValid() const noexcept {
        return _descriptor.IsValid() && _descriptor.Family == Primitive::FamilyIds::Event &&
               _descriptor.Versions.Contains(_outboundVersion);
    }

    /// <summary>Gets the descriptor to register with Mesh's bounded PrimitiveReceiverRegistry.</summary>
    constexpr const Mesh::PrimitiveReceiverDescriptor& Descriptor() const noexcept {
        return _descriptor;
    }

    bool Send(Event::EventTransportPacket packet) override {
        if (!IsValid() || !packet) return false;
        return _outbound.Submit(_outboundVersion, std::move(packet));
    }

    void SetReceiver(Event::IEventTransportReceiver* receiver) override {
        _receiver = receiver;
    }

    Mesh::PrimitiveReceiveDisposition Receive(
        const Mesh::MeshReceiveContext& context,
        Primitive::PrimitiveProtocolVersion version,
        Mesh::PrimitivePayloadView payload
    ) noexcept override {
        if (!IsValid() || !context.IsValid() || !payload.IsValid()) {
            return Mesh::PrimitiveReceiveDisposition::Malformed;
        }
        if (!_descriptor.Versions.Contains(version)) {
            return Mesh::PrimitiveReceiveDisposition::UnsupportedVersion;
        }
        if (_receiver == nullptr) {
            return Mesh::PrimitiveReceiveDisposition::TemporarilyUnavailable;
        }

        Event::EventTransportPacket packet;
        switch (_inboundOwner.TryOwn(payload, packet)) {
            case EventMeshInboundOwnershipResult::Malformed:
                return Mesh::PrimitiveReceiveDisposition::Malformed;
            case EventMeshInboundOwnershipResult::ResourceUnavailable:
                return Mesh::PrimitiveReceiveDisposition::ResourceUnavailable;
            case EventMeshInboundOwnershipResult::Owned:
                break;
        }
        if (!packet) return Mesh::PrimitiveReceiveDisposition::Malformed;

        _receiver->ReceiveEventTransportPacket(this, std::move(packet));
        return Mesh::PrimitiveReceiveDisposition::Accepted;
    }
};

} // namespace ESPressio::MeshAdapters
