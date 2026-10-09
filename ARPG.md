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

## Passive web, tags and attributes

ARPG players spend in a passive web per class instead of talents: `Arpg/ArpgTree.{h,cpp}` (the
paladin web, 88 nodes built in code from its regions and arms; notables name vanilla talents,
resolved within the class's own tabs). Points: level − 1, plus one per first kill of a dungeon or
raid final boss (`Arpg::Bosses()`). Saved in `character_arpg_web` and `character_arpg_bosses`,
which the server creates (it drops the first tree's `character_arpg_tree`). Kinds 8 take,
9 respec, 10 query, 11 give back; `SMSG_ARPG_TREE` (0x33F, version 2: regions, nodes with
positions, links). An ARPG character's vanilla talent spells go at its first hello, except the
web's and the class's talent-granted actives (Consecration, Holy Shock…), given free at the
talent row's level.

`Arpg/ArpgTags.{h,cpp}`: every spell's tags, read off its data plus a few names.
`Arpg/ArpgCharacter.{h,cpp}`: the attributes' ARPG effects, the web's totals and keystones.
Hooks: `Unit::SpellDamageBonusDone` and `MeleeDamageBonusDone` (tag damage), the healing bonus
done and taken, `Unit::RollSpellCritOutcome` (Divine Favour), `Unit::UpdateSpeed` (Agility,
Crusade, Unyielding), `Unit::DealDamage` (Martyr), the melee block outcome (Shield Wall),
`Player::UpdateBlockPercentage`, the spell radius in `Spell` target fill and persistent area
auras (Strength, Intellect), the kill path (life on kill, Crusade, boss points), the heal path
(Lightforged, Blessed Recovery), and `Arpg::UpdateSwing` (the once-a-second refresh: speed, the
Spirit cooldown modifier, Zealot's drain, level-ups). Avenger is now the Judgement tree's; Martyr's Ward,
Dawnbringer and Purifying Light keep their hooks for later skill trees. Design: the client's
`docs/ARPG-CHARACTER.md`.

## Skills

`Arpg/ArpgSkills.{h,cpp}`: five specialisation slots (open at 1/10/20/30/40), two skill points a
level from 2 to 51, 20 at most in a skill, and the paladin's Strike, Seals, Judgement,
Consecration and Hammer of Justice trees. Saved in `character_arpg_skill` and
`character_arpg_skill_node`. Kinds 12 slot (`u8` slot, `u8` skill), 13 take (`u16` node), 14 give
back (`u16` node), 15 respec a skill (`u8` skill); kind 10 answers with the skills too.
`SMSG_ARPG_SKILLS` (0x340; `NUM_MSG_TYPES` 0x341). Nodes are spell modifiers, kit rows
(`LearnedModifiers`) or keystone ranks; hooks in `Unit::AttackerStateUpdate` (swing nodes),
`Unit::RemoveNoStackAurasDueToAuraHolder` (Twin Seals), the PPM proc chance (Relentless), the
Judgement script (Righteous Mind), `Arpg::SelectMeleeVictim` (Long Arm), and the uniques' spell
damage and landing hooks. An ARPG character also gets its class trainers' spells free at their
level (`TeachClassSpells`, spells without ranks' first pass).

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
