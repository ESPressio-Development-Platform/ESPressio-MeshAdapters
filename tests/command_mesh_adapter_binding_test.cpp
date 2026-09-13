#include <ESPressio_CommandMeshAdapterBinding.hpp>
#include <ESPressio_CommandOutboundBinding.hpp>
#include <ESPressio_Persistence.hpp>
#include <ESPressio_Serializable.hpp>
#include <HostRuntime.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>

using namespace ESPressio;
namespace C=ESPressio::Command;

namespace {

System::DeviceIdentifier Device(std::uint8_t value){
    System::DeviceIdentifier::Storage bytes{};bytes.back()=value;return System::DeviceIdentifier{bytes};
}
Mesh::MembershipIncarnation Membership(std::uint8_t value){
    Mesh::MembershipIncarnation::Storage bytes{};bytes.back()=value;return Mesh::MembershipIncarnation{bytes};
}

struct EvidencePolicy final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::DestinationPrimitiveAdmission;
    using TerminalDisposition=Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=1'000'000'000ULL;
    static constexpr std::uint16_t MaximumAttempts=3;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=1'000'000ULL;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=1'000'000ULL;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=10'000'000ULL;
};
struct NoEvidencePolicy final {
    using PolicyCategory=Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence=Primitive::NoRemoteEvidence;
    using TerminalDisposition=Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds=0;
    static constexpr std::uint16_t MaximumAttempts=1;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds=0;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds=0;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds=0;
};
struct Retention final {
    static constexpr std::size_t MaximumTrackedOrigins=2;
    static constexpr std::size_t ReplayWindowEntries=4;
    using ResultRetention=C::VolatileResults;
};

struct Response final {
    std::uint32_t Value=0;
    ESPRESSIO_SERIALIZABLE_TYPE(Response)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};
struct ReplyCommand final : C::TransmissibleCommand<ReplyCommand,Response> {
    static constexpr C::CommandTypeId TypeId{0x4101};
    static constexpr std::string_view CanonicalName="MeshAdapters.Command.Reply";
    static constexpr std::size_t MaximumLiveInstances=3;
    static constexpr std::size_t MaximumPendingExecutions=1;
    static constexpr std::size_t MaximumPendingResponses=2;
    using ExecutionAdmissionPolicy=C::RequiredExecution;
    using RequestDeliveryPolicy=EvidencePolicy;
    using ResponseDeliveryPolicy=EvidencePolicy;
    using CompletionRetentionPolicy=Retention;
    std::uint32_t Value=0;
    ReplyCommand() noexcept = default;
    explicit ReplyCommand(std::uint32_t value) noexcept : Value(value) {}
    ESPRESSIO_SERIALIZABLE_TYPE(ReplyCommand)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};
struct FireCommand final : C::TransmissibleCommand<FireCommand,C::NoCommandResponse> {
    static constexpr C::CommandTypeId TypeId{0x4102};
    static constexpr std::string_view CanonicalName="MeshAdapters.Command.Fire";
    static constexpr std::size_t MaximumLiveInstances=2;
    static constexpr std::size_t MaximumPendingExecutions=1;
    using ExecutionAdmissionPolicy=C::RequiredExecution;
    using RequestDeliveryPolicy=NoEvidencePolicy;
    using CompletionRetentionPolicy=Retention;
    std::uint32_t Value=0;
    FireCommand() noexcept = default;
    explicit FireCommand(std::uint32_t value) noexcept : Value(value) {}
    ESPRESSIO_SERIALIZABLE_TYPE(FireCommand)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

template<std::size_t RecordBytes,std::size_t Records>
class Store final : public Persistence::IAtomicRecordStore {
    struct Entry final { bool Used=false;Persistence::AtomicRecordKey Key{};std::size_t Size=0;std::array<std::uint8_t,RecordBytes> Bytes{}; };
    std::array<Entry,Records> _entries{};
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override { return {true,true,RecordBytes,Records}; }
    Persistence::AtomicRecordStatus Recover() noexcept override { return Persistence::AtomicRecordStatus::Success; }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key,std::uint8_t* buffer,std::size_t capacity,std::size_t& bytesRead) noexcept override {
        bytesRead=0;
        for(const auto& entry:_entries){
            if(!entry.Used||!(entry.Key==key)) continue;
            if(!buffer||capacity<entry.Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
            std::memcpy(buffer,entry.Bytes.data(),entry.Size);bytesRead=entry.Size;return Persistence::AtomicRecordStatus::Success;
        }
        return Persistence::AtomicRecordStatus::NotFound;
    }
    Persistence::AtomicRecordStatus ReplaceAtomically(const Persistence::AtomicRecordKey& key,const std::uint8_t* data,std::size_t size) noexcept override {
        if(!key||!data||size>RecordBytes) return Persistence::AtomicRecordStatus::NoSpace;
        Entry* target=nullptr;
        for(auto& entry:_entries){
            if(entry.Used&&entry.Key==key){target=&entry;break;}
            if(!entry.Used&&!target) target=&entry;
        }
        if(!target) return Persistence::AtomicRecordStatus::NoSpace;
        target->Used=true;target->Key=key;target->Size=size;std::memcpy(target->Bytes.data(),data,size);
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(const Persistence::AtomicRecordKey& key) noexcept override {
        for(auto& entry:_entries) if(entry.Used&&entry.Key==key){entry=Entry{};break;}
        return Persistence::AtomicRecordStatus::Success;
    }
};
Persistence::AtomicRecordKey StoreKey(std::string_view text){
    Persistence::AtomicRecordKey key;assert(Persistence::AtomicRecordKey::TryCreate(text,key));return key;
}

struct Handler final {
    std::atomic<unsigned> ReplyCalls{0};
    std::atomic<unsigned> FireCalls{0};
    Response Reply(const ReplyCommand& command,const C::CommandExecutionContext&) {
        ++ReplyCalls;return {command.Value+7};
    }
    void Fire(const FireCommand&,const C::CommandExecutionContext&) { ++FireCalls; }
};

struct ReplySourceTransport final {
    bool Validate(const C::CommandOutboundContract&) noexcept { return true; }
    C::CommandOutboundAdmission Admit(System::DeviceIdentifier,const C::CommandRequestLease<ReplyCommand>&,C::CommandRequestDeliveryToken) noexcept {
        return {C::CommandOutboundAdmissionStatus::Accepted};
    }
    C::CommandRemoteResponseDestination ReserveRecovered(const C::CommandExecutionKey&) noexcept { return {}; }
    void ReleaseRecovered(C::CommandRemoteResponseDestination) noexcept {}
};
struct FireSourceTransport final {
    bool Validate(const C::CommandOutboundContract&) noexcept { return true; }
    C::CommandOutboundAdmission Admit(System::DeviceIdentifier,const C::CommandRequestLease<FireCommand>&,C::CommandRequestDeliveryToken) noexcept {
        return {C::CommandOutboundAdmissionStatus::Accepted};
    }
};

struct RouteResolver final {
    bool Running=true;
    static bool Validate(void* owner) noexcept { return static_cast<RouteResolver*>(owner)->Running; }
    static bool Resolve(void*,const System::DeviceIdentifier& device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xA000U+device.Bytes().back();return true;
    }
    static bool Broadcast(void*,Adapters::AdapterRouteToken& route) noexcept { route.Value=0xB001;return true; }
};

struct Adapter final {
    Adapters::AdapterBindingDescriptor Family{};
    std::array<std::uint8_t,512> LastWire{};
    std::size_t LastBytes=0;
    Adapters::AdapterRouteToken LastRoute{};
    Adapters::AdapterServiceClass LastService{Adapters::AdapterServiceClass::BestEffort};
    Primitive::PrimitivePolicyDescriptor LastPolicy{};
    std::atomic<unsigned> Submissions{0};

    Adapters::AdapterSubmissionDisposition SubmitOutbound(
        Primitive::PrimitiveFamilyId family,Adapters::AdapterServiceClass service,
        Primitive::PrimitiveProtocolVersion protocol,const void* source,Adapters::AdapterRouteToken route,
        Primitive::PrimitivePolicyDescriptor policy,std::uint64_t correlation) noexcept {
        assert(family==Primitive::FamilyIds::Command&&Family.EncodeOutbound);
        LastRoute=route;LastService=service;LastPolicy=policy;
        auto encoded=Family.EncodeOutbound(Family.Owner,protocol,correlation,source,{LastWire.data(),LastWire.size()});
        if(!encoded) return Adapters::AdapterSubmissionDisposition::Malformed;
        LastBytes=encoded.Bytes;++Submissions;
        return Adapters::AdapterSubmissionDisposition::Accepted;
    }
};

Task::TaskExecutorConfiguration RouterConfiguration(){
    Task::TaskExecutorConfiguration configuration{};
    configuration.Execution.Name="meshCmdRouter";
    configuration.Execution.StackSize=4096;
    configuration.QueueDepth=4;
    configuration.OverflowPolicy=Task::TaskQueueOverflowPolicy::Reject;
    configuration.QueueMemoryPolicy=Task::TaskMemoryPolicy::Internal;
    return configuration;
}
template<class Predicate> void Eventually(Predicate&& predicate){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}

template<class T,class Format>
std::size_t EncodeRequest(const T& value,const C::CommandExecutionKey& key,std::uint8_t* output,std::size_t capacity){
    assert(capacity>C::CommandRequestWireHeaderSize);
    Serializable::BoundedSerializationResult payload;
    if constexpr(std::is_same_v<Format,Serializable::DirectBinary>)
        payload=Serializable::SerializeDirectBinary(value,output+C::CommandRequestWireHeaderSize,capacity-C::CommandRequestWireHeaderSize);
    else if constexpr(std::is_same_v<Format,Serializable::CBOR>)
        payload=Serializable::SerializeBoundedCbor(value,output+C::CommandRequestWireHeaderSize,capacity-C::CommandRequestWireHeaderSize);
    else
        payload=Serializable::SerializeBoundedJson(value,output+C::CommandRequestWireHeaderSize,capacity-C::CommandRequestWireHeaderSize);
    assert(payload);
    const C::CommandRequestWireHeader header{key,{1234,Timing::TimeReliability::Acquiring},static_cast<std::uint32_t>(payload.Bytes)};
    assert(C::EncodeCommandRequestHeader(header,output,capacity));
    return C::CommandRequestWireHeaderSize+payload.Bytes;
}

}

int main(){
    HostRuntime platform;
    const auto localDevice=Device(1);
    assert(System::RuntimeIdentity::Install({localDevice,System::RuntimeIncarnationId{9}})==System::RuntimeIdentity::InstallationStatus::Success);

    using LedgerStore=Store<8192,8>;
    LedgerStore replyStore,fireStore;
    C::CommandResponseRouter<4> router(RouterConfiguration());
    C::RuntimeConfiguration config{};config.ExecutionLane.Name="meshCmdLane";config.ExecutionLane.StackSize=4096;config.ResponseRouter=router.Binding();
    C::Runtime runtime(config);Handler handler;
    assert(runtime.BindHandler<ReplyCommand>(handler,&Handler::Reply)==C::CommandRuntimeStatus::Success);
    assert(runtime.BindHandler<FireCommand>(handler,&Handler::Fire)==C::CommandRuntimeStatus::Success);
    assert(runtime.BindPersistence<ReplyCommand>(replyStore,StoreKey("mesh-reply"))==C::CommandRuntimeStatus::Success);
    assert(runtime.BindPersistence<FireCommand>(fireStore,StoreKey("mesh-fire"))==C::CommandRuntimeStatus::Success);

    ReplySourceTransport replySource;FireSourceTransport fireSource;
    C::CommandOutboundBinding<ReplyCommand,Serializable::DirectBinary> replyOutbound;
    C::CommandOutboundBinding<FireCommand,Serializable::DirectBinary> fireOutbound;
    assert((replyOutbound.Initialize<ReplySourceTransport,&ReplySourceTransport::Admit,&ReplySourceTransport::Validate,
        &ReplySourceTransport::ReserveRecovered,&ReplySourceTransport::ReleaseRecovered>(replySource)));
    assert((fireOutbound.Initialize<FireSourceTransport,&FireSourceTransport::Admit,&FireSourceTransport::Validate>(fireSource)));
    assert((runtime.BindTransport<ReplyCommand,Serializable::DirectBinary>(replyOutbound)==C::CommandRuntimeStatus::Success));
    assert((runtime.BindTransport<FireCommand,Serializable::DirectBinary>(fireOutbound)==C::CommandRuntimeStatus::Success));

    Primitive::TypeDirectory<2> directory;
    assert(directory.Register<ReplyCommand>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Register<FireCommand>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);
    assert(runtime.Initialize(directory.View())==C::CommandRuntimeStatus::Success);

    Adapter adapter;RouteResolver routes;
    const MeshAdapters::MeshRouteBinding routeBinding{&routes,&RouteResolver::Validate,&RouteResolver::Resolve,&RouteResolver::Broadcast};
    MeshAdapters::CommandMeshAdapterFamilyBinding<Adapter,2,4> family(adapter,routeBinding);
    assert((family.BindType<ReplyCommand,Serializable::DirectBinary>(runtime,Mesh::MeshRelayServiceClass::Responsive,
        Mesh::MeshRelayServiceClass::Responsive)==MeshAdapters::CommandMeshAdapterBindingStatus::Success));
    assert((family.BindType<FireCommand,Serializable::DirectBinary>(runtime,Mesh::MeshRelayServiceClass::BestEffort,
        Mesh::MeshRelayServiceClass::BestEffort)==MeshAdapters::CommandMeshAdapterBindingStatus::Success));
    assert(family.Freeze()==MeshAdapters::CommandMeshAdapterBindingStatus::Success);
    adapter.Family=family.AdapterBinding();
    assert(adapter.Family.IsValid()&&adapter.Family.RequiresValidatedOriginalSource);
    assert(adapter.Family.MaximumOutboundBytes>0&&adapter.Family.EncodeOutbound);
    const auto policyBinding=family.MeshPolicyBinding();assert(policyBinding);
    assert(runtime.Start()==C::CommandRuntimeStatus::Success);

    const auto remote=Device(7);
    const C::CommandExecutionKey replyKey{ReplyCommand::TypeId,remote,System::RuntimeIncarnationId{3},C::CommandId{11}};
    ReplyCommand reply{35};
    std::array<std::uint8_t,C::MaximumCompleteRequestWireBytes<ReplyCommand,Serializable::DirectBinary>> replyWire{};
    const auto replyBytes=EncodeRequest<ReplyCommand,Serializable::DirectBinary>(reply,replyKey,replyWire.data(),replyWire.size());
    Mesh::MeshReceiveContext replyContext{remote,Membership(4),101,3,false,Mesh::MeshRelayServiceClass::Responsive};
    Primitive::PrimitivePolicyDescriptor policy{};Adapters::AdapterSemanticProvenance provenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,replyContext,
        {replyWire.data(),replyBytes},policy,provenance)==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    assert(provenance.OriginalSource&&provenance.OriginalSource.Identity.Device==remote);
    assert(provenance.OriginalSource.Identity.Incarnation==System::RuntimeIncarnationId{3});
    assert(provenance.ImmediatePeer.Token==0xA007);
    assert(policy.Evidence==1);

    auto broadcastReply=replyContext;broadcastReply.Broadcast=true;
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,broadcastReply,
        {replyWire.data(),replyBytes},policy,provenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);

    const auto admitted=adapter.Family.AdmitInbound(adapter.Family.Owner,C::CommandProtocolVersion,
        {replyWire.data(),replyBytes},provenance);
    assert(admitted==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return handler.ReplyCalls.load()==1&&adapter.Submissions.load()==1;});
    assert(adapter.LastRoute.Value==0xA007);
    assert(adapter.LastService==Adapters::AdapterServiceClass::Responsive);
    C::CommandResponseWireHeader responseHeader{};
    assert(C::DecodeCommandResponseHeader(adapter.LastWire.data(),adapter.LastBytes,responseHeader));
    assert(responseHeader.Key==replyKey&&responseHeader.Executor.Device==localDevice);
    assert(responseHeader.Disposition==C::CommandResponseDisposition::Succeeded);

    // Exact terminal duplicate is idempotently owned by Command and may replay the response, but never re-runs the handler.
    Adapters::AdapterSemanticProvenance duplicateProvenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,replyContext,
        {replyWire.data(),replyBytes},policy,duplicateProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    const auto duplicate=adapter.Family.AdmitInbound(adapter.Family.Owner,C::CommandProtocolVersion,
        {replyWire.data(),replyBytes},duplicateProvenance);
    assert(duplicate==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);
    Eventually([&]{return adapter.Submissions.load()>=2;});
    assert(handler.ReplyCalls.load()==1);

    // A no-response NoRemoteEvidence Command is eligible for generic Mesh broadcast.
    const C::CommandExecutionKey fireKey{FireCommand::TypeId,remote,System::RuntimeIncarnationId{3},C::CommandId{12}};
    FireCommand fire{5};
    std::array<std::uint8_t,C::MaximumCompleteRequestWireBytes<FireCommand,Serializable::DirectBinary>> fireWire{};
    const auto fireBytes=EncodeRequest<FireCommand,Serializable::DirectBinary>(fire,fireKey,fireWire.data(),fireWire.size());
    Mesh::MeshReceiveContext fireContext{remote,Membership(4),102,3,true,Mesh::MeshRelayServiceClass::BestEffort};
    Adapters::AdapterSemanticProvenance fireProvenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,fireContext,
        {fireWire.data(),fireBytes},policy,fireProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    assert(policy.Evidence==0&&!fireProvenance.ImmediatePeer);
    assert(adapter.Family.AdmitInbound(adapter.Family.Owner,C::CommandProtocolVersion,
        {fireWire.data(),fireBytes},fireProvenance)==Primitive::PrimitiveAdmissionDisposition::Accepted);
    Eventually([&]{return handler.FireCalls.load()==1;});

    // Authenticated Mesh source and Command semantic source must agree exactly on DeviceIdentifier.
    auto wrongSource=fireContext;wrongSource.Source=Device(8);
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,wrongSource,
        {fireWire.data(),fireBytes},policy,fireProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);

    // Responses never use generic Mesh broadcast; a valid unicast response with no requester maps to Rejected.
    Response response{99};
    std::array<std::uint8_t,C::MaximumCompleteResponseWireBytes<ReplyCommand,Serializable::DirectBinary>> responseWire{};
    const System::DeviceRuntimeIdentity executor{remote,System::RuntimeIncarnationId{6}};
    const auto encodedResponse=C::EncodeCommandResponse<ReplyCommand,Serializable::DirectBinary>(
        {ReplyCommand::TypeId,localDevice,System::RuntimeIncarnationId{9},C::CommandId{88}},executor,
        C::CommandResponseDisposition::Succeeded,&response,responseWire.data(),responseWire.size());
    assert(encodedResponse);
    Mesh::MeshReceiveContext responseContext{remote,Membership(5),103,3,false,Mesh::MeshRelayServiceClass::Responsive};
    Adapters::AdapterSemanticProvenance responseProvenance{};
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,responseContext,
        {responseWire.data(),encodedResponse.Bytes},policy,responseProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Resolved);
    assert(responseProvenance.OriginalSource.Identity==executor);
    assert(adapter.Family.AdmitInbound(adapter.Family.Owner,C::CommandProtocolVersion,
        {responseWire.data(),encodedResponse.Bytes},responseProvenance)==Primitive::PrimitiveAdmissionDisposition::Rejected);
    responseContext.Broadcast=true;
    assert(policyBinding.Resolve(policyBinding.Owner,C::CommandProtocolVersion,responseContext,
        {responseWire.data(),encodedResponse.Bytes},policy,responseProvenance)==MeshAdapters::MeshAdapterPolicyResolution::Rejected);

    assert(runtime.Shutdown()==C::CommandRuntimeStatus::Success);
    return 0;
}
