#include "agent/agent.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "automap.h"
#include "cursor.h"
#include "engine/point.hpp"
#include "inv.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung_defs.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "quests.h"
#include "stores.h"
#include "towners.h"
#include "utils/paths.h"

namespace devilution::agent {
namespace {

/** Snapshot cadence. The game ticks at ~20 Hz; the consumers (Jev at ~3 Hz, the LLM
 *  supervisor seconds apart) never need per-frame state, so a few writes per second
 *  keep the file fresh without pointless disk churn. */
constexpr int SnapshotIntervalMs = 200;

/** World tiles outside this border are generator workspace: nothing can stand there and
 *  the lighting pass never marks them. Matches the offset SetAutomapView() uses. */
constexpr int MapBorder = 16;

constexpr double Pi = 3.14159265358979323846;

std::string PrefFile(const char *name)
{
	std::string path = paths::PrefPath();
	if (!path.empty() && path.back() != '/') path += '/';
	return path + name;
}

/**
 * @brief Compass bearing from `from` to `to`, degrees clockwise from north (0 = up).
 *
 * Bearing is how a player describes direction ("something to my left"). Exporting raw
 * dx/dy would hand the higher tiers the map's coordinate frame, which is not something
 * a player has.
 */
int BearingDegrees(Point from, Point to)
{
	const double dx = static_cast<double>(to.x - from.x);
	const double dy = static_cast<double>(to.y - from.y);
	double degrees = std::atan2(dx, -dy) * (180.0 / Pi);
	if (degrees < 0) degrees += 360.0;
	return static_cast<int>(degrees + 0.5);
}

/** Movement-metric distance: diagonal steps cost the same as orthogonal ones, matching
 *  how the engine's pathing behaves. */
int TileDistance(Point a, Point b)
{
	return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

/**
 * @brief What a player can tell about a monster's condition at a glance.
 *
 * Deliberately coarse: the UI shows no hit-point numbers, so exporting exact values
 * would be information the player does not have.
 */
const char *HealthBand(const Monster &monster)
{
	if (monster.maxHitPoints <= 0) return "unknown";
	const int percent = (monster.hitPoints * 100) / monster.maxHitPoints;
	if (percent >= 90) return "healthy";
	if (percent >= 50) return "wounded";
	if (percent >= 20) return "badly_wounded";
	return "near_death";
}

/** What a monster is visibly doing. Mode is the animation-level state ("delayed" is a
 *  monster mid-action such as eating); goal is the strategic intent behind it, which is the
 *  part that matters for a decision - a Scavenger on MonsterGoal::Healing has broken off to
 *  find a corpse and will come back healed, and standing at range re-issuing attack orders
 *  at it is not a plan. Both are things a player watching the screen can see. */
const char *MonsterModeName(MonsterMode mode)
{
	switch (mode) {
	case MonsterMode::Stand: return "standing";
	case MonsterMode::MoveNorthwards:
	case MonsterMode::MoveSouthwards:
	case MonsterMode::MoveSideways: return "moving";
	case MonsterMode::MeleeAttack: return "melee_attack";
	case MonsterMode::HitRecovery: return "hit_recovery";
	case MonsterMode::Death: return "dying";
	case MonsterMode::SpecialMeleeAttack: return "special_melee";
	case MonsterMode::FadeIn: return "fading_in";
	case MonsterMode::FadeOut: return "fading_out";
	case MonsterMode::RangedAttack: return "ranged_attack";
	case MonsterMode::SpecialStand: return "special_stand";
	case MonsterMode::SpecialRangedAttack: return "special_ranged";
	case MonsterMode::Delay: return "delayed";
	case MonsterMode::Charge: return "charging";
	case MonsterMode::Petrified: return "petrified";
	case MonsterMode::Heal: return "healing";
	case MonsterMode::Talk: return "talking";
	}
	return "unknown";
}

const char *MonsterGoalName(MonsterGoal goal)
{
	switch (goal) {
	case MonsterGoal::None: return "none";
	case MonsterGoal::Normal: return "normal";
	case MonsterGoal::Retreat: return "retreating";
	case MonsterGoal::Healing: return "seeking_healing";
	case MonsterGoal::Move: return "moving_to";
	case MonsterGoal::Attack: return "attacking";
	case MonsterGoal::Inquiring: return "inquiring";
	case MonsterGoal::Talking: return "talking";
	}
	return "unknown";
}

const char *LevelKind(dungeon_type type)
{
	switch (type) {
	case DTYPE_TOWN: return "town";
	case DTYPE_CATHEDRAL: return "cathedral";
	case DTYPE_CATACOMBS: return "catacombs";
	case DTYPE_CAVES: return "caves";
	case DTYPE_HELL: return "hell";
	case DTYPE_NEST: return "nest";
	case DTYPE_CRYPT: return "crypt";
	default: return "unknown";
	}
}

/**
 * @brief What a level transition looks like to a player who is standing on it.
 *
 * A player reads these off the staircase itself, which is why the landmark is only
 * exported once its tile has been seen (see AppendLandmarks).
 */
const char *TransitionKind(interface_mode mode)
{
	switch (mode) {
	case WM_DIABNEXTLVL: return "descend";
	case WM_DIABPREVLVL: return "ascend";
	case WM_DIABRTNLVL: return "return_level";
	case WM_DIABSETLVL: return "quest_level";
	case WM_DIABWARPLVL: return "warp";
	case WM_DIABTOWNWARP: return "town_portal";
	case WM_DIABTWARPUP: return "town_portal_up";
	case WM_DIABRETOWN: return "return_town";
	default: return "other";
	}
}

/** Object categories a player distinguishes on sight. Everything else is scenery until
 *  it turns out to matter. */
const char *ObjectKind(const Object &object)
{
	if (object.isDoor()) return "door";
	if (object.IsChest()) return "chest";
	if (object.IsBarrel()) return "barrel";
	if (object.IsShrine()) return "shrine";
	if (object.IsTrap()) return "trap";
	return "other";
}

std::string EscapeJson(const std::string &value)
{
	std::string out;
	out.reserve(value.size() + 8);
	for (const char c : value) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c; break;
		}
	}
	return out;
}

std::string Tile(Point position)
{
	return "[" + std::to_string(position.x) + "," + std::to_string(position.y) + "]";
}

/** Health and mana are stored in 64ths of a point; that fixed-point representation is an
 *  engine detail the tiers should never see. */
int WholePoints(int fixedPointValue)
{
	return fixedPointValue >> 6;
}

/**
 * @brief Is this world tile part of the map the player has seen?
 *
 * The lighting pass stamps DungeonFlag::Explored on world tiles as they come into view,
 * and that flag lives in the save file - but the lighting pass does not run in town,
 * where the automap's own grid (filled at town load) is the record instead. Consult both
 * so "what I remember" is complete wherever the agent happens to be.
 */
bool IsTileExplored(Point tile)
{
	if (HasAnyOf(dFlags[tile.x][tile.y], DungeonFlag::Explored)) return true;
	if (tile.x < MapBorder || tile.y < MapBorder) return false;
	const Point map { (tile.x - MapBorder) / 2, (tile.y - MapBorder) / 2 };
	if (map.x >= DMAXX || map.y >= DMAXY) return false;
	return AutomapView[map.x][map.y] != MAP_EXP_NONE;
}

/**
 * @brief Emit the map layer: what the player remembers plus what is in sight right now.
 *
 * One grid rather than two, because each cell states both facts without ambiguity - sight
 * takes precedence over memory ('o'/'x' vs '.'/'#') - so a reader never has to reconcile
 * layers and the file stays half the size.
 *
 * Coordinates are engine world tiles, the same space the vision system and the pathing
 * use. The in-game automap's grid is a half-resolution *view* of this (`(pos - 16) / 2`),
 * so reconstructing from it would discard detail that navigation needs.
 *
 * @param playerTile marked '@' so a reader can locate the player without cross-referencing.
 */
void AppendMapGrid(std::ostringstream &out, Point playerTile)
{
	std::string grid;
	grid.reserve((MAXDUNX + 1) * MAXDUNY);
	int exploredTiles = 0;
	int visibleTiles = 0;

	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			const bool explored = IsTileExplored(tile);
			const bool visible = IsTileVisible(tile);
			const bool walkable = IsTileWalkable(tile);
			if (explored) exploredTiles++;
			if (visible) visibleTiles++;

			if (tile == playerTile) {
				grid += '@';
			} else if (visible) {
				grid += walkable ? 'o' : 'x';
			} else if (!explored) {
				grid += ' ';
			} else {
				grid += walkable ? '.' : '#';
			}
		}
		grid += '\n';
	}

	out << ",\"map\":{"
	    << "\"grid_origin\":[0,0]"
	    << ",\"world_size\":[" << MAXDUNX << "," << MAXDUNY << "]"
	    << ",\"explored_tiles\":" << exploredTiles
	    << ",\"visible_tiles\":" << visibleTiles
	    << ",\"legend\":\"' '=never seen, '.'=seen floor, '#'=seen blocked, 'o'=in sight floor, 'x'=in sight blocked, '@'=player\""
	    << ",\"grid\":\"" << EscapeJson(grid) << "\""
	    << "}";
}

/**
 * @brief Emit the landmarks the player knows about: level transitions they have seen.
 *
 * The engine's trigger table records where every staircase, portal and quest entrance is,
 * but a player only learns of one by laying eyes on it, so each candidate is filtered by
 * whether its tile has ever been in view. Past that memory the entry carries the same
 * facts a player reads off the stairs themselves: which way they go, and how far.
 */
void AppendLandmarks(std::ostringstream &out, Point playerTile)
{
	out << ",\"landmarks\":[";
	bool first = true;
	for (int i = 0; i < numtrigs; i++) {
		const Point tile { trigs[i].position.x, trigs[i].position.y };
		if (!IsTileExplored(tile)) continue;

		if (!first) out << ",";
		first = false;
		out << "{\"kind\":\"" << TransitionKind(trigs[i]._tmsg) << "\""
		    << ",\"tile\":" << Tile(tile)
		    << ",\"target_level\":" << trigs[i]._tlvl
		    << ",\"in_sight\":" << (IsTileVisible(tile) ? "true" : "false")
		    << ",\"dist_tiles\":" << TileDistance(playerTile, tile)
		    << ",\"bearing_deg\":" << BearingDegrees(playerTile, tile)
		    << "}";
	}
	out << "]";
}

/**
 * @brief Emit the objects the player has seen: doors, containers, shrines, traps.
 *
 * Filtered exactly like landmarks - an object nobody has laid eyes on is not knowledge the
 * agent is allowed to have. Doors matter immediately, because they gate dungeon corridors
 * and the engine paths to them differently (see ExecuteCommand); containers matter for
 * loot.
 */
void AppendObjects(std::ostringstream &out, Point playerTile)
{
	out << ",\"objects\":[";
	bool first = true;
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (object._oDelFlag) continue;
		const Point tile = object.position;
		if (!IsTileExplored(tile)) continue;

		if (!first) out << ",";
		first = false;
		out << "{\"slot\":" << ActiveObjects[i]
		    << ",\"kind\":\"" << ObjectKind(object) << "\""
		    << ",\"tile\":" << Tile(tile)
		    << ",\"solid\":" << (object._oSolidFlag ? "true" : "false")
		    << ",\"in_sight\":" << (IsTileVisible(tile) ? "true" : "false")
		    << ",\"dist_tiles\":" << TileDistance(playerTile, tile)
		    << "}";
	}
	out << "]";
}

const char *ItemClassName(const Item &item)
{
	switch (item._iClass) {
	case ICLASS_WEAPON: return "weapon";
	case ICLASS_ARMOR: return "armor";
	case ICLASS_GOLD: return "gold";
	case ICLASS_QUEST: return "quest";
	case ICLASS_MISC: return "misc";
	default: return "other";
	}
}

/** What a miscellaneous item is for, in the terms a player thinks in: potions by effect,
 *  the rest by category. Weapons and armour are named well enough by their own name. */
const char *MiscEffect(const Item &item)
{
	if (item._itype == ItemType::Gold) return "gold";
	switch (item._iMiscId) {
	case IMISC_HEAL: return "healing potion";
	case IMISC_FULLHEAL: return "full healing potion";
	case IMISC_MANA: return "mana potion";
	case IMISC_FULLMANA: return "full mana potion";
	case IMISC_REJUV: return "rejuvenation potion";
	case IMISC_FULLREJUV: return "full rejuvenation potion";
	case IMISC_ELIXSTR: return "elixir of strength";
	case IMISC_ELIXMAG: return "elixir of magic";
	case IMISC_ELIXDEX: return "elixir of dexterity";
	case IMISC_ELIXVIT: return "elixir of vitality";
	case IMISC_SCROLL:
	case IMISC_SCROLLT: return "scroll";
	case IMISC_BOOK: return "book";
	case IMISC_STAFF: return "staff";
	case IMISC_RING: return "ring";
	case IMISC_AMULET: return "amulet";
	default: return "";
	}
}

/** Where an item can be worn (ILOC_*), which is the item's own property. Not to be
 *  confused with EquipSlotName below: ILOC_ONEHAND and INVLOC_RING_LEFT are both 1, and
 *  mapping one through the other's names silently mislabels every weapon and ring. */
const char *ItemEquipKind(int loc)
{
	switch (loc) {
	case ILOC_ONEHAND: return "one_hand";
	case ILOC_TWOHAND: return "two_hand";
	case ILOC_ARMOR: return "body";
	case ILOC_HELM: return "head";
	case ILOC_RING: return "ring";
	case ILOC_AMULET: return "amulet";
	case ILOC_BELT: return "belt";
	case ILOC_UNEQUIPABLE: return "unequipable";
	default: return "none";
	}
}

/** Which worn slot an entry in InvBody occupies (INVLOC_*). */
const char *EquipSlotName(int loc)
{
	switch (loc) {
	case INVLOC_HEAD: return "head";
	case INVLOC_RING_LEFT: return "ring_left";
	case INVLOC_RING_RIGHT: return "ring_right";
	case INVLOC_AMULET: return "amulet";
	case INVLOC_HAND_LEFT: return "hand_left";
	case INVLOC_HAND_RIGHT: return "hand_right";
	case INVLOC_CHEST: return "body";
	default: return "none";
	}
}

/** One item as the player sees it. An unidentified magic item reports its base name only -
 *  exactly what the game draws - because a prefix the character has not identified is not
 *  knowledge the agent is entitled to either. Writes fields without the enclosing braces so
 *  callers can merge in their own (tile, slot, index). */
void AppendItemFields(std::ostringstream &out, const Item &item)
{
	const bool identified = item._iIdentified || item._iMagical == ITEM_QUALITY_NORMAL;
	const std::string name = (item._iIdentified && item._iIName[0] != '\0') ? std::string(item._iIName) : std::string(item._iName);

	out << "\"name\":\"" << EscapeJson(name) << "\""
	    << ",\"class\":\"" << ItemClassName(item) << "\""
	    << ",\"effect\":\"" << MiscEffect(item) << "\""
	    << ",\"itype\":" << static_cast<int>(item._itype)
	    << ",\"magical\":" << static_cast<int>(item._iMagical)
	    << ",\"identified\":" << (identified ? "true" : "false")
	    << ",\"equip_slot\":\"" << ItemEquipKind(item._iLoc) << "\""
	    << ",\"min_damage\":" << static_cast<int>(item._iMinDam)
	    << ",\"max_damage\":" << static_cast<int>(item._iMaxDam)
	    << ",\"ac\":" << static_cast<int>(item._iAC)
	    << ",\"value\":" << item._ivalue
	    << ",\"identified_value\":" << item._iIvalue
	    << ",\"durability\":" << item._iDurability
	    << ",\"max_durability\":" << item._iMaxDur
	    << ",\"spell\":" << static_cast<int>(item._iSpell);
}

/** Loot on the floor, under the same rule as monsters: if it is not in the light, the
 *  player cannot see it, so the agent is not told about it. Items walked past and left in
 *  the dark drop out again, which is also what the game draws. */
void AppendGroundItems(std::ostringstream &out, Point playerTile)
{
	out << ",\"ground_items\":[";
	bool first = true;
	for (int i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		const Point tile = item.position;
		if (!IsTileVisible(tile)) continue;

		if (!first) out << ",";
		first = false;
		// ActiveItems holds uint8_t, and streaming a uint8_t emits a *character*: index 10
		// lands in the JSON as a raw newline and the whole snapshot fails to parse. Cast
		// explicitly; the object and monster tables are int and unsigned, so they are safe.
		out << "{\"index\":" << static_cast<int>(ActiveItems[i])
		    << ",\"tile\":" << Tile(tile)
		    << ",\"bearing_deg\":" << BearingDegrees(playerTile, tile)
		    << ",\"dist_tiles\":" << TileDistance(playerTile, tile)
		    << ",";
		AppendItemFields(out, item);
		out << "}";
	}
	out << "]";
}

/** The quest log: what the player has been told to do, and how far along it is.
 *
 *  _qlog is the game's own flag for "this belongs in the player's quest log", which is
 *  exactly the player-knowledge boundary - the Butcher is on level 2 regardless, but "kill
 *  the Butcher" is not a goal until a book or Cain has put it in the log. Reading it from
 *  Quests keeps the agent honest about what it is supposed to know. */
void AppendQuests(std::ostringstream &out)
{
	out << ",\"quests\":[";
	bool first = true;
	for (int i = 0; i < MAXQUESTS; i++) {
		const Quest &quest = Quests[i];

		if (!first) out << ",";
		first = false;
		const char *state = "unknown";
		switch (quest._qactive) {
		case QUEST_INIT: state = "not_started"; break;
		case QUEST_ACTIVE: state = "active"; break;
		case QUEST_DONE: state = "done"; break;
		case QUEST_NOTAVAIL: state = "unavailable"; break;
		}
		const std::string title = i < static_cast<int>(QuestsData.size()) ? QuestsData[i]._qlstr : std::string();
		out << "{\"id\":" << i
		    << ",\"title\":\"" << EscapeJson(title) << "\""
		    << ",\"state\":\"" << state << "\""
		    << ",\"level\":" << static_cast<int>(quest._qlevel)
		    << ",\"done\":" << (quest._qactive == QUEST_DONE ? "true" : "false")
		    << ",\"in_log\":" << (quest._qlog ? "true" : "false")
		    << "}";
	}
	out << "]";
}

/** The townsfolk the player can see. In town this is how an objective like "get healed"
 *  becomes a destination: Pepin is a Towner with _ttype TOWN_HEALER, and talking to him runs
 *  TalkToHealer -> StartHealer -> HealPlayer, which is the free heal. Nothing is
 *  reimplemented here; the agent just needs to know who is where to walk up and talk. */
void AppendTownsfolk(std::ostringstream &out, Point playerTile)
{
	out << ",\"townsfolk\":[";
	bool first = true;
	for (size_t i = 0; i < Towners.size(); i++) {
		const Towner &towner = Towners[i];
		const Point tile = towner.position;
		if (!IsTileExplored(tile)) continue;

		if (!first) out << ",";
		first = false;
		const auto shortName = TownerShortNames.find(towner._ttype);
		out << "{\"index\":" << i
		    << ",\"tile\":" << Tile(tile)
		    << ",\"name\":\"" << EscapeJson(shortName != TownerShortNames.end() ? shortName->second : "unknown") << "\""
		    << ",\"ttype\":" << static_cast<int>(towner._ttype)
		    << ",\"in_sight\":" << (IsTileVisible(tile) ? "true" : "false")
		    << ",\"dist_tiles\":" << TileDistance(playerTile, tile)
		    << ",\"bearing_deg\":" << BearingDegrees(playerTile, tile)
		    << "}";
	}
	out << "]";
}

/** What the character is wearing and carrying on the belt. The player sheet is the player's
 *  own knowledge, so nothing here needs a visibility filter. */
void AppendEquipment(std::ostringstream &out)
{
	const Player &player = *MyPlayer;

	out << ",\"equipment\":[";
	bool first = true;
	for (int loc = 0; loc < NUM_INVLOC; loc++) {
		const Item &item = player.InvBody[loc];
		if (item._itype == ItemType::None) continue;
		if (!first) out << ",";
		first = false;
		out << "{\"slot\":\"" << EquipSlotName(loc) << "\",";
		AppendItemFields(out, item);
		out << "}";
	}
	out << "]";

	out << ",\"belt\":[";
	first = true;
	for (int i = 0; i < MaxBeltItems; i++) {
		const Item &item = player.SpdList[i];
		if (item._itype == ItemType::None) continue;
		if (!first) out << ",";
		first = false;
		out << "{\"belt_slot\":" << i << ",";
		AppendItemFields(out, item);
		out << "}";
	}
	out << "]";
}

/** Minimal field readers for the command file. Deliberately tiny: the agent owns both
 *  ends of this channel, and pulling a JSON library into the engine for four fields is
 *  not worth the dependency. */
bool ReadField(const std::string &json, const char *key, std::string &out)
{
	const std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos) return false;
	const size_t start = json.find_first_not_of(" \t\r\n", pos + 1);
	if (start == std::string::npos) return false;

	if (json[start] == '"') {
		const size_t end = json.find('"', start + 1);
		if (end == std::string::npos) return false;
		out = json.substr(start + 1, end - start - 1);
		return true;
	}
	const size_t end = json.find_first_of(",}\r\n", start);
	out = json.substr(start, end == std::string::npos ? std::string::npos : end - start);
	return !out.empty();
}

bool ReadIntField(const std::string &json, const char *key, int &out)
{
	std::string raw;
	if (!ReadField(json, key, raw)) return false;
	try {
		out = std::stoi(raw);
	} catch (...) {
		return false;
	}
	return true;
}

/**
 * @brief Execute one agent command.
 *
 * Mirrors the engine's own local command handlers (OnWalk/OnObjectTileAction in msg.cpp)
 * so an agent-driven action takes exactly the same route through the engine as a
 * player-driven one - no special-cased movement or combat paths.
 */
void ExecuteCommand(const std::string &command, int x, int y, int slot)
{
	Player &player = *MyPlayer;

	if (command == "stop") {
		ClrPlrPath(player);
		player.destAction = ACTION_NONE;
		return;
	}
	if (command == "walk") {
		const Point position { x, y };
		if (!InDungeonBounds(position)) return;
		ClrPlrPath(player);
		MakePlrPath(player, position, true);
		player.destAction = ACTION_NONE;
		return;
	}
	if (command == "operate") {
		const Point position { x, y };
		const Object *object = FindObjectAtPosition(position);
		if (object == nullptr) return;
		MakePlrPath(player, position, !object->_oSolidFlag && !object->_oDoorFlag);
		player.destAction = ACTION_OPERATE;
		player.destParam1 = static_cast<int>(object->GetId());
		return;
	}
	if (command == "attack") {
		if (slot < 0 || slot >= MaxMonsters) return;
		const Point target = Monsters[slot].position.future;
		if (player.position.tile.WalkingDistance(target) > 1)
			MakePlrPath(player, target, false);
		player.destAction = ACTION_ATTACKMON;
		player.destParam1 = slot;
		return;
	}
	if (command == "pickup") {
		// Mirrors OnGotoGetItem: the index is an entry in Items[], and the path is made to
		// the dropped item's tile so the character walks over and takes it.
		const Point position { x, y };
		if (!InDungeonBounds(position)) return;
		if (slot < 0 || slot > MAXITEMS) return;
		if (Items[slot]._itype == ItemType::None) return;
		MakePlrPath(player, position, false);
		player.destAction = ACTION_PICKUPITEM;
		player.destParam1 = slot;
		return;
	}
	if (command == "use") {
		// Belt and worn items are addressed the same way the inventory UI addresses them.
		if (slot < INVITEM_BELT_FIRST) return;
		UseInvItem(slot);
		return;
	}
	if (command == "heal") {
		// The last step of talking to Pepin: the healer's home screen opens with "Talk to
		// Pepin" highlighted, and choosing it runs TalkID::Healer -> StartHealer -> HealPlayer.
		// StartStore is the engine's own entry for that, and calling it keeps the heal an
		// interaction: the character must be standing at the healer to use it.
		for (const Towner &towner : Towners) {
			if (towner._ttype != TOWN_HEALER) continue;
			if (player.position.tile.WalkingDistance(towner.position) > 1) continue;
			StartStore(TalkID::Healer);
			return;
		}
		return;
	}
	if (command == "talk") {
		// Mirrors OnTalkXY: destParam1 carries the towner index. Walking up to Pepin and
		// talking is the whole of "go get healed" - the engine does the healing.
		if (slot < 0 || slot >= static_cast<int>(Towners.size())) return;
		const Point position = Towners[slot].position;
		MakePlrPath(player, position, false);
		player.destAction = ACTION_TALK;
		player.destParam1 = slot;
		return;
	}
	if (command == "equip") {
		// The engine decides whether the character *can* wear it (stats, two hands, class);
		// whether it is an upgrade is the policy's call, not the engine's.
		Item &held = player.HoldItem;
		if (held.isEmpty()) return;
		if (!AutoEquip(player, held)) return; // cannot be worn: keep holding, let the caller stow it
		held.clear();
		NewCursor(CURSOR_HAND);
		return;
	}
	if (command == "stow") {
		// Belt before inventory: potions belong where they can be drunk from.
		Item &held = player.HoldItem;
		if (held.isEmpty()) return;
		if (!AutoPlaceItemInBelt(player, held, true) && !AutoPlaceItemInInventory(player, held, true))
			return; // no room: the character keeps holding it, which the snapshot reports
		held.clear();
		NewCursor(CURSOR_HAND);
		return;
	}
}

/**
 * @brief Consume at most one queued command and return the sequence number now applied.
 *
 * The command file is one-shot: it is deleted the moment it is read, and a sequence
 * number that is not newer than the last applied one is ignored, so a replayed or
 * half-written file can never re-fire an action.
 */
int ConsumeCommand()
{
	static int lastSeq = 0;

	const std::string path = PrefFile("agent-command.json");
	std::ifstream file(path);
	if (!file) return lastSeq;

	std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	file.close();
	std::remove(path.c_str());

	int seq = 0;
	if (!ReadIntField(body, "seq", seq) || seq <= lastSeq) return lastSeq;

	std::string command;
	if (!ReadField(body, "cmd", command)) return lastSeq;

	int x = 0;
	int y = 0;
	int slot = 0;
	ReadIntField(body, "x", x);
	ReadIntField(body, "y", y);
	ReadIntField(body, "slot", slot);

	ExecuteCommand(command, x, y, slot);
	lastSeq = seq;
	return lastSeq;
}

void AppendSnapshot(std::ostringstream &out, int lastCommandSeq)
{
	const Player &player = *MyPlayer;
	const Point playerTile = player.position.tile;

	out << "{\"in_game\":true";
	out << ",\"level\":{\"kind\":\"" << LevelKind(leveltype) << "\""
	    << ",\"dungeon_level\":" << static_cast<int>(currlevel)
	    << ",\"is_set_level\":" << (setlevel ? "true" : "false")
	    << "}";

	out << ",\"player\":{"
	    << "\"tile\":" << Tile(playerTile)
	    << ",\"hp\":" << WholePoints(player._pHitPoints)
	    << ",\"max_hp\":" << WholePoints(player._pMaxHP)
	    << ",\"mana\":" << WholePoints(player._pMana)
	    << ",\"max_mana\":" << WholePoints(player._pMaxMana)
	    << ",\"character_level\":" << static_cast<int>(player.getCharacterLevel())
	    << ",\"experience\":" << player._pExperience
	    << ",\"gold\":" << player._pGold
	    << ",\"armor_class\":" << static_cast<int>(player._pArmorClass)
	    << ",\"light_radius_tiles\":" << static_cast<int>(player._pLightRad)
	    << ",\"strength\":" << player._pStrength
	    << ",\"magic\":" << player._pMagic
	    << ",\"dexterity\":" << player._pDexterity
	    << ",\"vitality\":" << player._pVitality
	    << ",\"mode\":" << static_cast<int>(player._pmode)
	    << ",\"dest_action\":" << static_cast<int>(player.destAction)
	    << "}";

	// The hand is player-visible state with teeth: the engine only picks an item up when the
	// cursor is a free hand, so anything held has to be equipped or stowed first.
	out << ",\"hand_free\":" << (player.HoldItem.isEmpty() ? "true" : "false");
	out << ",\"holding\":";
	if (player.HoldItem.isEmpty()) {
		out << "null";
	} else {
		out << "{";
		AppendItemFields(out, player.HoldItem);
		out << "}";
	}

	// The perception filter. IsTileVisible is the engine's own answer to "is this tile in
	// the player's light radius with clear line of sight", so sight here is exactly what
	// the renderer would draw - nothing is reimplemented and nothing extra leaks.
	out << ",\"visible_monsters\":[";
	bool firstMonster = true;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.hitPoints <= 0) continue; // corpses are scenery, not threats
		const Point monsterTile = monster.position.tile;
		if (!IsTileVisible(monsterTile)) continue;

		if (!firstMonster) out << ",";
		firstMonster = false;
		out << "{"
		    << "\"slot\":" << ActiveMonsters[i]
		    << ",\"type\":\"" << EscapeJson(monster.type().data().name) << "\""
		    << ",\"tile\":" << Tile(monsterTile)
		    << ",\"bearing_deg\":" << BearingDegrees(playerTile, monsterTile)
		    << ",\"dist_tiles\":" << TileDistance(playerTile, monsterTile)
		    << ",\"mode\":" << static_cast<int>(monster.mode)
		    << ",\"mode_name\":\"" << MonsterModeName(monster.mode) << "\""
		    << ",\"goal\":\"" << MonsterGoalName(monster.goal) << "\""
		    << ",\"health\":\"" << HealthBand(monster) << "\""
		    << ",\"unique\":" << (monster.isUnique() ? "true" : "false")
		    << "}";
	}
	out << "]";

	AppendLandmarks(out, playerTile);
	AppendObjects(out, playerTile);
	AppendGroundItems(out, playerTile);
	AppendTownsfolk(out, playerTile);
	AppendQuests(out);
	AppendEquipment(out);
	AppendMapGrid(out, playerTile);

	out << ",\"last_command_seq\":" << lastCommandSeq << "}";
}

} // namespace

void Tick()
{
	using Clock = std::chrono::steady_clock;
	static Clock::time_point lastWrite = Clock::now() - std::chrono::milliseconds(SnapshotIntervalMs);

	const Clock::time_point now = Clock::now();
	if (now - lastWrite < std::chrono::milliseconds(SnapshotIntervalMs)) return;

	// Without a loaded game there is no player to describe or to act through. Leaving the
	// previous file in place is more honest than overwriting it with an empty snapshot,
	// because the reader can tell "stale" from "nothing to see".
	if (MyPlayer == nullptr) return;

	lastWrite = now;

	// Act first, then describe: the snapshot a consumer reads should already reflect the
	// order it just gave.
	const int lastCommandSeq = ConsumeCommand();

	std::ostringstream out;
	AppendSnapshot(out, lastCommandSeq);

	// Write to a temp file and rename it into place. Renaming is atomic, so a reader sees
	// either the whole previous snapshot or the whole new one - never a half-written file.
	// Writing in place is what tore a read mid-session: the reader polls at 200 ms and the
	// writer truncates the file it is about to fill.
	const std::string path = PrefFile("agent-snapshot.json");
	const std::string tmpPath = path + ".tmp";
	{
		std::ofstream file(tmpPath, std::ios::trunc);
		if (!file) return;
		file << out.str();
		file.flush();
		if (!file) return;
	}
	if (std::rename(tmpPath.c_str(), path.c_str()) != 0) return;
}

} // namespace devilution::agent
