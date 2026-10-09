/*
 * ARPG ground loot: see ArpgLoot.h.
 */

#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgCombat.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Groups/Group.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Chat/Chat.h"
#include "Entities/Item.h"
#include "Entities/ItemPrototype.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    // The corpse loot of `creature` if it is one `player` may take from now, else nullptr.
    Loot* CorpseLootFor(Player* player, Creature* creature)
    {
        Loot* loot = creature ? creature->m_loot : nullptr;
        if (!loot || loot->GetLootType() != LOOT_CORPSE || !loot->CanLoot(player))
            return nullptr;
        return loot;
    }

    // Whether `player` may pick `item` up now: it is theirs to take, not held by a roll, and not
    // a view-only or requirement-locked slot (the loot window's own test, HandleAutostoreLootItem).
    bool MayTake(Player* player, Loot* loot, LootItem* item)
    {
        if (!item || item->isBlocked || !item->IsAllowed(player, loot))
            return false;
        LootSlotType const type = item->GetSlotTypeForSharedLoot(player, loot);
        return type != LOOT_SLOT_VIEW && type != LOOT_SLOT_REQS && type != MAX_LOOT_SLOT_TYPE;
    }

    // Whether `proto` is a real item a monster might drop: not a quest item, key, NPC-only
    // "Monster -" weapon or placeholder, and for green and better something to wear or wield.
    bool DevLootable(ItemPrototype const* proto)
    {
        if (!proto || !proto->Name1 || proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_KEY ||
                proto->Class == ITEM_CLASS_MONEY || proto->Class == ITEM_CLASS_PERMANENT)
            return false;
        char const* name = proto->Name1;
        for (char const* junk : { "Monster", "Deprecated", "DEPRECATED", "TEST", "Test ", "[PH]", "OLD", "QA" })
            if (std::strncmp(name, junk, std::strlen(junk)) == 0)
                return false;
        if (proto->Quality >= ITEM_QUALITY_UNCOMMON)
            return proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR;
        return true;
    }

    // A random quality for a mixed drop, weighted toward the common end.
    uint32 MixedQuality()
    {
        uint32 const roll = urand(1, 100);
        if (roll <= 22) return ITEM_QUALITY_POOR;
        if (roll <= 40) return ITEM_QUALITY_NORMAL;
        if (roll <= 70) return ITEM_QUALITY_UNCOMMON;
        if (roll <= 88) return ITEM_QUALITY_RARE;
        if (roll <= 97) return ITEM_QUALITY_EPIC;
        return ITEM_QUALITY_LEGENDARY;
    }

    // A random droppable item of `quality` near item level `level`, widening the band until one is
    // found; 0 when the database has none of that quality.
    uint32 PickDevItem(uint32 quality, uint32 level)
    {
        for (uint32 band : { 3u, 8u, 20u, 100u })
        {
            std::vector<uint32> found;
            for (uint32 id = 0; id < sItemStorage.GetMaxEntry(); ++id)
            {
                ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(id);
                if (!proto || proto->Quality != quality || !DevLootable(proto))
                    continue;
                uint32 const ilvl = proto->ItemLevel;
                if (ilvl + band >= level && ilvl <= level + band)
                    found.push_back(id);
            }
            if (!found.empty())
                return found[urand(0, uint32(found.size() - 1))];
        }
        return 0;
    }
}

namespace Arpg
{
    uint32 PickRandomItem(uint32 quality, uint32 level)
    {
        return PickDevItem(quality, level);
    }

    void SendGroundLoot(Player* player, Creature* creature)
    {
        if (!Active(player) || !creature)
            return;

        uint32 gold = 0;
        LootItemList items;
        if (Loot* loot = CorpseLootFor(player, creature))
        {
            gold = loot->GetGoldAmount();
            LootItemList all;
            loot->GetLootItemsListFor(player, all);
            for (LootItem* item : all)
                if (MayTake(player, loot, item) && item->itemProto)
                    items.push_back(item);
        }
        if (items.size() > 255)
            items.resize(255);

        WorldPacket data(SMSG_ARPG_LOOT, 8 + 4 + 1 + items.size() * 11);
        data << creature->GetObjectGuid();
        data << uint32(gold);
        data << uint8(items.size());
        for (LootItem const* item : items)
        {
            data << uint8(item->lootSlot);
            data << uint32(item->itemId);
            data << uint32(item->itemProto->DisplayInfoID);
            data << uint8(item->itemProto->Quality);
            data << uint8(item->count);
        }
        player->GetSession()->SendPacket(data);
    }

    void OnCorpseLoot(Creature* creature)
    {
        if (!creature || !creature->m_loot)
            return;
        // The looters: the tapping group's members, or the tapper alone.
        if (Group* group = creature->GetGroupLootRecipient())
        {
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (Player* member = ObjectAccessor::FindPlayer(slot.guid))
                    if (member->IsInMap(creature))
                        SendGroundLoot(member, creature);
        }
        else if (Player* looter = creature->GetLootRecipient())
            SendGroundLoot(looter, creature);
    }

    void OnLootChanged(Loot* loot, Player* player)
    {
        if (!loot || !player || loot->GetLootType() != LOOT_CORPSE || !player->IsInWorld())
            return;
        ObjectGuid const guid = loot->GetLootGuid();
        if (!guid.IsCreature())
            return;
        Creature* creature = player->GetMap()->GetCreature(guid);
        if (!creature)
            return;
        // The taker, and the ARPG players of their group at the corpse: what was left for them
        // may have changed with it (a round robin turn passing, a shared item gone).
        SendGroundLoot(player, creature);
        if (Group* group = player->GetGroup())
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (slot.guid != player->GetObjectGuid())
                    if (Player* member = ObjectAccessor::FindPlayer(slot.guid))
                        if (member->IsInWorld() && member->GetMap() == player->GetMap())
                            SendGroundLoot(member, creature);
    }

    void PickLoot(Player* player, ObjectGuid corpseGuid, uint8 slot)
    {
        if (!Active(player) || !player->IsAlive() || !player->IsInWorld() || player->IsStunned())
            return;
        Creature* creature = corpseGuid.IsCreature() ? player->GetMap()->GetCreature(corpseGuid) : nullptr;
        if (!creature)
            return;

        // A loot window open on something else waits for its close: a release here, which an
        // emptied corpse sends, would close that window under the player.
        ObjectGuid const open = player->GetLootGuid();
        if (!open.IsEmpty() && open != corpseGuid)
            return;

        Loot* loot = CorpseLootFor(player, creature);
        float const range = GROUND_LOOT_RANGE + creature->GetCombatReach() + player->GetCombatReach();
        if (!loot || !player->IsWithinDistInMap(creature, range))
        {
            // Out of reach or nothing to take: the client's list is stale, so correct it.
            SendGroundLoot(player, creature);
            return;
        }

        if (slot == LOOT_SLOT_GOLD)
        {
            if (loot->GetGoldAmount() == 0)
            {
                SendGroundLoot(player, creature);
                return;
            }
            // Picking loot up breaks stealth, as opening a corpse does.
            player->DoLoot();
            // SendGold resends the list (OnLootChanged) and releases the corpse once nothing is
            // left on it for the player.
            loot->SendGold(player);
            return;
        }

        LootItem* item = loot->GetLootItemInSlot(slot);
        if (!MayTake(player, loot, item))
        {
            SendGroundLoot(player, creature);
            return;
        }
        player->DoLoot();
        // SendItem stores it (or reports full bags) and resends the list (OnLootChanged). Once
        // nothing is left for the player, the release settles the corpse as the loot window's
        // close does: what the player passed on goes to the rest of the group, and an empty corpse
        // loses its sparkle, opens for skinning and decays. Before that, the rest stays theirs.
        if (loot->SendItem(player, item) == EQUIP_ERR_OK && loot->IsLootedFor(player))
            loot->Release(player);
    }

    void QueryLoot(Player* player, ObjectGuid corpseGuid)
    {
        if (!Active(player) || !player->IsInWorld() || !corpseGuid.IsCreature())
            return;
        Creature* creature = player->GetMap()->GetCreature(corpseGuid);
        // Only a corpse the player can see: the client asks as one streams in.
        if (!creature || !player->HasAtClient(creature))
            return;
        SendGroundLoot(player, creature);
    }

    void DropDevLoot(Player* player, uint8 quality, uint8 count, uint8 level)
    {
        if (!player || !player->IsInWorld() || !Active(player))
            return;
        ChatHandler chat(player);
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_DEV_TOOLS))
        {
            chat.SendSysMessage("ARPG test loot is off on this server: set Arpg.DevTools = 1 in mangosd.conf.");
            return;
        }
        if (quality != DEV_LOOT_MIXED && quality > ITEM_QUALITY_LEGENDARY)
            return;
        count = std::min<uint8>(std::max<uint8>(count, 1), 16);
        uint32 const ilvl = level ? level : player->GetLevel();

        // A fresh creature a couple of yards ahead, killed at once, for the loot to lie on. Its
        // corpse goes after five minutes.
        float const o = player->GetOrientation();
        float const x = player->GetPositionX() + 2.5f * std::cos(o);
        float const y = player->GetPositionY() + 2.5f * std::sin(o);
        float z = player->GetPositionZ();
        player->UpdateAllowedPositionZ(x, y, z);
        uint32 const entry = sWorld.getConfig(CONFIG_UINT32_ARPG_DEV_CREATURE);
        Creature* corpse = player->SummonCreature(entry, x, y, z, o, TEMPSPAWN_CORPSE_TIMED_DESPAWN, 5 * MINUTE * IN_MILLISECONDS);
        if (!corpse)
        {
            chat.PSendSysMessage("ARPG test loot: could not spawn creature %u (Arpg.DevTools.Creature).", entry);
            return;
        }
        corpse->SetLootRecipient(player);
        Unit::Kill(player, corpse, DIRECT_DAMAGE, nullptr, false, false);
        // A creature with no loot of its own may have none made at all.
        if (!corpse->m_loot)
            corpse->m_loot = new Loot(player, corpse, LOOT_CORPSE);
        Loot* loot = corpse->m_loot;

        uint32 added = 0;
        for (uint8 i = 0; i < count; ++i)
        {
            uint32 const q = quality == DEV_LOOT_MIXED ? MixedQuality() : quality;
            if (uint32 const id = PickDevItem(q, ilvl))
            {
                loot->AddItem(id, 1, 0, Item::GenerateItemRandomPropertyId(id));
                ++added;
            }
        }
        loot->AddArpgDevGold(urand(ilvl * 10, ilvl * 60));
        // Lootable again, whatever the kill decided for an empty corpse.
        corpse->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
        OnCorpseLoot(corpse);
        chat.PSendSysMessage("ARPG test loot: %u items around item level %u.", added, ilvl);
    }
}
