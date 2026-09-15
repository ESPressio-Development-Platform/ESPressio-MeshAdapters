#pragma once

#include <cstdint>

#include <ESPressio_AdapterTransport.hpp>
#include <ESPressio_MeshRelayCapacity.hpp>

namespace ESPressio::MeshAdapters {

/// <summary>Fixed completion target supplied to the existing Mesh application lifecycle for one A2 outbound record.</summary>
/// <remarks>
/// The target carries only exact A2 record/transport correlation and the eventual lower-transport/M1 result. It owns no
/// worker, retry policy, route state or payload storage. Mesh lifecycle/managed-Radio code invokes it only when the
/// corresponding submission reaches a lower-transport terminal point or exact destination Primitive admission fact.
/// </remarks>
struct MeshApplicationCompletionTarget final {
    void* Owner{nullptr};
    void (*Complete)(
        void*,
        Adapters::AdapterRecordIdentity,
        std::uint64_t,
        Adapters::LowerTransportDisposition,
        Primitive::PrimitiveAdmissionDisposition,
        bool
    ) noexcept{nullptr};

    constexpr explicit operator bool() const noexcept { return Owner != nullptr && Complete != nullptr; }
};

/// <summary>Immediate result of transferring one immutable A2 payload into the existing Mesh application lifecycle.</summary>
struct MeshApplicationSubmissionResult final {
    Adapters::LowerTransportDisposition Disposition{Adapters::LowerTransportDisposition::ResourceUnavailable};
    std::uint64_t Generation{0};
    bool DeferredCompletion{false};
};

/// <summary>
/// Composition-owned Mesh application submission seam for one already-encoded Primitive representation.
/// </summary>
/// <remarks>
/// Family, protocol and policy are immutable neutral metadata owned by the A2 record and are forwarded intact so Mesh can
/// construct its generic application framing and enforce only Mesh-owned routing/lifecycle semantics. Mesh must not use
/// them to acquire Event/Command/State implementation knowledge or create a second P2 pursuit loop.
/// </remarks>
using MeshApplicationSubmitThunk = MeshApplicationSubmissionResult(*)(
    void*,
    Adapters::AdapterRecordIdentity,
    Primitive::PrimitiveFamilyId,
    Primitive::PrimitiveProtocolVersion,
    const Primitive::PrimitivePolicyDescriptor&,
    Mesh::MeshRelayServiceClass,
    Adapters::AdapterByteView,
    Adapters::AdapterRouteToken,
    MeshApplicationCompletionTarget
) noexcept;
using MeshApplicationValidateThunk = bool(*)(void*) noexcept;
using MeshApplicationCancelThunk = void(*)(void*,Adapters::AdapterRecordIdentity) noexcept;
using MeshApplicationQuiesceThunk = void(*)(void*) noexcept;

/// <summary>
/// Fixed composition binding to the already-existing Mesh application lifecycle/managed-Radio submission machinery.
/// </summary>
/// <remarks>
/// Implementations must transfer/retain any required ownership before Submit returns. They must not create a second A2
/// pursuit loop, worker pool, Radio fragmentation layer or Mesh routing engine. Service support is frozen and expressed
/// in Mesh's neutral six-class vocabulary. Destination admission and validated-original-source flags describe evidence
/// the bound Mesh composition can actually provide; no stronger evidence is inferred by this adapter.
/// </remarks>
struct MeshApplicationLifecycleBinding final {
    void* Owner{nullptr};
    MeshApplicationSubmitThunk Submit{nullptr};
    MeshApplicationValidateThunk Validate{nullptr};
    MeshApplicationCancelThunk Cancel{nullptr};
    MeshApplicationQuiesceThunk Quiesce{nullptr};
    std::uint8_t ServiceClassMask{0};
    bool ProvidesDestinationPrimitiveAdmission{false};
    bool ProvidesValidatedOriginalSource{false};

    constexpr bool Supports(Mesh::MeshRelayServiceClass service) const noexcept {
        const auto raw=static_cast<std::uint8_t>(service);
        return raw<Mesh::MeshRelayServiceClassCount &&
               (ServiceClassMask&(std::uint8_t{1}<<raw))!=0;
    }
    constexpr explicit operator bool() const noexcept {
        return Owner!=nullptr && Submit!=nullptr && Validate!=nullptr && ServiceClassMask!=0;
    }
};

constexpr bool ToMeshRelayServiceClass(
    Adapters::AdapterServiceClass service,
    Mesh::MeshRelayServiceClass& mapped) noexcept {
    switch(service) {
        case Adapters::AdapterServiceClass::Infrastructure:
            mapped=Mesh::MeshRelayServiceClass::Infrastructure; return true;
        case Adapters::AdapterServiceClass::Clock:
            mapped=Mesh::MeshRelayServiceClass::Clock; return true;
        case Adapters::AdapterServiceClass::Critical:
            mapped=Mesh::MeshRelayServiceClass::Critical; return true;
        case Adapters::AdapterServiceClass::Responsive:
            mapped=Mesh::MeshRelayServiceClass::Responsive; return true;
        case Adapters::AdapterServiceClass::Convergent:
            mapped=Mesh::MeshRelayServiceClass::Convergent; return true;
        case Adapters::AdapterServiceClass::BestEffort:
            mapped=Mesh::MeshRelayServiceClass::BestEffort; return true;
    }
    return false;
}

constexpr std::uint8_t ToAdapterServiceMask(std::uint8_t meshMask) noexcept {
    std::uint8_t result=0;
    for(std::uint8_t raw=0;raw<Adapters::AdapterServiceClassCount;++raw) {
        const auto adapter=static_cast<Adapters::AdapterServiceClass>(raw);
        Mesh::MeshRelayServiceClass mesh{};
        if(!ToMeshRelayServiceClass(adapter,mesh)) continue;
        const auto meshRaw=static_cast<std::uint8_t>(mesh);
        if((meshMask&(std::uint8_t{1}<<meshRaw))!=0)
            result=static_cast<std::uint8_t>(result|(std::uint8_t{1}<<raw));
    }
    return result;
}

/// <summary>Neutral A2 lower-transport binding backed by the existing Mesh application lifecycle.</summary>
/// <remarks>
/// The complete family wire representation remains A2-owned. Family/version/policy are forwarded as generic Primitive
/// metadata because Mesh owns the generic application envelope and route lifecycle. The binding never interprets
/// family-specific Types and never moves logical pursuit/retry ownership out of A2.
/// </remarks>
template<class TAdapterRuntime>
class MeshLowerTransportBinding final {
    TAdapterRuntime* _runtime{nullptr};
    MeshApplicationLifecycleBinding _mesh{};

    static void CompleteThunk(
        void* owner,
        Adapters::AdapterRecordIdentity record,
        std::uint64_t generation,
        Adapters::LowerTransportDisposition disposition,
        Primitive::PrimitiveAdmissionDisposition admission,
        bool hasAdmission) noexcept {
        auto& self=*static_cast<MeshLowerTransportBinding*>(owner);
        if(self._runtime==nullptr) return;
        (void)self._runtime->CompleteTransport({record,generation,disposition,admission,hasAdmission});
    }

    static Adapters::LowerTransportSubmitResult SubmitThunk(
        void* owner,
        Adapters::AdapterRecordIdentity record,
        Primitive::PrimitiveFamilyId family,
        Primitive::PrimitiveProtocolVersion protocol,
        const Primitive::PrimitivePolicyDescriptor& policy,
        Adapters::AdapterServiceClass service,
        Adapters::AdapterByteView bytes,
        Adapters::AdapterRouteToken route) noexcept {
        auto& self=*static_cast<MeshLowerTransportBinding*>(owner);
        Mesh::MeshRelayServiceClass meshService{};
        if(!self._mesh || !ToMeshRelayServiceClass(service,meshService) || !self._mesh.Supports(meshService))
            return {Adapters::LowerTransportDisposition::PermanentlyRejected,0,false};
        const auto submitted=self._mesh.Submit(
            self._mesh.Owner,record,family,protocol,policy,meshService,bytes,route,
            MeshApplicationCompletionTarget{&self,&MeshLowerTransportBinding::CompleteThunk});
        if(submitted.DeferredCompletion && submitted.Generation==0)
            return {Adapters::LowerTransportDisposition::ResourceUnavailable,0,false};
        return {submitted.Disposition,submitted.Generation,submitted.DeferredCompletion};
    }

    static bool ValidateThunk(void* owner) noexcept {
        auto& self=*static_cast<MeshLowerTransportBinding*>(owner);
        return self._runtime!=nullptr && self._mesh && self._mesh.Validate(self._mesh.Owner);
    }

    static void CancelThunk(void* owner,Adapters::AdapterRecordIdentity record) noexcept {
        auto& self=*static_cast<MeshLowerTransportBinding*>(owner);
        if(self._mesh && self._mesh.Cancel) self._mesh.Cancel(self._mesh.Owner,record);
    }

    static void QuiesceThunk(void* owner) noexcept {
        auto& self=*static_cast<MeshLowerTransportBinding*>(owner);
        if(self._mesh && self._mesh.Quiesce) self._mesh.Quiesce(self._mesh.Owner);
    }

public:
    MeshLowerTransportBinding(TAdapterRuntime& runtime,MeshApplicationLifecycleBinding mesh) noexcept
        :_runtime(&runtime),_mesh(mesh) {}
    MeshLowerTransportBinding(const MeshLowerTransportBinding&)=delete;
    MeshLowerTransportBinding& operator=(const MeshLowerTransportBinding&)=delete;

    constexpr bool IsValid() const noexcept {
        return _runtime!=nullptr && static_cast<bool>(_mesh) && ToAdapterServiceMask(_mesh.ServiceClassMask)!=0;
    }

    Adapters::LowerTransportBinding AdapterBinding() noexcept {
        if(!IsValid()) return {};
        Adapters::LowerTransportBinding binding{};
        binding.Owner=this;
        binding.Submit=&MeshLowerTransportBinding::SubmitThunk;
        binding.Validate=&MeshLowerTransportBinding::ValidateThunk;
        binding.Cancel=_mesh.Cancel?&MeshLowerTransportBinding::CancelThunk:nullptr;
        binding.Quiesce=_mesh.Quiesce?&MeshLowerTransportBinding::QuiesceThunk:nullptr;
        binding.ServiceClassMask=ToAdapterServiceMask(_mesh.ServiceClassMask);
        binding.ProvidesDestinationPrimitiveAdmission=_mesh.ProvidesDestinationPrimitiveAdmission;
        binding.ProvidesValidatedOriginalSource=_mesh.ProvidesValidatedOriginalSource;
        return binding;
    }
};

} // namespace ESPressio::MeshAdapters
