"""Agent-side client for the in-engine agent module.

The engine (Source/agent/agent.cpp) writes a player-perspective snapshot to
``agent-snapshot.json`` and consumes one-shot commands from ``agent-command.json``,
both in the game's pref directory. This module is the other end of that channel.

Design notes:
  * Commands are written via a temp file + atomic rename so the engine can never read a
    half-written command.
  * ``seq`` values are monotonic and must be strictly greater than the last one the engine
    applied, so the engine ignores replays. The counter is seeded from the snapshot's
    ``last_command_seq`` to survive a driver restart mid-session.
  * Nothing here reads game internals directly; every fact comes through the snapshot,
    which is filtered to what a player could perceive.
"""

import json
import os
import time

PREF_DIR = "/home/carl/.local/share/diasurgical/devilution"
SNAPSHOT_PATH = os.path.join(PREF_DIR, "agent-snapshot.json")
COMMAND_PATH = os.path.join(PREF_DIR, "agent-command.json")

_seq = 0


class GameNotRunning(RuntimeError):
    """No fresh snapshot: the game is at a menu, or the agent module is not loaded."""


def read_snapshot(max_age_seconds=2.0):
    """Return the latest snapshot, raising if it is missing or stale."""
    try:
        age = time.time() - os.path.getmtime(SNAPSHOT_PATH)
    except FileNotFoundError:
        raise GameNotRunning(f"no snapshot at {SNAPSHOT_PATH} (game at menu, or module not loaded)")
    if age > max_age_seconds:
        raise GameNotRunning(f"snapshot is {age:.1f}s old (game paused, closed, or not in a game)")
    with open(SNAPSHOT_PATH) as handle:
        return json.load(handle)


def send(cmd, **params):
    """Queue one command and return its sequence number."""
    global _seq
    if _seq == 0:
        # Survive a restart of this process: continue from whatever the engine has applied.
        try:
            _seq = read_snapshot()["last_command_seq"]
        except (GameNotRunning, KeyError):
            _seq = 0

    _seq += 1
    payload = {"seq": _seq, "cmd": cmd}
    payload.update(params)

    tmp_path = COMMAND_PATH + ".tmp"
    with open(tmp_path, "w") as handle:
        json.dump(payload, handle)
    os.replace(tmp_path, COMMAND_PATH)
    return _seq


def player_tile():
    return tuple(read_snapshot()["player"]["tile"])


def walk_to(x, y):
    return send("walk", x=x, y=y)


def stop():
    return send("stop")


def wait_for_arrival(x, y, timeout=20.0, epsilon=1):
    """Block until the player is within `epsilon` tiles of (x, y). Returns final tile."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        tile = player_tile()
        if max(abs(tile[0] - x), abs(tile[1] - y)) <= epsilon:
            return tile
        time.sleep(0.25)
    return player_tile()


def walkable_tiles(snapshot):
    """Tiles the player has seen and can stand on, per the snapshot's map grid."""
    grid = snapshot["map"]["grid"].split("\n")
    out = []
    for y, row in enumerate(grid):
        for x, char in enumerate(row):
            if char in ".o@":
                out.append((x, y))
    return out


def frontier_tiles(snapshot):
    """Seen floor tiles that touch an unseen tile - where exploration should head next."""
    grid = snapshot["map"]["grid"].split("\n")
    height = len(grid)
    result = []
    for y, row in enumerate(grid):
        for x, char in enumerate(row):
            if char not in ".o@":
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < len(row) and 0 <= ny < height and grid[ny][nx] == " ":
                    result.append((x, y))
                    break
    return result
