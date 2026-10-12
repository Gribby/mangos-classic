/*
 * ARPG class showcase: the five characters the benilla ARPG client's class screen stands, one per
 * class, each in its tier 2 set and an epic weapon (the client's docs/ARPG-CHARACTER.md, "Making
 * a character"). The client asks at the character screen, before any character is in the world
 * (CMSG_ARPG_SHOWCASE_QUERY, 0x34D, no body); the answer is SMSG_ARPG_SHOWCASE (0x34E):
 *
 *   uint8 count, then per character: uint8 class, race, sex, skin, face, hair style, hair colour,
 *   facial hair; then 19 equipment slots (EQUIPMENT_SLOT order), each uint32 ItemDisplayInfo id
 *   and uint8 InventoryType, as SMSG_CHAR_ENUM sends a roster entry's.
 *
 * The gear is named, not numbered: a set by its ItemSet.dbc name, a weapon by its item name, so
 * the database decides the entries and their displays. A name the database lacks is logged and
 * its slot left empty.
 */

#include "Common.h"
#include "Entities/ItemPrototype.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/SQLStorages.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "World/World.h"

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    struct ShowcaseDef
    {
        uint8 classId, race, sex, skin, face, hairStyle, hairColor, facialHair;
        char const* set;                  // ItemSet.dbc name
        std::array<char const*, 2> held;  // main hand, off hand (item names; nullptr none)
    };

    // The class screen's order, left to right.
    ShowcaseDef const SHOWCASE[] =
    {
        { CLASS_PALADIN, RACE_HUMAN,    GENDER_MALE,   2, 3, 5, 3, 2, "Judgement Armor",    { "Sulfuras, Hand of Ragnaros", nullptr } },
        { CLASS_MAGE,    RACE_UNDEAD,   GENDER_MALE,   1, 2, 4, 2, 1, "Netherwind Regalia", { "Atiesh, Greatstaff of the Guardian", nullptr } },
        { CLASS_DRUID,   RACE_NIGHTELF, GENDER_FEMALE, 3, 2, 3, 4, 1, "Stormrage Raiment",  { "Staff of the Shadow Flame", nullptr } },
        { CLASS_WARLOCK, RACE_ORC,      GENDER_MALE,   2, 1, 3, 1, 2, "Nemesis Raiment",    { "Dragonfang Blade", "Fire Runed Grimoire" } },
        { CLASS_SHAMAN,  RACE_TAUREN,   GENDER_MALE,   1, 2, 2, 1, 1, "The Ten Storms",     { "Aurastone Hammer", "Drillborer Disk" } },
    };

    struct ShowcaseSlot
    {
        uint32 display = 0;
        uint8 inventoryType = 0;
    };

    // The equipment slot an inventory type is worn in, or -1 for none shown.
    int SlotOf(uint32 inventoryType, bool held, int heldIndex)
    {
        switch (inventoryType)
        {
            case INVTYPE_HEAD: return EQUIPMENT_SLOT_HEAD;
            case INVTYPE_SHOULDERS: return EQUIPMENT_SLOT_SHOULDERS;
            case INVTYPE_BODY: return EQUIPMENT_SLOT_BODY;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE: return EQUIPMENT_SLOT_CHEST;
            case INVTYPE_WAIST: return EQUIPMENT_SLOT_WAIST;
            case INVTYPE_LEGS: return EQUIPMENT_SLOT_LEGS;
            case INVTYPE_FEET: return EQUIPMENT_SLOT_FEET;
            case INVTYPE_WRISTS: return EQUIPMENT_SLOT_WRISTS;
            case INVTYPE_HANDS: return EQUIPMENT_SLOT_HANDS;
            case INVTYPE_CLOAK: return EQUIPMENT_SLOT_BACK;
            case INVTYPE_TABARD: return EQUIPMENT_SLOT_TABARD;
            default: break;
        }
        if (!held)
            return -1;
        switch (inventoryType)
        {
            case INVTYPE_SHIELD:
            case INVTYPE_HOLDABLE:
            case INVTYPE_WEAPONOFFHAND: return EQUIPMENT_SLOT_OFFHAND;
            case INVTYPE_RANGED:
            case INVTYPE_THROWN:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_RELIC: return EQUIPMENT_SLOT_RANGED;
            default: return heldIndex == 0 ? EQUIPMENT_SLOT_MAINHAND : EQUIPMENT_SLOT_OFFHAND;
        }
    }

    uint32 SetByName(char const* name)
    {
        for (uint32 i = 0; i < sItemSetStore.GetNumRows(); ++i)
            if (ItemSetEntry const* set = sItemSetStore.LookupEntry(i))
                if (set->name[0] && std::strcmp(set->name[0], name) == 0)
                    return i;
        return 0;
    }

    ItemPrototype const* ItemByName(char const* name)
    {
        for (uint32 id = 0; id < sItemStorage.GetMaxEntry(); ++id)
            if (ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(id))
                if (proto->Name1 && std::strcmp(proto->Name1, name) == 0)
                    return proto;
        return nullptr;
    }

    void Wear(std::array<ShowcaseSlot, EQUIPMENT_SLOT_END>& slots, ItemPrototype const* proto, bool held, int heldIndex)
    {
        int const slot = SlotOf(proto->InventoryType, held, heldIndex);
        if (slot < 0 || slots[slot].display)
            return;
        slots[slot].display = proto->DisplayInfoID;
        slots[slot].inventoryType = uint8(proto->InventoryType);
    }

    // Each showcase character's gear, worked out once.
    std::vector<std::array<ShowcaseSlot, EQUIPMENT_SLOT_END>> const& Gear()
    {
        static std::mutex lock;
        static std::vector<std::array<ShowcaseSlot, EQUIPMENT_SLOT_END>> gear;
        std::lock_guard<std::mutex> guard(lock);
        if (!gear.empty())
            return gear;
        for (ShowcaseDef const& def : SHOWCASE)
        {
            std::array<ShowcaseSlot, EQUIPMENT_SLOT_END> slots{};
            if (uint32 const set = SetByName(def.set))
            {
                for (uint32 id = 0; id < sItemStorage.GetMaxEntry(); ++id)
                    if (ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(id))
                        if (proto->ItemSet == set)
                            Wear(slots, proto, false, 0);
            }
            else
                sLog.outError("ARPG showcase: no item set named \"%s\"", def.set);
            for (int i = 0; i < 2; ++i)
            {
                if (!def.held[i])
                    continue;
                if (ItemPrototype const* proto = ItemByName(def.held[i]))
                    Wear(slots, proto, true, i);
                else
                    sLog.outError("ARPG showcase: no item named \"%s\"", def.held[i]);
            }
            gear.push_back(slots);
        }
        return gear;
    }
}

void WorldSession::HandleArpgShowcaseQueryOpcode(WorldPacket& recvPacket)
{
    recvPacket.rpos(recvPacket.wpos());
    if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
        return;
    auto const& gear = Gear();
    WorldPacket data(SMSG_ARPG_SHOWCASE, 1 + std::size(SHOWCASE) * (8 + EQUIPMENT_SLOT_END * 5));
    data << uint8(std::size(SHOWCASE));
    for (size_t i = 0; i < std::size(SHOWCASE); ++i)
    {
        ShowcaseDef const& def = SHOWCASE[i];
        data << uint8(def.classId) << uint8(def.race) << uint8(def.sex) << uint8(def.skin) << uint8(def.face)
             << uint8(def.hairStyle) << uint8(def.hairColor) << uint8(def.facialHair);
        for (ShowcaseSlot const& slot : gear[i])
            data << uint32(slot.display) << uint8(slot.inventoryType);
    }
    SendPacket(data);
}
