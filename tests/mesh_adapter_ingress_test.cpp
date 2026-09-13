#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <ESPressio_MeshAdapterIngress.hpp>

using namespace ESPressio;

namespace {
System::DeviceIdentifier Device(std::uint8_t value){
    System::DeviceIdentifier::Storage bytes{};bytes.back()=value;return System::DeviceIdentifier{bytes};
}
Mesh::MembershipIncarnation Incarnation(std::uint8_t value){
    Mesh::MembershipIncarnation::Storage bytes{};bytes.back()=value;return Mesh::MembershipIncarnation{bytes};
}

struct Runtime final {
    Adapters::AdapterInboundCompletionTarget Pending{};
    std::uint64_t Correlation=0;
    std::size_t Calls=0;
    Adapters::AdapterSubmissionDisposition Next=Adapters::AdapterSubmissionDisposition::Accepted;
    Adapters::AdapterSubmissionDisposition AdmitTrustedInbound(
        Primitive::PrimitiveFamilyId family,Adapters::AdapterServiceClass service,
        Primitive::PrimitiveProtocolVersion version,Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance& provenance,Adapters::AdapterRouteToken,
        Primitive::PrimitivePolicyDescriptor policy,std::uint64_t correlation,
        Adapters::AdapterInboundCompletionTarget completion) noexcept {
        ++Calls;assert(family==Primitive::FamilyIds::Event);assert(service==Adapters::AdapterServiceClass::Responsive);
        assert(version==1&&bytes.Size==3&&bytes.Data[0]==4&&bytes.Data[2]==6);
        assert(!provenance.OriginalSource&&policy.Category==1&&policy.Evidence==0&&completion);
        Correlation=correlation;Pending=completion;return Next;
    }
    void Resolve(Primitive::PrimitiveAdmissionDisposition disposition) noexcept {
        assert(Pending);auto completion=Pending;Pending={};
        completion.Complete(completion.Owner,{Correlation,disposition,
            Primitive::EstablishesDestinationAdmission(disposition)
                ?Adapters::AdapterEvidence::DestinationPrimitiveAdmission:Adapters::AdapterEvidence::None});
    }
};

struct PolicyOwner final {
    static MeshAdapters::MeshAdapterPolicyResolution Resolve(
        void*,Primitive::PrimitiveProtocolVersion version,const Mesh::MeshReceiveContext& context,
        Adapters::AdapterByteView bytes,Primitive::PrimitivePolicyDescriptor& policy,
        Adapters::AdapterSemanticProvenance& provenance) noexcept {
        provenance={};
        if(version!=1) return MeshAdapters::MeshAdapterPolicyResolution::Unsupported;
        if(bytes.Size!=3||bytes.Data==nullptr) return MeshAdapters::MeshAdapterPolicyResolution::Malformed;
        if(context.Service!=Mesh::MeshRelayServiceClass::Responsive) return MeshAdapters::MeshAdapterPolicyResolution::Rejected;
        policy.Category=1;policy.Evidence=0;policy.Terminal=0;policy.MaximumResidenceNanoseconds=1'000'000'000ULL;
        policy.MaximumAttempts=2;policy.MaximumAdapterAdmissionWaitNanoseconds=500'000'000ULL;
        policy.MinimumRetrySpacingNanoseconds=1'000'000ULL;policy.MaximumRetrySpacingNanoseconds=10'000'000ULL;
        return MeshAdapters::MeshAdapterPolicyResolution::Resolved;
    }
};
struct Wake final {std::size_t Calls=0;static void Fire(void* owner) noexcept {++static_cast<Wake*>(owner)->Calls;}};
}

int main(){
    Runtime runtime;PolicyOwner owner;Wake wake;
    MeshAdapters::MeshAdapterIngressBridge<Runtime,2> bridge(runtime,
        {Primitive::FamilyIds::Event,{1,1},&owner,&PolicyOwner::Resolve},{&wake,&Wake::Fire});
    const std::array<std::uint8_t,3> bytes{{4,5,6}};
    const Mesh::MeshReceiveContext first{Device(1),Incarnation(1),7,2,true,Mesh::MeshRelayServiceClass::Responsive};

    const auto initialGeneration=bridge.AdmissionGeneration();
    assert(bridge.Receive(first,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable);
    assert(runtime.Calls==1&&bridge.AdmissionGeneration()==initialGeneration);
    assert(bridge.Receive(first,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable);
    assert(runtime.Calls==1);

    runtime.Resolve(Primitive::PrimitiveAdmissionDisposition::Accepted);
    assert(wake.Calls==1&&bridge.AdmissionGeneration()==initialGeneration+1);
    assert(bridge.Receive(first,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::Accepted);
    assert(runtime.Calls==1);

    const Mesh::MeshReceiveContext second{Device(1),Incarnation(1),8,2,true,Mesh::MeshRelayServiceClass::Responsive};
    assert(bridge.Receive(second,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::TemporarilyUnavailable);
    runtime.Resolve(Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);
    assert(bridge.Receive(second,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    const Mesh::MeshReceiveContext third{Device(1),Incarnation(1),9,2,true,Mesh::MeshRelayServiceClass::Responsive};
    runtime.Next=Adapters::AdapterSubmissionDisposition::ResourceUnavailable;
    assert(bridge.Receive(third,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::ResourceUnavailable);

    const Mesh::MeshReceiveContext wrongService{Device(1),Incarnation(1),10,2,true,Mesh::MeshRelayServiceClass::Critical};
    const auto callsBeforeWrongService=runtime.Calls;
    assert(bridge.Receive(wrongService,1,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::Rejected);
    assert(runtime.Calls==callsBeforeWrongService);

    assert(bridge.Receive(first,2,{bytes.data(),bytes.size()})==Primitive::PrimitiveAdmissionDisposition::Unsupported);
    assert(MeshAdapters::ToAdapterServiceClass(Mesh::MeshRelayServiceClass::Infrastructure)==Adapters::AdapterServiceClass::Infrastructure);
    assert(MeshAdapters::ToAdapterServiceClass(Mesh::MeshRelayServiceClass::Clock)==Adapters::AdapterServiceClass::Clock);
    assert(MeshAdapters::ToAdapterServiceClass(Mesh::MeshRelayServiceClass::Critical)==Adapters::AdapterServiceClass::Critical);
    assert(MeshAdapters::ToAdapterServiceClass(Mesh::MeshRelayServiceClass::Responsive)==Adapters::AdapterServiceClass::Responsive);
    assert(MeshAdapters::ToAdapterServiceClass(Mesh::MeshRelayServiceClass::Convergent)==Adapters::AdapterServiceClass::Convergent);
    assert(MeshAdapters::ToAdapterServiceClass(Mesh::MeshRelayServiceClass::BestEffort)==Adapters::AdapterServiceClass::BestEffort);
    return 0;
}
