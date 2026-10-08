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
}

namespace Arpg
{
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
}
