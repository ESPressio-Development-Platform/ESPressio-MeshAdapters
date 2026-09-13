#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ESPressio_AdapterBinding.hpp>
#include <ESPressio_EventRuntime.hpp>
#include <ESPressio_EventWire.hpp>

#include "ESPressio_MeshAdapterIngress.hpp"

namespace ESPressio::MeshAdapters {

/// <summary>Configuration result for one frozen Event family Mesh/A2 binding.</summary>
enum class EventMeshAdapterBindingStatus : std::uint8_t {
    Success=0,
    Frozen,
    DuplicateType,
    ResourceUnavailable,
    InvalidBinding,
    InvalidService
};

/// <summary>Borrowed synchronous source used only while A2 copies one local Event occurrence into Adapter-owned bytes.</summary>
/// <remarks>The Event lease is never retained by A2 or MeshAdapters; AdapterRuntime::SubmitOutbound invokes the encoder
/// synchronously before returning ownership status to the Event external target.</remarks>
struct EventMeshAdapterOutboundSource final {
    Event::EventTypeId TypeId{};
    const Event::EventLease* Occurrence{nullptr};
};

namespace Detail {
constexpr Adapters::AdapterResourceStatus MapEventWireEncode(Event::EventWireStatus status) noexcept {
    using S=Event::EventWireStatus;
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
/// Fixed pre-freeze Event Type table used by both Mesh policy validation and the A2 Event family admission/encoding thunks.
/// </summary>
/// <remarks>
/// Every entry stores the real Event::Runtime inbound binding plus one compile-time typed encoder. Inbound A2 execution
/// delegates to Event::Runtime::TryAdmitRemote and preserves the family's receipt/idempotency, local-origin replay
/// rejection, exact wire limits and Type runtime semantics. Outbound encoding copies directly from a borrowed EventLease
/// into Adapter-owned bytes during SubmitOutbound; the adapter never retains an Event object or lease.
/// </remarks>
template<std::size_t TMaximumTypes>
class EventMeshAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"Event MeshAdapter Type capacity must be non-zero");

    using EncodeThunk=Event::EventWireResult(*)(const Event::EventLease&,std::uint8_t*,std::size_t);

    struct Entry final {
        Event::EventTypeId TypeId{};
        Event::Runtime* Runtime{nullptr};
        Event::EventInboundBinding Inbound{};
        Primitive::PrimitivePolicyDescriptor Policy{};
        Mesh::MeshRelayServiceClass Service{Mesh::MeshRelayServiceClass::BestEffort};
        std::size_t MaximumWireBytes{0};
        EncodeThunk Encode{nullptr};
        bool Used{false};
    };

    std::array<Entry,TMaximumTypes> _entries{};
    std::size_t _count{0};
    std::size_t _maximumInboundBytes{0};
    std::size_t _maximumOutboundBytes{0};
    std::uint8_t _serviceMask{0};
    bool _requiresDestinationEvidence{false};
    bool _frozen{false};

    static constexpr std::uint8_t ServiceBit(Mesh::MeshRelayServiceClass service) noexcept {
        const auto mapped=ToAdapterServiceClass(service);
        return static_cast<std::uint8_t>(std::uint8_t{1}<<static_cast<std::uint8_t>(mapped));
    }
    const Entry* Find(Event::EventTypeId type) const noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }
    Entry* Find(Event::EventTypeId type) noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }

    static Primitive::PrimitiveAdmissionDisposition AdmitInbound(
        void* owner,
        Primitive::PrimitiveProtocolVersion protocol,
        Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance&) noexcept {
        auto& self=*static_cast<EventMeshAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=Event::EventProtocolVersion||!bytes.Data||bytes.Size<Event::EventWireHeaderSize)
            return Primitive::PrimitiveAdmissionDisposition::Malformed;
        Event::EventWireHeader header{};
        const auto parsed=Event::DecodeEventWireHeader(bytes.Data,bytes.Size,header);
        if(!parsed){
            return parsed.Status==Event::EventWireStatus::UnsupportedProtocol
                ?Primitive::PrimitiveAdmissionDisposition::Unsupported
                :Primitive::PrimitiveAdmissionDisposition::Malformed;
        }
        const auto* entry=self.Find(header.Key.TypeId);
        if(!entry||!entry->Runtime||!entry->Inbound) return Primitive::PrimitiveAdmissionDisposition::Unsupported;
        if(bytes.Size>entry->MaximumWireBytes) return Primitive::PrimitiveAdmissionDisposition::Malformed;
        return Event::ToPrimitiveAdmissionDisposition(entry->Runtime->TryAdmitRemote(entry->Inbound,bytes.Data,bytes.Size).Status);
    }

    static Adapters::AdapterEncodeResult EncodeOutbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,std::uint64_t,
        const void* source,Adapters::AdapterMutableByteView output) noexcept {
        auto& self=*static_cast<EventMeshAdapterFamilyBinding*>(owner);
        if(!self._frozen||protocol!=Event::EventProtocolVersion||!source||!output.Data)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto& outbound=*static_cast<const EventMeshAdapterOutboundSource*>(source);
        const auto* entry=self.Find(outbound.TypeId);
        if(!entry||!entry->Encode||!outbound.Occurrence||!static_cast<bool>(*outbound.Occurrence) ||
           outbound.Occurrence->Facts().TypeId!=entry->TypeId)
            return {Adapters::AdapterResourceStatus::InvalidConfiguration,0};
        const auto encoded=entry->Encode(*outbound.Occurrence,output.Data,output.Capacity);
        return {Detail::MapEventWireEncode(encoded.Status),encoded.Bytes};
    }

    static MeshAdapterPolicyResolution ResolvePolicy(
        void* owner,
        Primitive::PrimitiveProtocolVersion protocol,
        const Mesh::MeshReceiveContext& context,
        Adapters::AdapterByteView bytes,
        Primitive::PrimitivePolicyDescriptor& policy,
        Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<EventMeshAdapterFamilyBinding*>(owner);
        provenance={};
        if(!self._frozen) return MeshAdapterPolicyResolution::Rejected;
        if(protocol!=Event::EventProtocolVersion) return MeshAdapterPolicyResolution::Unsupported;
        if(!bytes.Data||bytes.Size<Event::EventWireHeaderSize) return MeshAdapterPolicyResolution::Malformed;
        Event::EventWireHeader header{};
        const auto parsed=Event::DecodeEventWireHeader(bytes.Data,bytes.Size,header);
        if(!parsed) return parsed.Status==Event::EventWireStatus::UnsupportedProtocol
            ?MeshAdapterPolicyResolution::Unsupported:MeshAdapterPolicyResolution::Malformed;
        const auto* entry=self.Find(header.Key.TypeId);
        if(!entry) return MeshAdapterPolicyResolution::Unsupported;
        if(context.Service!=entry->Service) return MeshAdapterPolicyResolution::Rejected;
        if(bytes.Size>entry->MaximumWireBytes) return MeshAdapterPolicyResolution::Malformed;
        // Locked M2 permits generic Mesh broadcast only for NoRemoteEvidence family policy.
        if(context.Broadcast&&entry->Policy.Evidence!=0U) return MeshAdapterPolicyResolution::Rejected;
        policy=entry->Policy;
        return MeshAdapterPolicyResolution::Resolved;
    }

public:
    EventMeshAdapterFamilyBinding() noexcept = default;
    EventMeshAdapterFamilyBinding(const EventMeshAdapterFamilyBinding&)=delete;
    EventMeshAdapterFamilyBinding& operator=(const EventMeshAdapterFamilyBinding&)=delete;

    /// <summary>Adds one transmissible Event Type and its real Event Runtime inbound binding/typed encoder before freeze.</summary>
    template<class TEvent,class TFormat>
    EventMeshAdapterBindingStatus BindType(Event::Runtime& runtime,Mesh::MeshRelayServiceClass service) noexcept {
        static_assert(TEvent::IsTransmissibleEvent&&TEvent::ValidateTier(),"Mesh Event binding requires a Transmissible Event");
        static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                      std::is_same_v<TFormat,Serializable::CBOR>||
                      std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Event MeshAdapter format");
        if(_frozen) return EventMeshAdapterBindingStatus::Frozen;
        if(!Mesh::IsMeshRelayServiceClass(service)) return EventMeshAdapterBindingStatus::InvalidService;
        if(Find(TEvent::TypeId)) return EventMeshAdapterBindingStatus::DuplicateType;
        if(_count==_entries.size()) return EventMeshAdapterBindingStatus::ResourceUnavailable;
        const auto inbound=runtime.template BindInbound<TEvent,TFormat>();
        if(!inbound) return EventMeshAdapterBindingStatus::InvalidBinding;
        constexpr auto maximum=Event::MaximumCompletePrimitiveWireBytes<TEvent,TFormat>;
        auto& entry=_entries[_count++];
        entry.TypeId=TEvent::TypeId;
        entry.Runtime=&runtime;
        entry.Inbound=inbound;
        entry.Policy=Primitive::PrimitivePolicyContract<typename TEvent::DeliveryPolicy>::Descriptor();
        entry.Service=service;
        entry.MaximumWireBytes=maximum;
        entry.Encode=[](const Event::EventLease& occurrence,std::uint8_t* output,std::size_t capacity) {
            if(!occurrence||occurrence.Facts().TypeId!=TEvent::TypeId) return Event::EventWireResult{};
            return Event::EncodeEventWire<TEvent,TFormat>(occurrence.template Get<TEvent>(),output,capacity);
        };
        entry.Used=true;
        if(maximum>_maximumInboundBytes) _maximumInboundBytes=maximum;
        if(maximum>_maximumOutboundBytes) _maximumOutboundBytes=maximum;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(service));
        _requiresDestinationEvidence=_requiresDestinationEvidence||entry.Policy.Evidence!=0U;
        return EventMeshAdapterBindingStatus::Success;
    }

    /// <summary>Freezes the non-empty Type table; later mutation is rejected.</summary>
    EventMeshAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return EventMeshAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_maximumOutboundBytes==0||_serviceMask==0)
            return EventMeshAdapterBindingStatus::InvalidBinding;
        _frozen=true;
        return EventMeshAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    /// <summary>Resolves the frozen outbound service/policy for one Event Type without adding occurrence-local routing state.</summary>
    bool TryGetOutboundContract(
        Event::EventTypeId type,Adapters::AdapterServiceClass& service,
        Primitive::PrimitivePolicyDescriptor& policy) const noexcept {
        if(!_frozen) return false;
        const auto* entry=Find(type);
        if(!entry||!entry->Encode) return false;
        service=ToAdapterServiceClass(entry->Service);
        policy=entry->Policy;
        return true;
    }

    /// <summary>Returns the bidirectional A2 Event family descriptor after freeze.</summary>
    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::Event;
        binding.Protocols={Event::EventProtocolVersion,Event::EventProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.MaximumOutboundBytes=_maximumOutboundBytes;
        binding.ServiceClassMask=_serviceMask;
        binding.RequiresDestinationAdmissionEvidence=_requiresDestinationEvidence;
        binding.Owner=this;
        binding.AdmitInbound=&EventMeshAdapterFamilyBinding::AdmitInbound;
        binding.EncodeOutbound=&EventMeshAdapterFamilyBinding::EncodeOutbound;
        return binding;
    }

    /// <summary>Returns the Mesh ingress policy/service resolver paired with this frozen Event binding.</summary>
    MeshAdapterPolicyBinding MeshPolicyBinding() noexcept {
        if(!_frozen) return {};
        return {Primitive::FamilyIds::Event,{Event::EventProtocolVersion,Event::EventProtocolVersion},
                this,&EventMeshAdapterFamilyBinding::ResolvePolicy};
    }
};

} // namespace ESPressio::MeshAdapters
