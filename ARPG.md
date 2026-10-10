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

`Arpg/ArpgSkills.{h,cpp}`: five specialisation slots (open at 1/10/20/30/40), one skill point a
level from 2 (59 at 60; the web, one every second level and one per final boss), 20 at most in a skill, and the paladin's Strike, Seals, Judgement,
Consecration and Hammer of Justice trees. Saved in `character_arpg_skill` and
`character_arpg_skill_node`. Kinds 12 slot (`u8` slot, `u8` skill), 13 take (`u16` node), 14 give
back (`u16` node), 15 respec a skill (`u8` skill); kind 10 answers with the skills too.
`SMSG_ARPG_SKILLS` (0x340). Nodes are spell modifiers, kit rows
(`LearnedModifiers`) or keystone ranks; hooks in `Unit::AttackerStateUpdate` (swing nodes),
`Unit::RemoveNoStackAurasDueToAuraHolder` (Twin Seals), the PPM proc chance (Relentless), the
Judgement script (Righteous Mind), `Arpg::SelectMeleeVictim` (Long Arm), and the uniques' spell
damage and landing hooks. An ARPG character also gets its class trainers' spells free at their
level (`TeachClassSpells`, spells without ranks' first pass).

## Packs

`Arpg/ArpgPacks.{h,cpp}`: open-world mobs lead packs of followers sized by level (Arpg.Packs,
on by default with Arpg.Enable). Hooks: `Creature::AddToWorld` (form a pack), the combat start
in `Unit` (the pack joins in), `MaNGOS::XP::Gain` (follower XP), the corpse loot (`Loot::ThinArpgLoot`),
`Arpg::DamageDoneMod` (follower damage), and kind 16, the dev tools' test pack (`u8` size,
optional `u8` tier). Champion and rare packs (from level 8) add affixed summons; their hooks are
`Unit::SpellDamageBonusTaken` and `MeleeDamageBonusTaken` (Stone Skin), `Unit::UpdateSpeed`
(Extra Fast), the melee proc point (`OnPackMelee`: Fire and Cold Enchanted, Vampiric, Thorns) and
the death (Fire Enchanted's burst). `SMSG_ARPG_CHAMPIONS` (0x341; `NUM_MSG_TYPES` 0x342) tells
each ARPG player of the champions within 100 yards. Design: the client's
`docs/ARPG-PROGRESSION.md`.

## Wire

`CMSG_ARPG_ACTION` = 0x33C, protocol version 2. Body: `u8 kind`, then

| kind | payload |
|---|---|
| 0 hello | `u8` version |
| 1 swing start | `u64` intended unit |
| 2 swing stop | — |
| 3 cast | `u32` spell, `u8` aim (0 enemy, 1 ally), `f32` x y z, `u64` intended unit |
| 4–16 | aim, loot, dev loot, web, skills, dev pack: see `Arpg/ArpgCombat.h` |
| 17 dodge | `f32` x y: roll toward the point (`Arpg/ArpgActions.h`) |
| 18 flask | — |
| 19 tier | `u8` the dungeon tier asked for (`Arpg/ArpgDungeons.h`) |
| 20 unseal | `u16` capstone node: unseal it with Codex fragments (`Arpg/ArpgSkills.h`) |
| 21 socket | `u8` skill, `u8` rune (0 empties): socket a rune |

Server to client, 0x33D to 0x342 (`NUM_MSG_TYPES` 0x343): loot, item mechanics, the web, skills,
champions, `SMSG_ARPG_STATUS` (0x342: `u8` flask charges, `u8` max, `u8` the next charge's
progress, `u32` ms until the roll is ready, `u32` the roll's cooldown).

## Dungeons and survival

`Arpg/ArpgDungeons.{h,cpp}` (Arpg.Dungeons, on): health scaled by the players present in instances
and for open-world elites, per-hit damage caps by source, dungeon champions and rares, a Warden
per dungeon, a Cache at each final boss, and difficulty tiers (`character_arpg_tier`, created by
the server). Hooks: `Creature::AddToWorld` (through `Arpg::OnCreatureAdded`), `RemoveFromWorld`
(`Arpg::OnCreatureRemoved`), the combat start in `Unit` (`OnScaledAggro`), the end of
`Unit::CalculateMeleeDamage` and `CalculateSpellDamage` and `Aura::PeriodicTick` (`DamageCap`),
and the corpse loot (`OnDungeonLoot`). `Arpg/ArpgActions.{h,cpp}`: the roll (a knockback; dodges
in `Unit::RollMeleeOutcomeAgainst` and `SpellHitResult` while airborne) and the flask (kills fill
it from `Unit::JustKilledCreature`). Design: the client's `docs/ARPG-PROGRESSION.md`.

## Threats

`Arpg/ArpgThreats.{h,cpp}`: a hostile creature's bolt (a travelling, single-target, harmful spell)
at an ARPG player flies at the point the player stood on (`Spell::AddUnitTarget` notes the line,
`Spell::DoAllEffectOnTarget` asks `BoltOutcome` at arrival: a dodge mid-roll, a miss off the
line). Open-world elites, champions, rares and bosses wind up telegraphed attacks (ring, cone,
blast) from `Unit` combat start (`OnTelegraphAggro`), on the creature's own event queue; they land
for a share of maximum health past the damage caps, and a stun, fear or confuse breaks them off.
`SMSG_ARPG_TELEGRAPH` = 0x344. Design: the client's `docs/ARPG-PROGRESSION.md`, "Threats and the
roll". Mana on hit is a stat: two web nodes (Bloodthirst, Devotion) and an item affix.

## Monster moves and globes

`Arpg/ArpgThreats.cpp`: every creature fighting an ARPG player without a heavy attack has one
move by family, class or name (lunge and charge lines, maul and cleave cones, slam rings, web and
bomb blasts, a caster's back-off), through the same wind-up chain; murlocs call their kin.
`Arpg/ArpgActions.cpp`: health globes, dropped on kills (`OnFlaskKill`), `SMSG_ARPG_GLOBE` 0x345,
taken with `CMSG_ARPG_ACTION` kind 23. A channel firing each tick (Arcane Missiles) takes each
missile's target off its aim line (`Arpg::ChannelTickTarget`, in `SingleEnemyTargetAura`).

## Town portal, death, junk

`Arpg/ArpgActions.{h,cpp}`: the town portal (`CMSG_ARPG_ACTION` kind 22: two seconds still, out of
combat, to the bind point; back from town within 30 minutes) and the death checkpoint
(`Player::RepopAtGraveyard` asks `Arpg::RespawnAtCheckpoint`: an ARPG character rises at once at
half health and mana, at a dungeon's entrance trigger target or the nearest graveyard).
`Arpg/ArpgLoot.cpp` `SellJunk`: a grey item with a sell price picked up by an ARPG player is its
price in gold (`Loot::SendItem`). Champion dangers are telegraphed through `Arpg::ShowTelegraph`
(Fire Enchanted's death burst, Cold Enchanted's nova).

## Pacing

`Arpg/ArpgCharacter.cpp`, for ARPG players: mana comes from fighting, not drinking (1% of the
maximum per swing that lands, 1% per skill hit at most four times a second, 3% per kill, plus
the web's and items' mana on hit per swing and skill hit); two
seconds out of combat, health and mana flow back at 8% a second; the run is 10% faster (the
Agility speed cap is 30% with it); every class spell's global cooldown is a third of vanilla's
(1.5 sec to 0.5) and its cast time half, as spell modifiers the client follows. `Player::AddGCD`
keeps a short GCD less the latency at 1 ms or more (zero would read as the spell's own GCD, a
negative one would wrap).

## Item affixes

`Arpg/ArpgAffixes.{h,cpp}`: a seed per eligible loot item (`LootItem::arpgSeed`, set after the
corpse's loot is made), kept with the stored item (`Loot::SendItem`, `character_arpg_item`),
added into the totals when worn (`ArpgTree.cpp` Refresh; `Player::_ApplyItemMods` refreshes),
and sent to the client after the hello and on store (`SMSG_ARPG_ITEM_AFFIXES`, 0x343;
`NUM_MSG_TYPES` 0x344).

## Codex pages and runes

`Arpg/ArpgCodex.{h,cpp}`: the Codex items (entries 90001 fragment, 90011-90019 runes, 90000 +
capstone node pages), written into `item_template` at start before the templates load
(`World::SetInitialWorldSettings`), their drops (from `Arpg::OnDungeonLoot`), and the pickup
(`Loot::SendItem` asks `Arpg::TakeCodexItem`, which keeps them out of the bags). The seals,
fragments, sockets and rune effects are `Arpg/ArpgSkills.{h,cpp}`; `SMSG_ARPG_SKILLS` is version 2.

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
