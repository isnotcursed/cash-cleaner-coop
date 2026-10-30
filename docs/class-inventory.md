# Runtime class inventory

The read-only UE4SS probe completed twice on Steam Build ID `24569523`. Both
passes found the same 524 unique game classes. The normal save and Steam userdata
hashes were unchanged after the run.

This page records only class names relevant to co-op design. It does not include
the private runtime log, object instances, property values, or save contents.

## Session and player

- `GameplayGameMode`
- `GameplayGameState`
- `GameplayPlayerController`
- `GameplayPlayerState`
- `PlayerCharacter`
- `PlayerCharacterMovementComponent`
- `StateCharacterComponent`

The presence of distinct game mode, game state, player controller, and player
state classes makes Unreal's normal server-authoritative ownership model the first
path to investigate. Their existence alone does not prove that the shipped game
configured replication or a network driver.

## Interactive world

- `InteractiveObject`
- `InteractiveObjectComponent`
- `InteractiveObjectSubsystem`
- `InteractiveObjectSpawner`
- `InteractiveObjectZone`
- `InteractionCharacterComponent`
- `PickupCharacterComponent`
- `PickupObjectComponent`
- `PhysicsAttachCharacterComponent`
- `PhysicsAttachObjectComponent`
- `PhysicsSubsystem`
- `MoneyValueObjectComponent`

These are the likely seams for stable object identifiers, host ownership,
pickup/drop arbitration, and money-state deltas.

## Inventory and economy

- `Inventory`
- `InventoryCharacterComponent`
- `GridInventoryComponent`
- `PointerInventoryComponent`
- `MarketSubsystem`

## Story and persistence

- `QuestSubsystem`
- `Quest`, `QuestAction`, `QuestActivity`, `QuestObjective`, `QuestReward`
- `SaveSubsystem`
- `SaveState_GameplayPlayerController`
- `WorldStateSubsystem`

The host should be the only peer that commits quest, world, and save effects.
Clients should receive validated effect deltas, not write saves independently.

## Next questions

1. Are the gameplay player classes marked replicated at runtime?
2. Is an `IpNetDriver`, `SteamNetDriver`, or replication graph configured?
3. Which functions perform pickup, drop, spawn, destroy, and quest completion?
4. Which object field can serve as a persistent network identity?
5. Can a second player pawn be spawned without loading or overwriting a save?

## Network-default probe

A follow-up read-only pass inspected class-default networking fields and relevant
UFunction flags for the core allowlist.

| Class | `bReplicates` | `bAlwaysRelevant` | `NetUpdateFrequency` |
|---|---:|---:|---:|
| `GameplayGameMode` | false | false | 10 |
| `GameplayGameState` | true | true | 10 |
| `GameplayPlayerController` | false | false | 100 |
| `GameplayPlayerState` | true | true | 1 |
| `PlayerCharacter` | true | false | 100 |
| `InteractiveObject` | false | false | 100 |
| `Inventory` | false | false | 100 |

Pickup and physics-attachment components also report `bReplicates=false`.
Subsystem classes are not actors/components with the same networking fields, so
their unrelated-property reads were discarded rather than treated as values.

No inspected pickup, interaction, quest, save, or world-state UFunction had the
Unreal `FUNC_Net` flag. Relevant callable seams include:

- `PickupCharacterComponent:Pickup`, `Grab`, `Drop`, `Throw`
- `PickupObjectComponent:IsPickedUp`, `IsGrabbed`, `GetPickupTransform`
- `PhysicsAttachCharacterComponent:AttachObject`, `DetachObject`
- `QuestSubsystem:StartQuest`, `FinishQuest`, `OnQuestObjectiveProgressChanged`
- `SaveSubsystem:Save`, `Load`, `BlockSave`, `DisableAutoSave`
- `WorldStateSubsystem:RegisterWorldEvent`, `UnregisterWorldEvent`, `PostWorldMessage`

Derived direction: retain Unreal replication for player/game state where it is
already enabled, but add an explicit host-authoritative event and snapshot layer
for interactive objects, inventory, physics ownership, quests, and persistence.

## Pickup parameter probe

The reflected runtime signatures on the compatibility baseline are:

- `Pickup(Objects: ArrayProperty, Type: EnumProperty, Force: BoolProperty) -> BoolProperty`
- `Grab(Objects: ArrayProperty) -> BoolProperty`
- `Drop(Object: ObjectProperty, CancelAim: BoolProperty, keepState: BoolProperty,
  Reason: EnumProperty, combined: BoolProperty)`
- `Throw(Force: BoolProperty)`
- `SetIsPickedUp(pickedUp: BoolProperty, grabbed: BoolProperty) -> BoolProperty`

The native mod hooks `Pickup`, `Grab`, `Drop`, `Throw`, plus the actual input paths
`AutoPickUp` and `AutoGrab`. After an automatic pickup it queries
`GetFirstPickedUpObject` or `GetCarryingLargeObject`, derives an FNV-1a ID from the
reflected class and persistent `UniqueID` GUID (falling back to the full name only
when the GUID is unavailable), and sends the guest action through the
authority protocol. A denial invokes `Drop` with a reflected parameter buffer;
the resulting hook is suppressed so it cannot create an echo packet.

An isolated guest connected to a deterministic denial peer rolled back all tested
objects: `BP_MoneyPack_C` through `AutoPickUp`, and `BP_SmallBox_C` plus
`BP_MoneyPile_C` through `AutoGrab`. The owner visually confirmed that each object
fell out of the player's hands. Two-client convergence is still pending. A
deterministic grant peer also verified the local positive path:
one money pack completed grant/drop epochs `2/3`, grant/drop epochs `4/5`, and a
third grant at epoch `6`. The owner visually confirmed take, throw, retake, place,
and retake. `Throw` requires its own hook, while `Drop` can receive an internal
object instead of the held actor; both hooks therefore query the component for the
actual held object before releasing its lease.

Two isolated clients proved that reflected runtime object names are not stable:
the same pack used suffixes `_2147469628` and `_2147469637`. Its inherited
`UniqueID` GUID and `Seed` were identical on both clients, so network IDs now hash
the class plus `UniqueID`. Host pickup then hid the matching guest actor, and both
pickup directions shared ownership correctly.

The first drop prototype (one transform followed by local physics) produced large
position and rotation divergence. Repeating Euler transforms for five seconds was
also insufficient for carefully stacked small objects. The replacement protocol
uses monotonic sequence numbers, ownership epochs, quaternion rotation, and fields
for linear/angular velocity. Host world streaming now inventories `InteractiveObject`
actors by `UniqueID`, skips held objects, and sends changed kinematic poses under a
bounded per-tick budget. This path builds and passes codec/order tests but has not
yet met an in-game numeric convergence threshold.
