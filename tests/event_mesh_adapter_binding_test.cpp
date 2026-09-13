#include <ESPressio_EventMeshAdapterBinding.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace ESPressio;
namespace E=ESPressio::Event;

struct Delivery final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::DestinationPrimitiveAdmission;
    using TerminalDisposition=Primitive::ReportTerminalFailureToFamily;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=10'000'000'000ULL;
    static constexpr std::uint16_t MaximumAttempts=3;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=1'000'000ULL;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=1'000'000ULL;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=10'000'000ULL;
};

struct Remote final : E::TransmissibleEvent<Remote> {
    static constexpr E::EventTypeId TypeId{0x2030405060708090ULL};
    static constexpr std::size_t MaximumLiveInstances=4;
    static constexpr std::size_t MaximumPendingInstances=2;
    static constexpr std::string_view CanonicalName="mesh.adapter.event.remote";
    using DeliveryPolicy=Delivery;
    std::uint32_t Value=0;
    ESPRESSIO_SERIALIZABLE_TYPE(Remote)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

template<class F> void Await(F&& f){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!f()){
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}

static bool SamePolicy(const Primitive::PrimitivePolicyDescriptor& left,
                       const Primitive::PrimitivePolicyDescriptor& right) noexcept {
    return left.CanonicalBytes()==right.CanonicalBytes();
}

int main(){
    HostRuntime platform;
    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<Remote>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    std::array<E::RemoteAdmissionReceipt,4> receipts{};
    E::RuntimeConfiguration config{};
    config.RemoteAdmissionReceipts=receipts.data();
    config.RemoteAdmissionReceiptCapacity=receipts.size();
    E::Runtime runtime(config);
    assert(runtime.Initialize(directory.View())==E::EventRuntimeStatus::Success);

    MeshAdapters::EventMeshAdapterFamilyBinding<2> family;
    assert((family.BindType<Remote,Serializable::DirectBinary>(runtime,Mesh::MeshRelayServiceClass::Responsive)
        ==MeshAdapters::EventMeshAdapterBindingStatus::Success));
    assert(family.Freeze()==MeshAdapters::EventMeshAdapterBindingStatus::Success);
    assert(family.IsFrozen()&&family.TypeCount()==1);
    assert((family.BindType<Remote,Serializable::DirectBinary>(runtime,Mesh::MeshRelayServiceClass::Responsive)
        ==MeshAdapters::EventMeshAdapterBindingStatus::Frozen));

    const auto adapter=family.AdapterBinding();
    assert(adapter.IsValid());
    assert(adapter.Family==Primitive::FamilyIds::Event);
    assert(adapter.Protocols.Contains(E::EventProtocolVersion));
    assert(adapter.Supports(Adapters::AdapterServiceClass::Responsive));
    assert(!adapter.Supports(Adapters::AdapterServiceClass::Critical));
    const auto policyBinding=family.MeshPolicyBinding();
    assert(policyBinding);

    assert(runtime.Start()==E::EventRuntimeStatus::Success);

    Remote payload;
    payload.Value=41;
    std::array<std::uint8_t,E::MaximumCompletePrimitiveWireBytes<Remote,Serializable::DirectBinary>> wire{};
    const auto encoded=Serializable::SerializeDirectBinary(payload,wire.data()+E::EventWireHeaderSize,
        wire.size()-E::EventWireHeaderSize);
    assert(encoded);
    System::DeviceIdentifier::Storage source{};
    source[0]=1;
    E::EventWireHeader header{{Remote::TypeId,{System::DeviceIdentifier{source},System::RuntimeIncarnationId{7}},
        E::ConceptualMessageId{18}},{123456,Timing::TimeReliability::Acquiring},static_cast<std::uint32_t>(encoded.Bytes)};
    assert(E::EncodeEventWireHeader(header,wire.data(),wire.size()));
    const auto bytes=E::EventWireHeaderSize+encoded.Bytes;
    const Adapters::AdapterByteView view{wire.data(),bytes};

    Primitive::PrimitivePolicyDescriptor resolved{};
    assert(policyBinding.Resolve(policyBinding.Owner,E::EventProtocolVersion,Mesh::MeshRelayServiceClass::Responsive,view,resolved)
        ==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    assert(SamePolicy(resolved,Primitive::PrimitivePolicyContract<Delivery>::Descriptor()));
    assert(policyBinding.Resolve(policyBinding.Owner,E::EventProtocolVersion,Mesh::MeshRelayServiceClass::Critical,view,resolved)
        ==MeshAdapters::MeshAdapterPolicyResolution::Rejected);

    Adapters::AdapterSemanticProvenance provenance{};
    Primitive::PrimitiveAdmissionDisposition first=Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable;
    Await([&]{
        first=adapter.AdmitInbound(adapter.Owner,E::EventProtocolVersion,view,provenance);
        return first!=Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable;
    });
    assert(first==Primitive::PrimitiveAdmissionDisposition::Accepted);
    assert(adapter.AdmitInbound(adapter.Owner,E::EventProtocolVersion,view,provenance)
        ==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    auto unknown=wire;
    unknown[4]^=1U;
    const Adapters::AdapterByteView unknownView{unknown.data(),bytes};
    assert(policyBinding.Resolve(policyBinding.Owner,E::EventProtocolVersion,Mesh::MeshRelayServiceClass::Responsive,
        unknownView,resolved)==MeshAdapters::MeshAdapterPolicyResolution::Unsupported);
    assert(adapter.AdmitInbound(adapter.Owner,E::EventProtocolVersion,unknownView,provenance)
        ==Primitive::PrimitiveAdmissionDisposition::Unsupported);

    assert(runtime.Shutdown()==E::EventRuntimeStatus::Success);
    return 0;
}
