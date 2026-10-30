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
