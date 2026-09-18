"""Tier 0 loot policy: take what is worth carrying, leave the rest on the floor.

Mechanical on purpose. Gold and potions are never wrong, so they need no judgement. Gear is
compared against what is worn by its own numbers - a weapon by damage, armour by its own
slot's AC - which is arithmetic, not a decision that benefits from a model. What a model
would add (is this worth a trip back to town, is this magic item worth identifying, should I
keep the gold for a suit of plate) is Tier 1/Tier 2 work and is not attempted here.

Two engine rules shape this file:

  * A picked-up item lands in the character's hand, not in the inventory, and the engine only
    starts a pickup when the cursor is a *free* hand. So the hand must be emptied after every
    item - equip it or stow it - or the next pickup silently never begins.
  * Gold is the exception: it auto-places into the purse and never occupies the hand.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agent_client as ac

MAX_BELT_POTIONS = 8

# INVITEM_BELT_FIRST: how the engine addresses belt slots to UseInvItem.
BELT_FIRST = 47

# Best first. A Warrior has no use for mana, so those are not drinking candidates.
HEALING_EFFECTS = ("full rejuvenation potion", "rejuvenation potion", "full healing potion", "healing potion")

POTION_EFFECTS = {
    "healing potion",
    "full healing potion",
    "mana potion",
    "full mana potion",
    "rejuvenation potion",
    "full rejuvenation potion",
}

# Which worn slot an item of a given kind competes with.
SLOT_FOR_KIND = {
    "head": "head",
    "body": "body",
    "one_hand": "hand",   # a shield is held, so it competes with the other hand
    "two_hand": "hand",
    "ring": "ring",
    "amulet": "amulet",
}


def worn(snapshot):
    return {entry["slot"]: entry for entry in snapshot["equipment"]}


def is_upgrade(snapshot, item):
    """True if the item beats whatever is worn in its own slot, by its own numbers."""
    kind = SLOT_FOR_KIND.get(item["equip_slot"])
    if kind is None:
        return False
    equipped = worn(snapshot)

    if kind == "hand":
        candidates = [equipped.get("hand_left"), equipped.get("hand_right")]
        current = [c for c in candidates if c and c["class"] == item["class"]]
    elif kind == "ring":
        current = [c for c in (equipped.get("ring_left"), equipped.get("ring_right")) if c]
    else:
        current = [c for c in (equipped.get(kind),) if c]

    if not current:
        return True  # nothing there at all

    best = current[0]
    if item["class"] == "weapon":
        return item["max_damage"] > best["max_damage"]
    if item["class"] == "armor":
        return item["ac"] > best["ac"]
    return False


def worth_taking(snapshot, item):
    """Should this floor item be picked up at all?

    Unknown magic is taken: a player cannot know whether it is worth carrying until they
    have it, and the item is free to look at once held.
    """
    if item["effect"] == "gold":
        return True
    if item["effect"] in POTION_EFFECTS:
        belt_potions = sum(1 for b in snapshot["belt"] if b["effect"] in POTION_EFFECTS)
        return belt_potions < MAX_BELT_POTIONS
    if item["class"] in ("weapon", "armor"):
        if item["magical"] > 0 and not item["identified"]:
            return True
        return is_upgrade(snapshot, item)
    return False


def take_one(item, timeout=25.0):
    """Walk to an item, pick it up, and get it out of the hand. Returns the resulting snapshot.

    The hand rule is why this always ends with equip-or-stow: leaving an item in hand makes
    every later pickup a silent no-op.
    """
    x, y = item["tile"]
    ac.walk_to(x, y)
    ac.wait_for_arrival(x, y, timeout=timeout)

    # Standing next to it is not enough - the engine picks up when the character is on or
    # beside the item, with a free hand.
    for _ in range(20):
        now = ac.read_snapshot()
        if not any(g["index"] == item["index"] for g in now["ground_items"]):
            break  # already gone: someone (something) got there first
        if now["hand_free"]:
            ac.send("pickup", x=x, y=y, slot=item["index"])
        time.sleep(0.4)

    now = ac.read_snapshot()
    if now["hand_free"]:
        return now

    held = now["holding"] or {}
    if held.get("class") in ("weapon", "armor") and is_upgrade(now, held):
        ac.send("equip")
    else:
        ac.send("stow")
    time.sleep(0.8)
    return ac.read_snapshot()


def clear_floor(snapshot, max_items=6, stop_if_monsters=True):
    """Take everything worth taking that is currently visible. Returns items taken.

    Stops the moment a monster shows up: looting while something is walking towards you is
    how characters die, and the reflex tier owns that situation.
    """
    taken = []
    for _ in range(max_items):
        snapshot = ac.read_snapshot()
        if stop_if_monsters and snapshot["visible_monsters"]:
            break
        candidates = [i for i in snapshot["ground_items"] if worth_taking(snapshot, i)]
        if not candidates:
            break
        item = min(candidates, key=lambda i: i["dist_tiles"])
        print("  looting %s (%s, %d tiles away)" % (
            item["name"], item["effect"] or item["class"], item["dist_tiles"]), flush=True)
        snapshot = take_one(item)
        taken.append(item["name"])
    return taken


def healing_potion(snapshot):
    """Belt slot of the best healing item carried, or None."""
    for effect in HEALING_EFFECTS:
        for entry in snapshot["belt"]:
            if entry["effect"] == effect:
                return entry["belt_slot"]
    return None


def drink(snapshot):
    """Drink the best healing item in the belt. Returns False if there is nothing to drink."""
    slot = healing_potion(snapshot)
    if slot is None:
        return False
    ac.send("use", slot=BELT_FIRST + slot)
    return True
