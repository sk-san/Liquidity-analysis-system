# Calculation-engine integration contract

The transport decoder must produce a `RoutedMarketEvent` containing:

- `symbol`: routing key for the per-symbol shadow book;
- `received_wall_time_ns`: local arrival timestamp;
- `payload`: canonical shadow-book event.

The stream must satisfy these rules:

1. Sequence numbers are monotonically increasing per symbol/channel.
2. Startup begins with a full L3 `SnapshotEvent` unless
   `require_initial_snapshot=false` is explicitly configured.
3. `ADD`, `DELETE`, `PARTIAL_CANCEL`, `MODIFY`, `REPLACE` and `EXECUTE` events
   are never coalesced.
4. An execution identifies the passive physical `entry_id`; the calculation
   engine does not select a counterparty.
5. `passive_remaining_quantity` should be supplied so the shadow book can
   compare its delta with the ABIDES authoritative state.
6. After a detected gap, incrementals are blocked until a new snapshot arrives.

The core API remains transport-independent. The bundled
`calculation_engine_service` consumes the repository's canonical `MDP1`
MessagePack frames over a ZeroMQ `PULL` socket. Alternative adapters may use
JSON, Protocol Buffers, or a fixed binary schema as long as they perform the
same validation and map into these C++ event types.
