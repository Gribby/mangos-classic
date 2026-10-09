# cmangos ARPG fork

This fork (Gribby/mangos-classic, branch `arpg`) is the server half of the benilla ARPG client
(Gribby/benilla, branch `arpg`; that repo's `ARPG.md` has the full brief and roadmap). It turns
tab-target combat into Diablo-style targetless combat for players on that client.

## Gating

- `Arpg.Enable = 1` in `mangosd.conf` (default 0).
- `Arpg.DevTools = 1` (default 0) lets ARPG players ask for test loot (`ACTION_DEV_LOOT`, kind 7:
  `u8` quality or 0xFF mixed, `u8` count, `u8` item level or 0): `Arpg::DropDevLoot` kills a fresh
  `Arpg.DevTools.Creature` (default 721) ahead of them and puts the loot on its corpse.
- A player becomes ARPG only after the client sends its hello; `Arpg::Active()` gates every hook
  and is false for charmed players. Playerbots and stock clients keep stock combat.

## Uniques

Named items whose ARPG wearer's spells work differently: `Arpg/ArpgUniques.{h,cpp}` (the table is
`Arpg::Uniques()`, one line per item). Built kits (all but Ravager, Bonereaver's Edge and Striker's Mark): A, extra projectiles (free casts of the same
spell, `Spell::SetArpgSecondary`, hooked in `Spell::cast`'s skillshot block); C chain, D
fragments, E burst, G arc, B pierce, H shockwave, K step (direct damage hooked after the spell's damage in
`Spell::DoAllEffectOnTarget`; chain jumps and fragments fly, drawn by a cosmetic
`SMSG_SPELL_GO` and landed by a delayed event); I echo in `OnLineLaunch`; F spread on landing (`Spell::DoAllEffectOnTarget`, before `OnAfterHit`) and on death, J raise, both from `Arpg::OnKill` in the creature kill path. `SMSG_ARPG_ITEM_MECHANICS` (0x33E: `u8` count,
then `u32` item and a C string per row) answers every hello. Design: the client's `docs/ARPG-UNIQUES.md`.

## Skill tree

ARPG players spend in a tree per class instead of talents: `Arpg/ArpgTree.{h,cpp}` (the paladin
tree; nodes name vanilla talents and spells, resolved from the DBCs at first use). Saved in
`character_arpg_tree`, which the server creates. Kinds 8 spend, 9 respec, 10 query;
`SMSG_ARPG_TREE` (0x33F). Hooks: `Player::LoadFromDB` (load before spells), `Player::addSpell` /
`removeSpell` (tree spells cost no talent points), `HandleLearnTalentOpcode` (ignored for ARPG
players), the Judgement script (Avenger), `Spell.cpp` heal path (Dawnbringer) and creature-type
checks (Purifying Light), `Arpg::UpdateSwing` (Martyr's Ward). Design: the client's
`docs/ARPG-SKILL-TREES.md`.

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
