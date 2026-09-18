"""Tier 0 combat reflexes: fight what is visible, disengage when losing.

Deterministic and local. No model is consulted per swing: a ~350 ms Jev round trip while
something is chewing on you is how characters die, which is exactly why the spec puts
contact fighting in the reflex tier. The model's role (Tier 1, later) is the judgement
call about whether a fight is worth taking - not steering each hit.

Policy:
  * target the nearest visible monster;
  * re-issue the attack order only when the engine has dropped it. Re-issuing every tick
    would reset the attack animation and the character would never land a blow, so the
    snapshot's `dest_action` is used as the "still swinging" signal (ACTION_ATTACKMON);
  * below `--hp-floor`, disengage: walk to the level's way up (a known `ascend` landmark)
    and take it, which leaves the level entirely and breaks contact.

Kill detection is deliberately weak-but-honest: a monster that was adjacent and healthy,
then vanished from the visible list, died - monsters do not fade out of sight from one
tile away. Experience is reported as the independent check.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agent_client as ac

ATTACK_MON = 20  # action_id::ACTION_ATTACKMON


def nearest(monsters):
    return min(monsters, key=lambda monster: monster["dist_tiles"])


def fight(seconds=120.0, hp_floor=0.35, verbose=True):
    snapshot = ac.read_snapshot()
    experience_start = snapshot["player"]["experience"]
    level_start = snapshot["player"]["character_level"]
    gold_start = snapshot["player"]["gold"]

    kills = 0
    adjacent_last = set()  # monster slots that were within two tiles last look
    idle_rounds = 0
    reason = "time budget"

    print("engaging: level %d, exp %d, hp %d/%d, floor %d%%" % (
        level_start, experience_start, snapshot["player"]["hp"],
        snapshot["player"]["max_hp"], hp_floor * 100), flush=True)

    started = time.time()
    while time.time() - started < seconds:
        snapshot = ac.read_snapshot()
        player = snapshot["player"]
        hp_ratio = player["hp"] / max(1, player["max_hp"])
        monsters = snapshot["visible_monsters"]

        adjacent_now = {m["slot"] for m in monsters if m["dist_tiles"] <= 2}
        for slot in adjacent_last - adjacent_now:
            kills += 1
            print("  KILL: slot %d gone from melee range (kills=%d, exp=%d, hp=%d)" % (
                slot, kills, player["experience"], player["hp"]), flush=True)
        adjacent_last = adjacent_now

        if hp_ratio < hp_floor:
            print("  disengaging at %d%% health" % (hp_ratio * 100), flush=True)
            return disengage(snapshot, kills, experience_start, level_start, gold_start)

        if not monsters:
            idle_rounds += 1
            if idle_rounds == 1:
                print("  nothing in sight; holding position", flush=True)
            # Long enough that a monster walking towards us would have arrived, short
            # enough that we do not stand around in a cleared room forever.
            if idle_rounds > 20:
                reason = "nothing left in sight"
                break
            time.sleep(0.3)
            continue
        idle_rounds = 0

        target = nearest(monsters)
        if player["dest_action"] != ATTACK_MON:
            ac.send("attack", slot=target["slot"])
            print("  attacking %s (slot %d, %d tiles, %s) hp=%d%% exp=%d" % (
                target["type"], target["slot"], target["dist_tiles"], target["health"],
                hp_ratio * 100, player["experience"]), flush=True)
        time.sleep(0.15)

    return report(ac.read_snapshot(), kills, experience_start, level_start, gold_start, reason)


def disengage(snapshot, kills, experience_start, level_start, gold_start):
    """Leave the level via the way up. Contact broken; the fight can be re-picked later."""
    ways_up = [l for l in snapshot["landmarks"] if l["kind"] == "ascend"]
    if not ways_up:
        print("  no known way up: retreating is impossible, stopping instead", flush=True)
        return report(snapshot, kills, experience_start, level_start, gold_start, "no escape route")

    tile = ways_up[0]["tile"]
    print("  falling back to the stairs at %s" % (tile,), flush=True)
    ac.walk_to(*tile)

    deadline = time.time() + 45
    while time.time() < deadline:
        time.sleep(0.5)
        try:
            snapshot = ac.read_snapshot()
        except ac.GameNotRunning:
            continue
        if snapshot["level"]["kind"] != "cathedral" or snapshot["level"]["dungeon_level"] == 0:
            break
        if snapshot["player"]["hp"] <= 0:
            break
        print("    falling back: tile=%s hp=%d/%d monsters=%d" % (
            tuple(snapshot["player"]["tile"]), snapshot["player"]["hp"],
            snapshot["player"]["max_hp"], len(snapshot["visible_monsters"])), flush=True)
    ac.stop()
    return report(ac.read_snapshot(), kills, experience_start, level_start, gold_start, "disengaged")


def report(snapshot, kills, experience_start, level_start, gold_start, reason):
    player = snapshot["player"]
    print("\nfight over (%s)" % reason)
    print("level: %s %s" % (snapshot["level"]["kind"], snapshot["level"]["dungeon_level"]))
    print("position: %s | hp %d/%d" % (tuple(player["tile"]), player["hp"], player["max_hp"]))
    print("kills (monsters lost from melee range): %d" % kills)
    print("experience: %d -> %d (+%d) | character level %d -> %d" % (
        experience_start, player["experience"], player["experience"] - experience_start,
        level_start, player["character_level"]))
    print("gold: %d -> %d" % (gold_start, player["gold"]))
    print("monsters still visible: %s" % (snapshot["visible_monsters"] or "none"))
    return snapshot


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=120.0)
    parser.add_argument("--hp-floor", type=float, default=0.35)
    args = parser.parse_args()
    fight(args.seconds, args.hp_floor)
