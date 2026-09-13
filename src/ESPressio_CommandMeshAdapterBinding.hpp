#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>

#include <ESPressio_AdapterBinding.hpp>
#include <ESPressio_CommandRuntime.hpp>
#include <ESPressio_CommandWireV1.hpp>

#include "ESPressio_MeshAdapterIngress.hpp"
#include "ESPressio_MeshRouteBinding.hpp"

namespace ESPressio::MeshAdapters {

/// <summary>Configuration result for one frozen Command family Mesh/A2 binding.</summary>
enum class CommandMeshAdapterBindingStatus : std::uint8_t {
    Success=0,
    Frozen,
    DuplicateType,
    ResourceUnavailable,
    InvalidBinding,
    InvalidService,
    RouteUnavailable
};

namespace Detail {

constexpr Primitive::PrimitiveAdmissionDisposition MapCommandRemoteAdmission(
    Command::CommandRemoteAdmissionStatus status) noexcept {
    using S=Command::CommandRemoteAdmissionStatus;
    using P=Primitive::PrimitiveAdmissionDisposition;
    switch(status) {
        case S::Admitted: return P::Accepted;
        case S::DuplicateTerminal:
        case S::StaleOriginRuntime:
        case S::ExecutionHistoryExpired: return P::AlreadyAccepted;
        case S::InProgress:
        case S::TemporarilyUnavailable: return P::TemporarilyUnavailable;
        case S::LedgerCapacityUnavailable: return P::ResourceUnavailable;
        case S::UnknownType:
        case S::UnsupportedProtocol: return P::Unsupported;
        case S::NoActiveRequester: return P::Rejected;
        case S::SchemaOrDecodeFailure:
        case S::Invalid: return P::Malformed;
    }
    return P::Rejected;
}

constexpr bool CommandAdmissionRetainsResponseDestination(
    Command::CommandRemoteAdmissionStatus status) noexcept {
    using S=Command::CommandRemoteAdmissionStatus;
    return status==S::Admitted || status==S::DuplicateTerminal ||
           status==S::StaleOriginRuntime || status==S::ExecutionHistoryExpired;
}

constexpr Adapters::AdapterResourceStatus MapCommandWireEncode(Command::CommandWireStatus status) noexcept {
    using S=Command::CommandWireStatus;
    switch(status) {
        case S::Success: return Adapters::AdapterResourceStatus::Success;
        case S::InsufficientOutput:
        case S::PayloadTooLarge: return Adapters::AdapterResourceStatus::TooLarge;
        case S::InvalidHeader:
        case S::UnsupportedProtocol:
        case S::UnknownType:
        case S::InvalidLength:
        case S::SchemaOrDecodeFailure: return Adapters::AdapterResourceStatus::InvalidConfiguration;
    }
    return Adapters::AdapterResourceStatus::InvalidConfiguration;
}

} // namespace Detail

/// <summary>
/// Frozen Command-family Mesh binding preserving Command execution/replay/response semantics while A2 owns byte pursuit.
/// </summary>
/// <remarks>
/// The binding has no Command registry, response worker, retry loop or dynamic callable. Per-Type entries are fixed before
/// Freeze and delegate inbound work to the real Command::Runtime. Response-bearing inbound requests receive a bounded
/// pre-reserved CommandRemoteResponseDestination. When Command later routes a retained typed response to that destination,
/// the destination synchronously offers it to A2; successful A2 ownership ends Command-side response storage ownership and
/// A2 then owns the response P2 delivery campaign. Generic Mesh broadcast rejects every response and every response-bearing
/// request, and permits a no-response request only when its frozen request policy is NoRemoteEvidence.
/// </remarks>
template<class TAdapterRuntime,std::size_t TMaximumTypes,std::size_t TMaximumResponseDestinations>
class CommandMeshAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"Command MeshAdapter Type capacity must be non-zero");
    static_assert(TMaximumResponseDestinations>0,"Command MeshAdapter response destination capacity must be non-zero");
    static_assert(TMaximumResponseDestinations<=UINT16_MAX,"Command response destination capacity must fit index");

    struct OutboundResponseSource final {
        std::size_t EntryIndex{0};
        Command::CommandExecutionKey Key{};
        System::DeviceRuntimeIdentity Executor{};
        Command::CommandResponseDisposition Disposition{Command::CommandResponseDisposition::Succeeded};
        const void* Payload{nullptr};
    };

    using EncodeResponseThunk=Command::CommandWireResult(*)(
        const OutboundResponseSource&,std::uint8_t*,std::size_t);

    struct Entry final {
        Command::CommandTypeId TypeId{};
        Command::Runtime* Runtime{nullptr};
        Command::CommandInboundBinding Inbound{};
        Primitive::PrimitivePolicyDescriptor RequestPolicy{};
        Primitive::PrimitivePolicyDescriptor ResponsePolicy{};
        Mesh::MeshRelayServiceClass RequestService{Mesh::MeshRelayServiceClass::BestEffort};
        Mesh::MeshRelayServiceClass ResponseService{Mesh::MeshRelayServiceClass::Responsive};
        std::size_t MaximumRequestWireBytes{0};
        std::size_t MaximumResponseWireBytes{0};
        EncodeResponseThunk EncodeResponse{nullptr};
        bool ResponseBearing{false};
        bool Used{false};
    };

    enum class DestinationState : std::uint8_t { Free=0,Reserved };
    struct ResponseDestination final {
        DestinationState State{DestinationState::Free};
        std::uint64_t Generation{0};
        std::size_t EntryIndex{0};
        Adapters::AdapterRouteToken Route{};
    };

    TAdapterRuntime* _adapter{nullptr};
    MeshRouteBinding _routes{};
    std::array<Entry,TMaximumTypes> _entries{};
    std::array<ResponseDestination,TMaximumResponseDestinations> _destinations{};
    std::size_t _count{0};
    std::size_t _maximumInboundBytes{0};
    std::size_t _maximumOutboundBytes{0};
    std::uint8_t _serviceMask{0};
    bool _requiresDestinationEvidence{false};
    bool _hasResponseBearing{false};
    bool _frozen{false};
    std::mutex _destinationMutex{};

    static constexpr std::uint8_t ServiceBit(Mesh::MeshRelayServiceClass service) noexcept {
        const auto mapped=ToAdapterServiceClass(service);
        return static_cast<std::uint8_t>(std::uint8_t{1}<<static_cast<std::uint8_t>(mapped));
    }

    const Entry* Find(Command::CommandTypeId type) const noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }
    Entry* Find(Command::CommandTypeId type) noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }
    std::size_t IndexOf(const Entry* entry) const noexcept {
        return entry?static_cast<std::size_t>(entry-_entries.data()):TMaximumTypes;
    }

    static std::uint64_t Correlation(std::size_t slot,std::uint64_t generation) noexcept {
        return (generation<<16U)|static_cast<std::uint64_t>(slot+1U);
    }

    Command::CommandRemoteResponseDestination ReserveResponseDestination(
        std::size_t entryIndex,Adapters::AdapterRouteToken route) noexcept {
        if(entryIndex>=_count||!route) return {};
        std::unique_lock<std::mutex> lock(_destinationMutex,std::try_to_lock);
        if(!lock.owns_lock()) return {};
        for(std::size_t i=0;i<_destinations.size();++i) {
            auto& slot=_destinations[i];
            if(slot.State!=DestinationState::Free||slot.Generation==std::numeric_limits<std::uint64_t>::max()) continue;
            ++slot.Generation;
            if(slot.Generation==0) continue;
            slot.State=DestinationState::Reserved;
            slot.EntryIndex=entryIndex;
            slot.Route=route;
            return {this,static_cast<std::uint16_t>(i),slot.Generation,&CommandMeshAdapterFamilyBinding::AcceptResponseThunk};
        }
        return {};
    }

    void ReleaseResponseDestination(std::uint16_t index,std::uint64_t generation) noexcept {
        std::lock_guard<std::mutex> lock(_destinationMutex);
        if(index>=_destinations.size()) return;
        auto& slot=_destinations[index];
        if(slot.State!=DestinationState::Reserved||slot.Generation!=generation) return;
        slot.State=DestinationState::Free;
        slot.EntryIndex=0;
        slot.Route={};
    }

    static bool AcceptResponseThunk(
        void* owner,std::uint16_t index,std::uint64_t generation,
        const Command::CommandExecutionKey& key,
        const System::DeviceRuntimeIdentity& executor,
        Command::CommandResponseDisposition disposition,
        Command::CommandResponsePayloadLease&& payload) noexcept {
        return static_cast<CommandMeshAdapterFamilyBinding*>(owner)->AcceptResponse(
            index,generation,key,executor,disposition,std::move(payload));
    }

    bool AcceptResponse(
        std::uint16_t index,std::uint64_t generation,
        const Command::CommandExecutionKey& key,
        const System::DeviceRuntimeIdentity& executor,
        Command::CommandResponseDisposition disposition,
        Command::CommandResponsePayloadLease&& payload) noexcept {
        std::size_t entryIndex=0;
        Adapters::AdapterRouteToken route{};
        {
            std::unique_lock<std::mutex> lock(_destinationMutex,std::try_to_lock);
            if(!lock.owns_lock()||index>=_destinations.size()) return false;
            auto& slot=_destinations[index];
            if(slot.State!=DestinationState::Reserved||slot.Generation!=generation||slot.EntryIndex>=_count) return false;
            entryIndex=slot.EntryIndex;
            route=slot.Route;
        }
        const auto& entry=_entries[entryIndex];
        if(!entry.Used||!entry.ResponseBearing||entry.TypeId!=key.TypeId||!entry.EncodeResponse||!route) {
            ReleaseResponseDestination(index,generation);
            return false;
        }
        if(disposition==Command::CommandResponseDisposition::Succeeded&&!payload.Payload()) {
            ReleaseResponseDestination(index,generation);
            return false;
        }

        const OutboundResponseSource source{entryIndex,key,executor,disposition,payload.Payload()};
        const auto correlation=Correlation(index,generation);
        const auto submitted=_adapter->SubmitOutbound(
            Primitive::FamilyIds::Command,ToAdapterServiceClass(entry.ResponseService),Command::CommandProtocolVersion,
            &source,route,entry.ResponsePolicy,correlation);
        ReleaseResponseDestination(index,generation);
        return submitted==Adapters::AdapterSubmissionDisposition::Accepted;
    }

    static Adapters::AdapterEncodeResult EncodeOutbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,std::uint64_t,
        const void* source,Adapters::AdapterMutableByteView output) noexcept {
        auto& self=*static_cast<CommandMeshAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=Command::CommandProtocolVersion||!source||!output.Data)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& response=*static_cast<const OutboundResponseSource*>(source);
        if(response.EntryIndex>=self._count) return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& entry=self._entries[response.EntryIndex];
        if(!entry.Used||!entry.ResponseBearing||!entry.EncodeResponse)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto encoded=entry.EncodeResponse(response,output.Data,output.Capacity);
        return {Detail::MapCommandWireEncode(encoded.Status),encoded.Bytes};
    }

    static Primitive::PrimitiveAdmissionDisposition AdmitInbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<CommandMeshAdapterFamilyBinding*>(owner);
        using P=Primitive::PrimitiveAdmissionDisposition;
        if(!self._frozen||protocol!=Command::CommandProtocolVersion||!bytes.Data||bytes.Size<5)
            return P::Malformed;

        const auto kind=static_cast<Command::CommandMessageKind>(bytes.Data[4]);
        if(kind==Command::CommandMessageKind::Request) {
            Command::CommandRequestWireHeader header{};
            const auto decoded=Command::DecodeCommandRequestHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol?P::Unsupported:P::Malformed;
            auto* entry=self.Find(header.Key.TypeId);
            if(!entry||!entry->Runtime||!entry->Inbound) return P::Unsupported;
            const System::DeviceRuntimeIdentity origin{header.Key.OriginDevice,header.Key.OriginRuntime};
            if(!provenance.OriginalSource||provenance.OriginalSource.Identity!=origin) return P::Rejected;

            Command::CommandRemoteResponseDestination destination{};
            if(entry->ResponseBearing) {
                if(!provenance.ImmediatePeer.Token) return P::Rejected;
                destination=self.ReserveResponseDestination(
                    self.IndexOf(entry),Adapters::AdapterRouteToken{provenance.ImmediatePeer.Token});
                if(!destination) return P::ResourceUnavailable;
            }
            const auto admitted=entry->Runtime->TryAdmitRemoteRequest(entry->Inbound,bytes.Data,bytes.Size,destination);
            if(entry->ResponseBearing&&!Detail::CommandAdmissionRetainsResponseDestination(admitted.Status))
                self.ReleaseResponseDestination(destination.Index,destination.Generation);
            return Detail::MapCommandRemoteAdmission(admitted.Status);
        }

        if(kind==Command::CommandMessageKind::Response) {
            Command::CommandResponseWireHeader header{};
            const auto decoded=Command::DecodeCommandResponseHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol?P::Unsupported:P::Malformed;
            auto* entry=self.Find(header.Key.TypeId);
            if(!entry||!entry->Runtime||!entry->Inbound||!entry->ResponseBearing) return P::Unsupported;
            if(!provenance.OriginalSource||provenance.OriginalSource.Identity!=header.Executor) return P::Rejected;
            return Detail::MapCommandRemoteAdmission(
                entry->Runtime->TryAdmitRemoteResponse(entry->Inbound,bytes.Data,bytes.Size).Status);
        }
        return P::Malformed;
    }

    static MeshAdapterPolicyResolution ResolvePolicy(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,const Mesh::MeshReceiveContext& context,
        Adapters::AdapterByteView bytes,Primitive::PrimitivePolicyDescriptor& policy,
        Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<CommandMeshAdapterFamilyBinding*>(owner);
        provenance={};
        if(!self._frozen) return MeshAdapterPolicyResolution::Rejected;
        if(protocol!=Command::CommandProtocolVersion) return MeshAdapterPolicyResolution::Unsupported;
        if(!bytes.Data||bytes.Size<5) return MeshAdapterPolicyResolution::Malformed;

        const auto kind=static_cast<Command::CommandMessageKind>(bytes.Data[4]);
        if(kind==Command::CommandMessageKind::Request) {
            Command::CommandRequestWireHeader header{};
            const auto decoded=Command::DecodeCommandRequestHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol
                ?MeshAdapterPolicyResolution::Unsupported:MeshAdapterPolicyResolution::Malformed;
            const auto* entry=self.Find(header.Key.TypeId);
            if(!entry) return MeshAdapterPolicyResolution::Unsupported;
            if(context.Service!=entry->RequestService) return MeshAdapterPolicyResolution::Rejected;
            if(bytes.Size>entry->MaximumRequestWireBytes) return MeshAdapterPolicyResolution::Malformed;
            const System::DeviceRuntimeIdentity origin{header.Key.OriginDevice,header.Key.OriginRuntime};
            if(origin.Device!=context.Source) return MeshAdapterPolicyResolution::Rejected;
            if(context.Broadcast&&(entry->ResponseBearing||entry->RequestPolicy.Evidence!=0U))
                return MeshAdapterPolicyResolution::Rejected;
            provenance.OriginalSource={origin,true};
            if(entry->ResponseBearing) {
                Adapters::AdapterRouteToken route{};
                if(!self._routes.TryResolveNode(context.Source,route)) return MeshAdapterPolicyResolution::Rejected;
                provenance.ImmediatePeer.Token=route.Value;
            }
            policy=entry->RequestPolicy;
            return MeshAdapterPolicyResolution::Resolved;
        }

        if(kind==Command::CommandMessageKind::Response) {
            if(context.Broadcast) return MeshAdapterPolicyResolution::Rejected;
            Command::CommandResponseWireHeader header{};
            const auto decoded=Command::DecodeCommandResponseHeader(bytes.Data,bytes.Size,header);
            if(!decoded) return decoded.Status==Command::CommandWireStatus::UnsupportedProtocol
                ?MeshAdapterPolicyResolution::Unsupported:MeshAdapterPolicyResolution::Malformed;
            const auto* entry=self.Find(header.Key.TypeId);
            if(!entry||!entry->ResponseBearing) return MeshAdapterPolicyResolution::Unsupported;
            if(context.Service!=entry->ResponseService) return MeshAdapterPolicyResolution::Rejected;
            if(bytes.Size>entry->MaximumResponseWireBytes) return MeshAdapterPolicyResolution::Malformed;
            if(header.Executor.Device!=context.Source) return MeshAdapterPolicyResolution::Rejected;
            provenance.OriginalSource={header.Executor,true};
            policy=entry->ResponsePolicy;
            return MeshAdapterPolicyResolution::Resolved;
        }
        return MeshAdapterPolicyResolution::Malformed;
    }

public:
    CommandMeshAdapterFamilyBinding(TAdapterRuntime& adapter,MeshRouteBinding routes={}) noexcept
        :_adapter(&adapter),_routes(routes) {}
    CommandMeshAdapterFamilyBinding(const CommandMeshAdapterFamilyBinding&)=delete;
    CommandMeshAdapterFamilyBinding& operator=(const CommandMeshAdapterFamilyBinding&)=delete;

    /// <summary>Adds one real initialized Command Runtime inbound binding before freeze.</summary>
    template<class TCommand,class TFormat>
    CommandMeshAdapterBindingStatus BindType(
        Command::Runtime& runtime,Mesh::MeshRelayServiceClass requestService,
        Mesh::MeshRelayServiceClass responseService=Mesh::MeshRelayServiceClass::Responsive) noexcept {
        static_assert(TCommand::IsTransmissibleCommand&&TCommand::ValidateTier(),
                      "Mesh Command binding requires a TransmissibleCommand");
        static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                      std::is_same_v<TFormat,Serializable::CBOR>||
                      std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Command MeshAdapter format");
        if(_frozen) return CommandMeshAdapterBindingStatus::Frozen;
        if(!Mesh::IsMeshRelayServiceClass(requestService)||!Mesh::IsMeshRelayServiceClass(responseService))
            return CommandMeshAdapterBindingStatus::InvalidService;
        if(Find(TCommand::TypeId)) return CommandMeshAdapterBindingStatus::DuplicateType;
        if(_count==_entries.size()) return CommandMeshAdapterBindingStatus::ResourceUnavailable;
        const auto inbound=runtime.template BindInbound<TCommand,TFormat>();
        if(!inbound) return CommandMeshAdapterBindingStatus::InvalidBinding;

        auto& entry=_entries[_count];
        entry.TypeId=TCommand::TypeId;
        entry.Runtime=&runtime;
        entry.Inbound=inbound;
        entry.RequestPolicy=Primitive::PrimitivePolicyContract<typename TCommand::RequestDeliveryPolicy>::Descriptor();
        entry.RequestService=requestService;
        entry.MaximumRequestWireBytes=Command::MaximumCompleteRequestWireBytes<TCommand,TFormat>;
        entry.ResponseBearing=!std::is_same_v<typename TCommand::ResponseType,Command::NoCommandResponse>;
        if constexpr(!std::is_same_v<typename TCommand::ResponseType,Command::NoCommandResponse>) {
            entry.ResponsePolicy=Primitive::PrimitivePolicyContract<typename TCommand::ResponseDeliveryPolicy>::Descriptor();
            entry.ResponseService=responseService;
            entry.MaximumResponseWireBytes=Command::MaximumCompleteResponseWireBytes<TCommand,TFormat>;
            entry.EncodeResponse=[](const OutboundResponseSource& source,std::uint8_t* output,std::size_t capacity) {
                return Command::EncodeCommandResponse<TCommand,TFormat>(
                    source.Key,source.Executor,source.Disposition,
                    static_cast<const typename TCommand::ResponseType*>(source.Payload),output,capacity);
            };
            _hasResponseBearing=true;
            if(entry.MaximumResponseWireBytes>_maximumOutboundBytes) _maximumOutboundBytes=entry.MaximumResponseWireBytes;
            _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(responseService));
            _requiresDestinationEvidence=_requiresDestinationEvidence||entry.ResponsePolicy.Evidence!=0U;
        }
        entry.Used=true;
        ++_count;
        if(entry.MaximumRequestWireBytes>_maximumInboundBytes) _maximumInboundBytes=entry.MaximumRequestWireBytes;
        if(entry.MaximumResponseWireBytes>_maximumInboundBytes) _maximumInboundBytes=entry.MaximumResponseWireBytes;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(requestService));
        return CommandMeshAdapterBindingStatus::Success;
    }

    /// <summary>Freezes the non-empty Type table and validates reply routing for every response-bearing Type.</summary>
    CommandMeshAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return CommandMeshAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_serviceMask==0||_adapter==nullptr)
            return CommandMeshAdapterBindingStatus::InvalidBinding;
        if(_hasResponseBearing&&!_routes.IsValid()) return CommandMeshAdapterBindingStatus::RouteUnavailable;
        _frozen=true;
        return CommandMeshAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    /// <summary>Returns the frozen A2 Command family descriptor.</summary>
    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::Command;
        binding.Protocols={Command::CommandProtocolVersion,Command::CommandProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.MaximumOutboundBytes=_maximumOutboundBytes;
        binding.ServiceClassMask=_serviceMask;
        binding.RequiresDestinationAdmissionEvidence=_requiresDestinationEvidence;
        binding.RequiresValidatedOriginalSource=true;
        binding.Owner=this;
        binding.AdmitInbound=&CommandMeshAdapterFamilyBinding::AdmitInbound;
        binding.EncodeOutbound=_maximumOutboundBytes?&CommandMeshAdapterFamilyBinding::EncodeOutbound:nullptr;
        return binding;
    }

    /// <summary>Returns the authenticated Mesh ingress resolver paired with the frozen Command binding.</summary>
    MeshAdapterPolicyBinding MeshPolicyBinding() noexcept {
        if(!_frozen) return {};
        return {Primitive::FamilyIds::Command,{Command::CommandProtocolVersion,Command::CommandProtocolVersion},
                this,&CommandMeshAdapterFamilyBinding::ResolvePolicy};
    }
};

} // namespace ESPressio::MeshAdapters
