"""Tier 0 session policy: play the level until the way down is usable.

Deterministic, no model calls. The loop is:

  in town        -> go to the cathedral entrance and take it
  in the dungeon -> fight what is visible, explore when nothing is, fall back to the
                    stairs when hurt, and descend once the level is quiet and the
                    character is healthy

Every choice here is mechanical, and that is the point: walking a labyrinth needs no
model, and a monster in contact range needs no model to be hit. Tier 1 (Jev) and Tier 2
(the LLM) are reserved for decisions with real uncertainty - whether a fight is worth
taking, what to buy, when to abandon a level - which is why this file contains none of
them.

Guard rails, in the order they are checked:
  * health below `hp_floor` leaves the level rather than dying in it
  * a descent needs a quiet level (nothing visible) and `descend_hp_floor` health
  * `--seconds` bounds the run; the character is left standing, deliberately
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agent_client as ac
import combat
import explore

XP_PER_LEVEL = {1: 2000, 2: 4000, 3: 8000, 4: 16000, 5: 32000}  # warrior thresholds, shipped data


def log(message):
    print("[%6.1fs] %s" % (time.time() - START, message), flush=True)


def wait_for_level_change(expect, timeout=60):
    """Poll until the level kind/level matches `expect` (a (kind, level) pair)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.5)
        try:
            snapshot = ac.read_snapshot()
        except ac.GameNotRunning:
            continue
        if (snapshot["level"]["kind"], snapshot["level"]["dungeon_level"]) == expect:
            return snapshot
    return None


def landmark(snapshot, kind):
    found = [l for l in snapshot["landmarks"] if l["kind"] == kind]
    return found[0] if found else None


def play(seconds, hp_floor=0.4, descend_hp_floor=0.6, engagement_seconds=25):
    deadline = time.time() + seconds
    snapshot = ac.read_snapshot()
    start = (snapshot["level"]["kind"], snapshot["level"]["dungeon_level"])
    log("start: %s level %d | hp %d/%d | exp %d" % (
        start[0], start[1], snapshot["player"]["hp"], snapshot["player"]["max_hp"],
        snapshot["player"]["experience"]))

    while time.time() < deadline:
        snapshot = ac.read_snapshot()
        player = snapshot["player"]
        hp_ratio = player["hp"] / max(1, player["max_hp"])
        dungeon_level = snapshot["level"]["dungeon_level"]
        in_town = snapshot["level"]["kind"] == "town"

        # --- success: this is the run's goal -------------------------------------------
        if dungeon_level >= 2:
            log("SUCCESS: reached dungeon level 2 with %d/%d hp, exp %d" % (
                player["hp"], player["max_hp"], player["experience"]))
            return True

        # --- town: travel to the cathedral --------------------------------------------
        if in_town:
            if hp_ratio < hp_floor:
                log("hurt in town (%d%%) - no healing implemented, ending the run" % (hp_ratio * 100))
                return False
            entrance = landmark(snapshot, "descend")
            if entrance is None:
                log("no known way down from town - cannot proceed")
                return False
            log("heading for the cathedral at %s" % (entrance["tile"],))
            ac.walk_to(*entrance["tile"])
            if wait_for_level_change(("cathedral", 1), timeout=90) is None:
                log("did not reach the cathedral - retrying")
            continue

        # --- dungeon -------------------------------------------------------------------
        if hp_ratio < hp_floor:
            log("hp %d%% below floor - falling back to the stairs" % (hp_ratio * 100))
            stairs = landmark(snapshot, "ascend")
            if stairs is None:
                log("no known way up - standing down instead of dying")
                ac.stop()
                return False
            ac.walk_to(*stairs["tile"])
            if wait_for_level_change(("town", 0), timeout=90) is not None:
                log("back in town")
            continue

        if snapshot["visible_monsters"]:
            before = (player["experience"], player["hp"])
            result = combat.fight(seconds=engagement_seconds, hp_floor=hp_floor)
            after = (result["player"]["experience"], result["player"]["hp"])
            log("engagement done: exp %d->%d, hp %d->%d" % (before[0], after[0], before[1], after[1]))
            continue

        stairs_down = landmark(snapshot, "descend")
        if stairs_down is not None and hp_ratio >= descend_hp_floor:
            log("level is quiet and hp is %d%% - taking the way down at %s" % (
                hp_ratio * 100, stairs_down["tile"]))
            ac.walk_to(*stairs_down["tile"])
            changed = wait_for_level_change(("cathedral", dungeon_level + 1), timeout=90)
            if changed is not None:
                log("descended to level %d" % changed["level"]["dungeon_level"])
            else:
                log("descent did not trigger - continuing to explore")
            continue

        # --- nothing to fight, way down unknown or unwise: explore ----------------------
        grid = explore.parse_grid(snapshot)
        player_tile = tuple(player["tile"])
        goal, path = explore.nearest_frontier(grid, player_tile)
        if goal is None:
            log("no frontier left on level %d and no usable way down" % dungeon_level)
            return False
        log("exploring toward %s (%d tiles of path, hp %d%%, seen %d)" % (
            goal, len(path), hp_ratio * 100, snapshot["map"]["explored_tiles"]))
        ac.walk_to(*goal)

        # Give the order time to play out; abandon it early if a monster shows up so the
        # reflex tier can deal with the threat instead of the character walking past it.
        order_deadline = time.time() + 8
        while time.time() < order_deadline and time.time() < deadline:
            time.sleep(0.4)
            try:
                now = ac.read_snapshot()
            except ac.GameNotRunning:
                break
            if now["visible_monsters"]:
                break
            tile = tuple(now["player"]["tile"])
            if max(abs(tile[0] - goal[0]), abs(tile[1] - goal[1])) <= 1:
                break

    log("time budget exhausted")
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=600.0)
    parser.add_argument("--hp-floor", type=float, default=0.4)
    parser.add_argument("--descend-hp-floor", type=float, default=0.6)
    parser.add_argument("--engagement-seconds", type=float, default=25.0)
    args = parser.parse_args()

    START = time.time()
    try:
        play(args.seconds, args.hp_floor, args.descend_hp_floor, args.engagement_seconds)
    except ac.GameNotRunning as exc:
        log("stopped: %s" % exc)
