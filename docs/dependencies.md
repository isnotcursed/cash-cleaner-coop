# Development dependencies

## UE4SS runtime

- Official repository: `UE4SS-RE/RE-UE4SS`
- Release: `experimental`
- Version: `v3.0.1-940-g01e0a584`

## UE4SS development archive

- Asset: `zDEV-UE4SS_v3.0.1-940-g01e0a584.zip`
- Size: 40,736,475 bytes
- SHA-256: `99320622895fd226aff2a0681bd3ad694a3115bd64e12e1ff7519c731b069cc5`
- ZIP integrity test: passed

The archive contains the runtime DLL/PDB, documentation, UE version layout
templates, and bundled mods. It does **not** contain C++ headers or import
libraries, so it is insufficient by itself for building a native UE4SS mod.
Obtaining the matching official source tree and its pinned submodules is a separate
dependency step.
