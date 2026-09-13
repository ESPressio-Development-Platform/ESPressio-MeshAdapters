#pragma once

#include <ESPressio_AdapterTransport.hpp>
#include <ESPressio_DeviceIdentifier.hpp>

namespace ESPressio::MeshAdapters {

/// <summary>Fixed composition seam resolving semantic Mesh destinations to stable opaque A2 route tokens.</summary>
/// <remarks>
/// Tokens are owned by the concrete Mesh transport composition and must remain valid for the complete maximum residence
/// of any A2 record that references them. This seam performs no allocation and owns no per-occurrence route registry.
/// A DeviceIdentifier is never packed, truncated or hashed into AdapterRouteToken by generic MeshAdapters code.
/// </remarks>
struct MeshRouteBinding final {
    void* Owner{nullptr};
    bool (*Validate)(void*) noexcept{nullptr};
    bool (*ResolveNode)(void*,const System::DeviceIdentifier&,Adapters::AdapterRouteToken&) noexcept{nullptr};
    bool (*ResolveBroadcast)(void*,Adapters::AdapterRouteToken&) noexcept{nullptr};

    constexpr explicit operator bool() const noexcept {
        return Owner!=nullptr&&Validate!=nullptr&&ResolveNode!=nullptr;
    }
    bool IsValid() const noexcept { return *this&&Validate(Owner); }
    bool TryResolveNode(const System::DeviceIdentifier& device,Adapters::AdapterRouteToken& route) const noexcept {
        route={};
        return *this&&device&&ResolveNode(Owner,device,route)&&static_cast<bool>(route);
    }
    bool TryResolveBroadcast(Adapters::AdapterRouteToken& route) const noexcept {
        route={};
        return *this&&ResolveBroadcast&&ResolveBroadcast(Owner,route)&&static_cast<bool>(route);
    }
};

} // namespace ESPressio::MeshAdapters
