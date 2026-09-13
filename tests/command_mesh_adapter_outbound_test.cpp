#include <ESPressio_Adapters.hpp>
#include <ESPressio_CommandMeshAdapterBinding.hpp>
#include <ESPressio_Persistence.hpp>
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
namespace C=ESPressio::Command;

namespace {

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
struct FireCommand final : C::TransmissibleCommand<FireCommand,C::NoCommandResponse> {
    static constexpr C::CommandTypeId TypeId{0x4201};
    static constexpr std::string_view CanonicalName="MeshAdapters.Command.Outbound";
    static constexpr std::size_t MaximumLiveInstances=2;
    static constexpr std::size_t MaximumPendingExecutions=1;
    using ExecutionAdmissionPolicy=C::RequiredExecution;
    using RequestDeliveryPolicy=NoEvidencePolicy;
    using CompletionRetentionPolicy=Retention;
    std::uint32_t Value=0;
    FireCommand() noexcept=default;
    explicit FireCommand(std::uint32_t value) noexcept:Value(value){}
    ESPRESSIO_SERIALIZABLE_TYPE(FireCommand)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY("value",Value))
};

template<std::size_t RecordBytes,std::size_t Records>
class Store final : public Persistence::IAtomicRecordStore {
    struct Entry final {
        bool Used=false;
        Persistence::AtomicRecordKey Key{};
        std::size_t Size=0;
        std::array<std::uint8_t,RecordBytes> Bytes{};
    };
    std::array<Entry,Records> _entries{};
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override { return {true,true,RecordBytes,Records}; }
    Persistence::AtomicRecordStatus Recover() noexcept override { return Persistence::AtomicRecordStatus::Success; }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key,std::uint8_t* buffer,
                                         std::size_t capacity,std::size_t& bytesRead) noexcept override {
        bytesRead=0;
        for(const auto& entry:_entries) {
            if(!entry.Used||!(entry.Key==key)) continue;
            if(!buffer||capacity<entry.Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
            std::memcpy(buffer,entry.Bytes.data(),entry.Size);
            bytesRead=entry.Size;
            return Persistence::AtomicRecordStatus::Success;
        }
        return Persistence::AtomicRecordStatus::NotFound;
    }
    Persistence::AtomicRecordStatus ReplaceAtomically(const Persistence::AtomicRecordKey& key,
                                                       const std::uint8_t* data,std::size_t size) noexcept override {
        if(!key||!data||size>RecordBytes) return Persistence::AtomicRecordStatus::NoSpace;
        Entry* target=nullptr;
        for(auto& entry:_entries) {
            if(entry.Used&&entry.Key==key) { target=&entry;break; }
            if(!entry.Used&&!target) target=&entry;
        }
        if(!target) return Persistence::AtomicRecordStatus::NoSpace;
        target->Used=true;target->Key=key;target->Size=size;
        std::memcpy(target->Bytes.data(),data,size);
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(const Persistence::AtomicRecordKey& key) noexcept override {
        for(auto& entry:_entries) if(entry.Used&&entry.Key==key) { entry=Entry{};break; }
        return Persistence::AtomicRecordStatus::Success;
    }
};

Persistence::AtomicRecordKey StoreKey(std::string_view text) {
    Persistence::AtomicRecordKey key;
    assert(Persistence::AtomicRecordKey::TryCreate(text,key));
    return key;
}

struct Handler final {
    void Fire(const FireCommand&,const C::CommandExecutionContext&) {}
};

System::DeviceIdentifier Device(std::uint8_t marker) {
    System::DeviceIdentifier::Storage bytes{};
    bytes.back()=marker;
    return System::DeviceIdentifier{bytes};
}

template<class Predicate>
void Eventually(Predicate&& predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}

struct RouteResolver final {
    static bool Validate(void*) noexcept { return true; }
    static bool Resolve(void*,const System::DeviceIdentifier& device,Adapters::AdapterRouteToken& route) noexcept {
        if(!device) return false;
        route.Value=0xA000U+device.Bytes().back();
        return true;
    }
    static bool Broadcast(void*,Adapters::AdapterRouteToken& route) noexcept {
        route.Value=0xB001U;
        return true;
    }
};

using Arena=Adapters::StaticByteArena<Adapters::ByteClass<256,8>,Adapters::ByteClass<1024,4>>;
using Domain=Adapters::StaticCapacityDomain<1024,4,Arena>;
using Inbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Inbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using Outbound=Adapters::CapacityPlane<Adapters::AdapterDirection::Outbound,
    Domain,Domain,Domain,Domain,Domain,Domain,Domain,Domain>;
using AdapterRuntime=Adapters::AdapterRuntime<Inbound,Outbound,2,4,1,1,8>;

struct LowerTransport final {
    std::array<std::uint8_t,512> Last{};
    std::size_t Bytes=0;
    Adapters::AdapterRouteToken Route{};
    Adapters::AdapterServiceClass Service{Adapters::AdapterServiceClass::BestEffort};
    std::atomic<unsigned> Calls{0};

    static bool Validate(void*) noexcept { return true; }
    static Adapters::LowerTransportSubmitResult Submit(
        void* owner,
        Adapters::AdapterRecordIdentity,
        Primitive::PrimitiveFamilyId,
        Primitive::PrimitiveProtocolVersion,
        const Primitive::PrimitivePolicyDescriptor&,
        Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,
        Adapters::AdapterRouteToken route) noexcept {
        auto& self=*static_cast<LowerTransport*>(owner);
        assert(bytes.Data&&bytes.Size<=self.Last.size());
        std::memcpy(self.Last.data(),bytes.Data,bytes.Size);
        self.Bytes=bytes.Size;
        self.Route=route;
        self.Service=service;
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

template<class TFamily>
struct FireOutboundOwner final {
    TFamily* Family=nullptr;
    C::CommandOutboundAdmission Admit(
        System::DeviceIdentifier target,
        const C::CommandRequestLease<FireCommand>& request,
        C::CommandRequestDeliveryToken token) noexcept {
        return Family->template SubmitRequest<FireCommand>(target,request,token);
    }
    bool Validate(const C::CommandOutboundContract& contract) noexcept {
        return Family->template ValidateOutboundContract<FireCommand,Serializable::DirectBinary>(contract);
    }
};

} // namespace

int main() {
    HostRuntime platform;
    const auto local=Device(1);
    const auto remote=Device(7);
    assert(System::RuntimeIdentity::Install({local,System::RuntimeIncarnationId{9}})==
           System::RuntimeIdentity::InstallationStatus::Success);

    Primitive::TypeDirectory<1> directory;
    assert(directory.Register<FireCommand>()==Primitive::TypeDirectoryRegistrationStatus::Success);
    assert(directory.Initialize()==Primitive::TypeDirectoryInitializationStatus::Success);

    AdapterRuntime adapter;
    RouteResolver routes;
    const MeshAdapters::MeshRouteBinding routeBinding{
        &routes,&RouteResolver::Validate,&RouteResolver::Resolve,&RouteResolver::Broadcast};
    using Family=MeshAdapters::CommandMeshAdapterFamilyBinding<AdapterRuntime,1,2>;
    Family family(adapter,routeBinding);
    assert((family.ConfigureType<FireCommand,Serializable::DirectBinary>(
                Mesh::MeshRelayServiceClass::BestEffort)==
            MeshAdapters::CommandMeshAdapterBindingStatus::Success));

    FireOutboundOwner<Family> outboundOwner{&family};
    C::CommandOutboundBinding<FireCommand,Serializable::DirectBinary> outbound;
    assert((outbound.Initialize<FireOutboundOwner<Family>,
        &FireOutboundOwner<Family>::Admit,
        &FireOutboundOwner<Family>::Validate>(outboundOwner)));

    Store<4096,4> store;
    C::RuntimeConfiguration commandConfiguration{};
    commandConfiguration.ExecutionLane.Name="meshCmdOut";
    commandConfiguration.ExecutionLane.StackSize=4096;
    C::Runtime commandRuntime(commandConfiguration);
    Handler handler;
    assert(commandRuntime.BindHandler<FireCommand>(handler,&Handler::Fire)==C::CommandRuntimeStatus::Success);
    assert(commandRuntime.BindPersistence<FireCommand>(store,StoreKey("mesh-command-out"))==C::CommandRuntimeStatus::Success);
    assert((commandRuntime.BindTransport<FireCommand,Serializable::DirectBinary>(outbound)==
            C::CommandRuntimeStatus::Success));
    assert(commandRuntime.Initialize(directory.View())==C::CommandRuntimeStatus::Success);
    assert((family.AttachRuntime<FireCommand,Serializable::DirectBinary>(commandRuntime)==
            MeshAdapters::CommandMeshAdapterBindingStatus::Success));
    assert(family.Freeze()==MeshAdapters::CommandMeshAdapterBindingStatus::Success);

    LowerTransport lower;
    assert(adapter.BindFamily(family.AdapterBinding())==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.BindTransport(lower.Binding())==Adapters::AdapterRuntimeStatus::Success);
    Task::TaskExecutionConfiguration worker{};
    worker.Name="meshA2Cmd";
    worker.StackSize=4096;
    assert(adapter.Initialize(worker,worker)==Adapters::AdapterRuntimeStatus::Success);
    assert(adapter.Start()==Adapters::AdapterRuntimeStatus::Success);
    assert(commandRuntime.Start()==C::CommandRuntimeStatus::Success);

    const auto submitted=C::CommandTypeRuntime<FireCommand>::Get().SubmitRemoteNoResponse<false>(remote,73U);
    assert(static_cast<bool>(submitted));
    Eventually([&]{ return lower.Calls.load()>=1; });
    assert(lower.Route.Value==0xA007U);
    assert(lower.Service==Adapters::AdapterServiceClass::BestEffort);

    C::CommandRequestWireHeader header{};
    assert(C::DecodeCommandRequestHeader(lower.Last.data(),lower.Bytes,header));
    assert(header.Key.TypeId==FireCommand::TypeId);
    assert(header.Key.OriginDevice==local);
    assert(header.Key.OriginRuntime==System::RuntimeIncarnationId{9});
    assert(header.Key.Id==submitted.Id);
    FireCommand payload{};
    const auto decoded=Serializable::DeserializeBoundedDirectBinary(
        lower.Last.data()+C::CommandRequestWireHeaderSize,
        lower.Bytes-C::CommandRequestWireHeaderSize,payload);
    assert(decoded&&payload.Value==73U);

    assert(commandRuntime.Shutdown()==C::CommandRuntimeStatus::Success);
    assert(adapter.Shutdown()==Adapters::AdapterRuntimeStatus::Success);
    return 0;
}
