#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#include <ESPressio_AdapterProvenance.hpp>
#include <ESPressio_AdapterTransport.hpp>
#include <ESPressio_AdapterTypes.hpp>
#include <ESPressio_Clock.hpp>
#include <ESPressio_PrimitiveAdmission.hpp>
#include <ESPressio_PrimitiveFamilyRegistry.hpp>
#include <ESPressio_PrimitivePolicy.hpp>
#include <ESPressio_PrimitiveTypes.hpp>
#include <ESPressio_PrimitiveReceiverRegistry.hpp>

namespace ESPressio::MeshAdapters {

/// <summary>Fixed infrastructure wake emitted when an asynchronous A2 family admission becomes observable to Mesh.</summary>
struct MeshAdapterAdmissionWakeTarget final {
    void* Context{nullptr};
    void (*Wake)(void*) noexcept{nullptr};
    constexpr explicit operator bool() const noexcept { return Context != nullptr && Wake != nullptr; }
};

/// <summary>Result of resolving the immutable family policy carried by one inbound family representation.</summary>
enum class MeshAdapterPolicyResolution : std::uint8_t { Resolved=0, Unsupported, Rejected, Malformed };

using MeshAdapterPolicyResolver = MeshAdapterPolicyResolution(*)(
    void*, Primitive::PrimitiveProtocolVersion, Adapters::AdapterByteView,
    Primitive::PrimitivePolicyDescriptor&) noexcept;

/// <summary>Frozen family-specific policy resolver used before A2 accepts Mesh-owned inbound bytes.</summary>
struct MeshAdapterPolicyBinding final {
    Primitive::PrimitiveFamilyId Family{Primitive::FamilyIds::Invalid};
    Primitive::PrimitiveProtocolVersionRange Protocols{};
    void* Owner{nullptr};
    MeshAdapterPolicyResolver Resolve{nullptr};
    constexpr explicit operator bool() const noexcept {
        return Primitive::FamilyIds::IsUsable(Family) && Family != Primitive::FamilyIds::MeshControl &&
               Protocols.IsValid() && Owner != nullptr && Resolve != nullptr;
    }
};

/// <summary>Explicit neutral mapping between Mesh relay service and A2 runtime service; numeric enum equivalence is never assumed.</summary>
constexpr Adapters::AdapterServiceClass ToAdapterServiceClass(Mesh::MeshRelayServiceClass service) noexcept {
    switch(service) {
        case Mesh::MeshRelayServiceClass::Infrastructure: return Adapters::AdapterServiceClass::Infrastructure;
        case Mesh::MeshRelayServiceClass::Clock: return Adapters::AdapterServiceClass::Clock;
        case Mesh::MeshRelayServiceClass::Critical: return Adapters::AdapterServiceClass::Critical;
        case Mesh::MeshRelayServiceClass::Responsive: return Adapters::AdapterServiceClass::Responsive;
        case Mesh::MeshRelayServiceClass::Convergent: return Adapters::AdapterServiceClass::Convergent;
        case Mesh::MeshRelayServiceClass::BestEffort: return Adapters::AdapterServiceClass::BestEffort;
    }
    return Adapters::AdapterServiceClass::BestEffort;
}

/// <summary>Maps immediate A2 ownership failure to the exact neutral M1 fact visible at the Mesh family boundary.</summary>
constexpr Primitive::PrimitiveAdmissionDisposition ToPrimitiveAdmission(Adapters::AdapterSubmissionDisposition disposition) noexcept {
    using A=Adapters::AdapterSubmissionDisposition; using P=Primitive::PrimitiveAdmissionDisposition;
    switch(disposition) {
        case A::Accepted: return P::TemporarilyUnavailable;
        case A::Busy:
        case A::NotRunning: return P::TemporarilyUnavailable;
        case A::ResourceUnavailable:
        case A::RepresentationTooLarge: return P::ResourceUnavailable;
        case A::Unsupported: return P::Unsupported;
        case A::Malformed: return P::Malformed;
        case A::InvalidConfiguration:
        case A::Rejected: return P::Rejected;
    }
    return P::Rejected;
}

/// <summary>Bounded asynchronous Mesh-to-A2 admission bridge preserving exact destination-family M1 semantics.</summary>
/// <remarks>
/// The first Receive transfers complete byte ownership to A2 and reports TemporarilyUnavailable, never false destination
/// admission. Exact family completion is retained under the authenticated Mesh occurrence key. Completion advances a
/// generation and fixed wake; DeferredLocal then retries and receives the retained M1 without another A2 enqueue.
/// Mesh membership incarnation is not converted into Primitive RuntimeIncarnationId; no stronger provenance is invented.
/// </remarks>
template<class TAdapterRuntime,std::size_t TMaximumCorrelations>
class MeshAdapterIngressBridge final : public Mesh::IPrimitiveReceiver {
    static_assert(TMaximumCorrelations>0,"MeshAdapter ingress correlation capacity must be non-zero");
    static_assert(TMaximumCorrelations<=std::numeric_limits<std::uint16_t>::max(),"Correlation slot must fit token");
    static constexpr std::uint64_t GenerationMask=0x0000FFFFFFFFFFFFULL;

    enum class State : std::uint8_t { Free=0,Pending,Completed };
    struct Entry final {
        System::DeviceIdentifier Source{};
        Mesh::MembershipIncarnation SourceIncarnation{};
        Mesh::MeshMessageId MessageId{0};
        std::uint64_t SlotGeneration{0};
        std::uint64_t ExpiryNanoseconds{0};
        Primitive::PrimitiveAdmissionDisposition Completion{Primitive::PrimitiveAdmissionDisposition::Malformed};
        State Current{State::Free};
    };

    TAdapterRuntime* _runtime{nullptr}; MeshAdapterPolicyBinding _policy{}; MeshAdapterAdmissionWakeTarget _wake{};
    std::array<Entry,TMaximumCorrelations> _entries{}; std::mutex _mutex{};
    std::atomic<std::uint64_t> _admissionGeneration{0};

    static std::uint64_t Correlation(std::size_t slot,std::uint64_t generation) noexcept {
        return ((generation&GenerationMask)<<16U)|static_cast<std::uint64_t>(slot+1U);
    }
    static bool DecodeCorrelation(std::uint64_t correlation,std::size_t& slot,std::uint64_t& generation) noexcept {
        const auto encodedSlot=static_cast<std::uint16_t>(correlation&0xFFFFU); generation=(correlation>>16U)&GenerationMask;
        if(encodedSlot==0U||generation==0U) return false; slot=static_cast<std::size_t>(encodedSlot-1U);
        return slot<TMaximumCorrelations;
    }
    static bool SameOccurrence(const Entry& entry,const Mesh::MeshReceiveContext& context) noexcept {
        return entry.Current!=State::Free&&entry.Source==context.Source&&entry.SourceIncarnation==context.SourceIncarnation&&
               entry.MessageId==context.DeliveryMessageId;
    }
    static std::uint64_t AddSaturating(std::uint64_t now,std::uint64_t duration) noexcept {
        return duration>std::numeric_limits<std::uint64_t>::max()-now?std::numeric_limits<std::uint64_t>::max():now+duration;
    }
    void SweepExpiredLocked(std::uint64_t now) noexcept {
        for(auto& entry:_entries) if(entry.Current!=State::Free&&entry.ExpiryNanoseconds!=0U&&entry.ExpiryNanoseconds<=now)
            entry.Current=State::Free;
    }
    bool AllocateLocked(const Mesh::MeshReceiveContext& context,std::uint64_t expiry,std::size_t& slot,std::uint64_t& generation) noexcept {
        for(std::size_t i=0;i<_entries.size();++i){ auto& entry=_entries[i];
            if(entry.Current!=State::Free||entry.SlotGeneration==GenerationMask) continue;
            generation=entry.SlotGeneration+1U; if(generation==0U) continue;
            entry.Source=context.Source; entry.SourceIncarnation=context.SourceIncarnation; entry.MessageId=context.DeliveryMessageId;
            entry.SlotGeneration=generation; entry.ExpiryNanoseconds=expiry;
            entry.Completion=Primitive::PrimitiveAdmissionDisposition::Malformed; entry.Current=State::Pending; slot=i; return true;
        }
        return false;
    }
    void ReleaseIfPending(std::size_t slot,std::uint64_t generation) noexcept {
        std::lock_guard<std::mutex> lock(_mutex); auto& entry=_entries[slot];
        if(entry.Current==State::Pending&&entry.SlotGeneration==generation) entry.Current=State::Free;
    }
    static void Complete(void* owner,const Adapters::AdapterInboundCompletion& completion) noexcept {
        static_cast<MeshAdapterIngressBridge*>(owner)->Complete(completion);
    }
    void Complete(const Adapters::AdapterInboundCompletion& completion) noexcept {
        std::size_t slot=0; std::uint64_t generation=0; if(!DecodeCorrelation(completion.Correlation,slot,generation)) return;
        {
            std::lock_guard<std::mutex> lock(_mutex); auto& entry=_entries[slot];
            if(entry.Current!=State::Pending||entry.SlotGeneration!=generation) return;
            entry.Completion=completion.Admission; entry.Current=State::Completed;
        }
        auto current=_admissionGeneration.load(std::memory_order_relaxed);
        while(current!=std::numeric_limits<std::uint64_t>::max()&&
              !_admissionGeneration.compare_exchange_weak(current,current+1U,std::memory_order_release,std::memory_order_relaxed)) {}
        if(_wake) _wake.Wake(_wake.Context);
    }

public:
    MeshAdapterIngressBridge(TAdapterRuntime& runtime,MeshAdapterPolicyBinding policy,MeshAdapterAdmissionWakeTarget wake={}) noexcept
        : _runtime(&runtime),_policy(policy),_wake(wake) {}
    MeshAdapterIngressBridge(const MeshAdapterIngressBridge&)=delete;
    MeshAdapterIngressBridge& operator=(const MeshAdapterIngressBridge&)=delete;
    std::uint64_t AdmissionGeneration() const noexcept { return _admissionGeneration.load(std::memory_order_acquire); }
    Primitive::PrimitiveFamilyId Family() const noexcept { return _policy.Family; }

    std::size_t Expire(std::uint64_t now=System::Clock::Monotonic().NowNanoseconds()) noexcept {
        std::lock_guard<std::mutex> lock(_mutex); std::size_t released=0;
        for(auto& entry:_entries) if(entry.Current!=State::Free&&entry.ExpiryNanoseconds!=0U&&entry.ExpiryNanoseconds<=now){
            entry.Current=State::Free; ++released;
        }
        return released;
    }

    Primitive::PrimitiveAdmissionDisposition Receive(const Mesh::MeshReceiveContext& context,
        Primitive::PrimitiveProtocolVersion version,Mesh::PrimitivePayloadView payload) noexcept override {
        using P=Primitive::PrimitiveAdmissionDisposition;
        if(_runtime==nullptr||!_policy||!context.IsValid()||!payload.IsValid()||payload.Size==0U) return P::Malformed;
        if(!_policy.Protocols.Contains(version)) return P::Unsupported;

        Primitive::PrimitivePolicyDescriptor familyPolicy{}; const Adapters::AdapterByteView bytes{payload.Data,payload.Size};
        switch(_policy.Resolve(_policy.Owner,version,bytes,familyPolicy)){
            case MeshAdapterPolicyResolution::Resolved: break;
            case MeshAdapterPolicyResolution::Unsupported: return P::Unsupported;
            case MeshAdapterPolicyResolution::Rejected: return P::Rejected;
            case MeshAdapterPolicyResolution::Malformed: return P::Malformed;
        }

        const auto now=System::Clock::Monotonic().NowNanoseconds();
        const auto retention=familyPolicy.MaximumAdapterAdmissionWaitNanoseconds!=0U
            ?familyPolicy.MaximumAdapterAdmissionWaitNanoseconds:familyPolicy.MaximumResidenceNanoseconds;
        const auto expiry=retention==0U?now:AddSaturating(now,retention);
        std::size_t slot=0; std::uint64_t slotGeneration=0;
        {
            std::unique_lock<std::mutex> lock(_mutex,std::try_to_lock);
            if(!lock.owns_lock()) return P::TemporarilyUnavailable;
            SweepExpiredLocked(now);
            for(auto& entry:_entries){
                if(!SameOccurrence(entry,context)) continue;
                if(entry.Current==State::Pending) return P::TemporarilyUnavailable;
                const auto result=entry.Completion; entry.Current=State::Free; return result;
            }
            if(!AllocateLocked(context,expiry,slot,slotGeneration)) return P::ResourceUnavailable;
        }

        Adapters::AdapterSemanticProvenance provenance{}; const auto correlation=Correlation(slot,slotGeneration);
        const auto submitted=_runtime->AdmitTrustedInbound(_policy.Family,ToAdapterServiceClass(context.Service),version,bytes,
            provenance,Adapters::AdapterRouteToken{},familyPolicy,correlation,{this,&MeshAdapterIngressBridge::Complete});
        if(submitted==Adapters::AdapterSubmissionDisposition::Accepted) return P::TemporarilyUnavailable;
        ReleaseIfPending(slot,slotGeneration); return ToPrimitiveAdmission(submitted);
    }
};

} // namespace ESPressio::MeshAdapters
