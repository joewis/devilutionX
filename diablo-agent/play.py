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
import loot

XP_PER_LEVEL = {1: 2000, 2: 4000, 3: 8000, 4: 16000, 5: 32000}  # warrior thresholds, shipped data

# How long a monster the engine cannot path to is set aside before being retried. Long
# enough to explore and open the door it is probably behind, short enough to notice a
# change. Without this the loop re-engages the same unreachable monster forever.
UNREACHABLE_MEMORY_SECONDS = 60

# Health fraction at which a healing item gets drunk. Above the retreat floor, so the
# character tops up and stays in the fight rather than only drinking once it is losing.
POTION_HP_THRESHOLD = 0.55

# Walk to Pepin and talk when below this. The heal is free, so seeking it beats both
# spending a potion and giving up on the run.
HEAL_SEEK_THRESHOLD = 0.75

# Health below which the character will not start a fight, and will instead withdraw to
# town and be healed. Chosen from observation: one level-1 engagement took 67 hp to 22.
ENGAGE_HP_FLOOR = 0.6


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


def open_door(snapshot, tile, tried):
    """Walk to a door and open it.

    Tier 0's half of the door problem: a door is a door. Tier 1's question - whether opening
    *this* one is wise, e.g. when it would shut off an escape route - is deliberately not
    answered here.

    `tried` records doors already attempted so one that refuses to open cannot trap the loop.
    """
    if tile in tried:
        return False
    tried.add(tile)
    x, y = tile
    log("opening door at %s" % (tile,))
    ac.walk_to(x, y)  # the engine paths to a tile adjacent to an object, not onto it

    # Wait until the character is actually there: a walk order can be tens of tiles, and
    # operating from across the level does nothing at all.
    deadline = time.time() + 45
    while time.time() < deadline:
        time.sleep(0.5)
        try:
            now = ac.read_snapshot()
        except ac.GameNotRunning:
            break
        player_tile = tuple(now["player"]["tile"])
        if max(abs(player_tile[0] - x), abs(player_tile[1] - y)) <= 1:
            break

    ac.send("operate", x=x, y=y)
    time.sleep(1.5)
    return True


def try_open_any_known_door(snapshot, reachable, tried):
    """Fallback: open any seen door that is on our side of something."""
    for obj in snapshot["objects"]:
        if obj["kind"] != "door":
            continue
        tile = (obj["tile"][0], obj["tile"][1])
        if tile in tried:
            continue
        neighbours = [(tile[0] + dx, tile[1] + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))]
        if any(n in reachable for n in neighbours):
            return open_door(snapshot, tile, tried)
    return False


def play(seconds, hp_floor=0.4, descend_hp_floor=0.6, engagement_seconds=25, target_level=3):
    deadline = time.time() + seconds
    tried_doors = set()
    blocked_goals = set()
    unreachable_until = {}
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

        # A dead character has no actions to take; issuing them just spams the engine.
        if player["hp"] <= 0:
            log("the character is dead - stopping here")
            return False

        # Drink before retreating. The belt is player knowledge and drinking is mechanical,
        # so it belongs in the reflex tier: the character died at 58/70 with three potions
        # unopened, which is exactly what this prevents.
        if hp_ratio < POTION_HP_THRESHOLD and loot.drink(snapshot):
            log("hp %d%% - drinking a potion from the belt" % (hp_ratio * 100))
            time.sleep(0.6)
            continue


        # --- success: this is the run's goal -------------------------------------------
        if dungeon_level >= target_level:
            log("SUCCESS: reached dungeon level %d with %d/%d hp, exp %d" % (target_level, 
                player["hp"], player["max_hp"], player["experience"]))
            return True

        # --- town: travel to the cathedral --------------------------------------------
        if in_town:
            # Pepin heals for free, so a hurt character in town has something to *do*
            # rather than a reason to stop. Talking to him is the whole interaction.
            healer = next((t for t in snapshot.get("townsfolk", []) if t["name"] == "pepin"), None)
            if healer is not None and hp_ratio < HEAL_SEEK_THRESHOLD:
                before_hp = player["hp"]
                log("hp %d%% - going to Pepin at %s to be healed" % (hp_ratio * 100, healer["tile"]))
                ac.walk_to(*healer["tile"])
                ac.wait_for_arrival(healer["tile"][0], healer["tile"][1], timeout=30, epsilon=1)
                ac.send("talk", slot=healer["index"])
                time.sleep(2.0)
                ac.send("heal")  # selects "Talk to Pepin" on the screen that just opened
                time.sleep(2.0)
                log("healer result: hp %d -> %d" % (before_hp, ac.read_snapshot()["player"]["hp"]))
                continue

            if hp_ratio < hp_floor:
                log("hurt in town (%d%%), no healer known, nothing to drink - ending the run" % (hp_ratio * 100))
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
        # Never keep an item in hand. The engine only begins a pickup when the cursor is a
        # free hand, and it fails silently otherwise, so a character left holding something
        # stops looting forever without a single error.
        if not snapshot["hand_free"]:
            held = snapshot.get("holding") or {}
            if held.get("class") in ("weapon", "armor") and loot.is_upgrade(snapshot, held):
                log("holding %s - equipping it" % held.get("name"))
                ac.send("equip")
            else:
                log("holding %s - stowing it" % held.get("name"))
                ac.send("stow")
            time.sleep(0.8)
            continue

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

        # Set aside monsters the engine could not path to recently: they are usually behind
        # a door nobody has opened yet, so exploring is what makes them reachable again.
        now = time.time()
        unreachable_until = {s: t for s, t in unreachable_until.items() if t > now}
        ignore = set(unreachable_until)
        engageable = [m for m in snapshot["visible_monsters"] if m["slot"] not in ignore]

        if engageable and hp_ratio >= ENGAGE_HP_FLOOR:
            before = (player["experience"], player["hp"])
            result, stuck = combat.fight(
                seconds=engagement_seconds, hp_floor=hp_floor, ignore_slots=ignore)
            because = result["_unreachable_reason"]
            for slot in stuck:
                unreachable_until[slot] = time.time() + UNREACHABLE_MEMORY_SECONDS
            after = (result["player"]["experience"], result["player"]["hp"])
            log("engagement done: exp %d->%d, hp %d->%d%s" % (
                before[0], after[0], before[1], after[1],
                " (%s)" % because if because else ""))
            # Corpses are where loot comes from, and the drop lands in sight.
            taken = loot.clear_floor(result)
            if taken:
                log("looted: %s" % ", ".join(taken))
            continue

        if engageable and hp_ratio < ENGAGE_HP_FLOOR:
            log("hp %d%% - too hurt to start a fight; backing off to town to heal" % (hp_ratio * 100))
            stairs = landmark(snapshot, "ascend")
            if stairs is not None:
                ac.walk_to(*stairs["tile"])
                wait_for_level_change(("town", 0), timeout=90)
            continue

        # Everything in sight is unreachable right now. The usual cause is a shut door
        # between here and there, so this is a route problem, not a reason to write the
        # target off: plan a way through the doors we know about and open any shut one on
        # that route. Only when no route through known floor exists at all does the answer
        # become "explore more", because that is what reveals the way round.
        if snapshot["visible_monsters"]:
            target = min(snapshot["visible_monsters"], key=lambda m: m["dist_tiles"])
            tile = tuple(target["tile"])
            player_tile = tuple(player["tile"])
            _, path = explore.bfs(explore.passable_grid(snapshot), player_tile, lambda t: t == tile)
            if path:
                shut_door = explore.first_closed_door_on_path(snapshot, path)
                if shut_door is not None:
                    log("the way to %s (slot %d, %d tiles) runs through the door at %s" % (
                        target["type"], target["slot"], target["dist_tiles"], shut_door))
                    open_door(snapshot, shut_door, tried_doors)
                    continue
                log("working round to %s (slot %d): %d tiles of open route" % (
                    target["type"], target["slot"], len(path)))
                ac.walk_to(*tile)
                time.sleep(0.6)
                continue
            log("%s (slot %d) is in sight with no route through known floor - exploring to find one" % (
                target["type"], target["slot"]))
            unreachable_until[target["slot"]] = time.time() + UNREACHABLE_MEMORY_SECONDS

        # Anything already visible on the floor is worth more than unexplored floor.
        taken = loot.clear_floor(snapshot, stop_if_monsters=False, max_items=4)
        if taken:
            log("looted: %s" % ", ".join(taken))
            snapshot = ac.read_snapshot()
            player = snapshot["player"]

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
        grid = explore.passable_grid(snapshot)
        player_tile = tuple(player["tile"])
        goal, path = explore.nearest_frontier(grid, player_tile, avoid=blocked_goals)
        if goal is None:
            # Nothing to aim at. Before declaring the level finished, clear a door: the
            # unseen space behind it is invisible, not absent.
            if try_open_any_known_door(snapshot, explore.reachable_from(snapshot, player_tile), tried_doors):
                continue
            log("level %d exhausted: no frontier and no door left to open" % dungeon_level)
            return False

        # A shut door on the route cannot be walked through: the engine only opens a door
        # that is the destination itself. Interrupt, open it, replan.
        shut_door = explore.first_closed_door_on_path(snapshot, path)
        if shut_door is not None:
            open_door(snapshot, shut_door, tried_doors)
            continue

        log("exploring toward %s (%d tiles of path, hp %d%%, seen %d)" % (
            goal, len(path), hp_ratio * 100, snapshot["map"]["explored_tiles"]))
        ac.walk_to(*goal)

        # Give the order time to play out; abandon it early if a monster shows up so the
        # reflex tier can deal with the threat instead of the character walking past it.
        order_deadline = time.time() + 8
        stuck_since = None
        last_tile = player_tile
        while time.time() < order_deadline and time.time() < deadline:
            time.sleep(0.4)
            try:
                now = ac.read_snapshot()
            except ac.GameNotRunning:
                break
            if now["visible_monsters"]:
                break
            tile = tuple(now["player"]["tile"])

            # The goal is a tile we intend to stand on, so arrival must be exact. Treating
            # "one tile away" as arrived hands back a goal beside the character, which is
            # instantly satisfied, and the outer loop re-plans the same tile forever without
            # ever taking a step (observed: the map frozen at 190 tiles for minutes).
            if tile == goal:
                break

            # No movement since the previous look means the engine cannot satisfy this order
            # - the unseen tile turned out to be rock - so drop this goal and try another.
            if tile == last_tile:
                if stuck_since is None:
                    stuck_since = time.time()
                elif time.time() - stuck_since > 4:
                    log("goal %s unreachable (no movement for 4s) - trying another frontier" % (goal,))
                    blocked_goals.add(goal)
                    break
            else:
                last_tile = tile
                stuck_since = None

    log("time budget exhausted")
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-level", type=int, default=3,
                    help="descend until this dungeon level is reached")
    parser.add_argument("--seconds", type=float, default=600.0)
    parser.add_argument("--hp-floor", type=float, default=0.4)
    parser.add_argument("--descend-hp-floor", type=float, default=0.6)
    parser.add_argument("--engagement-seconds", type=float, default=25.0)
    args = parser.parse_args()

    START = time.time()
    try:
        play(args.seconds, args.hp_floor, args.descend_hp_floor, args.engagement_seconds, args.target_level)
    except ac.GameNotRunning as exc:
        log("stopped: %s" % exc)
