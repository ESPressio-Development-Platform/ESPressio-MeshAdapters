# Tranche 8 MeshAdapters Integration Gate

Date: 2026-09-13
Branch: `primitives_redesign`

This file records the final cross-repository integration gate for Primitive Platform Redesign Tranche 8.

## Purpose

`ESPressio-MeshAdapters` is the optional family integration layer between neutral A2/Mesh transport semantics and Event, Command, State and future Primitive families. It must not create a second family runtime, retry engine, fragment scheduler or semantic identity system.

The final Tranche-8 gate executes this repository's real family contracts while resolving the live `primitives_redesign` branches of Mesh, Adapters, Primitive and the family libraries.

## Required ownership boundaries

- A2 owns finite logical pursuit after synchronous byte ownership handoff.
- Mesh owns routing, forwarding, Broadcast dissemination and Mesh application lifecycle.
- Radio/R3 owns physical fragmentation/reassembly/arbitration.
- Event/Command/State runtimes own their respective family semantics and M1 decisions.
- route tokens are opaque transport facts; no DeviceIdentifier packing/truncation is permitted.
- Mesh membership incarnation is never substituted for System runtime incarnation.
- authenticated semantic provenance remains distinct from immediate peer/route provenance.

## Family replacement coverage

The current contracts cover:

- neutral Mesh -> A2 ingress correlation;
- neutral A2 -> Mesh lower transport;
- Event ingress and local outbound Event -> A2 handoff;
- remote-origin Event suppression from local re-egress;
- Command ingress, exact terminal duplicate behavior, local no-response egress, response-bearing terminal delivery failure and durable recovered-response routing;
- State ingress, local convergence egress and terminal exhaustion feedback into State-owned `NeedsConvergence`;
- generic Broadcast restrictions for families requiring destination admission, response-bearing Command, and canonical State;
- predecessor Event-only transport/submission architecture removal.

## Final dependency baseline

The integration run triggered by this file must resolve the finalized `ESPressio-Mesh/primitives_redesign` branch containing the M8-23/M8-24 closure work and must use `ESPressio-Adapters/primitives_redesign` including the mixed-policy evidence correction.

Formal Tranche-8 closure is recorded only after the combined and focused workflows succeed against those live dependency tips and the Primitive living handoff is updated with the exact evidence.

No version or release action is authorized by this integration record.
