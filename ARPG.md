# cmangos ARPG fork

This fork (Gribby/mangos-classic, branch `arpg`) is the server half of the benilla ARPG client
(Gribby/benilla, branch `arpg`; that repo's `ARPG.md` has the full brief and roadmap). It turns
tab-target combat into Diablo-style targetless combat for players on that client.

## Gating

- `Arpg.Enable = 1` in `mangosd.conf` (default 0).
- A player becomes ARPG only after the client sends its hello; `Arpg::Active()` gates every hook
  and is false for charmed players. Playerbots and stock clients keep stock combat.

## Wire

`CMSG_ARPG_ACTION` = 0x33C (`NUM_MSG_TYPES` 0x33D), protocol version 2. Body: `u8 kind`, then

| kind | payload |
|---|---|
| 0 hello | `u8` version |
| 1 swing start | `u64` intended unit |
| 2 swing stop | — |
| 3 cast | `u32` spell, `u8` aim (0 enemy, 1 ally), `f32` x y z, `u64` intended unit |

## Where

- `src/game/Arpg/ArpgCombat.{h,cpp}`: swings (arc strike or whiff, extra attacks), `CastAt` (picks
  the unit: arc for melee abilities, first enemy along the aimed line for skillshots, friend nearest
  the cursor for helpful spells), facing helpers.
- `src/game/Arpg/ArpgHandler.cpp`: the opcode handler.
- Hooks: `Player` (state, `Update`, teleport), `Unit` (no stock auto-attack, facing-based
  dodge/parry/block, backpedal speed), `Spell` (no facing checks; skillshot line re-resolved at
  release; with nobody on the line, or the target dead, the shot flies out to max range and is
  spent), `SpellEffects` (Charge), `UnitAuraProcHandler` (Flurry), `Opcodes`, `World` (config).

Every hook is commented `ARPG:`.

## Build

CMake as upstream (`-DPCH=1` recommended; re-run cmake after adding files, sources are globbed).
On a ~7 GB sandbox use `make -j2`, and compile single files while iterating
(`make Arpg/ArpgCombat.o` in a non-PCH build's `src/game`). `PetAI.cpp` fails without PCH
upstream; that is not ours.
