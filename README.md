# ESPressio MeshAdapters

Optional, dependency-correct integration between ESPressio Mesh and conceptual primitive-family libraries.

The first adapter is `EventMeshTransport`. It implements both Event's ownership-bearing `IEventTransport` contract and Mesh's bounded external `IPrimitiveReceiver` contract.

- Outbound Event packets retain their existing shared immutable backing and are transferred to an injected Mesh submission path without a per-recipient byte copy.
- Inbound Mesh payloads are borrowed, so an injected bounded packet owner must transfer them into Event-owned storage before Event's asynchronous transport manager receives them.
- The Event family protocol range, semantic fingerprint, advertised visibility, and selected outbound version are supplied by composition.
- Authenticated Mesh source/incarnation/delivery identity remains receive context. Event intentionally reduces it to Remote local-dispatch provenance rather than storing Mesh route state on Event objects.

`EventMeshNodeSubmission` is the bounded outbound implementation for one Node-resolving Event transport route. It obtains the destination incarnation and immutable deadline from an injected context provider, issues a fresh per-delivery `MeshMessageId`, admits an Event-family application aggregate, and retains exactly one shared Event packet reference until terminal aggregate release. Composition enumerates admitted handles to begin routing; the adapter does not invent a route service or wire framing.

MeshAdapters defines no radio, route algorithm, Group identity, packet framing, cryptographic handshake, or default payload byte capacity.

During the coordinated Mesh implementation tranche, use the matching propagation branches for Mesh, Event, Primitive, System, and Radio: `structural_realignment_propagation_ESPressio-Mesh` (Mesh itself uses `structural_realignment_propagation`). Observable remains on `structural_realignment`.
