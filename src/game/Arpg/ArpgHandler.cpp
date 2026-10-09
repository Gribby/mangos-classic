/*
 * CMSG_ARPG_ACTION, the benilla ARPG client's one packet: the hello, the held swing and the cast
 * at an aim point. See ArpgCombat.h.
 *
 * The client says hello when it enters the world, which can be before the server has finished
 * loading the character; the session then drops it ("the player has not logged in yet"). A swing
 * or a cast, which only the ARPG client sends, therefore counts as the hello too.
 *
 *   uint8 kind, then by kind:
 *     ACTION_HELLO        uint8 version; answered with the uniques' tooltip lines
 *     ACTION_SWING_START  uint64 intended (the unit under the cursor, 0 for none)
 *     ACTION_SWING_STOP   -
 *     ACTION_CAST         uint32 spell id, uint8 aim (CastAim), float x, float y, float z,
 *                         uint64 intended
 *     ACTION_AIM          float x, float y, float z, uint64 intended (re-aims the running cast)
 *     ACTION_LOOT         uint64 corpse, uint8 loot slot (0xFF the gold): pick it up off the ground
 *     ACTION_LOOT_QUERY   uint64 corpse: send its ground loot list
 *     ACTION_DEV_LOOT     uint8 quality (0xFF mixed), uint8 count, uint8 level (0 the player's):
 *                         with Arpg.DevTools on, drop test loot at the player's feet
 *
 * The loot kinds run on the world thread, as the stock loot opcodes do (they are thread-unsafe:
 * a split of gold reaches group members on other maps).
 */

#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgUniques.h"

#include "Server/WorldSession.h"
#include "Server/WorldPacket.h"
#include "Entities/Player.h"
#include "World/World.h"
#include "Log/Log.h"
#include "Spells/SpellMgr.h"
#include "Spells/Spell.h"

#include <cmath>

void WorldSession::HandleArpgActionOpcode(WorldPacket& recvPacket)
{
    uint8 kind;
    recvPacket >> kind;

    Player* player = GetPlayer();
    if (!player || !sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
    {
        recvPacket.rpos(recvPacket.wpos());
        return;
    }

    switch (kind)
    {
        case Arpg::ACTION_HELLO:
        {
            uint8 version;
            recvPacket >> version;
            if (version != Arpg::ARPG_PROTOCOL_VERSION)
            {
                sLog.outError("ARPG: %s sent protocol version %u, this server speaks %u",
                              player->GetName(), uint32(version), uint32(Arpg::ARPG_PROTOCOL_VERSION));
                return;
            }
            Arpg::OnHello(player);
            // The uniques' tooltip lines, with every hello: the client may have missed a first.
            Arpg::SendUniques(player);
            break;
        }
        case Arpg::ACTION_SWING_START:
        {
            ObjectGuid intended;
            recvPacket >> intended;
            // Only the ARPG client sends this, so it stands in for a hello the server missed.
            Arpg::OnHello(player);
            player->SetArpgSwingTarget(intended);
            player->SetArpgSwinging(true);
            // A swing ends a running wand or Auto Shot, as the stock attack does.
            player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            break;
        }
        case Arpg::ACTION_SWING_STOP:
            player->SetArpgSwinging(false);
            player->SetArpgSwingTarget(ObjectGuid());
            break;
        case Arpg::ACTION_CAST:
        {
            uint32 spellId;
            uint8 aim;
            float x, y, z;
            ObjectGuid intended;
            recvPacket >> spellId >> aim >> x >> y >> z >> intended;

            SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
            if (!spellInfo)
                return;
            // Only the ARPG client sends this, so it stands in for a hello the server missed.
            Arpg::OnHello(player);
            // Every refusal answers as a cast failure, so the client's pending cast and GCD clear.
            // Only the player's own body casts this way, not a possessed unit.
            if (player->GetMover() != player ||
                    !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                Spell::SendCastResult(player, spellInfo, SPELL_FAILED_ERROR);
                return;
            }
            if (!player->HasActiveSpell(spellId) || IsPassiveSpell(spellInfo))
            {
                sLog.outError("ARPG: %s casts spell %u, which they do not have", player->GetName(), spellId);
                Spell::SendCastResult(player, spellInfo, SPELL_FAILED_NOT_KNOWN);
                return;
            }

            Arpg::CastAt(player, spellInfo, aim == Arpg::AIM_ALLY ? Arpg::AIM_ALLY : Arpg::AIM_ENEMY, x, y, z, intended);
            break;
        }
        case Arpg::ACTION_AIM:
        {
            float x, y, z;
            ObjectGuid intended;
            recvPacket >> x >> y >> z >> intended;
            Arpg::OnHello(player);
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
                Arpg::UpdateAim(player, x, y, z, intended);
            break;
        }
        case Arpg::ACTION_LOOT:
        {
            ObjectGuid corpse;
            uint8 slot;
            recvPacket >> corpse >> slot;
            Arpg::OnHello(player);
            GetMessager().AddMessage([corpse, slot](WorldSession* session)
            {
                if (Player* looter = session->GetPlayer())
                    if (looter->IsInWorld())
                        Arpg::PickLoot(looter, corpse, slot);
            });
            break;
        }
        case Arpg::ACTION_LOOT_QUERY:
        {
            ObjectGuid corpse;
            recvPacket >> corpse;
            Arpg::OnHello(player);
            GetMessager().AddMessage([corpse](WorldSession* session)
            {
                if (Player* looter = session->GetPlayer())
                    if (looter->IsInWorld())
                        Arpg::QueryLoot(looter, corpse);
            });
            break;
        }
        case Arpg::ACTION_DEV_LOOT:
        {
            uint8 quality, count, level;
            recvPacket >> quality >> count >> level;
            Arpg::OnHello(player);
            GetMessager().AddMessage([quality, count, level](WorldSession* session)
            {
                if (Player* looter = session->GetPlayer())
                    if (looter->IsInWorld())
                        Arpg::DropDevLoot(looter, quality, count, level);
            });
            break;
        }
        default:
            recvPacket.rpos(recvPacket.wpos());
            break;
    }
}
