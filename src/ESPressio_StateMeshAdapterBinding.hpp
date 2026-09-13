#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>

#include <ESPressio_AdapterBinding.hpp>
#include <ESPressio_StateRemoteAdmission.hpp>
#include <ESPressio_StateRuntime.hpp>
#include <ESPressio_StateTransportBinding.hpp>
#include <ESPressio_StateWireV1.hpp>

#include "ESPressio_MeshAdapterIngress.hpp"
#include "ESPressio_MeshRouteBinding.hpp"

namespace ESPressio::MeshAdapters {

enum class StateMeshAdapterBindingStatus : std::uint8_t {
    Success=0,
    Frozen,
    DuplicateType,
    ResourceUnavailable,
    InvalidBinding,
    InvalidService,
    RouteUnavailable
};

/// <summary>Coalesced composition wake. It schedules service context only and never services State inline.</summary>
struct StateMeshAdapterServiceWake final {
    void* Owner=nullptr;
    void (*Wake)(void*) noexcept=nullptr;

    constexpr explicit operator bool() const noexcept { return Owner&&Wake; }
    void Signal() const noexcept { if(*this) Wake(Owner); }
};

namespace Detail {

struct ParsedStateEnvelope final {
    State::StateTypeId TypeId{};
    System::DeviceRuntimeIdentity SemanticSource{};
    State::StateWireStatus Status{State::StateWireStatus::InvalidHeader};
    bool Parsed=false;
};

inline ParsedStateEnvelope ParseStateEnvelope(Adapters::AdapterByteView bytes) noexcept {
    ParsedStateEnvelope result{};
    if(!bytes.Data||bytes.Size<5) return result;

    const auto kind=static_cast<State::StateMessageKind>(bytes.Data[4]);
    if(!State::IsValidStateMessageKind(kind)) return result;

    if(kind==State::StateMessageKind::Publication) {
        State::StatePublicationWireHeader header{};
        const auto decoded=State::DecodeStatePublicationHeader(bytes.Data,bytes.Size,header);
        result.Status=decoded.Status;
        if(!decoded) return result;
        result.TypeId=header.TypeId;
        result.SemanticSource=State::StateMessageSourceRole(kind)==State::StateSemanticSourceRole::Owner
            ?header.Owner:header.Requester;
    } else if(State::IsStateSnapshotControlKind(kind)) {
        State::StateSnapshotControlWireHeader header{};
        const auto decoded=State::DecodeStateSnapshotControlHeader(bytes.Data,bytes.Size,header);
        result.Status=decoded.Status;
        if(!decoded) return result;
        result.TypeId=header.Control.TypeId;
        result.SemanticSource=State::StateMessageSourceRole(kind)==State::StateSemanticSourceRole::Owner
            ?header.Control.Owner:header.Control.Requester;
    } else if(State::IsStateAcceptanceControlKind(kind)) {
        State::StateAcceptanceControlWireHeader header{};
        const auto decoded=State::DecodeStateAcceptanceControl(bytes.Data,bytes.Size,header);
        result.Status=decoded.Status;
        if(!decoded) return result;
        result.TypeId=header.Control.TypeId;
        result.SemanticSource=State::StateMessageSourceRole(kind)==State::StateSemanticSourceRole::Owner
            ?header.Control.Owner:header.Control.Requester;
    } else {
        State::StateControlWireHeader header{};
        const auto decoded=State::DecodeStateControl(bytes.Data,bytes.Size,header);
        result.Status=decoded.Status;
        if(!decoded) return result;
        result.TypeId=header.TypeId;
        result.SemanticSource=State::StateMessageSourceRole(kind)==State::StateSemanticSourceRole::Owner
            ?header.Owner:header.Requester;
    }

    result.Status=State::StateWireStatus::Success;
    result.Parsed=static_cast<bool>(result.TypeId)&&static_cast<bool>(result.SemanticSource);
    return result;
}

constexpr Adapters::AdapterResourceStatus MapStateWireEncode(State::StateWireStatus status) noexcept {
    switch(status) {
        case State::StateWireStatus::Success:
            return Adapters::AdapterResourceStatus::Success;
        case State::StateWireStatus::InsufficientOutput:
        case State::StateWireStatus::PayloadTooLarge:
            return Adapters::AdapterResourceStatus::TooLarge;
        default:
            return Adapters::AdapterResourceStatus::InvalidConfiguration;
    }
}

template<class TState,class TFormat>
State::StateWireResult EncodeStateOutboundMessage(
    const State::StateOutboundMessage<TState>& message,
    std::uint8_t* output,
    std::size_t capacity) {
    if(!State::IsValidStateMessageKind(message.Kind)) return {};

    if(message.Kind==State::StateMessageKind::Publication) {
        if(!message.HasSnapshot) return {};
        return State::EncodeStatePublication<TState,TFormat>(
            message.Snapshot,message.Version,message.Owner,message.Requester,message.Session,output,capacity);
    }

    const State::StateControlWireHeader control{
        message.Kind,TState::TypeId,message.Owner,message.Requester,
        message.Session,message.Resync,message.ControlCode};

    if(State::IsStateSnapshotControlKind(message.Kind)) {
        if(!message.HasSnapshot) return {};
        return State::EncodeStateSnapshotControl<TState,TFormat>(
            control,message.Snapshot,message.Version,output,capacity);
    }
    if(State::IsStateAcceptanceControlKind(message.Kind))
        return State::EncodeStateAcceptanceControl({control,message.Version},output,capacity);
    if(State::IsStateCommonOnlyControlKind(message.Kind))
        return State::EncodeStateControl(control,output,capacity);
    return {};
}

} // namespace Detail

/// <summary>Frozen State/Mesh/A2 bridge. State owns sessions/versions/convergence; A2 owns finite byte pursuit.</summary>
template<class TStateRuntime,std::size_t TMaximumTypes,std::size_t TMaximumOutboundCampaigns=(TMaximumTypes*4)>
class StateMeshAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"State MeshAdapter Type capacity must be non-zero");
    static_assert(TMaximumOutboundCampaigns>0,"State MeshAdapter campaign capacity must be non-zero");

    using AdmitThunk=State::StateRemoteAdmissionResult(*)(
        TStateRuntime&,const std::uint8_t*,std::size_t,State::StateValidatedIngressContext) noexcept;
    using EncodeThunk=State::StateWireResult(*)(const void*,std::uint8_t*,std::size_t);
    using ExhaustionThunk=State::StateRemoteStatus(*)(
        TStateRuntime&,const State::StateConvergenceHandle&) noexcept;
    using ServiceThunk=bool(*)(TStateRuntime&) noexcept;

    struct Entry final {
        State::StateTypeId TypeId{};
        TStateRuntime* Runtime=nullptr;
        Primitive::PrimitivePolicyDescriptor Policy{};
        Mesh::MeshRelayServiceClass RelayService{Mesh::MeshRelayServiceClass::Convergent};
        State::StatePayloadFormat Format{State::StatePayloadFormat::DirectBinary};
        std::size_t MaximumWireBytes=0;
        AdmitThunk Admit=nullptr;
        EncodeThunk Encode=nullptr;
        ExhaustionThunk ReportExhausted=nullptr;
        ServiceThunk ServiceLatest=nullptr;
        bool Used=false;
    };

    enum class CampaignState : std::uint8_t { Free=0,InFlight,ExhaustionPending };

    struct Campaign final {
        CampaignState State{CampaignState::Free};
        std::uint64_t Generation=0;
        std::size_t EntryIndex=0;
        State::StateConvergenceHandle Handle{};
    };

    struct OutboundSource final {
        std::size_t EntryIndex=0;
        const void* Message=nullptr;
    };

    void* _adapterOwner=nullptr;
    Adapters::AdapterSubmissionDisposition (*_submit)(
        void*,Primitive::PrimitiveFamilyId,Adapters::AdapterServiceClass,
        Primitive::PrimitiveProtocolVersion,const void*,Adapters::AdapterRouteToken,
        Primitive::PrimitivePolicyDescriptor,std::uint64_t) noexcept=nullptr;
    MeshRouteBinding _routes{};
    StateMeshAdapterServiceWake _wake{};
    std::array<Entry,TMaximumTypes> _entries{};
    std::array<Campaign,TMaximumOutboundCampaigns> _campaigns{};
    std::size_t _count=0;
    std::size_t _maximumInboundBytes=0;
    std::size_t _maximumOutboundBytes=0;
    std::uint8_t _serviceMask=0;
    bool _requiresDestinationEvidence=false;
    bool _frozen=false;
    std::mutex _campaignMutex{};

    static constexpr std::uint8_t ServiceBit(Mesh::MeshRelayServiceClass service) noexcept {
        return static_cast<std::uint8_t>(
            std::uint8_t{1}<<static_cast<std::uint8_t>(ToAdapterServiceClass(service)));
    }

    Entry* Find(State::StateTypeId type) noexcept {
        for(std::size_t i=0;i<_count;++i)
            if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }

    const Entry* Find(State::StateTypeId type) const noexcept {
        for(std::size_t i=0;i<_count;++i)
            if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }

    std::size_t IndexOf(const Entry* entry) const noexcept {
        return entry?static_cast<std::size_t>(entry-_entries.data()):TMaximumTypes;
    }

    static std::uint64_t Correlation(std::size_t slot,std::uint64_t generation) noexcept {
        return (generation<<16U)|static_cast<std::uint64_t>(slot+1U);
    }

    static bool DecodeCorrelation(
        std::uint64_t value,std::size_t& slot,std::uint64_t& generation) noexcept {
        const auto raw=static_cast<std::uint16_t>(value&0xffffU);
        if(!raw) return false;
        slot=static_cast<std::size_t>(raw-1U);
        generation=value>>16U;
        return slot<TMaximumOutboundCampaigns&&generation!=0;
    }

    bool ReserveCampaign(
        std::size_t entryIndex,
        const State::StateConvergenceHandle& handle,
        std::uint64_t& correlation) noexcept {
        correlation=0;
        if(!handle) return true;
        std::unique_lock<std::mutex> lock(_campaignMutex,std::try_to_lock);
        if(!lock.owns_lock()) return false;
        for(std::size_t i=0;i<_campaigns.size();++i) {
            auto& slot=_campaigns[i];
            if(slot.State!=CampaignState::Free||
               slot.Generation==std::numeric_limits<std::uint64_t>::max())
                continue;
            ++slot.Generation;
            if(!slot.Generation) continue;
            slot.State=CampaignState::InFlight;
            slot.EntryIndex=entryIndex;
            slot.Handle=handle;
            correlation=Correlation(i,slot.Generation);
            return true;
        }
        return false;
    }

    void ReleaseCampaign(std::uint64_t correlation) noexcept {
        std::size_t index=0;
        std::uint64_t generation=0;
        if(!DecodeCorrelation(correlation,index,generation)) return;
        std::lock_guard<std::mutex> lock(_campaignMutex);
        auto& slot=_campaigns[index];
        if(slot.State==CampaignState::Free||slot.Generation!=generation) return;
        const auto keep=slot.Generation;
        slot=Campaign{};
        slot.Generation=keep;
    }

    static bool EvidenceEstablished(
        const Entry& entry,Adapters::AdapterEvidence evidence) noexcept {
        if(entry.Policy.Evidence==0)
            return evidence!=Adapters::AdapterEvidence::None;
        return evidence==Adapters::AdapterEvidence::DestinationPrimitiveAdmission;
    }

    static void FeedbackOutbound(
        void* owner,const Adapters::AdapterFamilyFeedback& feedback) noexcept {
        static_cast<StateMeshAdapterFamilyBinding*>(owner)->OnFeedback(feedback);
    }

    void OnFeedback(const Adapters::AdapterFamilyFeedback& feedback) noexcept {
        std::size_t index=0;
        std::uint64_t generation=0;
        if(!DecodeCorrelation(feedback.Correlation,index,generation)) return;

        bool wake=false;
        {
            std::lock_guard<std::mutex> lock(_campaignMutex);
            auto& slot=_campaigns[index];
            if(slot.State!=CampaignState::InFlight||
               slot.Generation!=generation||slot.EntryIndex>=_count)
                return;

            if(EvidenceEstablished(_entries[slot.EntryIndex],feedback.Evidence)) {
                const auto keep=slot.Generation;
                slot=Campaign{};
                slot.Generation=keep;
            } else {
                slot.State=CampaignState::ExhaustionPending;
                wake=true;
            }
        }
        if(wake) _wake.Signal();
    }

    static Adapters::AdapterEncodeResult EncodeOutbound(
        void* owner,
        Primitive::PrimitiveProtocolVersion protocol,
        std::uint64_t,
        const void* source,
        Adapters::AdapterMutableByteView output) noexcept {
        auto& self=*static_cast<StateMeshAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=State::StateProtocolVersion||!source||!output.Data)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};

        const auto& outbound=*static_cast<const OutboundSource*>(source);
        if(outbound.EntryIndex>=self._count||!outbound.Message)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& entry=self._entries[outbound.EntryIndex];
        if(!entry.Used||!entry.Encode)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};

        const auto encoded=entry.Encode(outbound.Message,output.Data,output.Capacity);
        return {Detail::MapStateWireEncode(encoded.Status),encoded.Bytes};
    }

    static Primitive::PrimitiveAdmissionDisposition AdmitInbound(
        void* owner,
        Primitive::PrimitiveProtocolVersion protocol,
        Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<StateMeshAdapterFamilyBinding*>(owner);
        using P=Primitive::PrimitiveAdmissionDisposition;
        if(!self._frozen||protocol!=State::StateProtocolVersion||!bytes.Data||bytes.Size<5)
            return P::Malformed;

        const auto parsed=Detail::ParseStateEnvelope(bytes);
        if(!parsed.Parsed)
            return parsed.Status==State::StateWireStatus::UnsupportedProtocol?P::Unsupported:P::Malformed;
        const auto* entry=self.Find(parsed.TypeId);
        if(!entry||!entry->Runtime||!entry->Admit) return P::Unsupported;
        if(bytes.Size>entry->MaximumWireBytes) return P::Malformed;
        if(!provenance.OriginalSource||
           provenance.OriginalSource.Identity!=parsed.SemanticSource)
            return P::Rejected;

        return entry->Admit(
            *entry->Runtime,bytes.Data,bytes.Size,
            State::StateValidatedIngressContext{provenance.OriginalSource.Identity}).Disposition;
    }

    static MeshAdapterPolicyResolution ResolvePolicy(
        void* owner,
        Primitive::PrimitiveProtocolVersion protocol,
        const Mesh::MeshReceiveContext& context,
        Adapters::AdapterByteView bytes,
        Primitive::PrimitivePolicyDescriptor& policy,
        Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<StateMeshAdapterFamilyBinding*>(owner);
        provenance={};
        if(!self._frozen) return MeshAdapterPolicyResolution::Rejected;
        if(protocol!=State::StateProtocolVersion) return MeshAdapterPolicyResolution::Unsupported;
        if(context.Broadcast) return MeshAdapterPolicyResolution::Rejected;

        const auto parsed=Detail::ParseStateEnvelope(bytes);
        if(!parsed.Parsed)
            return parsed.Status==State::StateWireStatus::UnsupportedProtocol
                ?MeshAdapterPolicyResolution::Unsupported
                :MeshAdapterPolicyResolution::Malformed;
        const auto* entry=self.Find(parsed.TypeId);
        if(!entry) return MeshAdapterPolicyResolution::Unsupported;
        if(context.Service!=entry->RelayService) return MeshAdapterPolicyResolution::Rejected;
        if(bytes.Size>entry->MaximumWireBytes) return MeshAdapterPolicyResolution::Malformed;
        if(parsed.SemanticSource.Device!=context.Source) return MeshAdapterPolicyResolution::Rejected;

        provenance.OriginalSource={parsed.SemanticSource,true};
        policy=entry->Policy;
        return MeshAdapterPolicyResolution::Resolved;
    }

public:
    StateMeshAdapterFamilyBinding() noexcept=default;
    StateMeshAdapterFamilyBinding(const StateMeshAdapterFamilyBinding&)=delete;
    StateMeshAdapterFamilyBinding& operator=(const StateMeshAdapterFamilyBinding&)=delete;

    template<class TAdapterRuntime>
    StateMeshAdapterBindingStatus BindAdapter(
        TAdapterRuntime& adapter,
        MeshRouteBinding routes,
        StateMeshAdapterServiceWake wake) noexcept {
        if(_frozen) return StateMeshAdapterBindingStatus::Frozen;
        if(_adapterOwner) return StateMeshAdapterBindingStatus::InvalidBinding;
        if(!routes.IsValid()||!wake) return StateMeshAdapterBindingStatus::RouteUnavailable;

        _adapterOwner=&adapter;
        _routes=routes;
        _wake=wake;
        _submit=[](
            void* owner,
            Primitive::PrimitiveFamilyId family,
            Adapters::AdapterServiceClass service,
            Primitive::PrimitiveProtocolVersion protocol,
            const void* source,
            Adapters::AdapterRouteToken route,
            Primitive::PrimitivePolicyDescriptor policy,
            std::uint64_t correlation) noexcept {
            return static_cast<TAdapterRuntime*>(owner)->SubmitOutbound(
                family,service,protocol,source,route,policy,correlation);
        };
        return StateMeshAdapterBindingStatus::Success;
    }

    template<class TState,class TFormat>
    StateMeshAdapterBindingStatus BindType(
        TStateRuntime& runtime,
        Mesh::MeshRelayServiceClass service=Mesh::MeshRelayServiceClass::Convergent) noexcept {
        static_assert(TState::IsTransmissibleState&&TState::ValidateTier());
        static_assert(
            std::is_same_v<TFormat,Serializable::DirectBinary>||
            std::is_same_v<TFormat,Serializable::CBOR>||
            std::is_same_v<TFormat,Serializable::JSON>);

        if(_frozen) return StateMeshAdapterBindingStatus::Frozen;
        if(!Mesh::IsMeshRelayServiceClass(service)) return StateMeshAdapterBindingStatus::InvalidService;
        if(Find(TState::TypeId)) return StateMeshAdapterBindingStatus::DuplicateType;
        if(_count==_entries.size()) return StateMeshAdapterBindingStatus::ResourceUnavailable;

        const auto common=TState::GetPrimitiveTypeDescriptor();
        const auto* descriptor=State::GetStateTypeDescriptor(common);
        if(!descriptor||!descriptor->ConvergencePolicy)
            return StateMeshAdapterBindingStatus::InvalidBinding;

        constexpr auto format=State::Detail::StateBindingPayloadFormat<TFormat>();
        const auto formatIndex=std::is_same_v<TFormat,Serializable::DirectBinary>
            ?std::size_t{0}
            :(std::is_same_v<TFormat,Serializable::CBOR>?std::size_t{1}:std::size_t{2});
        std::size_t maximum=descriptor->MaximumPublicationWireBytes[formatIndex];
        if(descriptor->MaximumSnapshotControlWireBytes[formatIndex]>maximum)
            maximum=descriptor->MaximumSnapshotControlWireBytes[formatIndex];
        if(State::StateControlWireHeaderSize>maximum)
            maximum=State::StateControlWireHeaderSize;
        if(State::StateAcceptanceControlWireHeaderSize>maximum)
            maximum=State::StateAcceptanceControlWireHeaderSize;
        if(!maximum) return StateMeshAdapterBindingStatus::InvalidBinding;

        auto& entry=_entries[_count++];
        entry.TypeId=TState::TypeId;
        entry.Runtime=&runtime;
        entry.Policy=*descriptor->ConvergencePolicy;
        entry.RelayService=service;
        entry.Format=format;
        entry.MaximumWireBytes=maximum;
        entry.Admit=[](
            TStateRuntime& target,
            const std::uint8_t* data,
            std::size_t size,
            State::StateValidatedIngressContext ingress) noexcept {
            return target.template AdmitRemote<TState,TFormat>(data,size,ingress);
        };
        entry.Encode=[](const void* source,std::uint8_t* output,std::size_t capacity) {
            return Detail::EncodeStateOutboundMessage<TState,TFormat>(
                *static_cast<const State::StateOutboundMessage<TState>*>(source),output,capacity);
        };
        entry.ReportExhausted=[](
            TStateRuntime& target,const State::StateConvergenceHandle& handle) noexcept {
            return target.template ReportConvergenceExhausted<TState>(handle);
        };
        entry.ServiceLatest=[](TStateRuntime& target) noexcept {
            return target.template ServiceLatest<TState>();
        };
        entry.Used=true;

        if(maximum>_maximumInboundBytes) _maximumInboundBytes=maximum;
        if(maximum>_maximumOutboundBytes) _maximumOutboundBytes=maximum;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(service));
        _requiresDestinationEvidence=
            _requiresDestinationEvidence||entry.Policy.Evidence!=0;
        return StateMeshAdapterBindingStatus::Success;
    }

    template<class TState,class TFormat>
    bool InitializeTransport(State::StateTransportBinding<TState,TFormat>& binding) noexcept {
        if(_frozen||!_adapterOwner||!Find(TState::TypeId)) return false;
        return binding.template Initialize<
            StateMeshAdapterFamilyBinding,
            &StateMeshAdapterFamilyBinding::template AdmitOutbound<TState>,
            &StateMeshAdapterFamilyBinding::template ValidateTransport<TState,TFormat>,
            &StateMeshAdapterFamilyBinding::Wake>(*this);
    }

    template<class TState,class TFormat>
    bool ValidateTransport(const State::StateTransportContract& contract) noexcept {
        const auto* entry=Find(TState::TypeId);
        return !_frozen&&_adapterOwner&&_submit&&_routes.IsValid()&&
            entry&&entry->Runtime&&entry->Encode&&
            contract.TypeId==TState::TypeId&&
            contract.Format==State::Detail::StateBindingPayloadFormat<TFormat>()&&
            contract.MaximumPublicationWireBytes<=entry->MaximumWireBytes&&
            contract.MaximumSnapshotControlWireBytes<=entry->MaximumWireBytes&&
            contract.ConvergencePolicy&&
            contract.ConvergencePolicy->CanonicalBytes()==entry->Policy.CanonicalBytes();
    }

    template<class TState>
    State::StateTransportAdmission AdmitOutbound(
        const State::StateOutboundMessage<TState>& message) noexcept {
        if(!_frozen||!_adapterOwner||!_submit||!State::IsValidStateMessageKind(message.Kind))
            return {State::StateTransportAdmissionStatus::Quiesced};
        auto* entry=Find(TState::TypeId);
        if(!entry||!entry->Runtime||!entry->Encode)
            return {State::StateTransportAdmissionStatus::InvalidDestination};

        System::DeviceRuntimeIdentity local{};
        if(!System::RuntimeIdentity::TryRead(local))
            return {State::StateTransportAdmissionStatus::InvalidDestination};
        const auto sourceRole=State::StateMessageSourceRole(message.Kind);
        const auto source=sourceRole==State::StateSemanticSourceRole::Owner
            ?message.Owner:message.Requester;
        const auto destination=sourceRole==State::StateSemanticSourceRole::Owner
            ?message.Requester:message.Owner;
        if(source!=local||!destination.Device)
            return {State::StateTransportAdmissionStatus::InvalidDestination};

        Adapters::AdapterRouteToken route{};
        if(!_routes.TryResolveNode(destination.Device,route))
            return {State::StateTransportAdmissionStatus::InvalidDestination};

        std::uint64_t correlation=0;
        const auto handle=message.GetConvergenceHandle();
        const auto entryIndex=IndexOf(entry);
        if(!ReserveCampaign(entryIndex,handle,correlation))
            return {State::StateTransportAdmissionStatus::CapacityUnavailable};

        const OutboundSource encodedSource{entryIndex,&message};
        const auto submitted=_submit(
            _adapterOwner,Primitive::FamilyIds::State,
            ToAdapterServiceClass(entry->RelayService),State::StateProtocolVersion,
            &encodedSource,route,entry->Policy,correlation);
        if(submitted==Adapters::AdapterSubmissionDisposition::Accepted)
            return {State::StateTransportAdmissionStatus::Accepted};
        if(correlation) ReleaseCampaign(correlation);

        switch(submitted) {
            case Adapters::AdapterSubmissionDisposition::Busy:
            case Adapters::AdapterSubmissionDisposition::ResourceUnavailable:
                return {State::StateTransportAdmissionStatus::CapacityUnavailable};
            case Adapters::AdapterSubmissionDisposition::NotRunning:
                return {State::StateTransportAdmissionStatus::Quiesced};
            default:
                return {State::StateTransportAdmissionStatus::InvalidDestination};
        }
    }

    void Wake() noexcept { _wake.Signal(); }

    /// <summary>Runs one bounded composition service quantum: terminal convergence feedback first, then one State pass per Type.</summary>
    bool Service() noexcept {
        bool progressed=false;
        for(std::size_t i=0;i<_campaigns.size();++i) {
            State::StateConvergenceHandle handle{};
            std::size_t entryIndex=TMaximumTypes;
            std::uint64_t generation=0;
            {
                std::unique_lock<std::mutex> lock(_campaignMutex,std::try_to_lock);
                if(!lock.owns_lock()) {
                    _wake.Signal();
                    break;
                }
                const auto& slot=_campaigns[i];
                if(slot.State!=CampaignState::ExhaustionPending||slot.EntryIndex>=_count)
                    continue;
                handle=slot.Handle;
                entryIndex=slot.EntryIndex;
                generation=slot.Generation;
            }

            const auto status=_entries[entryIndex].ReportExhausted(
                *_entries[entryIndex].Runtime,handle);
            if(status==State::StateRemoteStatus::Busy) {
                _wake.Signal();
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(_campaignMutex);
                auto& slot=_campaigns[i];
                if(slot.State==CampaignState::ExhaustionPending&&
                   slot.Generation==generation) {
                    const auto keep=slot.Generation;
                    slot=Campaign{};
                    slot.Generation=keep;
                    progressed=true;
                }
            }
        }

        for(std::size_t i=0;i<_count;++i) {
            auto& entry=_entries[i];
            if(entry.Used&&entry.Runtime&&entry.ServiceLatest&&
               entry.ServiceLatest(*entry.Runtime))
                progressed=true;
        }
        return progressed;
    }

    StateMeshAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return StateMeshAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_serviceMask==0)
            return StateMeshAdapterBindingStatus::InvalidBinding;
        if(_adapterOwner&&(!_submit||!_routes.IsValid()||!_wake))
            return StateMeshAdapterBindingStatus::InvalidBinding;
        _frozen=true;
        return StateMeshAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::State;
        binding.Protocols={State::StateProtocolVersion,State::StateProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.MaximumOutboundBytes=_adapterOwner?_maximumOutboundBytes:0;
        binding.ServiceClassMask=_serviceMask;
        binding.RequiresDestinationAdmissionEvidence=
            _adapterOwner&&_requiresDestinationEvidence;
        binding.RequiresValidatedOriginalSource=true;
        binding.Owner=this;
        binding.AdmitInbound=&StateMeshAdapterFamilyBinding::AdmitInbound;
        binding.EncodeOutbound=_adapterOwner?&StateMeshAdapterFamilyBinding::EncodeOutbound:nullptr;
        binding.Feedback=_adapterOwner?&StateMeshAdapterFamilyBinding::FeedbackOutbound:nullptr;
        return binding;
    }

    MeshAdapterPolicyBinding MeshPolicyBinding() noexcept {
        if(!_frozen) return {};
        return {
            Primitive::FamilyIds::State,
            {State::StateProtocolVersion,State::StateProtocolVersion},
            this,&StateMeshAdapterFamilyBinding::ResolvePolicy};
    }
};

} // namespace ESPressio::MeshAdapters
