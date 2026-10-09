/*
 * ARPG tags: see ArpgTags.h.
 */

#include "Arpg/ArpgTags.h"

#include "Server/DBCStores.h"
#include "Spells/SpellMgr.h"

#include <cstring>
#include <vector>

namespace
{
    using namespace Arpg;

    bool StartsWith(char const* name, char const* prefix)
    {
        return std::strncmp(name, prefix, std::strlen(prefix)) == 0;
    }

    bool EndsWith(char const* name, char const* suffix)
    {
        size_t const n = std::strlen(name), m = std::strlen(suffix);
        return n >= m && std::strcmp(name + n - m, suffix) == 0;
    }

    uint32 Derive(SpellEntry const* spell)
    {
        uint32 tags = 0;
        switch (spell->School)
        {
            case SPELL_SCHOOL_NORMAL: tags |= TagBit(TAG_PHYSICAL); break;
            case SPELL_SCHOOL_HOLY:   tags |= TagBit(TAG_HOLY);     break;
            case SPELL_SCHOOL_FIRE:   tags |= TagBit(TAG_FIRE);     break;
            case SPELL_SCHOOL_NATURE: tags |= TagBit(TAG_NATURE);   break;
            case SPELL_SCHOOL_FROST:  tags |= TagBit(TAG_FROST);    break;
            case SPELL_SCHOOL_SHADOW: tags |= TagBit(TAG_SHADOW);   break;
            case SPELL_SCHOOL_ARCANE: tags |= TagBit(TAG_ARCANE);   break;
            default: break;
        }
        switch (spell->DmgClass)
        {
            case SPELL_DAMAGE_CLASS_MELEE:  tags |= TagBit(TAG_MELEE);      break;
            case SPELL_DAMAGE_CLASS_RANGED: tags |= TagBit(TAG_PROJECTILE); break;
            case SPELL_DAMAGE_CLASS_MAGIC:  tags |= TagBit(TAG_SPELL);      break;
            default: break;
        }
        if (spell->speed > 0.0f)
            tags |= TagBit(TAG_PROJECTILE);
        if (IsChanneledSpell(spell))
            tags |= TagBit(TAG_CHANNEL);
        if (spell->Mechanic == MECHANIC_STUN || spell->Mechanic == MECHANIC_ROOT || spell->Mechanic == MECHANIC_FEAR ||
                spell->Mechanic == MECHANIC_POLYMORPH || spell->Mechanic == MECHANIC_KNOCKOUT ||
                spell->Mechanic == MECHANIC_SLEEP)
            tags |= TagBit(TAG_CONTROL);

        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (spell->EffectRadiusIndex[i])
                tags |= TagBit(TAG_AREA);
            switch (spell->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    tags |= TagBit(TAG_HEAL);
                    break;
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    tags |= TagBit(TAG_GROUND) | TagBit(TAG_AREA) | TagBit(TAG_DURATION);
                    break;
                case SPELL_EFFECT_LEAP:
                case SPELL_EFFECT_CHARGE:
                    tags |= TagBit(TAG_MOVEMENT);
                    break;
                case SPELL_EFFECT_SUMMON:
                case SPELL_EFFECT_SUMMON_WILD:
                case SPELL_EFFECT_SUMMON_GUARDIAN:
                case SPELL_EFFECT_SUMMON_PET:
                    tags |= TagBit(TAG_MINION);
                    break;
                case SPELL_EFFECT_SUMMON_TOTEM:
                    tags |= TagBit(TAG_TOTEM);
                    break;
                default:
                    if (spell->Effect[i] >= SPELL_EFFECT_SUMMON_TOTEM_SLOT1 && spell->Effect[i] <= SPELL_EFFECT_SUMMON_TOTEM_SLOT1 + 3)
                        tags |= TagBit(TAG_TOTEM);
                    break;
            }
            switch (spell->EffectApplyAuraName[i])
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_HEAL:
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                    tags |= TagBit(TAG_DURATION);
                    if (spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                        tags |= TagBit(TAG_HEAL);
                    break;
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_ROOT:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_FEAR:
                    tags |= TagBit(TAG_CONTROL);
                    break;
                case SPELL_AURA_MOD_INCREASE_SPEED:
                    tags |= TagBit(TAG_MOVEMENT);
                    break;
                default:
                    break;
            }
        }

        // By name: the roles the data doesn't spell out. A Seal's procs and a paladin aura's
        // damage share the Seal's or the aura's own name.
        if (char const* name = spell->SpellName[0])
        {
            if (StartsWith(name, "Seal of "))
                tags |= TagBit(TAG_SEAL) | TagBit(TAG_MELEE);
            if (EndsWith(name, " Aura") && spell->SpellFamilyName == SPELLFAMILY_PALADIN)
                tags |= TagBit(TAG_AURA);
            if (StartsWith(name, "Retribution Aura") || StartsWith(name, "Sanctity Aura"))
                tags |= TagBit(TAG_AURA);
            if (StartsWith(name, "Curse of "))
                tags |= TagBit(TAG_CURSE);
            if (EndsWith(name, " Shout"))
                tags |= TagBit(TAG_SHOUT);
            if (std::strstr(name, " Totem"))
                tags |= TagBit(TAG_TOTEM);
        }
        return tags;
    }
}

namespace Arpg
{
    uint32 TagsOf(SpellEntry const* spellInfo)
    {
        if (!spellInfo)
            return TagBit(TAG_MELEE) | TagBit(TAG_PHYSICAL);
        static std::vector<uint32> const table = []()
        {
            std::vector<uint32> out(sSpellTemplate.GetMaxEntry(), 0);
            for (uint32 id = 1; id < sSpellTemplate.GetMaxEntry(); ++id)
                if (SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(id))
                    out[id] = Derive(spell);
            return out;
        }();
        return spellInfo->Id < table.size() ? table[spellInfo->Id] : Derive(spellInfo);
    }

    char const* TagName(Tag tag)
    {
        static char const* const names[MAX_TAG] =
        {
            "Physical", "Holy", "Fire", "Frost", "Arcane", "Nature", "Shadow",
            "Melee", "Spell", "Projectile", "Area", "Ground", "Channel", "Duration",
            "Heal", "Control", "Movement", "Minion", "Aura", "Seal", "Curse", "Shout", "Totem",
        };
        return tag < MAX_TAG ? names[tag] : "";
    }
}
