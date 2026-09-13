#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ESPressio_AdapterRuntime.hpp>
#include <ESPressio_EventOutboundBinding.hpp>

#include "ESPressio_EventMeshAdapterBinding.hpp"

namespace ESPressio::MeshAdapters {

template<class TAdapterRuntime,std::size_t TMaximumTypes,class TEvent,class TFormat>
class EventMeshAdapterOutboundTarget final {
    static_assert(TEvent::IsTransmissibleEvent&&TEvent::ValidateTier(),
                  "Event Mesh outbound target requires a valid TransmissibleEvent");
    static_assert(std::is_same_v<TFormat,Serializable::DirectBinary>||
                  std::is_same_v<TFormat,Serializable::CBOR>||
                  std::is_same_v<TFormat,Serializable::JSON>,"Unsupported Event MeshAdapter format");

    using FamilyBinding=EventMeshAdapterFamilyBinding<TMaximumTypes>;

    TAdapterRuntime* _adapter{nullptr};
    FamilyBinding* _family{nullptr};
    Adapters::AdapterRouteToken _route{};
    bool _broadcast{false};
    Event::EventOutboundBinding<TEvent> _binding{};
    bool _initialized{false};

    bool Validate() noexcept {
        if(!_initialized||_adapter==nullptr||_family==nullptr||!_route||!_family->IsFrozen()) return false;
        Adapters::AdapterServiceClass service{};
        Primitive::PrimitivePolicyDescriptor policy{};
        if(!_family->TryGetOutboundContract(TEvent::TypeId,service,policy)) return false;
        return !_broadcast||policy.Evidence==0U;
    }

    Event::EventTargetAdmission Admit(const Event::EventLease& occurrence) noexcept {
        if(!_initialized||_adapter==nullptr||_family==nullptr||!_route)
            return Event::EventTargetAdmission::Quiesced;
        Adapters::AdapterServiceClass service{};
        Primitive::PrimitivePolicyDescriptor policy{};
        if(!_family->TryGetOutboundContract(TEvent::TypeId,service,policy) || (_broadcast&&policy.Evidence!=0U))
            return Event::EventTargetAdmission::Quiesced;
        const EventMeshAdapterOutboundSource source{TEvent::TypeId,&occurrence};
        const auto submitted=_adapter->SubmitOutbound(
            Primitive::FamilyIds::Event,service,Event::EventProtocolVersion,&source,_route,policy,
            occurrence.Facts().MessageId.Value());
        switch(submitted) {
            case Adapters::AdapterSubmissionDisposition::Accepted:
                return Event::EventTargetAdmission::Accepted;
            case Adapters::AdapterSubmissionDisposition::Busy:
            case Adapters::AdapterSubmissionDisposition::ResourceUnavailable:
                return Event::EventTargetAdmission::CapacityUnavailable;
            case Adapters::AdapterSubmissionDisposition::RepresentationTooLarge:
            case Adapters::AdapterSubmissionDisposition::Unsupported:
            case Adapters::AdapterSubmissionDisposition::InvalidConfiguration:
            case Adapters::AdapterSubmissionDisposition::NotRunning:
            case Adapters::AdapterSubmissionDisposition::Rejected:
            case Adapters::AdapterSubmissionDisposition::Malformed:
                return Event::EventTargetAdmission::Quiesced;
        }
        return Event::EventTargetAdmission::Quiesced;
    }

public:
    EventMeshAdapterOutboundTarget(
        TAdapterRuntime& adapter,FamilyBinding& family,Adapters::AdapterRouteToken route,bool broadcast=false) noexcept
        :_adapter(&adapter),_family(&family),_route(route),_broadcast(broadcast) {}
    EventMeshAdapterOutboundTarget(const EventMeshAdapterOutboundTarget&)=delete;
    EventMeshAdapterOutboundTarget& operator=(const EventMeshAdapterOutboundTarget&)=delete;

    Event::EventRuntimeStatus Initialize() noexcept {
        if(_initialized) return Event::EventRuntimeStatus::AlreadyInitialized;
        _initialized=true;
        const auto status=_binding.template Initialize<
            EventMeshAdapterOutboundTarget,
            &EventMeshAdapterOutboundTarget::Admit,
            &EventMeshAdapterOutboundTarget::Validate>(*this);
        if(status!=Event::EventRuntimeStatus::Success) _initialized=false;
        return status;
    }

    void NotifyCapacityChanged() const noexcept {
        if(_initialized) _binding.NotifyCapacityChanged();
    }

    void Shutdown() noexcept {
        if(!_initialized) return;
        _binding.Shutdown();
        _initialized=false;
    }

    bool IsInitialized() const noexcept { return _initialized; }
    Adapters::AdapterRouteToken Route() const noexcept { return _route; }
    bool IsBroadcast() const noexcept { return _broadcast; }
};

} // namespace ESPressio::MeshAdapters
