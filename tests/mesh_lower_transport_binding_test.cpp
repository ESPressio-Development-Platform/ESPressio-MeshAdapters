#include <ESPressio_MeshLowerTransportBinding.hpp>

#include <array>
#include <cassert>
#include <cstdint>

using namespace ESPressio;

struct MockAdapterRuntime final {
    Adapters::LowerTransportCompletion Last{};
    std::size_t CompletionCount=0;

    Adapters::AdapterSubmissionDisposition CompleteTransport(
        const Adapters::LowerTransportCompletion& completion) noexcept {
        Last=completion;
        ++CompletionCount;
        return Adapters::AdapterSubmissionDisposition::Accepted;
    }
};

struct MeshLifecycle final {
    bool Running=true;
    bool Cancelled=false;
    bool Quiesced=false;
    Adapters::AdapterRecordIdentity Record{};
    Mesh::MeshRelayServiceClass Service{Mesh::MeshRelayServiceClass::BestEffort};
    Adapters::AdapterByteView Bytes{};
    Adapters::AdapterRouteToken Route{};
    MeshAdapters::MeshApplicationCompletionTarget Completion{};

    static MeshAdapters::MeshApplicationSubmissionResult Submit(
        void* owner,
        Adapters::AdapterRecordIdentity record,
        Mesh::MeshRelayServiceClass service,
        Adapters::AdapterByteView bytes,
        Adapters::AdapterRouteToken route,
        MeshAdapters::MeshApplicationCompletionTarget completion) noexcept {
        auto& self=*static_cast<MeshLifecycle*>(owner);
        self.Record=record;
        self.Service=service;
        self.Bytes=bytes;
        self.Route=route;
        self.Completion=completion;
        return {Adapters::LowerTransportDisposition::Accepted,17,true};
    }

    static bool Validate(void* owner) noexcept {
        return static_cast<MeshLifecycle*>(owner)->Running;
    }

    static void Cancel(void* owner,Adapters::AdapterRecordIdentity record) noexcept {
        auto& self=*static_cast<MeshLifecycle*>(owner);
        self.Cancelled=(record.Slot==self.Record.Slot && record.Generation==self.Record.Generation);
    }

    static void Quiesce(void* owner) noexcept {
        static_cast<MeshLifecycle*>(owner)->Quiesced=true;
    }
};

static constexpr std::uint8_t ServiceBit(Mesh::MeshRelayServiceClass service) noexcept {
    return static_cast<std::uint8_t>(std::uint8_t{1}<<static_cast<std::uint8_t>(service));
}

int main() {
    MockAdapterRuntime runtime;
    MeshLifecycle lifecycle;
    MeshAdapters::MeshApplicationLifecycleBinding mesh{};
    mesh.Owner=&lifecycle;
    mesh.Submit=&MeshLifecycle::Submit;
    mesh.Validate=&MeshLifecycle::Validate;
    mesh.Cancel=&MeshLifecycle::Cancel;
    mesh.Quiesce=&MeshLifecycle::Quiesce;
    mesh.ServiceClassMask=static_cast<std::uint8_t>(
        ServiceBit(Mesh::MeshRelayServiceClass::Clock)|
        ServiceBit(Mesh::MeshRelayServiceClass::Responsive));
    mesh.ProvidesDestinationPrimitiveAdmission=true;
    mesh.ProvidesValidatedOriginalSource=true;

    MeshAdapters::MeshLowerTransportBinding<MockAdapterRuntime> transport(runtime,mesh);
    assert(transport.IsValid());
    auto binding=transport.AdapterBinding();
    assert(binding);
    assert(binding.ProvidesDestinationPrimitiveAdmission);
    assert(binding.ProvidesValidatedOriginalSource);
    assert(binding.Supports(Adapters::AdapterServiceClass::Clock));
    assert(binding.Supports(Adapters::AdapterServiceClass::Responsive));
    assert(!binding.Supports(Adapters::AdapterServiceClass::Critical));
    assert(binding.Validate(binding.Owner));

    std::array<std::uint8_t,4> bytes{{1,2,3,4}};
    const Adapters::AdapterRecordIdentity record{
        Adapters::AdapterDirection::Outbound,
        Adapters::AdapterCapacityDomainKind::ResponsivePrivate,
        2,
        9
    };
    const Adapters::AdapterRouteToken route{0x1234};
    const auto submitted=binding.Submit(
        binding.Owner,record,Adapters::AdapterServiceClass::Responsive,
        {bytes.data(),bytes.size()},route);
    assert(submitted.Disposition==Adapters::LowerTransportDisposition::Accepted);
    assert(submitted.Generation==17);
    assert(submitted.DeferredCompletion);
    assert(lifecycle.Record.Slot==record.Slot && lifecycle.Record.Generation==record.Generation);
    assert(lifecycle.Service==Mesh::MeshRelayServiceClass::Responsive);
    assert(lifecycle.Bytes.Data==bytes.data() && lifecycle.Bytes.Size==bytes.size());
    assert(lifecycle.Route.Value==route.Value);
    assert(lifecycle.Completion);

    lifecycle.Completion.Complete(
        lifecycle.Completion.Owner,
        record,
        submitted.Generation,
        Adapters::LowerTransportDisposition::Accepted,
        Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted,
        true);
    assert(runtime.CompletionCount==1);
    assert(runtime.Last.Record.Slot==record.Slot);
    assert(runtime.Last.Record.Generation==record.Generation);
    assert(runtime.Last.TransportGeneration==17);
    assert(runtime.Last.Disposition==Adapters::LowerTransportDisposition::Accepted);
    assert(runtime.Last.HasDestinationAdmission);
    assert(runtime.Last.DestinationAdmission==Primitive::PrimitiveAdmissionDisposition::AlreadyAccepted);

    binding.Cancel(binding.Owner,record);
    binding.Quiesce(binding.Owner);
    assert(lifecycle.Cancelled);
    assert(lifecycle.Quiesced);

    lifecycle.Running=false;
    assert(!binding.Validate(binding.Owner));

    Mesh::MeshRelayServiceClass mapped{};
    assert(MeshAdapters::ToMeshRelayServiceClass(Adapters::AdapterServiceClass::Infrastructure,mapped));
    assert(mapped==Mesh::MeshRelayServiceClass::Infrastructure);
    assert(MeshAdapters::ToMeshRelayServiceClass(Adapters::AdapterServiceClass::BestEffort,mapped));
    assert(mapped==Mesh::MeshRelayServiceClass::BestEffort);

    return 0;
}
