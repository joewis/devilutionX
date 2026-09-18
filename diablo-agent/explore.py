"""Deterministic frontier exploration for the Diablo agent (Tier 0 navigation).

Walks toward the nearest boundary between what the character has seen and what it has
not, repeatedly, until the level is explored or a guard trips. No LLM and no Jev is
involved: exploration is mechanical, and spending a model call on "which way is unexplored"
would be both slower and worse.

Guards, in priority order:
  * a monster becomes visible -> stop, so perception can be inspected (and combat, which is
    not implemented yet, is not accidentally walked into)
  * health falls below `hp_floor` -> stop
  * time budget exhausted -> stop

Paths are computed over *seen* tiles only, so the character is never routed through
territory the agent has no knowledge of. The engine still chooses its own route to the
requested tile - the same thing happens when a player clicks a distant spot - but the
destination is always a tile the agent has actually seen.
"""

import argparse
import os
import sys
import time
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agent_client as ac

SEEN_FLOOR = ".o@"  # seen-and-walkable markers in the snapshot's map grid


def parse_grid(snapshot):
    grid = snapshot["map"]["grid"].split("\n")
    return [row for row in grid if row]


def passable_grid(snapshot):
    """The map grid with known doors marked passable.

    IsTileWalkable reports a closed door as solid. That is correct for standing on a tile
    and wrong for travelling through one: the engine walks the character into a closed door
    and opens it on the way, and so does a player. Without this correction the frontier
    search treats every doorway as a wall, declares a level finished, and leaves the rooms
    behind those doors unexplored - which is exactly what happened on the first run.
    """
    grid = [list(row) for row in parse_grid(snapshot)]
    for obj in snapshot["objects"]:
        if obj["kind"] != "door":
            continue
        x, y = obj["tile"]
        if 0 <= y < len(grid) and 0 <= x < len(grid[y]) and grid[y][x] in "x#":
            grid[y][x] = "."
    return ["".join(row) for row in grid]


def bfs(grid, start, is_goal):
    """Breadth-first search over seen floor tiles; returns (goal, path) or (None, [])."""
    width = len(grid[0])
    height = len(grid)
    queue = deque([(start, [start])])
    visited = {start}
    while queue:
        (x, y), path = queue.popleft()
        if is_goal((x, y)):
            return (x, y), path
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < width and 0 <= ny < height):
                continue
            if (nx, ny) in visited or grid[ny][nx] not in SEEN_FLOOR:
                continue
            visited.add((nx, ny))
            queue.append(((nx, ny), path + [(nx, ny)]))
    return None, []


def is_frontier(grid, tile):
    """A seen floor tile with an unseen neighbour: the edge of current knowledge."""
    x, y = tile
    width = len(grid[0])
    height = len(grid)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < width and 0 <= ny < height and grid[ny][nx] == " ":
            return True
    return False


def nearest_frontier(grid, start, avoid=()):
    """Nearest frontier by actual walking distance, not straight-line."""
    avoid = set(avoid)
    return bfs(grid, start, lambda tile: tile not in avoid and is_frontier(grid, tile))


def first_closed_door_on_path(snapshot, path):
    """The first still-shut door tile on a route, or None.

    The frontier search treats doors as passable because the engine opens a door the
    character walks into - but only when that door is the *destination*, never as a
    waypoint on the way somewhere else. A route crossing a shut door therefore has to be
    interrupted: walk to the door, open it, then replan. Skipping this step does not fail
    loudly; the character simply stands still forever while the agent keeps re-issuing an
    order the engine cannot satisfy.
    """
    doors = {(o["tile"][0], o["tile"][1]) for o in snapshot["objects"] if o["kind"] == "door"}
    raw = parse_grid(snapshot)
    for x, y in path:
        if (x, y) in doors and raw[y][x] in "x#":
            return (x, y)
    return None


def explore(seconds, hp_floor, stop_on_monster, quiet=False):
    started = time.time()
    snapshot = ac.read_snapshot()
    start_tile = tuple(snapshot["player"]["tile"])
    explored_start = snapshot["map"]["explored_tiles"]

    def log(message):
        if not quiet:
            print(message, flush=True)

    log("exploring %s from %s (explored=%d)" % (snapshot["level"], start_tile, explored_start))

    reason = "time budget exhausted"
    last_position = start_tile
    stuck_ticks = 0

    while time.time() - started < seconds:
        snapshot = ac.read_snapshot()
        player = tuple(snapshot["player"]["tile"])
        hp_ratio = snapshot["player"]["hp"] / max(1, snapshot["player"]["max_hp"])
        monsters = snapshot["visible_monsters"]

        if monsters and stop_on_monster:
            reason = "monster visible: %s" % monsters
            break
        if hp_ratio < hp_floor:
            reason = "health at %d%% (floor %d%%)" % (hp_ratio * 100, hp_floor * 100)
            break

        grid = passable_grid(snapshot)
        goal, path = nearest_frontier(grid, player)

        if goal is None:
            reason = "no frontier left - level fully explored"
            break

        # Re-issue the order periodically: the engine drops a stale path when it decides the
        # character is blocked (a door, a monster, a wall corner), and re-issuing resumes it.
        ac.walk_to(*goal)
        log("  -> heading to frontier %s (%d tiles of path), explored=%d hp=%d%%" % (
            goal, len(path), snapshot["map"]["explored_tiles"], hp_ratio * 100))

        # Wait for progress or for the character to stop making it.
        deadline = time.time() + 6.0
        while time.time() < deadline:
            time.sleep(0.5)
            try:
                now = ac.read_snapshot()
            except ac.GameNotRunning:
                continue
            if now["visible_monsters"] and stop_on_monster:
                reason = "monster visible: %s" % now["visible_monsters"]
                break
            tile = tuple(now["player"]["tile"])
            if max(abs(tile[0] - goal[0]), abs(tile[1] - goal[1])) <= 1:
                break
            if tile == last_position:
                stuck_ticks += 1
                if stuck_ticks >= 4:
                    break
            else:
                stuck_ticks = 0
                last_position = tile
        if reason.startswith("monster"):
            break

    final = ac.read_snapshot()
    print("\nstopped: %s" % reason)
    print("time: %.1fs | explored %d -> %d tiles (+%d)" % (
        time.time() - started, explored_start, final["map"]["explored_tiles"],
        final["map"]["explored_tiles"] - explored_start))
    print("player: %s hp=%d/%d | in sight: %d tiles" % (
        tuple(final["player"]["tile"]), final["player"]["hp"], final["player"]["max_hp"],
        final["map"]["visible_tiles"]))
    print("objects seen: %d | landmarks: %s" % (
        len(final["objects"]), [l["kind"] for l in final["landmarks"]]))
    print("monsters visible: %s" % (final["visible_monsters"] or "none"))
    return final


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--hp-floor", type=float, default=0.6)
    parser.add_argument("--stop-on-monster", action="store_true", default=True)
    args = parser.parse_args()
    explore(args.seconds, args.hp_floor, args.stop_on_monster)
