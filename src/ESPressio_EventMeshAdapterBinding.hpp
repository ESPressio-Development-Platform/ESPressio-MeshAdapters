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

/// <summary>
/// Fixed pre-freeze Event Type table used by both Mesh policy validation and the A2 Event family admission thunk.
/// </summary>
/// <remarks>
/// Every entry stores the real Event::Runtime inbound binding. Inbound A2 execution therefore delegates to
/// Event::Runtime::TryAdmitRemote and preserves the family's receipt/idempotency, local-origin replay rejection,
/// exact wire limits and Type runtime semantics. No descriptor-level shortcut bypasses the Event family Runtime.
/// </remarks>
template<std::size_t TMaximumTypes>
class EventMeshAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"Event MeshAdapter Type capacity must be non-zero");

    struct Entry final {
        Event::EventTypeId TypeId{};
        Event::Runtime* Runtime{nullptr};
        Event::EventInboundBinding Inbound{};
        Primitive::PrimitivePolicyDescriptor Policy{};
        Mesh::MeshRelayServiceClass Service{Mesh::MeshRelayServiceClass::BestEffort};
        std::size_t MaximumWireBytes{0};
        bool Used{false};
    };

    std::array<Entry,TMaximumTypes> _entries{};
    std::size_t _count{0};
    std::size_t _maximumInboundBytes{0};
    std::uint8_t _serviceMask{0};
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

    /// <summary>Adds one transmissible Event Type and its real Event Runtime inbound binding before freeze.</summary>
    template<class TEvent,class TFormat>
    EventMeshAdapterBindingStatus BindType(Event::Runtime& runtime,Mesh::MeshRelayServiceClass service) noexcept {
        static_assert(TEvent::IsTransmissibleEvent&&TEvent::ValidateTier(),"Mesh Event binding requires a Transmissible Event");
        static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                      std::is_same_v<TFormat,Serializable::CBOR>||
                      std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Event MeshAdapter format");
        if(_frozen) return EventMeshAdapterBindingStatus::Frozen;
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
        entry.Used=true;
        if(maximum>_maximumInboundBytes) _maximumInboundBytes=maximum;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(service));
        return EventMeshAdapterBindingStatus::Success;
    }

    /// <summary>Freezes the non-empty Type table; later mutation is rejected.</summary>
    EventMeshAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return EventMeshAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_serviceMask==0) return EventMeshAdapterBindingStatus::InvalidBinding;
        _frozen=true;
        return EventMeshAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    /// <summary>Returns the A2 Event family descriptor after freeze.</summary>
    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::Event;
        binding.Protocols={Event::EventProtocolVersion,Event::EventProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.ServiceClassMask=_serviceMask;
        binding.Owner=this;
        binding.AdmitInbound=&EventMeshAdapterFamilyBinding::AdmitInbound;
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
