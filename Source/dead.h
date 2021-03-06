/**
 * @file dead.h
 *
 * Interface of functions for placing dead monsters.
 */
#pragma once

#include <array>
#include <cstdint>

#include "engine.h"

namespace devilution {

static constexpr unsigned MaxDead = 31;

struct DeadStruct {
	std::array<uint8_t *, 8> _deadData;
	int _deadFrame;
	int _deadWidth;
	uint8_t _deadtrans;
};

extern DeadStruct dead[MaxDead];
extern int8_t stonendx;

void InitDead();
void AddDead(Point tilePosition, int8_t dv, direction ddir);
void SetDead();

} // namespace devilution
