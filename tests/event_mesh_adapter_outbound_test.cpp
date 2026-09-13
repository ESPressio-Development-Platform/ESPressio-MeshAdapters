#include <ESPressio_Adapters.hpp>
#include <ESPressio_EventMeshAdapterOutboundTarget.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>

using namespace ESPressio;
namespace E=ESPressio::Event;

namespace {

struct Delivery final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::NoRemoteEvidence;
    using TerminalDisposition=Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=0;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=0;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};

struct LocalEvent final : E::TransmissibleEvent<LocalEvent> {
    static constexpr E::EventTypeId TypeId{0x6A01};
    static constexpr std::size_t MaximumLiveInstances=4;
    static constexpr std::size_t MaximumPendingInstances=2;
    static constexpr std::string_view CanonicalName="MeshAdapters.Event.Outbound";
    using DeliveryPolicy=Delivery;
    std::uint32_t Value=0;
    LocalEvent() noexcept=default;
    explicit LocalEvent(std::uint32_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(LocalEvent)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};bytes[0]=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}

template<class Predicate>
void Eventually(Predicate&& predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}

using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,8>,Adapters::ByteClass<1024,4>>;
using Domain=Adapters::StaticCapacityDomain<1024,4,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using AdapterRuntime=Adapters::AdapterRuntime<Inbound,Outbound,2,4,1,1,8>;

struct LowerTransport final {
    std::array<std::uint8_t,512> Last{};
    std::size_t Bytes=0;
    Adapters::AdapterRouteToken Route{};
    Adapters::AdapterServiceClass Service{Adapters::AdapterServiceClass::BestEffort};
    std::atomic<unsigned> Calls{0};

    static bool Validate(void*) noexcept { return true; }
    static Adapters::LowerTransportSubmitResult Submit(
        void* owner,Adapters::AdapterRecordIdentity,Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,Adapters::AdapterRouteToken route) noexcept {
        auto& self=*static_cast<LowerTransport*>(owner);
        assert(bytes.Data&&bytes.Size<=self.Last.size());
        std::memcpy(self.Last.data(),bytes.Data,bytes.Size);
        self.Bytes=bytes.Size;self.Route=route;self.Service=service;
        const auto generation=++self.Calls;
        return {Adapters::LowerTransportDisposition::Accepted,generation,false};
    }

    Adapters::LowerTransportBinding Binding() noexcept {
        Adapters::LowerTransportBinding binding{};
        binding.Owner=this;
        binding.Submit=&LowerTransport::Submit;
        binding.Validate=&LowerTransport::Validate;
        binding.ServiceClassMask=0x3f;
        binding.ProvidesDestinationPrimitiveAdmission=false;
        binding.ProvidesValidatedOriginalSource=true;
        return binding;
    }
};

} // namespace

int main() {
    HostRuntime platform;
    const auto local=Identity(1,17);
    const auto remote=Identity(2,31);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<LocalEvent>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    std::array<E::RemoteAdmissionReceipt,4> receipts{};
    E::RuntimeConfiguration eventConfiguration{};
    eventConfiguration.DispatchLane.Name="meshEventOut";
    eventConfiguration.DispatchLane.StackSize=4096;
    eventConfiguration.RemoteAdmissionReceipts=receipts.data();
    eventConfiguration.RemoteAdmissionReceiptCapacity=receipts.size();
    E::Runtime eventRuntime(eventConfiguration);
    assert(eventRuntime.Initialize(directory.View())==E::EventRuntimeStatus::Success);

    MeshAdapters::EventMeshAdapterFamilyBinding<1> family;
    assert((family.BindType<LocalEvent,Serializable::DirectBinary>(
        eventRuntime,Mesh::MeshRelayServiceClass::BestEffort)==MeshAdapters::EventMeshAdapterBindingStatus::Success));
    assert(family.Freeze()==MeshAdapters::EventMeshAdapterBindingStatus::Success);

    AdapterRuntime adapter;
    LowerTransport transport;
    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(transport.Binding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};worker.Name="meshA2";worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);

    using Target=MeshAdapters::EventMeshAdapterOutboundTarget<
        AdapterRuntime,1,LocalEvent,Serializable::DirectBinary>;
    Target target(adapter,family,Adapters::AdapterRouteToken{0x7001},false);
    assert(target.Initialize()==E::EventRuntimeStatus::Success);
    assert(eventRuntime.Start()==E::EventRuntimeStatus::Success);

    const auto dispatched=LocalEvent::TryDispatch(73U);
    assert(dispatched.Status==E::EventDispatchStatus::Accepted);
    Eventually([&]{return transport.Calls.load()>=1;});
    assert(transport.Route.Value==0x7001);
    assert(transport.Service==Adapters::AdapterServiceClass::BestEffort);

    E::EventWireHeader outboundHeader{};
    assert(E::DecodeEventWireHeader(transport.Last.data(),transport.Bytes,outboundHeader));
    assert(outboundHeader.Key.TypeId==LocalEvent::TypeId);
    assert(outboundHeader.Key.Origin==local);
    assert(outboundHeader.Key.MessageId==dispatched.MessageId);
    LocalEvent outboundPayload{};
    const auto decoded=Serializable::DeserializeBoundedDirectBinary(
        transport.Last.data()+E::EventWireHeaderSize,transport.Bytes-E::EventWireHeaderSize,outboundPayload);
    assert(decoded&&outboundPayload.Value==73U);

    LocalEvent remotePayload{91U};
    std::array<std::uint8_t,E::MaximumCompletePrimitiveWireBytes<LocalEvent,Serializable::DirectBinary>> remoteWire{};
    const auto remoteEncoded=Serializable::SerializeDirectBinary(
        remotePayload,remoteWire.data()+E::EventWireHeaderSize,remoteWire.size()-E::EventWireHeaderSize);
    assert(remoteEncoded);
    const E::EventWireHeader remoteHeader{{LocalEvent::TypeId,remote,E::ConceptualMessageId{44}},
        {900,Timing::TimeReliability::Acquiring},static_cast<std::uint32_t>(remoteEncoded.Bytes)};
    assert(E::EncodeEventWireHeader(remoteHeader,remoteWire.data(),remoteWire.size()));
    const auto remoteBytes=E::EventWireHeaderSize+remoteEncoded.Bytes;
    const auto binding=family.AdapterBinding();
    assert(binding.AdmitInbound(binding.Owner,E::EventProtocolVersion,{remoteWire.data(),remoteBytes},{})
        ==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return E::EventTypeRuntime<LocalEvent>::Get().LiveInstances()==0;});
    assert(transport.Calls.load()==1);

    assert(eventRuntime.Shutdown()==E::EventRuntimeStatus::Success);
    target.Shutdown();
    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
