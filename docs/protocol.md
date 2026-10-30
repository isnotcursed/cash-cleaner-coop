# Co-op protocol model

This is the game-free model under `protocol/`. It intentionally contains no
UE4SS or game dependencies.

## Authority

- The host is the only peer that accepts or rejects pickup and drop requests.
- An interactive object has one holder, one simulation owner, and an ownership
  epoch.
- Each accepted ownership change increments the epoch.
- A replica applies state only when sender and epoch match its current assignment.
- Every message carries a session number. Loading a host save starts a new session,
  so delayed messages from the old world cannot apply.

## Idempotency

Every player action has a `RequestId`. The host records applied IDs and returns a
duplicate result without applying the action again. Quest stages merge by maximum,
so reordered progress cannot move a quest backwards.

## Current scenarios

1. Two players pick up the same object: exactly one wins.
2. A duplicated drop request cannot move or create the object twice.
3. A state update from a former owner and older epoch is rejected.
4. A delayed update from before a host load is rejected by session number.
5. Quest progress is idempotent and monotonic.

## Mutation twins

The same scenarios must fail when each guard is deliberately disabled:

- `pickup_no_arbitration`
- `no_dedupe`
- `accept_stale_epoch`
- `accept_old_session`

The test binary prints `CAUGHT` for every twin and fails if a broken guard escapes.

## Next increment

The model now includes a deterministic virtual clock and in-memory datagram link
with latency, jitter, loss, and duplication. A small reliable layer provides
sequence numbers, acknowledgements, retry timers, ordered delivery, and duplicate
suppression.

Two additional scenarios prove that:

- dropping every packet's first transmission still delivers all application
  messages exactly once and in order through retransmission;
- duplicating every transmission still delivers each application message once.

The `no_retransmit` and `no_receive_dedupe` mutation twins are both caught.

Next: connect request and state messages to two complete peer replicas, then test
late-join snapshots and ownership handoff over the impaired link.

## Late join

`WorldSnapshot` atomically carries the session number, object state, current holder,
ownership epoch, and quest stages. A late joiner test now enters after another
player has picked up an object and advanced a quest, then verifies that all of that
authority and story state arrives intact.

The `join_omit_ownership` twin strips owner and epoch data from the snapshot; the
same scenario catches it. Current total: 8 scenarios, 7 twins, 0 failures.
