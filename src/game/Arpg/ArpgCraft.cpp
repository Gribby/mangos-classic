/*
 * ARPG crafting: see ArpgCraft.h.
 */

#include "Arpg/ArpgCraft.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgSkills.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "World/World.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace Arpg;

    struct OrbDef
    {
        uint8 id;
        char const* name;
        char const* text;
        uint32 gem;               // the vanilla gem whose icon it wears
        uint32 weight;            // how often it drops, against the rest
        uint32 quality;           // its name's colour on the ground
    };

    OrbDef const ORBS[] =
    {
        { ORB_AWAKENING,  "Orb of Awakening",    "Turns a white item green, with 1-2 affixes.",                 13926, 30, 2 },
        { ORB_REFINEMENT, "Prism of Refinement", "Turns a green item blue, adding affixes up to 3-4.",          12361, 20, 3 },
        { ORB_ASCENSION,  "Orb of Ascension",    "Turns a blue item purple, adding affixes up to 5-6.",         12799,  6, 4 },
        { ORB_FEL,        "Fel Orb",             "Rerolls every affix on an item, as many as it had.",          12364, 14, 3 },
        { ORB_INVOCATION, "Star of Invocation",  "Adds one affix to an item with room for it.",                 12363, 10, 3 },
        { ORB_VOID,       "Void Pearl",          "Removes one affix from an item, at random.",                   7971, 10, 3 },
        { ORB_LENS,       "Arcane Lens",         "Rerolls the value of every affix on an item.",                12800, 10, 3 },
    };
    // A gem missing from the world database: a plain pearl's icon.
    constexpr uint32 FALLBACK_DISPLAY = 7311;

    // Orb drops: a champion, a rare (or Warden), a dungeon boss, and anything else, in percent
    // (each dungeon tier adds TIER_ORB_PCT to the chances); a salvage's, by the item's rarity.
    constexpr uint32 CHAMPION_ORB_PCT = 20, RARE_ORB_PCT = 100, TRASH_ORB_PCT = 1, TIER_ORB_PCT = 5;
    constexpr uint32 SALVAGE_ORB_PCT[7] = { 0, 0, 20, 35, 50, 0, 60 };

    OrbDef const* OrbOf(uint8 id)
    {
        for (OrbDef const& orb : ORBS)
            if (orb.id == id)
                return &orb;
        return nullptr;
    }

    std::string Escaped(char const* text)
    {
        std::string out;
        for (char const* c = text; *c; ++c)
        {
            if (*c == '\'' || *c == '\\')
                out += '\\';
            out += *c;
        }
        return out;
    }

    uint32 DisplayOf(uint32 gem)
    {
        if (auto result = WorldDatabase.PQuery("SELECT displayid FROM item_template WHERE entry = %u", gem))
            if (uint32 const display = result->Fetch()[0].GetUInt32())
                return display;
        return FALLBACK_DISPLAY;
    }

    // The item at the client's bag and slot (see ArpgCraft.h), and its server position.
    Item* ItemAt(Player* player, uint8 bag, uint8 slot, uint8& outBag, uint8& outSlot)
    {
        if (!slot)
            return nullptr;
        if (bag == 255)
        {
            if (slot > EQUIPMENT_SLOT_END)
                return nullptr;
            outBag = INVENTORY_SLOT_BAG_0;
            outSlot = uint8(EQUIPMENT_SLOT_START + slot - 1);
        }
        else if (bag == 0)
        {
            if (slot > INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START)
                return nullptr;
            outBag = INVENTORY_SLOT_BAG_0;
            outSlot = uint8(INVENTORY_SLOT_ITEM_START + slot - 1);
        }
        else if (bag <= INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START)
        {
            outBag = uint8(INVENTORY_SLOT_BAG_START + bag - 1);
            outSlot = uint8(slot - 1);
        }
        else
            return nullptr;
        return player->GetItemByPos(outBag, outSlot);
    }

    void Say(Player* player, char const* colour, std::string const& text)
    {
        ChatHandler(player).PSendSysMessage("%s%s|r", colour, text.c_str());
    }

    void Refused(Player* player, std::string const& text)
    {
        Say(player, "|cffff4040", text);
    }

    // Gambling (Diablo IV's): gold for a random item of a slot the player names.
    struct GambleKind
    {
        char const* name;
        uint8 types[4];     // its inventory types (0: no more)
    };

    GambleKind const GAMBLE_KINDS[GAMBLE_KIND_COUNT] =
    {
        { "head",            { INVTYPE_HEAD } },
        { "shoulders",       { INVTYPE_SHOULDERS } },
        { "chest",           { INVTYPE_CHEST, INVTYPE_ROBE } },
        { "hands",           { INVTYPE_HANDS } },
        { "waist",           { INVTYPE_WAIST } },
        { "legs",            { INVTYPE_LEGS } },
        { "feet",            { INVTYPE_FEET } },
        { "wrists",          { INVTYPE_WRISTS } },
        { "back",            { INVTYPE_CLOAK } },
        { "neck",            { INVTYPE_NECK } },
        { "finger",          { INVTYPE_FINGER } },
        { "trinket",         { INVTYPE_TRINKET } },
        { "one-hand weapon", { INVTYPE_WEAPON, INVTYPE_WEAPONMAINHAND, INVTYPE_WEAPONOFFHAND } },
        { "two-hand weapon", { INVTYPE_2HWEAPON } },
        { "off hand",        { INVTYPE_SHIELD, INVTYPE_HOLDABLE } },
        { "ranged weapon",   { INVTYPE_RANGED, INVTYPE_THROWN, INVTYPE_RANGEDRIGHT, INVTYPE_RELIC } },
    };

    // A gamble's rarity: better odds than a drop's, as Diablo IV's are.
    uint8 GambleRarity()
    {
        uint32 const roll = urand(1, 100);
        if (roll <= GAMBLE_UNIQUE_PCT)
            return RARITY_UNIQUE;
        if (roll <= GAMBLE_UNIQUE_PCT + GAMBLE_PURPLE_PCT)
            return RARITY_PURPLE;
        if (roll <= GAMBLE_UNIQUE_PCT + GAMBLE_PURPLE_PCT + GAMBLE_BLUE_PCT)
            return RARITY_BLUE;
        return RARITY_GREEN;
    }

    // "1g 8s", "30s".
    std::string Money(uint32 copper)
    {
        std::string out;
        if (copper >= 10000)
            out = std::to_string(copper / 10000) + "g";
        if (copper % 10000 >= 100 || out.empty())
            out += (out.empty() ? "" : " ") + std::to_string(copper / 100 % 100) + "s";
        return out;
    }

    char const* RarityWord(uint8 rarity)
    {
        switch (rarity)
        {
            case RARITY_UNIQUE: return "|cffe6cc80a unique|r";
            case RARITY_PURPLE: return "|cffa335eea purple|r";
            case RARITY_BLUE: return "|cff0070dda blue|r";
            default: return "|cff1eff00a green|r";
        }
    }

    void Gamble(Player* player, uint16 kind)
    {
        if (!kind || kind > GAMBLE_KIND_COUNT)
            return;
        GambleKind const& k = GAMBLE_KINDS[kind - 1];
        if (player->IsInCombat())
        {
            Refused(player, "Not in combat.");
            return;
        }
        uint32 const cost = GambleCost(player->GetLevel());
        if (player->GetMoney() < cost)
        {
            Refused(player, "A gamble costs " + Money(cost) + "; you have too little gold.");
            return;
        }
        uint8 const rarity = GambleRarity();
        uint32 const id = GambleItem(player, rarity, player->GetLevel(), [&k](ItemPrototype const* proto)
        {
            for (uint8 type : k.types)
                if (type && proto->InventoryType == type)
                    return true;
            return false;
        });
        if (!id)
        {
            Refused(player, std::string("Nothing to gamble for that slot at your level: ") + k.name + ".");
            return;
        }
        player->ModifyMoney(-int32(cost));
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(id);
        Say(player, "|cffe6cc80", "You gamble " + Money(cost) + " for your " + k.name + ": " + RarityWord(rarity) + ", " +
                                  (proto && proto->Name1 ? proto->Name1 : "an item") + ".");
    }
}

namespace Arpg
{
    void EnsureCraftItems()
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        for (OrbDef const& orb : ORBS)
        {
            // class 12 would be quest items; 15 (miscellaneous), subclass 0, stacks of 20.
            std::string const text = std::string(orb.text) + " Kept in your purse; use it in the Forge.";
            WorldDatabase.DirectPExecute(
                "REPLACE INTO item_template (entry, class, subclass, name, displayid, Quality, stackable, description, ItemLevel, SellPrice) "
                "VALUES (%u, 15, 0, '%s', %u, %u, 20, '%s', 1, 0)",
                ORB_ITEM_BASE + orb.id, Escaped(orb.name).c_str(), DisplayOf(orb.gem), orb.quality, Escaped(text.c_str()).c_str());
        }
        sLog.outString("ARPG crafting: wrote %u orbs into item_template", uint32(std::size(ORBS)));
    }

    bool TakeCraftItem(Player* player, uint32 item, uint32 count)
    {
        if (item <= ORB_ITEM_BASE || item >= ORB_ITEM_BASE + MAX_ORB || !Active(player))
            return false;
        OrbDef const* orb = OrbOf(uint8(item - ORB_ITEM_BASE));
        if (!orb)
            return false;
        count = std::max<uint32>(count, 1);
        uint32 const held = AddHeld(player, uint16(PURSE_ORB + orb->id), int32(count));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "You take %s%s (%u in your purse).", count > 1 ? "" : "the ", orb->name, held);
        Say(player, "|cffe6cc80", buf);
        SendPurse(player);
        return true;
    }

    uint32 RandomOrbItem()
    {
        uint32 total = 0;
        for (OrbDef const& orb : ORBS)
            total += orb.weight;
        uint32 roll = urand(0, total - 1);
        for (OrbDef const& orb : ORBS)
        {
            if (roll < orb.weight)
                return ORB_ITEM_BASE + orb.id;
            roll -= orb.weight;
        }
        return ORB_ITEM_BASE + ORB_AWAKENING;
    }

    void AddOrbsToLoot(Loot* loot, uint32 count)
    {
        if (!loot)
            return;
        for (uint32 i = 0; i < count; ++i)
            loot->AddItem(RandomOrbItem(), 1, 0, 0);
    }

    void GiveOrbs(Player* player, uint32 count)
    {
        if (!player || !count)
            return;
        std::map<uint8, uint32> got;
        for (uint32 i = 0; i < count; ++i)
            ++got[uint8(RandomOrbItem() - ORB_ITEM_BASE)];
        std::string text;
        for (auto const& [id, n] : got)
        {
            AddHeld(player, uint16(PURSE_ORB + id), int32(n));
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s%s", text.empty() ? "" : ", ", n > 1 ? (std::to_string(n) + " x ").c_str() : "",
                          OrbOf(id)->name);
            text += buf;
        }
        Say(player, "|cffe6cc80", "To your purse: " + text + ".");
        SendPurse(player);
    }

    void OnCraftLoot(Creature* victim)
    {
        if (!victim || !victim->m_loot || !sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        Player* looter = victim->GetLootRecipient();
        if (!looter || !Active(looter))
            return;
        uint32 const tierPct = uint32(LootTier(victim->GetMap())) * TIER_ORB_PCT;
        uint8 const champion = ChampionTier(victim);
        uint32 count = 0;
        if (IsDungeonBoss(victim))
            count = urand(1, 2) + (roll_chance_i(int32(tierPct * 4)) ? 1 : 0);
        else if (champion >= 2)
            count = (roll_chance_i(int32(RARE_ORB_PCT)) ? 1 : 0) + (roll_chance_i(int32(25 + tierPct)) ? 1 : 0);
        else if (champion == 1)
            count = roll_chance_i(int32(CHAMPION_ORB_PCT + tierPct)) ? 1 : 0;
        else if (victim->GetLevel() + 5 >= looter->GetLevel())
            count = roll_chance_i(int32(TRASH_ORB_PCT + tierPct / 5)) ? 1 : 0;
        AddOrbsToLoot(victim->m_loot, count);
    }

    uint32 GambleCost(uint32 level)
    {
        return std::max<uint32>(GAMBLE_COST_MIN, GAMBLE_COST_PER_LEVEL_SQ * level * level);
    }

    void Craft(Player* player, uint8 op, uint8 bag, uint8 slot, uint16 affix)
    {
        if (!player || !Active(player))
            return;
        if (op == CRAFT_GAMBLE)
        {
            Gamble(player, affix);
            return;
        }
        uint8 posBag = 0, posSlot = 0;
        Item* item = ItemAt(player, bag, slot, posBag, posSlot);
        if (!item)
        {
            Refused(player, "Choose an item first.");
            return;
        }
        if (!Craftable(item))
        {
            Refused(player, CraftError(CRAFT_NOT_ITEM));
            return;
        }
        if (player->IsInCombat())
        {
            Refused(player, "Not in combat.");
            return;
        }
        if (item->IsInTrade())
        {
            Refused(player, "Not while it is in a trade.");
            return;
        }
        if (op == CRAFT_SALVAGE)
        {
            if (bag == 255)
            {
                Refused(player, "Take the item off to salvage it.");
                return;
            }
            uint8 const rarity = ShownRarity(item);
            std::string const name = item->GetProto()->Name1;
            std::vector<uint16> const shards = SalvageShards(item);
            player->DestroyItem(posBag, posSlot, true);
            std::string text = "You salvage " + name;
            for (size_t i = 0; i < shards.size(); ++i)
            {
                AddHeld(player, uint16(PURSE_SHARD + shards[i]), 1);
                text += (i ? ", " : ": ") + ShardName(shards[i]);
            }
            text += ".";
            Say(player, "|cffe6cc80", text);
            // An item with nothing on it gives nothing more (no orbs from vendor whites).
            if (!shards.empty() && roll_chance_i(int32(SALVAGE_ORB_PCT[std::min<uint8>(rarity, 6)])))
                GiveOrbs(player, 1);
            else
                SendPurse(player);
            return;
        }
        uint16 thing;
        std::string what;
        if (op == CRAFT_SHARD)
        {
            if (!affix || affix > AffixTotal())
                return;
            thing = uint16(PURSE_SHARD + affix);
            what = ShardName(affix);
        }
        else if (OrbDef const* orb = OrbOf(op))
        {
            thing = uint16(PURSE_ORB + orb->id);
            what = orb->name;
        }
        else
            return;
        if (!Held(player, thing))
        {
            Refused(player, "You hold no " + what + ".");
            return;
        }
        std::string done;
        CraftResult const result = CraftItem(player, item, op, affix, done);
        if (result != CRAFT_OK)
        {
            Refused(player, CraftError(result));
            return;
        }
        AddHeld(player, thing, -1);
        Say(player, "|cffe6cc80", what + ": " + done);
        SendPurse(player);
    }

    void SendPurse(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        WorldPacket data(SMSG_ARPG_PURSE, 512);
        data << uint8(std::size(ORBS));
        for (OrbDef const& orb : ORBS)
            data << uint8(orb.id) << uint16(std::min<uint32>(Held(player, uint16(PURSE_ORB + orb.id)), 65535))
                 << orb.name << orb.text;
        std::vector<std::pair<uint16, uint32>> shards;
        for (uint16 affix = 1; affix <= AffixTotal(); ++affix)
            if (uint32 const held = Held(player, uint16(PURSE_SHARD + affix)))
                shards.emplace_back(affix, held);
        data << uint16(shards.size());
        for (auto const& [affix, held] : shards)
            data << uint16(affix) << uint16(std::min<uint32>(held, 65535)) << ShardName(affix);
        player->GetSession()->SendPacket(data);
    }
}
