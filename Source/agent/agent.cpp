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
#include "engine/point.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung_defs.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
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
		    << ",\"health\":\"" << HealthBand(monster) << "\""
		    << ",\"unique\":" << (monster.isUnique() ? "true" : "false")
		    << "}";
	}
	out << "]";

	AppendLandmarks(out, playerTile);
	AppendObjects(out, playerTile);
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

	const std::string path = PrefFile("agent-snapshot.json");
	std::ofstream file(path, std::ios::trunc);
	if (!file) return;
	file << out.str();
}

} // namespace devilution::agent
