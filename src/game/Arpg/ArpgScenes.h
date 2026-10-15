/*
 * ARPG scenes: short cutscenes over the live world (the client's docs/ARPG-ACT-1.md, "Scenes").
 *
 * A scene is a camera track and subtitles, authored around an anchor (an NPC or creature, found by
 * name near the character), sent as SMSG_ARPG_SCENE and played by the client's cinematic player:
 * letterboxed, the UI and cursor hidden, ESC skips. The camera keys are in the anchor's frame
 * (forward toward the character, left, up, in yards), so a scene fits wherever the anchor stands.
 * While it plays the character is rooted and creatures leave it alone; the anchor turns to the
 * character and acts its cues (talk, point, roar). It ends when the client says the scene is over
 * (CMSG_COMPLETE_CINEMATIC, at its end or on a skip), or a few seconds after its length.
 *
 * A scene plays once per character (character_arpg_scene), on taking its act quest or on first
 * coming near its anchor while its act quest is under way (a leader's reveal).
 */

#ifndef MANGOS_ARPG_SCENES_H
#define MANGOS_ARPG_SCENES_H

#include "Common.h"

class Player;

namespace Arpg
{
    // A quest was taken: its scene, if any, plays on the character's next update.
    void OnSceneQuestAccepted(Player* player, uint32 quest);

    // Every update of an ARPG character: a scene starts, acts its cues, or ends.
    void UpdateScenes(Player* player);

    // Whether `player` is watching a scene (its actions are refused).
    bool InScene(Player const* player);

    // The client says the scene on screen is over (CMSG_COMPLETE_CINEMATIC).
    void OnSceneDone(Player* player);

    // A logged-out character's scene state goes.
    void ForgetScenes(Player* player);
}

#endif
