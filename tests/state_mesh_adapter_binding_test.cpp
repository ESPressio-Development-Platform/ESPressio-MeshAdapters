#include <ESPressio_StateMeshAdapterBinding.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <ESPressio_RuntimeIdentity.hpp>
#include <ESPressio_TypeDirectory.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

using namespace ESPressio;
namespace S=ESPressio::State;

namespace {

struct Value final {
    std::uint32_t Number=0;
    constexpr bool operator==(const Value& other) const noexcept { return Number==other.Number; }
    ESPRESSIO_SERIALIZABLE_TYPE(Value)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("number",Number))
};

struct ConvergencePolicy final {
    using PolicyCategory=Primitive::StateConvergencePolicyTag;
    using RequiredEvidence=Primitive::NoRemoteEvidence;
    using Supersession=Primitive::LatestAuthoritativeValue;
    using ExhaustionDisposition=Primitive::DormantNeedsConvergence;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=0;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=0;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};

struct RemoteState final : S::TransmissibleState<RemoteState,Value> {
    static constexpr S::StateTypeId TypeId{0x5801};
    static constexpr std::string_view CanonicalName="MeshAdapters.State.Remote";
    using ConvergencePolicy=::ConvergencePolicy;
};

System::DeviceRuntimeIdentity Identity(std::uint8_t marker,std::uint32_t runtime) {
    System::DeviceIdentifier::Storage bytes{};
    bytes[0]=marker;
    return {System::DeviceIdentifier{bytes},System::RuntimeIncarnationId{runtime}};
}

Mesh::MembershipIncarnation Membership(std::uint8_t marker) {
    Mesh::MembershipIncarnation::Storage bytes{};
    bytes.back()=marker;
    return Mesh::MembershipIncarnation{bytes};
}

Timing::QualifiedTime Capture() {
    return {500,Timing::TimeReliability::Synchronized};
}

} // namespace

int main() {
    using Format=Serializable::DirectBinary;
    const auto local=Identity(1,11);
    const auto remote=Identity(2,41);
    assert(System::RuntimeIdentity::Install(local)==System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<RemoteState>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    using Runtime=S::Runtime<S::TypeConfiguration<RemoteState,S::MaximumRemoteOwners<1>,S::MaximumSubscribers<0>>>;
    Runtime runtime;
    assert(runtime.Initialize(directory.View(),&Capture)==S::StateRuntimeStatus::Success);
    assert(runtime.Start()==S::StateRuntimeStatus::Success);

    // Establish ordinary State-family remote-owner continuity first. The Mesh adapter must not own this table.
    const auto reserved=runtime.ReserveSubscriptionSession<RemoteState>(remote.Device);
    assert(reserved && reserved.Handle.OwnerDevice==remote.Device);
    const S::StateSnapshot<RemoteState> baseline{{10},{100,Timing::TimeReliability::Synchronized}};
    assert(runtime.InstallSubscribeSnapshot<RemoteState>(remote,reserved.Handle.Session,{false,1},baseline)==S::StateRemoteStatus::Success);

    MeshAdapters::StateMeshAdapterFamilyBinding<Runtime,1> family;
    assert((family.BindType<RemoteState,Format>(runtime,Mesh::MeshRelayServiceClass::Convergent)
            ==MeshAdapters::StateMeshAdapterBindingStatus::Success));
    assert(family.Freeze()==MeshAdapters::StateMeshAdapterBindingStatus::Success);
    const auto binding=family.AdapterBinding();
    const auto policyBinding=family.MeshPolicyBinding();
    assert(binding.IsValid()&&binding.RequiresValidatedOriginalSource);
    assert(policyBinding);

    // Owner-origin Publication: runtime incarnation comes from authenticated State bytes, never Mesh membership state.
    const S::StateSnapshot<RemoteState> newer{{42},{200,Timing::TimeReliability::Holdover}};
    std::array<std::uint8_t,S::MaximumCompleteStatePublicationWireBytes<RemoteState,Format>> publication{};
    const auto encoded=S::EncodeStatePublication<RemoteState,Format>(newer,{false,2},remote,local,reserved.Handle.Session,
                                                                     publication.data(),publication.size());
    assert(encoded);
    const Mesh::MeshReceiveContext context{remote.Device,Membership(9),501,4,false,Mesh::MeshRelayServiceClass::Convergent};
    Primitive::PrimitivePolicyDescriptor policy{};
    Adapters::AdapterSemanticProvenance provenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,S::StateProtocolVersion,context,
        {publication.data(),encoded.Bytes},policy,provenance)==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    assert(provenance.OriginalSource&&provenance.OriginalSource.Validated);
    assert(provenance.OriginalSource.Identity==remote);
    assert(provenance.OriginalSource.Identity.Incarnation.Value()==41);
    assert(policy.Category==2&&policy.Evidence==0);

    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {publication.data(),encoded.Bytes},provenance)==Primitive::PrimitiveAdmissionDisposition::Accepted);
    S::StateSnapshot<RemoteState> read{};
    assert(runtime.TryReadRemote<RemoteState>(remote.Device,read));
    assert(read.Value.Number==42&&read.TruthTime.Nanoseconds==200);

    // Exact duplicate remains family-idempotent and does not require adapter-local session/version state.
    assert(binding.AdmitInbound(binding.Owner,S::StateProtocolVersion,
        {publication.data(),encoded.Bytes},provenance)==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    // Generic State broadcast is forbidden and rejected without leaking stale provenance.
    auto broadcast=context;
    broadcast.Broadcast=true;
    Adapters::AdapterSemanticProvenance broadcastProvenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,S::StateProtocolVersion,broadcast,
        {publication.data(),encoded.Bytes},policy,broadcastProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);
    assert(!broadcastProvenance.OriginalSource);

    // Authenticated Mesh device and the role-specific State semantic source must agree.
    auto wrongSource=context;
    wrongSource.Source=Identity(3,99).Device;
    Adapters::AdapterSemanticProvenance wrongProvenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,S::StateProtocolVersion,wrongSource,
        {publication.data(),encoded.Bytes},policy,wrongProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);
    assert(!wrongProvenance.OriginalSource);

    // Service-class selection is frozen per Type and is not inferred from the packet at runtime.
    auto wrongService=context;
    wrongService.Service=Mesh::MeshRelayServiceClass::Responsive;
    assert(policyBinding.Resolve(policyBinding.Owner,S::StateProtocolVersion,wrongService,
        {publication.data(),encoded.Bytes},policy,wrongProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);

    // Requester-origin control selects Requester as semantic provenance, not Owner or immediate relay identity.
    S::StateControlWireHeader request{};
    request.Kind=S::StateMessageKind::SubscribeRequest;
    request.TypeId=RemoteState::TypeId;
    request.Owner=local;
    request.Requester=remote;
    request.Session=S::StateSessionToken{77};
    std::array<std::uint8_t,S::StateControlWireHeaderSize> control{};
    const auto controlEncoded=S::EncodeStateControl(request,control.data(),control.size());
    assert(controlEncoded);
    const Mesh::MeshReceiveContext requestContext{remote.Device,Membership(10),502,4,false,Mesh::MeshRelayServiceClass::Convergent};
    Adapters::AdapterSemanticProvenance requestProvenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,S::StateProtocolVersion,requestContext,
        {control.data(),controlEncoded.Bytes},policy,requestProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    assert(requestProvenance.OriginalSource.Identity==remote);

    // A forged requester runtime identity from another device is rejected before State mutation.
    request.Requester=Identity(4,41);
    assert(S::EncodeStateControl(request,control.data(),control.size()));
    assert(policyBinding.Resolve(policyBinding.Owner,S::StateProtocolVersion,requestContext,
        {control.data(),control.size()},policy,requestProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);

    assert(runtime.Shutdown()==S::StateRuntimeStatus::Success);
    return 0;
}
