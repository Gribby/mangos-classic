/*
 * ARPG tags, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Tags").
 *
 * Every spell carries a set of tags, read off its own data (school, damage class, effects, auras)
 * plus a few by name (Seals, Auras, Curses). Passive web nodes and item affixes name tags, never
 * spells: "+4% Holy damage" raises every spell tagged Holy. A spell's bonus is the sum of the
 * player's bonuses for each of its tags (Arpg::DamageDoneMod, ArpgCharacter.h).
 */

#ifndef MANGOS_ARPG_TAGS_H
#define MANGOS_ARPG_TAGS_H

#include "Common.h"

struct SpellEntry;

namespace Arpg
{
    enum Tag : uint8
    {
        // Damage type
        TAG_PHYSICAL = 0,
        TAG_HOLY,
        TAG_FIRE,
        TAG_FROST,
        TAG_ARCANE,
        TAG_NATURE,
        TAG_SHADOW,
        // Delivery
        TAG_MELEE,
        TAG_SPELL,
        TAG_PROJECTILE,
        TAG_AREA,
        TAG_GROUND,
        TAG_CHANNEL,
        TAG_DURATION,
        // Role
        TAG_HEAL,
        TAG_CONTROL,
        TAG_MOVEMENT,
        TAG_MINION,
        TAG_AURA,
        TAG_SEAL,
        TAG_CURSE,
        TAG_SHOUT,
        TAG_TOTEM,
        MAX_TAG
    };

    constexpr uint32 TagBit(Tag tag) { return uint32(1) << tag; }

    // The tags of `spellInfo`; nullptr is a weapon swing (Melee, Physical).
    uint32 TagsOf(SpellEntry const* spellInfo);

    // The tag's display name ("Holy").
    char const* TagName(Tag tag);
}

#endif
