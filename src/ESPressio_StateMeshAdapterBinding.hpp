#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ESPressio_AdapterBinding.hpp>
#include <ESPressio_StateRemoteAdmission.hpp>
#include <ESPressio_StateRuntime.hpp>
#include <ESPressio_StateWireV1.hpp>

#include "ESPressio_MeshAdapterIngress.hpp"

namespace ESPressio::MeshAdapters {

/// <summary>Configuration result for one frozen State family Mesh/A2 binding.</summary>
enum class StateMeshAdapterBindingStatus : std::uint8_t {
    Success=0,
    Frozen,
    DuplicateType,
    ResourceUnavailable,
    InvalidBinding,
    InvalidService
};

namespace Detail {

struct ParsedStateEnvelope final {
    State::StateTypeId TypeId{};
    System::DeviceRuntimeIdentity SemanticSource{};
    State::StateWireStatus Status{State::StateWireStatus::InvalidHeader};
    bool Parsed{false};
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

} // namespace Detail

/// <summary>
/// Frozen State-family Mesh binding preserving the real State V1 provenance/session/convergence admission boundary.
/// </summary>
/// <remarks>
/// The binding never mutates State sessions or replica tables itself. It parses only the fixed V1 envelope needed to
/// select the frozen Type and authenticate the role-specific semantic source. That complete DeviceRuntimeIdentity must
/// name the authenticated Mesh source DeviceIdentifier before it is promoted to A2 OriginalSource provenance. The exact
/// family bytes are then delegated to State::Runtime::AdmitRemote, which remains sole owner of bounded decode, session,
/// version, baseline/resync and convergence mutation. Canonical State V1 is never admitted through generic Mesh broadcast.
/// </remarks>
template<class TStateRuntime,std::size_t TMaximumTypes>
class StateMeshAdapterFamilyBinding final {
    static_assert(TMaximumTypes>0,"State MeshAdapter Type capacity must be non-zero");

    using AdmitThunk=State::StateRemoteAdmissionResult(*)(
        TStateRuntime&,const std::uint8_t*,std::size_t,State::StateValidatedIngressContext) noexcept;

    struct Entry final {
        State::StateTypeId TypeId{};
        TStateRuntime* Runtime{nullptr};
        Primitive::PrimitivePolicyDescriptor Policy{};
        Mesh::MeshRelayServiceClass Service{Mesh::MeshRelayServiceClass::Convergent};
        std::size_t MaximumWireBytes{0};
        AdmitThunk Admit{nullptr};
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
    const Entry* Find(State::StateTypeId type) const noexcept {
        for(std::size_t i=0;i<_count;++i) if(_entries[i].Used&&_entries[i].TypeId==type) return &_entries[i];
        return nullptr;
    }

    static Primitive::PrimitiveAdmissionDisposition AdmitInbound(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,Adapters::AdapterByteView bytes,
        const Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<StateMeshAdapterFamilyBinding*>(owner);
        using P=Primitive::PrimitiveAdmissionDisposition;
        if(!self._frozen||protocol!=State::StateProtocolVersion||!bytes.Data||bytes.Size<5)
            return P::Malformed;
        const auto parsed=Detail::ParseStateEnvelope(bytes);
        if(!parsed.Parsed) return parsed.Status==State::StateWireStatus::UnsupportedProtocol?P::Unsupported:P::Malformed;
        const auto* entry=self.Find(parsed.TypeId);
        if(!entry||!entry->Runtime||!entry->Admit) return P::Unsupported;
        if(bytes.Size>entry->MaximumWireBytes) return P::Malformed;
        if(!provenance.OriginalSource||provenance.OriginalSource.Identity!=parsed.SemanticSource) return P::Rejected;
        return entry->Admit(*entry->Runtime,bytes.Data,bytes.Size,
                            State::StateValidatedIngressContext{provenance.OriginalSource.Identity}).Disposition;
    }

    static MeshAdapterPolicyResolution ResolvePolicy(
        void* owner,Primitive::PrimitiveProtocolVersion protocol,const Mesh::MeshReceiveContext& context,
        Adapters::AdapterByteView bytes,Primitive::PrimitivePolicyDescriptor& policy,
        Adapters::AdapterSemanticProvenance& provenance) noexcept {
        auto& self=*static_cast<StateMeshAdapterFamilyBinding*>(owner);
        provenance={};
        if(!self._frozen) return MeshAdapterPolicyResolution::Rejected;
        if(protocol!=State::StateProtocolVersion) return MeshAdapterPolicyResolution::Unsupported;
        // Locked M2: canonical State V1 never uses generic Mesh broadcast.
        if(context.Broadcast) return MeshAdapterPolicyResolution::Rejected;
        const auto parsed=Detail::ParseStateEnvelope(bytes);
        if(!parsed.Parsed) return parsed.Status==State::StateWireStatus::UnsupportedProtocol
            ?MeshAdapterPolicyResolution::Unsupported:MeshAdapterPolicyResolution::Malformed;
        const auto* entry=self.Find(parsed.TypeId);
        if(!entry) return MeshAdapterPolicyResolution::Unsupported;
        if(context.Service!=entry->Service) return MeshAdapterPolicyResolution::Rejected;
        if(bytes.Size>entry->MaximumWireBytes) return MeshAdapterPolicyResolution::Malformed;
        if(parsed.SemanticSource.Device!=context.Source) return MeshAdapterPolicyResolution::Rejected;
        provenance.OriginalSource={parsed.SemanticSource,true};
        policy=entry->Policy;
        return MeshAdapterPolicyResolution::Resolved;
    }

public:
    StateMeshAdapterFamilyBinding() noexcept = default;
    StateMeshAdapterFamilyBinding(const StateMeshAdapterFamilyBinding&)=delete;
    StateMeshAdapterFamilyBinding& operator=(const StateMeshAdapterFamilyBinding&)=delete;

    /// <summary>Adds one configured TransmissibleState Type and exact real State Runtime admission thunk before freeze.</summary>
    template<class TState,class TFormat>
    StateMeshAdapterBindingStatus BindType(
        TStateRuntime& runtime,Mesh::MeshRelayServiceClass service=Mesh::MeshRelayServiceClass::Convergent) noexcept {
        static_assert(TState::IsTransmissibleState&&TState::ValidateTier(),
                      "Mesh State binding requires a valid TransmissibleState Type");
        static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                      std::is_same_v<TFormat,Serializable::CBOR>||
                      std::is_same_v<TFormat,Serializable::JSON>,"Unsupported State MeshAdapter format");
        if(_frozen) return StateMeshAdapterBindingStatus::Frozen;
        if(!Mesh::IsMeshRelayServiceClass(service)) return StateMeshAdapterBindingStatus::InvalidService;
        if(Find(TState::TypeId)) return StateMeshAdapterBindingStatus::DuplicateType;
        if(_count==_entries.size()) return StateMeshAdapterBindingStatus::ResourceUnavailable;

        const auto common=TState::GetPrimitiveTypeDescriptor();
        const auto* descriptor=State::GetStateTypeDescriptor(common);
        if(!descriptor||!descriptor->ConvergencePolicy) return StateMeshAdapterBindingStatus::InvalidBinding;
        const auto formatIndex=std::is_same_v<TFormat,Serializable::DirectBinary>?std::size_t{0}:
            (std::is_same_v<TFormat,Serializable::CBOR>?std::size_t{1}:std::size_t{2});
        std::size_t maximum=descriptor->MaximumPublicationWireBytes[formatIndex];
        if(descriptor->MaximumSnapshotControlWireBytes[formatIndex]>maximum)
            maximum=descriptor->MaximumSnapshotControlWireBytes[formatIndex];
        if(maximum<State::StateSnapshotControlWireHeaderSize) maximum=State::StateSnapshotControlWireHeaderSize;
        if(maximum==0) return StateMeshAdapterBindingStatus::InvalidBinding;

        auto& entry=_entries[_count++];
        entry.TypeId=TState::TypeId;
        entry.Runtime=&runtime;
        entry.Policy=*descriptor->ConvergencePolicy;
        entry.Service=service;
        entry.MaximumWireBytes=maximum;
        entry.Admit=[](TStateRuntime& target,const std::uint8_t* data,std::size_t size,
                       State::StateValidatedIngressContext ingress) noexcept {
            return target.template AdmitRemote<TState,TFormat>(data,size,ingress);
        };
        entry.Used=true;
        if(maximum>_maximumInboundBytes) _maximumInboundBytes=maximum;
        _serviceMask=static_cast<std::uint8_t>(_serviceMask|ServiceBit(service));
        return StateMeshAdapterBindingStatus::Success;
    }

    StateMeshAdapterBindingStatus Freeze() noexcept {
        if(_frozen) return StateMeshAdapterBindingStatus::Frozen;
        if(_count==0||_maximumInboundBytes==0||_serviceMask==0) return StateMeshAdapterBindingStatus::InvalidBinding;
        _frozen=true;
        return StateMeshAdapterBindingStatus::Success;
    }

    bool IsFrozen() const noexcept { return _frozen; }
    std::size_t TypeCount() const noexcept { return _count; }

    /// <summary>Returns the frozen inbound A2 State family descriptor.</summary>
    Adapters::AdapterBindingDescriptor AdapterBinding() noexcept {
        if(!_frozen) return {};
        Adapters::AdapterBindingDescriptor binding{};
        binding.Family=Primitive::FamilyIds::State;
        binding.Protocols={State::StateProtocolVersion,State::StateProtocolVersion};
        binding.MaximumInboundBytes=_maximumInboundBytes;
        binding.ServiceClassMask=_serviceMask;
        binding.RequiresValidatedOriginalSource=true;
        binding.Owner=this;
        binding.AdmitInbound=&StateMeshAdapterFamilyBinding::AdmitInbound;
        return binding;
    }

    /// <summary>Returns the authenticated Mesh ingress resolver paired with this frozen State binding.</summary>
    MeshAdapterPolicyBinding MeshPolicyBinding() noexcept {
        if(!_frozen) return {};
        return {Primitive::FamilyIds::State,{State::StateProtocolVersion,State::StateProtocolVersion},
                this,&StateMeshAdapterFamilyBinding::ResolvePolicy};
    }
};

} // namespace ESPressio::MeshAdapters
