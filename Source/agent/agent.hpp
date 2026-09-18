#pragma once

namespace devilution::agent {

/**
 * @brief Per-tick agent hook: the perception side of the spec (Tier 0 plumbing).
 *
 * Writes the player-perspective snapshot defined in `diablo-agent/SPEC.md` section 3a
 * to `<pref path>/agent-snapshot.json`, throttled (see SnapshotIntervalMs).
 *
 * Called at the end of GameLogic() because that is the first point in the frame where
 * the per-tile visibility flags have been refreshed (ProcessLightList/ProcessVisionList
 * run just before) - reading them any earlier would export last frame's sight.
 *
 * Contract: only what the player could perceive leaves this module. Entities are
 * filtered by DungeonFlag::Visible, i.e. tiles inside the light radius with an
 * unobstructed line of sight, and the map layer is the engine's own explored-tile
 * memory. Dumping the raw monster/object tables would be the cheat that made the
 * previous attempt worthless.
 */
void Tick();

} // namespace devilution::agent
