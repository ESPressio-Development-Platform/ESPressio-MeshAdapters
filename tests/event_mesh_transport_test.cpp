#include <cassert>
#include <cstdint>
#include <utility>

#include <ESPressio_EventMeshTransport.hpp>

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


class PacketOwner final : public MeshAdapters::IEventMeshInboundPacketOwner {
public:
    MeshAdapters::EventMeshInboundOwnershipResult Next{
        MeshAdapters::EventMeshInboundOwnershipResult::Owned
    };

    MeshAdapters::EventMeshInboundOwnershipResult TryOwn(
        Mesh::PrimitivePayloadView payload,
        Event::EventTransportPacket& packet
    ) noexcept override {
        if (Next != MeshAdapters::EventMeshInboundOwnershipResult::Owned) return Next;
        Event::EventTransportBuffer bytes;
        bytes.insert(bytes.end(), payload.Data, payload.Data + payload.Size);
        packet = Event::EventTransportPacket(std::move(bytes));
        return MeshAdapters::EventMeshInboundOwnershipResult::Owned;
    }
};


class Outbound final : public MeshAdapters::IEventMeshOutboundSubmission {
public:
    Event::EventProtocolVersion Version{0};
    Event::EventTransportPacket Packet{};
    bool Accept{true};

    bool Submit(
        Event::EventProtocolVersion version,
        Event::EventTransportPacket packet
    ) noexcept override {
        Version = version;
        Packet = std::move(packet);
        return Accept;
    }
};


class Receiver final : public Event::IEventTransportReceiver {
public:
    Event::IEventTransport* Transport{nullptr};
    Event::EventTransportPacket Packet{};

    void ReceiveEventTransportPacket(
        Event::IEventTransport* transport,
        Event::EventTransportPacket packet
    ) override {
        Transport = transport;
        Packet = std::move(packet);
    }
};

} // namespace

int main() {
    PacketOwner owner;
    Outbound outbound;
    Primitive::ContractFingerprint fingerprint;
    MeshAdapters::EventMeshTransport transport(
        {1, 2}, fingerprint, 2, owner, outbound
    );

    assert(transport.IsValid());
    assert(transport.Descriptor().Family == Primitive::FamilyIds::Event);
    assert(transport.Descriptor().Versions.Minimum == 1U);
    assert(transport.Descriptor().Versions.Maximum == 2U);

    Event::EventTransportBuffer outboundBytes{0x11, 0x22};
    Event::EventTransportPacket outboundPacket(
        std::move(outboundBytes), Event::EventMessageId(71)
    );
    const auto outboundBacking = outboundPacket.Buffer();
    assert(transport.Send(std::move(outboundPacket)));
    assert(outbound.Version == 2U);
    assert(outbound.Packet.Buffer() == outboundBacking);
    assert(outbound.Packet.MessageID() == Event::EventMessageId(71));

    const std::uint8_t inboundBytes[] = {0x33, 0x44, 0x55};
    const Mesh::MeshReceiveContext context{Device(9), Incarnation(19), 81, 4, false};
    const Mesh::PrimitivePayloadView payload{inboundBytes, sizeof(inboundBytes)};

    assert(transport.Receive(context, 2, payload) ==
           Mesh::PrimitiveReceiveDisposition::TemporarilyUnavailable);

    Receiver receiver;
    transport.SetReceiver(&receiver);
    assert(transport.Receive(context, 3, payload) ==
           Mesh::PrimitiveReceiveDisposition::UnsupportedVersion);
    assert(transport.Receive(context, 2, payload) == Mesh::PrimitiveReceiveDisposition::Accepted);
    assert(receiver.Transport == &transport);
    assert(receiver.Packet.Size() == sizeof(inboundBytes));
    assert(receiver.Packet.Data()[0] == 0x33);

    owner.Next = MeshAdapters::EventMeshInboundOwnershipResult::ResourceUnavailable;
    assert(transport.Receive(context, 2, payload) ==
           Mesh::PrimitiveReceiveDisposition::ResourceUnavailable);
    owner.Next = MeshAdapters::EventMeshInboundOwnershipResult::Malformed;
    assert(transport.Receive(context, 2, payload) == Mesh::PrimitiveReceiveDisposition::Malformed);

    MeshAdapters::EventMeshTransport invalid({2, 1}, fingerprint, 2, owner, outbound);
    assert(!invalid.IsValid());
    return 0;
}
