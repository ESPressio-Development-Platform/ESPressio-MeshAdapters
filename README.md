# ESPressio MeshAdapters

Optional, dependency-correct integration between ESPressio Mesh and conceptual primitive-family libraries.

The first adapter is `EventMeshTransport`. It implements both Event's ownership-bearing `IEventTransport` contract and Mesh's bounded external `IPrimitiveReceiver` contract.

- Outbound Event packets retain their existing shared immutable backing and are transferred to an injected Mesh submission path without a per-recipient byte copy.
- Inbound Mesh payloads are borrowed, so an injected bounded packet owner must transfer them into Event-owned storage before Event's asynchronous transport manager receives them.
- The Event family protocol range, semantic fingerprint, advertised visibility, and selected outbound version are supplied by composition.
- Authenticated Mesh source/incarnation/delivery identity remains receive context. Event intentionally reduces it to Remote local-dispatch provenance rather than storing Mesh route state on Event objects.

`EventMeshNodeSubmission` is the bounded outbound implementation for one Node-resolving Event transport route. It obtains the destination incarnation and immutable deadline from an injected context provider, issues a fresh per-delivery `MeshMessageId`, admits an Event-family application aggregate, and retains exactly one shared Event packet reference until terminal aggregate release. Composition enumerates admitted handles to begin routing; the adapter does not invent a route service or wire framing.

`EventMeshSelectiveSubmission` is the corresponding Group/CapabilitySelector bridge. It resolves authenticated Active remote profiles exactly once through Mesh's bounded resolver, assigns each frozen device/incarnation an independent `MeshMessageId`, and admits one aggregate sharing the original immutable Event packet and deadline. A matching local profile is handed to an injected local Event dispatcher rather than fabricated as a remote Node delivery. Remote-capacity failure occurs before any local dispatch; after remote admission, local rejection is an explicit independent result and cannot roll back accepted remote deliveries. Resolver overflow fails the whole selection and never truncates it.

`EventMeshBroadcastSubmission` is the synchronous Event-family bridge into a configured `MeshV1BroadcastCoordinator`. It supplies the Event primitive descriptor and borrows the already-owned immutable packet only while Mesh copies it into the explicit protected-frame workspace. Deadline, hop limit and the bounded direct-neighbour plan remain composition policy. It retains no packet, retry, acknowledgement or recipient outcome and reports only whether Mesh completed the best-effort submission attempt.

MeshAdapters defines no radio, route algorithm, Group identity, packet framing, cryptographic handshake, or default payload byte capacity. The adapter consumes Mesh's opaque `GroupIdentifier` and authenticated profile semantics without redefining them.

During the coordinated Mesh implementation tranche, use the matching propagation branches for Mesh, Event, Primitive, System, and Radio: `structural_realignment_propagation_ESPressio-Mesh` (Mesh itself uses `structural_realignment_propagation`). Observable remains on `structural_realignment`.
