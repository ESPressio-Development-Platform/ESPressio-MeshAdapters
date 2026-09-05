# ESPressio MeshAdapters

Optional, dependency-correct integration between ESPressio Mesh and conceptual primitive-family libraries.

The first adapter is `EventMeshTransport`. It implements both Event's ownership-bearing `IEventTransport` contract and Mesh's bounded external `IPrimitiveReceiver` contract.

- Outbound Event packets retain their existing shared immutable backing and are transferred to an injected Mesh submission path without a per-recipient byte copy.
- Inbound Mesh payloads are borrowed, so an injected bounded packet owner must transfer them into Event-owned storage before Event's asynchronous transport manager receives them.
- The Event family protocol range, semantic fingerprint, advertised visibility, and selected outbound version are supplied by composition.
- Authenticated Mesh source/incarnation/delivery identity remains receive context. Event intentionally reduces it to Remote local-dispatch provenance rather than storing Mesh route state on Event objects.

MeshAdapters defines no radio, route algorithm, Group identity, packet framing, cryptographic handshake, or default payload byte capacity.

During the coordinated Mesh implementation tranche, use the matching propagation branches for Mesh, Event, Primitive, and System: `structural_realignment_propagation_ESPressio-Mesh` (Mesh itself uses `structural_realignment_propagation`).
