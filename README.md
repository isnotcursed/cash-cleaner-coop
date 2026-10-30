# Cash Cleaner Co-op

Experimental co-op mod for **Cash Cleaner Simulator**.

The project is in the reverse-engineering and protocol-design phase. It is not a
playable multiplayer release yet.

## Compatibility baseline

- Steam App ID: `2488370`
- Tested Steam Build ID: `24569523`
- Engine: Unreal Engine 5.4
- Loader: UE4SS `v3.0.1-940-g01e0a584`

No game files, assets, saves, logs, or decompiled code are included.

## Current probe

`ue4ss/Mods/CashCleanerCoopProbe` is a read-only runtime inventory probe. It logs
unique game-specific class names after a map loads. For a small allowlist of core
classes, it also reads class-default networking fields and function flags. It does
not read live actor state, alter objects, or inspect save contents.

Install it into an existing UE4SS test installation:

```sh
./tools/install-probe.sh /path/to/CashCleanerSimulator/Binaries/Win64/ue4ss
```

Enable it by adding this line to `ue4ss/Mods/mods.txt`:

```text
CashCleanerCoopProbe : 1
```

## Status

- [x] Isolated UE4SS boot smoke test
- [x] Runtime class inventory (524 unique classes on the compatibility baseline)
- [x] Initial fake-world state and reliable-link model (10 scenarios, 9 twins)
- [ ] Player avatar replication
- [ ] Item and machine ownership
- [ ] Quest and save-state synchronization
- [ ] Automated two-client tests

## Safety

Development uses an isolated game copy and isolated Proton prefix. Back up saves
before testing any game mod. This project does not bypass anti-cheat or online
service restrictions.

## Protocol tests

The game-free C++ model covers host arbitration, idempotent requests, ownership
epochs, session changes after host load, and monotonic quest progress:

```sh
./tools/test-protocol.sh
```

## License

MIT. See [LICENSE](LICENSE).
