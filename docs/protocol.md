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

Add a deterministic virtual clock and lossy in-memory datagram link, then prove
join snapshots, retransmission, reordering, and ownership handoff without running
the game.
