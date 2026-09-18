# Diablo agent — specification v0.2

**Repo**: `github.com/joewis/devilutionX` (our `origin`; pushes land in your GitHub)
**Upstream**: `github.com/diasurgical/DevilutionX` (our `upstream`, for rebasing)
**Local**: `/home/carl/src/devilutionX` · branch **`agent`**, based on upstream `0af00422`
**Spec**: `/home/carl/diablo-agent/SPEC.md`

Goal: an agent plays Diablo (DevilutionX) from a **player's perspective** — it may not
know what a human could not perceive — with three decision tiers: deterministic local
behaviour, TypeSafe/Jev judgments, and an LLM supervisor that is consulted rarely.

> **v0.2 changes**: repo is now the user's fork; navigation/exploration is a
> deterministic local algorithm, not an LLM concern; the explored-map memory is the
> game's own — no parallel store is maintained.

---

## 0. Running the game (verified 2026-09-18)

```bash
cd /home/carl/.local/share/diasurgical/devilution
DISPLAY=:0 /usr/bin/devilutionx        # no dummy audio driver needed
```

- Installed binary: `games-engines/devilutionx-1.5.5` (`/usr/bin/devilutionx`), windowed,
  main menu confirmed rendering on `:0` (Single Player / Multi Player / Settings …).
- **Verified 2026-09-18: audio works, no `SDL_AUDIODRIVER=dummy`.** The game runs with a
  live sink-input (`s16le 2ch 22050Hz`) on carl's daemon. Two independent faults had to
  be fixed:
  1. **`media-libs/libsdl2` had no audio backend** — it was built with only `dummy`
     (no `alsa`/`pulseaudio` USE; nothing had ever needed SDL audio, since the earlier
     POC ran headless with the dummy driver). Fixed by `/etc/portage/package.use/media-libs-libsdl2`
     (`pulseaudio alsa`) + rebuild. Without this the game exits instantly with
     `SDL Error: No available audio device`.
  2. **The card was owned by root.** The 2026-08 Bluetooth-audio experiment ran
     `pulseaudio-bt.service` as root (because Xorg `:0` is root's), with its socket
     unreachable from `carl`. That experiment is **retired**; `carl` now runs his own
     per-user PulseAudio (`pulseaudio.socket`/`pulseaudio.service` user units).
- `~/.config/pulse/client.conf` names carl's socket explicitly
  (`default-server = unix:/run/user/1001/pulse/native`) because agent/tool shells do not
  carry `XDG_RUNTIME_DIR`; the daemon's default sink is the real card
  (`alsa_output.pci-0000_00_1f.3.analog-stereo`). Card 0 (USB C-Media) is capture-only,
  so ALSA's `default` has no playback device — that is not a fault.
- Data dir: `DIABDAT.MPQ` + Hellfire MPQs + `diablo.ini` already in place here.
- Test characters exist (single-player and multiplayer). To enter a game from the main
  menu, pressing Enter three times with pauses between starts the Carl single-player game.
- **BT leftovers stay installed by request** (2026-09-18): `bluez`, the BT USE flags
  (`pulseaudio bluetooth`, `libpulse dbus`, `alsa-plugins pulseaudio`) and
  `/etc/portage/package.use/pulseaudio-bt` are inert but intentional — the user may
  revisit Bluetooth audio later. Do not purge them.
- Operator access: x11vnc on `:0`, port 5900.
- **This is the installed release build, not our fork.** Agent work requires building
  the fork, which needs SDL2_mixer and SDL2_ttf dev packages (currently missing per
  `pkg-config`, though the runtime libs resolve).

---

## 1. Decision tiers

Three rules decide where a decision lives:

1. Derivable from state by a fixed rule → **local**
2. Needs relative-strength / risk weighing under uncertainty → **Jev**
3. Needs goal-setting, world knowledge, dialogue, or a plan spanning the whole run → **LLM**
4. **Any deadline under ~350 ms is local by construction.** Upper tiers only
   *parameterize* local rules; they are never in the reaction path.

Rule 4 is load-bearing: upper tiers set policy and parameters, local executes.

### Tier 0 — Local (deterministic, in-engine, per-frame / per-navigation-node)

No network. No judgment. Runs continuously.

- **Movement execution**: follow the current path, walk to the target tile (engine
  pathfinding does the actual stepping — we issue a destination, not per-tile input).
- **Navigation & exploration** (§4) — the labyrinth is explored by algorithm.
  *"Where to click next to get to the next position" is local, not an LLM call.*
- **Combat execution**: swing when in range, face the target, close the last tiles.
- **Emergency potion** at a health threshold (a 350 ms round trip at 4 HP is fatal).
- **Panic disengage** when health is critical and no potion remains.
- **Portal-out reflex** when the portal rule fires (§6).
- Pick up items under foot, open doors that lie on the route, step off hazards, unstick.

### Tier 1 — TypeSafe / Jev (~3 Hz, ~351 ms measured)

Judgment under uncertainty over the **perceived** state. This is where the user's
example lands: *another group of monsters approaches mid-fight — continue attacking
or retreat, based on the player's and the monster's strength.*

- Primary `choice`: `continue_fight` | `retreat` | `kite` | `portal_out`.
- **Door decisions** — opening a door is a commitment you cannot see past: it may
  hide a pack, a shortcut, or a dead end. Jev decides *whether/when*; local opens it.
- Item upgrade comparison, weapon/spell choice for the current opponent.
- Shrine risk, potion economy, and the descend-viability cross-check.

Answers set the committed intent; a `confidence` floor falls back to the Tier 0
default and enqueues the situation for the LLM.

### Tier 2 — LLM supervisor (event-driven; seconds to minutes)

Consulted rarely, never in the combat loop, and **never for navigation**. Scope:

- Goal-setting ("go to the cathedral, play the first level").
- **Cadaver recovery** planning (re-kit, when to attempt recovery vs push on).
- Town economy (buy/sell/heal), stat-point allocation on level-up.
- Chat and other players; the "follow the other player" scenario.
- Novel events, quest decisions, and re-planning when an escalation says the
  current approach failed.

Explicitly **not** the LLM's job: exploring the level, choosing the next tile,
route planning, routine combat, or anything a fixed rule or a calibrated judgment
already covers.

---

## 2. Perception model — what the agent may know

| Channel | Grants | Denies |
|---|---|---|
| Sight | Entities on tiles that are **visible** and **lit** (engine's own predicate) | Anything outside the light radius, behind walls, or unlit |
| Hearing | Event **bearing** + loudness band | Exact position, distance, identity beyond what the sound implies |
| Memory | The game's own **explored** tiles, persisted in the save | Anything never explored |
| Self | Own stats, inventory, gear, gold, belt, message log, current action | — |

**The memory channel is the game's implementation, not ours.** DevilutionX already
tracks discovered tiles (`DungeonFlag::Explored`, the `AutomapView` grid, persisted
in `SavedFlags`). We read that. We do **not** maintain a second map, a duplicate
visited-set, or any parallel memory of where we have been.

Engine sources (verified on the clone):

- `DungeonFlag::Visible` — `IsTileVisible()`; set/cleared per frame by
  `DoVision(position, radius, MapExplorationType, bool)` (`Source/lighting.cpp`;
  generic ray-caster in `Source/vision.cpp`).
- `DungeonFlag::Lit` — `IsTileLit()`, the same predicate the monster draw path uses
  (`Source/engine/render/scrollrt.cpp`). **Reuse it; do not reimplement sight.**
- `DungeonFlag::Explored` — the discovery memory (in `SavedFlags`).
- `DungeonFlag::DeadPlayer` — the engine's own marker for where a player died
  (`Source/player.cpp:1051`, `Source/multi.cpp:975`). Cadaver location, free.
- Positional audio — `Source/utils/soundsample.cpp` stereo panning
  (`StereoSeparation = 6000.F`) plus volume attenuation gives bearing + loudness.

### Knowledge boundary

- **Allowed**: static game data — monster stats, item affixes, shrine effects, spell
  tables, level-generation rules, quest structure (`Source/tables/*`, `txtdata/`).
  This is what a veteran human player has.
- **Forbidden**: live world state for the current level beyond perception — the
  generated map, the spawn table, monster positions, the stairs' location before
  discovery.

---

## 3. State structure

Two consumers, two shapes. The raw navigational map is **never** exported.

### 3a. The Tier 1 snapshot (perceived state, one per decision cycle)

```json
{
  "tick": 14823,
  "self": { "class": "warrior", "char_level": 3, "hp": 42, "max_hp": 61,
            "mana": 8, "max_mana": 15, "ac": 21, "dmg": [3, 9],
            "light_radius_tiles": 9, "action": "melee_attack", "target_id": "m1" },
  "visible_monsters": [
    { "id": "m1", "type": "skeleton", "bearing_deg": 15, "dist_tiles": 4,
      "attacking": true, "health": "healthy", "pack_size": 3, "is_unique": false } ],
  "heard": [ { "event": "bow_fire", "bearing_deg": 10, "loudness": "loud", "age_s": 0.4 } ],
  "items_visible": [ { "id": "i7", "kind": "short_sword", "bearing_deg": -20,
                       "dist_tiles": 1, "quality": "magic" } ],
  "inventory": { "healing_potions_belt": 2 },
  "equipped": { "weapon": "short_sword", "armor": "rags" },
  "gold": 340,
  "door": { "bearing_deg": 90, "dist_tiles": 3, "state": "closed" },
  "navigation": { "objective": "explore", "frontier_exists": true, "blocked_s": 0 },
  "messages": [ { "text": "You hit the Skeleton", "age_s": 1.2 } ]
}
```

A heard event carries bearing and loudness only — never distance, never identity
beyond what the sound implies. `dist_tiles` appears for visible entities only.

### 3b. The Tier 2 brief (event-driven, no map)

```json
{
  "run": { "attempt": 1, "deaths": 1, "elapsed_s": 2210 },
  "objective": "reach dungeon level 2 alive",
  "character": { "class": "warrior", "char_level": 4, "hp": 61, "max_hp": 61,
                 "ac": 29, "gold": 120, "potions": 1 },
  "situation": { "location": "dungeon_1", "cadaver": { "present": true,
                  "recovered": false, "depth": "dungeon_1" },
                 "stairs_known": true, "explored_pct": 78 },
  "events": [ "died to skeleton pack", "level up (+5 points unallocated)" ],
  "chat": [ { "from": "joerg", "text": "come back up, I have potions" } ],
  "jev_log_tail": [ { "t": 14820, "choice": "retreat", "conf": 0.71 } ]
}
```

`explored_pct` is a progress summary for me — not a map, and not an input to
navigation.

---

## 4. Navigation & exploration (Tier 0, deterministic)

The user's rule: exploring a labyrinth does not need expensive calls. A level-0
frontier algorithm walks the maze; the LLM is not involved.

Inputs — all read from the game's own data, no duplicate store:

- explored/unexplored state: `DungeonFlag::Explored` / automap grid
- currently visible tiles: `DungeonFlag::Visible` (an unexplored tile that is
  visible is a corridor continuing — the cheapest source of frontiers)
- walkability, doors, and the engine's pathfinding for actual movement

Algorithm:

1. Build the walkable graph over **explored** tiles plus currently visible ones.
2. Compute the **frontier**: walkable explored/visible tiles adjacent to tiles never
   explored or visible — the boundary of knowledge.
3. Rank frontier targets nearest-first by path length (BFS/Dijkstra on the local
   graph); tie-break to avoid oscillation (prefer unchosen frontiers and straight
   continuations of the current corridor).
4. Issue a walk command to the chosen target — the engine walks the character.
5. On arrival or obstruction, re-evaluate and loop.
6. **Doors on the route**: local rule opens an unlocked door (a cheap action). A
   locked door, or a door where the Tier 1 judgment vetoes, is skipped and marked.
7. **Stairs-down detected** → record the landmark (the automap exposes it) → route
   to it once the viability gate (§6) allows.
8. **Stuck handling**: no progress for N seconds → mark the frontier unreachable and
   take the next; all frontiers unreachable → escalate.
9. **Termination**: no frontier remains and no stairs found → escalate (the stairs
   always exist, so this means a real bug or an unsupported level feature).

Doors, precisely: **Tier 1 decides whether to open** (it is a commitment under
uncertainty — see §5); **Tier 0 performs the open** and handles the door physically.

Because navigation runs in-engine against the game's own map, raw map data never
crosses the tier boundary — which is also why the anti-cheat audit stays simple (§7).

---

## 5. Jev battery

One call per cycle (~3 Hz), persistent HTTP connection, whole battery in one call —
question count is nearly free (1 q = 314 ms, 7 q = 351 ms measured).

Primary decision, confidence-gated:

```
"next_action": {
  "type": "choice",
  "instructions": "Given the player's condition and the threats in view, what should the player do next?",
  "criteria": {
    "continue_fight": "Keep attacking the current target; the player can win.",
    "retreat":        "Disengage toward safer ground; the fight is not winnable as is.",
    "kite":           "Attack while backing away to split the group.",
    "portal_out":     "Open a town portal and leave; survival is at stake."
  }
}
```

Door decision (fires when a door blocks the route or is adjacent):

```
"door_action": {
  "type": "choice",
  "instructions": "The player stands before a closed door. What should they do?",
  "criteria": {
    "open_now":     "Open it; the player can handle what is likely behind it.",
    "prepare_first":"Drink a potion, reposition, then open.",
    "leave_it":     "Do not open; continue exploring elsewhere.",
    "retreat":      "Back away from the door entirely."
  }
}
```

Supporting `noul`s (features for the log and gating): `threat_now`, `outnumbered`,
`strength_advantage`, `low_on_potions`, `safe_to_loot`, `viable_to_descend`,
`item_is_upgrade`, `shrine_worthwhile`.

Decision rule: the `choice` drives the committed intent, `confidence` gates it;
below floor → Tier 0 default + escalate to me. Nouls are logged so the battery can
be tuned against outcomes later.

Measured budget: p50 351 ms, min 245 ms persistent; ~1000 in / 161 out tokens per
cycle. Harness: `/tmp/ts_latency_probe.py` (to be moved into the repo).

---

## 6. Survival policies

**Cadaver recovery.** Death is a setback with a retrieval task, not a wipe. Items and
half the gold remain where the character fell; the engine marks that tile
(`DungeonFlag::DeadPlayer`). The walk back is *necessary*, not a loss — the explored
map is intact, so the route is known.

1. Respawn in town, heal, and re-kit from town/stash with whatever is affordable —
   the recovered gear matters more than the temporary kit.
2. Walk back down the known route.
3. Assess the cadaver site from perception; the pack that killed you is usually
   still there.
4. Prefer kiting the pack away from the body, or a portal retreat and re-approach,
   over a straight fight with no gear.
5. Recover items and gold, then resume the objective.

**Town portal as escape.** Danger + potions exhausted → portal out. Fired by a Tier 0
reflex rule so it happens *before* death rather than after; `portal_out` is also
available as a Tier 1 choice while there is still time to deliberate. Portal
semantics (duration, whether it can be re-entered for the return trip) are to be
verified empirically, not assumed.

**Viability gate.** Descend when the character can survive what is below, not when the
stairs are found. Starting heuristic (tunable after run 1): level ≥ 3, ≥ 2 potions,
weapon better than the starting kit — with Jev's `viable_to_descend` as the
calibrated cross-check.

---

## 7. Anti-cheat & audit

1. **One filtered snapshot** is the only input to Jev and to the LLM. Ground truth
   reaching either tier voids the exercise.
2. **Every decision is logged** with its snapshot, answers, confidence, and action.
3. **Mechanical audit test**: replay the logs and assert no decision references an
   entity that was not `Visible`/audible in the snapshot that produced it.
4. **Cheat doors structurally closed**: agent code has no path to the `dev.*` Lua
   family (`dev/level/map.cpp`, `dev/search.cpp`, `dev/monsters.cpp`, `dev/items.cpp`,
   `dev/level/warp.cpp`, `dev/player/*`) nor to raw monster/item enumeration. Last
   time's monster-table read came through exactly there.
5. **Raw map data never leaves the process** — navigation is in-engine (§4), so the
   only map-derived number crossing the boundary is a progress percentage.

---

## 8. Architecture

```
DevilutionX fork (branch: agent)
├── Source/…/agent/            ← new native module (C++)
│   ├── perceive()   → Tier 1 snapshot (§3a)   reads Visible/Lit/Explored/DeadPlayer
│   ├── navigate()   → Tier 0 exploration (§4)  frontier search + walk commands
│   ├── act()        → engine calls             move, attack, pickup, door, potion, portal
│   ├── reflex loop  → per-frame Tier 0         committed intent + thresholds
│   ├── jev client   → Tier 1 @ ~3 Hz           persistent conn, worker thread, battery §5
│   └── bridge       → Tier 2 I/O               escalation queue out, directive in
└── (no dev.* linkage from agent code)

Hermes side
└── escalation watcher (1-min cron): non-empty queue → wake me with the brief (§3b)
    → I write a directive (goal | recover cadaver | re-kit in town | follow player | stop)
    → plus stat allocation and town purchases
```

Tier 2 is **event-driven, not polled for decisions**. The engine writes escalation
events — objective reached, death, level-up, navigation stuck, chat message, novel
encounter — and the watcher wakes me only when there is something for me. A slow
heartbeat gives oversight without spending calls on routine play.

---

## 9. Open items to verify empirically

1. Portal semantics — duration, re-entry for the return trip.
2. Exact death-drop behaviour (items + half gold) observed live.
3. ~~Whether monsters in LOS but unlit are perceived as silhouettes~~ — settled in
   favour of *not* exporting them: the snapshot exposes an entity only when its tile
   carries `DungeonFlag::Visible`, which is the same predicate the renderer uses to draw
   it lit. If a silhouette proves to be genuinely player-visible, the channel can be
   widened deliberately; until then the narrower rule stands.
4. ~~Whether `Lit` already folds in gear/spell light radius~~ — the exposed
   `light_radius_tiles` matched the observed sight range exactly (radius 10 → a monster
   first appeared at 10 tiles and remained visible while it closed).
5. Multiplayer: monster HP scaling; party-member visibility on the automap.
6. Does the engine expose a path-length query over explored tiles (for frontier
   ranking), or do we compute the local graph ourselves? — currently computed
   agent-side with a BFS over the snapshot's seen-tile grid.
7. Escalation transport: spool file vs socket; brief size and cadence.
8. **Path routing vs knowledge.** A walk order is handed to the engine's own pathfinder,
   which knows the whole level, so the route may cross tiles the agent has not seen. This
   matches what a human gets when clicking a distant tile, but the *destination* is always
   a tile the agent has seen. Decide whether routing must also be restricted to known
   tiles (it would need a single-step command cadence).
9. Ground items are not yet in the snapshot (loot decisions need them).

---

## 10. Success criteria

- **Primary**: descend to dungeon level 2 alive, with a viable character.
- Not required: clearing every monster. Killing, looting, and levelling are
  instrumental — necessary to survive levels 2 and 3.
- Death → restart in town → recover the cadaver → continue. Not a failure.
- Review checkpoints: after the first full session or 3 deaths, whichever is first.

---

## 11. Verification log

Everything below was observed live against the built fork (branch `agent`), not reasoned
about from the source.

| Channel | Observation |
| --- | --- |
| Town memory | 6400 cells explored, 0 in sight. The lighting pass does not run in town, so memory there comes from the automap grid; `DungeonFlag::Explored` is 0 everywhere in town. |
| Dungeon memory | On cathedral level 1, 184 of 12544 world tiles explored (1.5%) on arrival at the stairs, rising to 302 after ~5 s of exploration. The level layout, its monsters and its other staircases were absent from the snapshot. |
| Sight | 135–149 tiles in sight at radius 10, matching the rendered lit area. |
| Monsters | None exported while at the entrance; a Skeleton Captain and a Fallen One appeared at exactly 10 tiles (the light radius) once the character advanced, with correct bearing, distance and coarse health. Walking away, they stayed in the list as they chased, and hit for 3 damage. |
| Landmarks | The cathedral entrance was known in town as `descend` at [25,29]; the level-1 stairs up were known as `ascend` at [56,66]. Both were filtered by "tile has been seen". |
| Objects | 8 inside the cathedral entrance, later 10: doors, one chest, solid scenery, each flagged in sight or merely remembered. |
| Actions | `walk` orders executed through the engine's own pathing: 50 tiles across town, down to level 1, then to a frontier, then back to the stairs and up to town. Command sequence numbers acknowledged in the next snapshot. |
| Deliberate non-action | With combat unimplemented the character took 39 damage while merely retreating, which is the expected cost of not fighting back — the reason Tier 0 reflexes come next. |
| Combat | A Fallen One and a Skeleton Captain were killed in melee (+46, +99, +52 experience), confirming kills independently of the agent's own reporting. |
| **Descent to level 2** | **Reached cathedral level 2 alive** at 62/70 hp with 1665 experience: level 1 explored through its doors, monsters cleared on the way, then the way down taken once the level was quiet and health was 88%. |

### Two navigation traps found the hard way

1. **Doors are not walls, but pathing treats them as destination-only.** `IsTileWalkable`
   reports a shut door as solid, which is right for standing and wrong for travelling — the
   engine opens a door the character walks *into*, never one used as a waypoint. A frontier
   search that honours the raw grid therefore declares a level finished while half of it sits
   behind doors (observed: 996 tiles "explored", 586 seen floor, 24 seen-but-walled-off).
   Fix: treat known doors as passable when *planning*, but interrupt the route at the first
   shut door on it, open it, and replan.
2. **An unsatisfiable order fails silently, not loudly.** The engine simply stands still
   while the agent re-issues the same order forever (observed: 90 s livelocked on one goal).
   A stuck detector — no movement for 4 s under a live order — marks that goal unreachable
   and picks another frontier.

