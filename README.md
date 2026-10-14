# ARPG play statistics

The ARPG server's play statistics (cmangos `src/game/Arpg/ArpgStats.h`), sent here by
`send-arpg-stats.bat` (which `update-arpg-server.bat` runs) from the server's run folder.

`stats/YYYY-MM-DD.jsonl`: one JSON object per line.
- `death`: the character, its level and zone, its killer, the blows of its last 10 seconds.
- `pack`: a pack fight's size, level, seconds from first aggro to the last death, damage it dealt.
- `session`: every 5 minutes and at logout, the session so far (levels, kills, deaths, damage by
  spell, damage taken by source, combat seconds, seconds low on mana or health, casts refused for
  want of power, flasks, rolls, minutes per zone). The last line of an id is the latest.
