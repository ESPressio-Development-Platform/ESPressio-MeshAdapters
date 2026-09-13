# ESPressio MeshAdapters

`ESPressio-MeshAdapters` is the family-neutral integration layer between ESPressio Mesh, the generic ESPressio Adapters runtime (A2), and transmissible primitive families such as Event, Command and State.

The current `primitives_redesign` architecture deliberately does **not** implement a second Event-specific mesh transport, route engine, retry engine, fragmentation layer, or application runtime. MeshAdapters translates between already-owned family semantics and the bounded adapter/mesh contracts; ownership remains with the layer that defines each concern.

## Architecture

### Mesh ingress

`ESPressio_MeshAdapterIngress.hpp` bridges authenticated Mesh application delivery into A2. Mesh forwarding/deduplication and local primitive admission remain independent facts: network `Seen`/`Forwarded` state does not imply local family acceptance, and a locally deferred admission may be retried without re-fanning the packet through the mesh.

Borrowed Mesh bytes are copied into bounded adapter-owned storage before asynchronous processing. Only a family result of `Accepted` or `AlreadyAccepted` establishes destination Primitive admission evidence.

### A2 outbound to Mesh

`ESPressio_MeshLowerTransportBinding.hpp` is the neutral lower transport used by A2 for outbound primitive bytes. A2 owns logical pursuit, bounded retry state and required evidence; Mesh owns route/forwarding/application lifecycle; Radio owns physical contention, framing and fragmentation beneath Mesh.

`ESPressio_MeshRouteBinding.hpp` resolves semantic `DeviceIdentifier` destinations to opaque `AdapterRouteToken` values. Route tokens are transport facts and must not encode, truncate or substitute for device identity.

### Event

`ESPressio_EventMeshAdapterBinding.hpp` adapts authenticated Event V1 ingress into the real Event runtime. Event semantics, type registration, deserialization, idempotency/provenance and local dispatch remain owned by ESPressio-Event.

`ESPressio_EventMeshAdapterOutboundTarget.hpp` is the Event `ExternalAdapter` target for local outbound occurrences. Borrowed Event lease data is synchronously encoded into A2-owned bytes. Remote-origin Event occurrences are not re-egressed through the external adapter, preventing source feedback loops.

### Command

`ESPressio_CommandMeshAdapterBinding.hpp` adapts Command V1 ingress and outbound request/response traffic without duplicating the Command runtime. Command remains owner of execution, persistence, replay/idempotency, completion semantics and response routing.

For response-bearing requests, MeshAdapters retains only a bounded generation-safe delivery correlation token while A2 pursues the owned bytes. Terminal A2 failure without required destination-admission evidence publishes the existing Command delivery failure exactly once. Durable responses recovered after restart reserve a bounded MeshAdapter destination during Command initialization and are emitted through the same A2 response encoder after start.

### State

`ESPressio_StateMeshAdapterBinding.hpp` adapts State V1 ingress and the real `StateTransportBinding` egress seam. State remains sole owner of owner-authoritative truth, sessions, versions, baselines, resync and convergence state.

MeshAdapters may retain only the immutable `StateConvergenceHandle` needed to correlate terminal A2 pursuit feedback. A terminal pursuit failure is returned to State from service context through `ReportConvergenceExhausted`; the adapter does not create a second State retry schedule.

## Resource and lifecycle rules

All queues, byte arenas, delivery correlations and response destinations are explicitly bounded. No MeshAdapter path may introduce hidden unbounded allocation or a family-local retry worker. Registration/topology and family bindings are frozen before runtime start. Transport bindings that must participate in family initialization are configured before the family runtime initializes, but their start validation observes the final frozen MeshAdapter composition.

The redesign intentionally removes the predecessor Event-only `EventMeshTransport`, `EventMeshNodeSubmission`, `EventMeshSelectiveSubmission`, and `EventMeshBroadcastSubmission` architecture. There is no compatibility shim for those paths in the clean 1.0.0 contract.

## Validation

The `tests/` contracts exercise the neutral Mesh/A2 boundary and real Event, Command and State runtimes. In particular they cover Event remote-to-local dispatch and no re-egress, Command idempotency, local request delivery, terminal request-delivery failure and durable recovered-response routing, and State ingress plus terminal convergence-exhaustion feedback.
