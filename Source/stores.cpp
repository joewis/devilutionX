/**
 * @file stores.cpp
 *
 * Implementation of functionality for stores and towner dialogs.
 */
#include "all.h"
#include "options.h"
#include <algorithm>

namespace devilution {

namespace {

/** The current towner being interacted with */
_talker_id talker;

/** Is the curren dialog full size */
bool stextsize;

/** Number of text lines in the current dialog */
int stextsmax;
/** Remember currently selected text line from stext while displaying a dialog */
int stextlhold;
/** Currently selected text line from stext */
int stextsel;
/** Text lines */
STextStruct stext[STORE_LINES];

/** Does the current panel have a scrollbar */
bool stextscrl;
/** Remember last scoll position */
int stextvhold;
/** Scoll position */
int stextsval;
/** Next scoll position */
int stextdown;
/** Previous scoll position */
int stextup;
/** Count down for the push state of the scroll up button */
char stextscrlubtn;
/** Count down for the push state of the scroll down button */
char stextscrldbtn;

/** Remember current store while displaying a dialog */
talk_id stextshold;

/** Start of possible gossip dialogs for current store */
_speech_id gossipstart;
/** End of possible gossip dialogs for current store */
_speech_id gossipend;

/** Maps from towner IDs to NPC names. */
const char *const talkname[] = {
	"Griswold",
	"Pepin",
	"",
	"Ogden",
	"Cain",
	"Farnham",
	"Adria",
	"Gillian",
	"Wirt"
};

/** pointer to switch lists depending on visite=d shop */
ItemStruct* storeItem;
int storeItemCount;
talk_id activeStore, buyStore, sellStore;

void DrawSTextBack(CelOutputBuffer out)
{
	CelDrawTo(out, PANEL_X + 344, 327 + UI_OFFSET_Y, pSTextBoxCels, 1, 271);
	DrawHalfTransparentRectTo(out, PANEL_X + 347, UI_OFFSET_Y + 28, 265, 297);
}

void DrawSSlider(CelOutputBuffer out, int y1, int y2)
{
	int yd1, yd2, yd3;

	yd1 = y1 * 12 + 44 + UI_OFFSET_Y;
	yd2 = y2 * 12 + 44 + UI_OFFSET_Y;
	if (stextscrlubtn != -1)
		CelDrawTo(out, PANEL_X + 601, yd1, pSTextSlidCels, 12, 12);
	else
		CelDrawTo(out, PANEL_X + 601, yd1, pSTextSlidCels, 10, 12);
	if (stextscrldbtn != -1)
		CelDrawTo(out, PANEL_X + 601, yd2, pSTextSlidCels, 11, 12);
	else
		CelDrawTo(out, PANEL_X + 601, yd2, pSTextSlidCels, 9, 12);
	yd1 += 12;
	for (yd3 = yd1; yd3 < yd2; yd3 += 12) {
		CelDrawTo(out, PANEL_X + 601, yd3, pSTextSlidCels, 14, 12);
	}
	if (stextsel == 22)
		yd3 = stextlhold;
	else
		yd3 = stextsel;
	if (storenumh > 1)
		yd3 = 1000 * (stextsval + ((yd3 - stextup) >> 2)) / (storenumh - 1) * (y2 * 12 - y1 * 12 - 24) / 1000;
	else
		yd3 = 0;
	CelDrawTo(out, PANEL_X + 601, (y1 + 1) * 12 + 44 + UI_OFFSET_Y + yd3, pSTextSlidCels, 13, 12);
}

void AddSLine(int y)
{
	stext[y]._sx = 0;
	stext[y]._syoff = 0;
	stext[y]._sstr[0] = 0;
	stext[y]._sline = 1;
}

void AddSTextVal(int y, int val)
{
	stext[y]._sval = val;
}

void OffsetSTextY(int y, int yo)
{
	stext[y]._syoff = yo;
}

void AddSText(int x, int y, bool j, const char *str, text_color clr, bool sel)
{
	stext[y]._sx = x;
	stext[y]._syoff = 0;
	strcpy(stext[y]._sstr, str);
	stext[y]._sjust = j;
	stext[y]._sclr = clr;
	stext[y]._sline = 0;
	stext[y]._ssel = sel;
}

void PrintStoreItem(ItemStruct *x, int l, text_color iclr)
{
	char sstr[128];
	char str, dex;
	BYTE mag;

	sstr[0] = '\0';
	if (x->_iIdentified) {
		if (x->_iMagical != ITEM_QUALITY_UNIQUE) {
			if (x->_iPrePower != -1) {
				PrintItemPower(x->_iPrePower, x);
				strcat(sstr, tempstr);
			}
		}
		if (x->_iSufPower != -1) {
			PrintItemPower(x->_iSufPower, x);
			if (sstr[0])
				strcat(sstr, ",  ");
			strcat(sstr, tempstr);
		}
	}
	if (x->_iMiscId == IMISC_STAFF && x->_iMaxCharges) {
		sprintf(tempstr, "Charges: %i/%i", x->_iCharges, x->_iMaxCharges);
		if (sstr[0])
			strcat(sstr, ",  ");
		strcat(sstr, tempstr);
	}
	if (sstr[0]) {
		AddSText(40, l, false, sstr, iclr, false);
		l++;
	}
	sstr[0] = '\0';
	if (x->_iClass == ICLASS_WEAPON)
		sprintf(sstr, "Damage: %i-%i  ", x->_iMinDam, x->_iMaxDam);
	if (x->_iClass == ICLASS_ARMOR)
		sprintf(sstr, "Armor: %i  ", x->_iAC);
	if (x->_iMaxDur != DUR_INDESTRUCTIBLE && x->_iMaxDur) {
		sprintf(tempstr, "Dur: %i/%i,  ", x->_iDurability, x->_iMaxDur);
		strcat(sstr, tempstr);
	} else {
		strcat(sstr, "Indestructible,  ");
	}
	if (x->_itype == ITYPE_MISC)
		sstr[0] = '\0';
	str = x->_iMinStr;
	mag = x->_iMinMag;
	dex = x->_iMinDex;
	if (str == 0 && mag == 0 && dex == 0) {
		strcat(sstr, "No required attributes");
	} else {
		strcpy(tempstr, "Required:");
		if (str)
			sprintf(tempstr + strlen(tempstr), " %i Str", str);
		if (mag)
			sprintf(tempstr + strlen(tempstr), " %i Mag", mag);
		if (dex)
			sprintf(tempstr + strlen(tempstr), " %i Dex", dex);
		strcat(sstr, tempstr);
	}
	AddSText(40, l++, false, sstr, iclr, false);
	if (x->_iMagical == ITEM_QUALITY_UNIQUE) {
		if (x->_iIdentified)
			AddSText(40, l, false, "Unique Item", iclr, false);
	}
}

void StoreAutoPlace()
{
	bool done = false;
	if (AutoEquipEnabled(plr[myplr], plr[myplr].HoldItem) && AutoEquip(myplr, plr[myplr].HoldItem)) {
		done = true;
	}

	if (!done) {
		AutoPlaceItemInBelt(myplr, plr[myplr].HoldItem, true) || AutoPlaceItemInInventory(myplr, plr[myplr].HoldItem, true);
	}
}

void S_Scroll(int idx)
{
	ClearSText(5, 21);
	stextup = 5;

	for (int l = 5; l < 20; l += 4) {
		if (idx >= storenumh)
			break;
		if (!storeItem[idx].isEmpty()) {
			text_color iclr = COL_WHITE;
			if (storeItem[idx]._iMagical)
				iclr = COL_BLUE;

			if (!storeItem[idx]._iStatFlag)
				iclr = COL_RED;

			if (storeItem[idx]._iMagical && storeItem[idx]._iIdentified) {
				AddSText(20, l, false, storeItem[idx]._iIName, iclr, true);
				AddSTextVal(l, storeItem[idx]._iIvalue);
			} else {
				AddSText(20, l, false, storeItem[idx]._iName, iclr, true);
				AddSTextVal(l, storeItem[idx]._ivalue);
			}

			PrintStoreItem(&storeItem[idx], l + 1, iclr);
			stextdown = l;
		}
		idx++;
	}

	stextsmax = storenumh - 4;
	if (stextsmax < 0)
		stextsmax = 0;
}

/**
 * @brief Purchases an item from witch, smith or healer.
 */
void S_StartBuy()
{
	storenumh = 0;
	for (int i = 0; !storeItem[i].isEmpty() && i < storeItemCount; i++)
		storenumh++;

	stextsize = true;
	stextscrl = true;
	stextsval = 0;
	//sprintf(tempstr, "I have these items for sale:             Your gold: %i", plr[myplr]._pGold);
	AddSText(0, 1, true, tempstr, COL_GOLD, false);
	AddSLine(3);
	AddSLine(21);
	S_Scroll(stextsval);
	AddSText(0, 22, true, "Back", COL_WHITE, false);
	OffsetSTextY(22, 6);
}

void S_BuyEnter()
{
	int idx, i;
	bool done;

	if (stextsel == 22) {
		StartStore(activeStore);
		stextsel = 12;
	} else {
		stextlhold = stextsel;
		stextvhold = stextsval;
		stextshold = buyStore;
		idx = stextsval + ((stextsel - stextup) >> 2);
		if (plr[myplr]._pGold < storeItem[idx]._iIvalue) {
			StartStore(STORE_NOMONEY);
		} else {
			plr[myplr].HoldItem = storeItem[idx];
			NewCursor(plr[myplr].HoldItem._iCurs + CURSOR_FIRSTITEM);
			done = false;
			if (AutoEquipEnabled(plr[myplr], plr[myplr].HoldItem) && AutoEquip(myplr, plr[myplr].HoldItem, false)) {
				done = true;
			}

			if (done || AutoPlaceItemInInventory(myplr, plr[myplr].HoldItem, false))
				StartStore(STORE_CONFIRM);
			else
				StartStore(STORE_NOROOM);
			NewCursor(CURSOR_HAND);
		}
	}
}

void BuyItem()
{
	int idx;

	TakePlrsMoney(plr[myplr].HoldItem._iIvalue);
	if (plr[myplr].HoldItem._iMagical == ITEM_QUALITY_NORMAL)
		plr[myplr].HoldItem._iIdentified = false;
	StoreAutoPlace();

	idx = stextvhold + ((stextlhold - stextup) >> 2);

	if (buyStore == STORE_SPBUY){
        int xx = 0;
        for (int i = 0; idx >= 0; i++) {
            if (!premiumitem[i].isEmpty()) {
                idx--;
                xx = i;
            }
        }

        premiumitem[xx]._itype = ITYPE_NONE;
        numpremium--;
        SpawnPremium(myplr);
	}else{
        if (idx == storeItemCount - 1) {
            storeItem[storeItemCount - 1]._itype = ITYPE_NONE;
        } else {
            for (; !storeItem[idx + 1].isEmpty(); idx++) {
                storeItem[idx] = storeItem[idx + 1];
            }
            storeItem[idx]._itype = ITYPE_NONE;
        }
	}

	CalcPlrInv(myplr, true);
}

void S_StartSmith()
{
	stextsize = false;
	stextscrl = false;
	AddSText(0, 1, true, "Welcome to the", COL_GOLD, false);
	AddSText(0, 3, true, "Blacksmith's shop", COL_GOLD, false);
	AddSText(0, 7, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 10, true, "Talk to Griswold", COL_BLUE, true);
	AddSText(0, 12, true, "Buy basic items", COL_WHITE, true);
	AddSText(0, 14, true, "Buy premium items", COL_WHITE, true);
	AddSText(0, 16, true, "Sell items", COL_WHITE, true);
	AddSText(0, 18, true, "Repair items", COL_WHITE, true);
	AddSText(0, 20, true, "Leave the shop", COL_WHITE, true);
	AddSLine(5);
	storenumh = 20;
	activeStore = STORE_SMITH;
}

bool SellOk(int i)
{
	ItemStruct *pI;

	if (i >= 0) {
		pI = &plr[myplr].InvList[i];
	} else {
		pI = &plr[myplr].SpdList[-(i + 1)];
	}

    if (pI->isEmpty())
        return false;
    if (pI->_itype == ITYPE_GOLD)
        return false;
    if (pI->_iClass == ICLASS_QUEST)
        return false;
    if (pI->IDidx == IDI_LAZSTAFF)
        return false;

	if (activeStore == STORE_SMITH){
        if (pI->_iMiscId > IMISC_OILFIRST && pI->_iMiscId < IMISC_OILLAST)
            return true;
        if (pI->_itype == ITYPE_MISC)
            return false;
        if (pI->_itype == ITYPE_STAFF && (!gbIsHellfire || pI->_iSpell != SPL_NULL))
            return false;
    }

	if (activeStore == STORE_WITCH){
        bool rv = false;
        if (pI->_itype == ITYPE_MISC)
            rv = true;
        if (pI->_iMiscId > 29 && pI->_iMiscId < 41)
            rv = false;
        if (pI->_itype == ITYPE_STAFF && (!gbIsHellfire || pI->_iSpell != SPL_NULL))
            rv = true;
        if (pI->IDidx >= IDI_FIRSTQUEST && pI->IDidx <= IDI_LASTQUEST)
            rv = false;
        return rv;
	}

    return true;
}

void S_StartSell()
{
	int i;
	bool sellok;

	stextsize = true;
	sellok = false;
	storenumh = 0;

	for (i = 0; i < 48; i++)
		storehold[i]._itype = ITYPE_NONE;

	for (i = 0; i < plr[myplr]._pNumInv; i++) {
		if (storenumh >= 48)
			break;
		if (SellOk(i)) {
			sellok = true;
			storehold[storenumh] = plr[myplr].InvList[i];

			if (storehold[storenumh]._iMagical != ITEM_QUALITY_NORMAL && storehold[storenumh]._iIdentified)
				storehold[storenumh]._ivalue = storehold[storenumh]._iIvalue;

			if ((storehold[storenumh]._ivalue >>= 2) == 0)
				storehold[storenumh]._ivalue = 1;

			storehold[storenumh]._iIvalue = storehold[storenumh]._ivalue;
			storehidx[storenumh++] = i;
		}
	}

	for (i = 0; i < MAXBELTITEMS; i++) {
		if (storenumh >= 48)
			break;
		if (SellOk(-(i + 1))) {
			storehold[storenumh] = plr[myplr].SpdList[i];
			sellok = true;

			if (storehold[storenumh]._iMagical != ITEM_QUALITY_NORMAL && storehold[storenumh]._iIdentified)
				storehold[storenumh]._ivalue = storehold[storenumh]._iIvalue;

			if (!(storehold[storenumh]._ivalue >>= 2))
				storehold[storenumh]._ivalue = 1;

			storehold[storenumh]._iIvalue = storehold[storenumh]._ivalue;
			storehidx[storenumh++] = -(i + 1);
		}
	}

	if (!sellok) {
		stextscrl = false;
		sprintf(tempstr, "You have nothing I want.             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
	} else {
		stextscrl = true;
		stextsval = 0;
		stextsmax = plr[myplr]._pNumInv;
		sprintf(tempstr, "Which item is for sale?             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		S_Scroll(stextsval);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
	}
}

bool SmithRepairOk(int i)
{
	if (plr[myplr].InvList[i].isEmpty())
		return false;
	if (plr[myplr].InvList[i]._itype == ITYPE_MISC)
		return false;
	if (plr[myplr].InvList[i]._itype == ITYPE_GOLD)
		return false;
	if (plr[myplr].InvList[i]._iDurability == plr[myplr].InvList[i]._iMaxDur)
		return false;

	return true;
}

void S_StartSRepair()
{
	bool repairok;
	int i;

	stextsize = true;
	repairok = false;
	storenumh = 0;
	for (i = 0; i < 48; i++)
		storehold[i]._itype = ITYPE_NONE;
	if (!plr[myplr].InvBody[INVLOC_HEAD].isEmpty() && plr[myplr].InvBody[INVLOC_HEAD]._iDurability != plr[myplr].InvBody[INVLOC_HEAD]._iMaxDur) {
		repairok = true;
		AddStoreHoldRepair(plr[myplr].InvBody, -1);
	}
	if (!plr[myplr].InvBody[INVLOC_CHEST].isEmpty() && plr[myplr].InvBody[INVLOC_CHEST]._iDurability != plr[myplr].InvBody[INVLOC_CHEST]._iMaxDur) {
		repairok = true;
		AddStoreHoldRepair(&plr[myplr].InvBody[INVLOC_CHEST], -2);
	}
	if (!plr[myplr].InvBody[INVLOC_HAND_LEFT].isEmpty() && plr[myplr].InvBody[INVLOC_HAND_LEFT]._iDurability != plr[myplr].InvBody[INVLOC_HAND_LEFT]._iMaxDur) {
		repairok = true;
		AddStoreHoldRepair(&plr[myplr].InvBody[INVLOC_HAND_LEFT], -3);
	}
	if (!plr[myplr].InvBody[INVLOC_HAND_RIGHT].isEmpty() && plr[myplr].InvBody[INVLOC_HAND_RIGHT]._iDurability != plr[myplr].InvBody[INVLOC_HAND_RIGHT]._iMaxDur) {
		repairok = true;
		AddStoreHoldRepair(&plr[myplr].InvBody[INVLOC_HAND_RIGHT], -4);
	}
	for (i = 0; i < plr[myplr]._pNumInv; i++) {
		if (storenumh >= 48)
			break;
		if (SmithRepairOk(i)) {
			repairok = true;
			AddStoreHoldRepair(&plr[myplr].InvList[i], i);
		}
	}
	if (!repairok) {
		stextscrl = false;
		sprintf(tempstr, "You have nothing to repair.             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
		return;
	}

	stextscrl = true;
	stextsval = 0;
	stextsmax = plr[myplr]._pNumInv;
	sprintf(tempstr, "Repair which item?             Your gold: %i", plr[myplr]._pGold);
	AddSText(0, 1, true, tempstr, COL_GOLD, false);
	AddSLine(3);
	AddSLine(21);
	S_Scroll(stextsval);
	AddSText(0, 22, true, "Back", COL_WHITE, true);
	OffsetSTextY(22, 6);
}

void FillManaPlayer()
{
	if (!sgOptions.Gameplay.bAdriaRefillsMana)
		return;
	if (plr[myplr]._pMana != plr[myplr]._pMaxMana) {
		PlaySFX(IS_CAST8);
	}
	plr[myplr]._pMana = plr[myplr]._pMaxMana;
	plr[myplr]._pManaBase = plr[myplr]._pMaxManaBase;
	drawmanaflag = true;
}

void S_StartWitch()
{
	FillManaPlayer();
	stextsize = false;
	stextscrl = false;
	AddSText(0, 2, true, "Witch's shack", COL_GOLD, false);
	AddSText(0, 9, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 12, true, "Talk to Adria", COL_BLUE, true);
	AddSText(0, 14, true, "Buy items", COL_WHITE, true);
	AddSText(0, 16, true, "Sell items", COL_WHITE, true);
	AddSText(0, 18, true, "Recharge staves", COL_WHITE, true);
	AddSText(0, 20, true, "Leave the shack", COL_WHITE, true);
	AddSLine(5);
	storenumh = 20;
	activeStore = STORE_WITCH;
}

bool WitchRechargeOk(int i)
{
	bool rv;

	rv = false;
	if (plr[myplr].InvList[i]._itype == ITYPE_STAFF
	    && plr[myplr].InvList[i]._iCharges != plr[myplr].InvList[i]._iMaxCharges) {
		rv = true;
	}
	if ((plr[myplr].InvList[i]._iMiscId == IMISC_UNIQUE || plr[myplr].InvList[i]._iMiscId == IMISC_STAFF)
	    && plr[myplr].InvList[i]._iCharges < plr[myplr].InvList[i]._iMaxCharges) {
		rv = true;
	}
	return rv;
}

void AddStoreHoldRecharge(ItemStruct itm, int i)
{
	storehold[storenumh] = itm;
	storehold[storenumh]._ivalue += spelldata[itm._iSpell].sStaffCost;
	storehold[storenumh]._ivalue = storehold[storenumh]._ivalue * (storehold[storenumh]._iMaxCharges - storehold[storenumh]._iCharges) / (storehold[storenumh]._iMaxCharges * 2);
	storehold[storenumh]._iIvalue = storehold[storenumh]._ivalue;
	storehidx[storenumh] = i;
	storenumh++;
}

void S_StartWRecharge()
{
	int i;
	bool rechargeok;

	stextsize = true;
	rechargeok = false;
	storenumh = 0;

	for (i = 0; i < 48; i++) {
		storehold[i]._itype = ITYPE_NONE;
	}

	if ((plr[myplr].InvBody[INVLOC_HAND_LEFT]._itype == ITYPE_STAFF || plr[myplr].InvBody[INVLOC_HAND_LEFT]._iMiscId == IMISC_UNIQUE)
	    && plr[myplr].InvBody[INVLOC_HAND_LEFT]._iCharges != plr[myplr].InvBody[INVLOC_HAND_LEFT]._iMaxCharges) {
		rechargeok = true;
		AddStoreHoldRecharge(plr[myplr].InvBody[INVLOC_HAND_LEFT], -1);
	}

	for (i = 0; i < plr[myplr]._pNumInv; i++) {
		if (storenumh >= 48)
			break;
		if (WitchRechargeOk(i)) {
			rechargeok = true;
			AddStoreHoldRecharge(plr[myplr].InvList[i], i);
		}
	}

	if (!rechargeok) {
		stextscrl = false;
		sprintf(tempstr, "You have nothing to recharge.             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
	} else {
		stextscrl = true;
		stextsval = 0;
		stextsmax = plr[myplr]._pNumInv;
		sprintf(tempstr, "Recharge which item?             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		S_Scroll(stextsval);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
	}
}

void S_StartNoMoney()
{
	StartStore(stextshold);
	stextscrl = false;
	stextsize = true;
	ClearSText(5, 23);
	AddSText(0, 14, true, "You do not have enough gold", COL_WHITE, true);
}

void S_StartNoRoom()
{
	StartStore(stextshold);
	stextscrl = false;
	ClearSText(5, 23);
	AddSText(0, 14, true, "You do not have enough room in inventory", COL_WHITE, true);
}

void S_StartConfirm()
{
	bool idprint;

	StartStore(stextshold);
	stextscrl = false;
	ClearSText(5, 23);
	text_color iclr = COL_WHITE;

	if (plr[myplr].HoldItem._iMagical != ITEM_QUALITY_NORMAL)
		iclr = COL_BLUE;
	if (!plr[myplr].HoldItem._iStatFlag)
		iclr = COL_RED;

	idprint = plr[myplr].HoldItem._iMagical != ITEM_QUALITY_NORMAL;

	if (stextshold == STORE_SIDENTIFY)
		idprint = false;
	if (plr[myplr].HoldItem._iMagical != ITEM_QUALITY_NORMAL && !plr[myplr].HoldItem._iIdentified) {
		if (stextshold == STORE_SSELL)
			idprint = false;
		if (stextshold == STORE_WSELL)
			idprint = false;
		if (stextshold == STORE_SREPAIR)
			idprint = false;
		if (stextshold == STORE_WRECHARGE)
			idprint = false;
	}
	if (idprint)
		AddSText(20, 8, false, plr[myplr].HoldItem._iIName, iclr, false);
	else
		AddSText(20, 8, false, plr[myplr].HoldItem._iName, iclr, false);

	AddSTextVal(8, plr[myplr].HoldItem._iIvalue);
	PrintStoreItem(&plr[myplr].HoldItem, 9, iclr);

	switch (stextshold) {
	case STORE_BBOY:
		strcpy(tempstr, "Do we have a deal?");
		break;
	case STORE_SIDENTIFY:
		strcpy(tempstr, "Are you sure you want to identify this item?");
		break;
	case STORE_HBUY:
	case STORE_SPBUY:
	case STORE_WBUY:
	case STORE_SBUY:
		strcpy(tempstr, "Are you sure you want to buy this item?");
		break;
	case STORE_WRECHARGE:
		strcpy(tempstr, "Are you sure you want to recharge this item?");
		break;
	case STORE_SSELL:
	case STORE_WSELL:
		strcpy(tempstr, "Are you sure you want to sell this item?");
		break;
	case STORE_SREPAIR:
		strcpy(tempstr, "Are you sure you want to repair this item?");
		break;
	default:
		app_fatal("Unknown store dialog %d", stextshold);
	}
	AddSText(0, 15, true, tempstr, COL_WHITE, false);
	AddSText(0, 18, true, "Yes", COL_WHITE, true);
	AddSText(0, 20, true, "No", COL_WHITE, true);
}

void S_StartBoy()
{
	stextsize = false;
	stextscrl = false;
	AddSText(0, 2, true, "Wirt the Peg-legged boy", COL_GOLD, false);
	AddSLine(5);
	if (!boyitem.isEmpty()) {
		AddSText(0, 8, true, "Talk to Wirt", COL_BLUE, true);
		AddSText(0, 12, true, "I have something for sale,", COL_GOLD, false);
		AddSText(0, 14, true, "but it will cost 50 gold", COL_GOLD, false);
		AddSText(0, 16, true, "just to take a look. ", COL_GOLD, false);
		AddSText(0, 18, true, "What have you got?", COL_WHITE, true);
		AddSText(0, 20, true, "Say goodbye", COL_WHITE, true);
	} else {
		AddSText(0, 12, true, "Talk to Wirt", COL_BLUE, true);
		AddSText(0, 18, true, "Say goodbye", COL_WHITE, true);
	}
}

void S_StartBBoy()
{
	stextsize = true;
	stextscrl = false;
	sprintf(tempstr, "I have this item for sale:             Your gold: %i", plr[myplr]._pGold);
	AddSText(0, 1, true, tempstr, COL_GOLD, false);
	AddSLine(3);
	AddSLine(21);
	text_color iclr = COL_WHITE;

	if (boyitem._iMagical != ITEM_QUALITY_NORMAL)
		iclr = COL_BLUE;
	if (!boyitem._iStatFlag)
		iclr = COL_RED;
	if (boyitem._iMagical != ITEM_QUALITY_NORMAL)
		AddSText(20, 10, false, boyitem._iIName, iclr, true);
	else
		AddSText(20, 10, false, boyitem._iName, iclr, true);

	if (gbIsHellfire)
		AddSTextVal(10, boyitem._iIvalue - (boyitem._iIvalue >> 2));
	else
		AddSTextVal(10, boyitem._iIvalue + (boyitem._iIvalue >> 1));
	PrintStoreItem(&boyitem, 11, iclr);
	AddSText(0, 22, true, "Leave", COL_WHITE, true);
	OffsetSTextY(22, 6);
}

void HealPlayer()
{
	if (plr[myplr]._pHitPoints != plr[myplr]._pMaxHP) {
		PlaySFX(IS_CAST8);
	}
	plr[myplr]._pHitPoints = plr[myplr]._pMaxHP;
	plr[myplr]._pHPBase = plr[myplr]._pMaxHPBase;
	drawhpflag = true;
}

void S_StartHealer()
{
	HealPlayer();
	stextsize = false;
	stextscrl = false;
	AddSText(0, 1, true, "Welcome to the", COL_GOLD, false);
	AddSText(0, 3, true, "Healer's home", COL_GOLD, false);
	AddSText(0, 9, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 12, true, "Talk to Pepin", COL_BLUE, true);
	AddSText(0, 14, true, "Buy items", COL_WHITE, true);
	AddSText(0, 16, true, "Leave Healer's home", COL_WHITE, true);
	AddSLine(5);
	storenumh = 20;
	activeStore = STORE_HEALER;
}

void S_StartStory()
{
	stextsize = false;
	stextscrl = false;
	AddSText(0, 2, true, "The Town Elder", COL_GOLD, false);
	AddSText(0, 9, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 12, true, "Talk to Cain", COL_BLUE, true);
	AddSText(0, 14, true, "Identify an item", COL_WHITE, true);
	AddSText(0, 18, true, "Say goodbye", COL_WHITE, true);
	AddSLine(5);
}

bool IdItemOk(ItemStruct *i)
{
	if (i->isEmpty()) {
		return false;
	}
	if (i->_iMagical == ITEM_QUALITY_NORMAL) {
		return false;
	}
	return !i->_iIdentified;
}

void AddStoreHoldId(ItemStruct itm, int i)
{
	storehold[storenumh] = itm;
	storehold[storenumh]._ivalue = 100;
	storehold[storenumh]._iIvalue = 100;
	storehidx[storenumh] = i;
	storenumh++;
}

void S_StartSIdentify()
{
	bool idok;
	int i;

	idok = false;
	stextsize = true;
	storenumh = 0;

	for (i = 0; i < 48; i++)
		storehold[i]._itype = ITYPE_NONE;

	if (IdItemOk(&plr[myplr].InvBody[INVLOC_HEAD])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_HEAD], -1);
	}
	if (IdItemOk(&plr[myplr].InvBody[INVLOC_CHEST])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_CHEST], -2);
	}
	if (IdItemOk(&plr[myplr].InvBody[INVLOC_HAND_LEFT])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_HAND_LEFT], -3);
	}
	if (IdItemOk(&plr[myplr].InvBody[INVLOC_HAND_RIGHT])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_HAND_RIGHT], -4);
	}
	if (IdItemOk(&plr[myplr].InvBody[INVLOC_RING_LEFT])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_RING_LEFT], -5);
	}
	if (IdItemOk(&plr[myplr].InvBody[INVLOC_RING_RIGHT])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_RING_RIGHT], -6);
	}
	if (IdItemOk(&plr[myplr].InvBody[INVLOC_AMULET])) {
		idok = true;
		AddStoreHoldId(plr[myplr].InvBody[INVLOC_AMULET], -7);
	}

	for (i = 0; i < plr[myplr]._pNumInv; i++) {
		if (storenumh >= 48)
			break;
		if (IdItemOk(&plr[myplr].InvList[i])) {
			idok = true;
			AddStoreHoldId(plr[myplr].InvList[i], i);
		}
	}

	if (!idok) {
		stextscrl = false;
		sprintf(tempstr, "You have nothing to identify.             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
	} else {
		stextscrl = true;
		stextsval = 0;
		stextsmax = plr[myplr]._pNumInv;
		sprintf(tempstr, "Identify which item?             Your gold: %i", plr[myplr]._pGold);
		AddSText(0, 1, true, tempstr, COL_GOLD, false);
		AddSLine(3);
		AddSLine(21);
		S_Scroll(stextsval);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		OffsetSTextY(22, 6);
	}
}

void S_StartIdShow()
{
	StartStore(stextshold);
	stextscrl = false;
	ClearSText(5, 23);
	text_color iclr = COL_WHITE;

	if (plr[myplr].HoldItem._iMagical != ITEM_QUALITY_NORMAL)
		iclr = COL_BLUE;
	if (!plr[myplr].HoldItem._iStatFlag)
		iclr = COL_RED;

	AddSText(0, 7, true, "This item is:", COL_WHITE, false);
	AddSText(20, 11, false, plr[myplr].HoldItem._iIName, iclr, false);
	PrintStoreItem(&plr[myplr].HoldItem, 12, iclr);
	AddSText(0, 18, true, "Done", COL_WHITE, true);
}

void S_StartTalk()
{
	int i, sn, sn2, la;

	stextsize = false;
	stextscrl = false;
	sprintf(tempstr, "Talk to %s", talkname[talker]);
	AddSText(0, 2, true, tempstr, COL_GOLD, false);
	AddSLine(5);
	if (gbIsSpawn) {
		sprintf(tempstr, "Talking to %s", talkname[talker]);
		AddSText(0, 10, true, tempstr, COL_WHITE, false);
		AddSText(0, 12, true, "is not available", COL_WHITE, false);
		AddSText(0, 14, true, "in the shareware", COL_WHITE, false);
		AddSText(0, 16, true, "version", COL_WHITE, false);
		AddSText(0, 22, true, "Back", COL_WHITE, true);
		return;
	}

	sn = 0;
	for (i = 0; i < MAXQUESTS; i++) {
		if (quests[i]._qactive == QUEST_ACTIVE && Qtalklist[talker][i] != TEXT_NONE && quests[i]._qlog)
			sn++;
	}

	if (sn > 6) {
		sn = 14 - (sn >> 1);
		la = 1;
	} else {
		sn = 15 - sn;
		la = 2;
	}

	sn2 = sn - 2;

	for (i = 0; i < MAXQUESTS; i++) {
		if (quests[i]._qactive == QUEST_ACTIVE && Qtalklist[talker][i] != TEXT_NONE && quests[i]._qlog) {
			AddSText(0, sn, true, questlist[i]._qlstr, COL_WHITE, true);
			sn += la;
		}
	}
	AddSText(0, sn2, true, "Gossip", COL_BLUE, true);
	AddSText(0, 22, true, "Back", COL_WHITE, true);
}

void S_StartTavern()
{
	stextsize = false;
	stextscrl = false;
	AddSText(0, 1, true, "Welcome to the", COL_GOLD, false);
	AddSText(0, 3, true, "Rising Sun", COL_GOLD, false);
	AddSText(0, 9, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 12, true, "Talk to Ogden", COL_BLUE, true);
	AddSText(0, 18, true, "Leave the tavern", COL_WHITE, true);
	AddSLine(5);
	storenumh = 20;
}

void S_StartBarMaid()
{
	stextsize = false;
	stextscrl = false;
	AddSText(0, 2, true, "Gillian", COL_GOLD, false);
	AddSText(0, 9, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 12, true, "Talk to Gillian", COL_BLUE, true);
	AddSText(0, 18, true, "Say goodbye", COL_WHITE, true);
	AddSLine(5);
	storenumh = 20;
}

void S_StartDrunk()
{
	stextsize = false;
	stextscrl = false;
	AddSText(0, 2, true, "Farnham the Drunk", COL_GOLD, false);
	AddSText(0, 9, true, "Would you like to:", COL_GOLD, false);
	AddSText(0, 12, true, "Talk to Farnham", COL_BLUE, true);
	AddSText(0, 18, true, "Say Goodbye", COL_WHITE, true);
	AddSLine(5);
	storenumh = 20;
}

void S_SmithEnter()
{
	switch (stextsel) {
	case 10:
		talker = TOWN_SMITH;
		stextlhold = 10;
		stextshold = STORE_SMITH;
		gossipstart = TEXT_GRISWOLD2;
		gossipend = TEXT_GRISWOLD13;
		StartStore(STORE_GOSSIP);
		break;
	case 12:
        buyStore = STORE_SBUY;
        storeItem = smithitem;
        storeItemCount = SMITH_ITEMS;
        sprintf(tempstr, "I have these items for sale:             Your gold: %i", plr[myplr]._pGold);
		StartStore(STORE_SBUY);
		break;
	case 14:
        buyStore = STORE_SPBUY;
        storeItem = premiumitem;
        storeItemCount = numpremium;
		sprintf(tempstr, "I have these premium items for sale:             Your gold: %i", plr[myplr]._pGold);
		StartStore(STORE_SPBUY);
		break;
	case 16:
        sellStore = STORE_SSELL;
        storeItem = storehold;
        storeItemCount = sizeof(storehold)/sizeof(storehold[48]); //48 but better not to hard code in two places.
        StartStore(STORE_SSELL);
		break;
	case 18:
        storeItem = storehold;
        storeItemCount = sizeof(storehold)/sizeof(storehold[48]); //48 but better not to hard code in two places.
        StartStore(STORE_SREPAIR);
		break;
	case 20:
		stextflag = STORE_NONE;
		break;
	}
}

bool StoreGoldFit(int idx)
{
	int i, sz, cost, numsqrs;

	cost = storehold[idx]._iIvalue;
	sz = cost / MaxGold;
	if (cost % MaxGold != 0)
		sz++;

	NewCursor(storehold[idx]._iCurs + CURSOR_FIRSTITEM);
	numsqrs = cursW / 28 * (cursH / 28);
	NewCursor(CURSOR_HAND);

	if (numsqrs >= sz)
		return true;

	for (i = 0; i < NUM_INV_GRID_ELEM; i++) {
		if (plr[myplr].InvGrid[i] == 0)
			numsqrs++;
	}

	for (i = 0; i < plr[myplr]._pNumInv; i++) {
		if (plr[myplr].InvList[i]._itype == ITYPE_GOLD && plr[myplr].InvList[i]._ivalue != MaxGold) {
			if (cost + plr[myplr].InvList[i]._ivalue <= MaxGold)
				cost = 0;
			else
				cost -= MaxGold - plr[myplr].InvList[i]._ivalue;
		}
	}

	sz = cost / MaxGold;
	if (cost % MaxGold)
		sz++;

	return numsqrs >= sz;
}

/**
 * @brief Add gold pile to the players invetory
 * @param v The value of the gold pile
 */
void PlaceStoreGold(int v)
{
	bool done;
	int ii, xx, yy, i;

	done = false;

	for (i = 0; i < NUM_INV_GRID_ELEM && !done; i++) {
		yy = 10 * (i / 10);
		xx = i % 10;
		if (plr[myplr].InvGrid[xx + yy] == 0) {
			ii = plr[myplr]._pNumInv;
			GetGoldSeed(myplr, &golditem);
			plr[myplr].InvList[ii] = golditem;
			plr[myplr]._pNumInv++;
			plr[myplr].InvGrid[xx + yy] = plr[myplr]._pNumInv;
			plr[myplr].InvList[ii]._ivalue = v;
			SetGoldCurs(myplr, ii);
			done = true;
		}
	}
}

/**
 * @brief Sells an item from the player's inventory or belt.
 */
void StoreSellItem()
{
	int i, idx, cost;

	idx = stextvhold + ((stextlhold - stextup) >> 2);
	if (storehidx[idx] >= 0)
		RemoveInvItem(myplr, storehidx[idx]);
	else
		RemoveSpdBarItem(myplr, -(storehidx[idx] + 1));
	cost = storehold[idx]._iIvalue;
	storenumh--;
	if (idx != storenumh) {
		while (idx < storenumh) {
			storehold[idx] = storehold[idx + 1];
			storehidx[idx] = storehidx[idx + 1];
			idx++;
		}
	}
	plr[myplr]._pGold += cost;
	for (i = 0; i < plr[myplr]._pNumInv && cost > 0; i++) {
		if (plr[myplr].InvList[i]._itype == ITYPE_GOLD && plr[myplr].InvList[i]._ivalue != MaxGold) {
			if (cost + plr[myplr].InvList[i]._ivalue <= MaxGold) {
				plr[myplr].InvList[i]._ivalue += cost;
				SetGoldCurs(myplr, i);
				cost = 0;
			} else {
				cost -= MaxGold - plr[myplr].InvList[i]._ivalue;
				plr[myplr].InvList[i]._ivalue = MaxGold;
				SetGoldCurs(myplr, i);
			}
		}
	}
	if (cost > 0) {
		while (cost > MaxGold) {
			PlaceStoreGold(MaxGold);
			cost -= MaxGold;
		}
		PlaceStoreGold(cost);
	}
}

void S_SellEnter()
{
	int idx;

	if (stextsel == 22) {
		StartStore(activeStore);
		stextsel = 16;
	} else {
		stextlhold = stextsel;
		idx = stextsval + ((stextsel - stextup) >> 2);
		stextshold = sellStore;
		stextvhold = stextsval;
		plr[myplr].HoldItem = storehold[idx];

		if (StoreGoldFit(idx))
			StartStore(STORE_CONFIRM);
		else
			StartStore(STORE_NOROOM);
	}
}

/**
 * @brief Repairs an item in the player's inventory or body in the smith.
 */
void SmithRepairItem()
{
	int i, idx;

	TakePlrsMoney(plr[myplr].HoldItem._iIvalue);

	idx = stextvhold + ((stextlhold - stextup) >> 2);
	storehold[idx]._iDurability = storehold[idx]._iMaxDur;

	i = storehidx[idx];
	if (i < 0) {
		if (i == -1)
			plr[myplr].InvBody[INVLOC_HEAD]._iDurability = plr[myplr].InvBody[INVLOC_HEAD]._iMaxDur;
		if (i == -2)
			plr[myplr].InvBody[INVLOC_CHEST]._iDurability = plr[myplr].InvBody[INVLOC_CHEST]._iMaxDur;
		if (i == -3)
			plr[myplr].InvBody[INVLOC_HAND_LEFT]._iDurability = plr[myplr].InvBody[INVLOC_HAND_LEFT]._iMaxDur;
		if (i == -4)
			plr[myplr].InvBody[INVLOC_HAND_RIGHT]._iDurability = plr[myplr].InvBody[INVLOC_HAND_RIGHT]._iMaxDur;
	} else {
		plr[myplr].InvList[i]._iDurability = plr[myplr].InvList[i]._iMaxDur;
	}
}

void S_SRepairEnter()
{
	int idx;

	if (stextsel == 22) {
		StartStore(STORE_SMITH);
		stextsel = 18;
	} else {
		stextshold = STORE_SREPAIR;
		stextlhold = stextsel;
		stextvhold = stextsval;
		idx = stextsval + ((stextsel - stextup) >> 2);
		plr[myplr].HoldItem = storehold[idx];
		if (plr[myplr]._pGold < storehold[idx]._iIvalue)
			StartStore(STORE_NOMONEY);
		else
			StartStore(STORE_CONFIRM);
	}
}

void S_WitchEnter()
{
	switch (stextsel) {
	case 12:
		stextlhold = 12;
		talker = TOWN_WITCH;
		stextshold = STORE_WITCH;
		gossipstart = TEXT_ADRIA2;
		gossipend = TEXT_ADRIA13;
		StartStore(STORE_GOSSIP);
		return;
	case 14:
        buyStore = STORE_WBUY;
        storeItem = witchitem;
        storeItemCount = WITCH_ITEMS;
        sprintf(tempstr, "I have these items for sale:             Your gold: %i", plr[myplr]._pGold);
		StartStore(STORE_WBUY);
		return;
	case 16:
		sellStore = STORE_WSELL;
		storeItem = storehold;
        storeItemCount = sizeof(storehold)/sizeof(storehold[48]); //48 but better not to hard code in two places.
		StartStore(STORE_WSELL);
		return;
	case 18:
        storeItem = storehold;
        storeItemCount = sizeof(storehold)/sizeof(storehold[48]); //48 but better not to hard code in two places.
		StartStore(STORE_WRECHARGE);
		return;
	case 20:
		stextflag = STORE_NONE;
		break;
	}
}

/**
 * @brief Recharges an item in the player's inventory or body in the witch.
 */
void WitchRechargeItem()
{
	int i, idx;

	TakePlrsMoney(plr[myplr].HoldItem._iIvalue);

	idx = stextvhold + ((stextlhold - stextup) >> 2);
	storehold[idx]._iCharges = storehold[idx]._iMaxCharges;

	i = storehidx[idx];
	if (i < 0)
		plr[myplr].InvBody[INVLOC_HAND_LEFT]._iCharges = plr[myplr].InvBody[INVLOC_HAND_LEFT]._iMaxCharges;
	else
		plr[myplr].InvList[i]._iCharges = plr[myplr].InvList[i]._iMaxCharges;

	CalcPlrInv(myplr, true);
}

void S_WRechargeEnter()
{
	int idx;

	if (stextsel == 22) {
		StartStore(STORE_WITCH);
		stextsel = 18;
	} else {
		stextshold = STORE_WRECHARGE;
		stextlhold = stextsel;
		stextvhold = stextsval;
		idx = stextsval + ((stextsel - stextup) >> 2);
		plr[myplr].HoldItem = storehold[idx];
		if (plr[myplr]._pGold < storehold[idx]._iIvalue)
			StartStore(STORE_NOMONEY);
		else
			StartStore(STORE_CONFIRM);
	}
}

void S_BoyEnter()
{
	if (!boyitem.isEmpty() && stextsel == 18) {
		if (plr[myplr]._pGold < 50) {
			stextshold = STORE_BOY;
			stextlhold = 18;
			stextvhold = stextsval;
			StartStore(STORE_NOMONEY);
		} else {
			TakePlrsMoney(50);
			StartStore(STORE_BBOY);
		}
	} else if ((stextsel == 8 && !boyitem.isEmpty()) || (stextsel == 12 && boyitem.isEmpty())) {
		talker = TOWN_PEGBOY;
		stextshold = STORE_BOY;
		stextlhold = stextsel;
		gossipstart = TEXT_WIRT2;
		gossipend = TEXT_WIRT12;
		StartStore(STORE_GOSSIP);
	} else {
		stextflag = STORE_NONE;
	}
}

void BoyBuyItem()
{
	TakePlrsMoney(plr[myplr].HoldItem._iIvalue);
	StoreAutoPlace();
	boyitem._itype = ITYPE_NONE;
	stextshold = STORE_BOY;
	CalcPlrInv(myplr, true);
	stextlhold = 12;
}

void S_BBuyEnter()
{
	if (stextsel != 10) {
		stextflag = STORE_NONE;
		return;
	}

	stextshold = STORE_BBOY;
	stextvhold = stextsval;
	stextlhold = 10;
	int price = boyitem._iIvalue;
	if (gbIsHellfire)
		price -= boyitem._iIvalue >> 2;
	else
		price += boyitem._iIvalue >> 1;

	if (plr[myplr]._pGold < price) {
		StartStore(STORE_NOMONEY);
		return;
	}

	plr[myplr].HoldItem = boyitem;
	plr[myplr].HoldItem._iIvalue = price;
	NewCursor(plr[myplr].HoldItem._iCurs + CURSOR_FIRSTITEM);

	bool done = false;
	if (AutoEquipEnabled(plr[myplr], plr[myplr].HoldItem) && AutoEquip(myplr, plr[myplr].HoldItem, false)) {
		done = true;
	}

	if (!done) {
		done = AutoPlaceItemInInventory(myplr, plr[myplr].HoldItem, false);
	}

	StartStore(done ? STORE_CONFIRM : STORE_NOROOM);

	NewCursor(CURSOR_HAND);
}

void StoryIdItem()
{
	int idx;

	idx = storehidx[((stextlhold - stextup) >> 2) + stextvhold];
	if (idx < 0) {
		if (idx == -1)
			plr[myplr].InvBody[INVLOC_HEAD]._iIdentified = true;
		if (idx == -2)
			plr[myplr].InvBody[INVLOC_CHEST]._iIdentified = true;
		if (idx == -3)
			plr[myplr].InvBody[INVLOC_HAND_LEFT]._iIdentified = true;
		if (idx == -4)
			plr[myplr].InvBody[INVLOC_HAND_RIGHT]._iIdentified = true;
		if (idx == -5)
			plr[myplr].InvBody[INVLOC_RING_LEFT]._iIdentified = true;
		if (idx == -6)
			plr[myplr].InvBody[INVLOC_RING_RIGHT]._iIdentified = true;
		if (idx == -7)
			plr[myplr].InvBody[INVLOC_AMULET]._iIdentified = true;
	} else {
		plr[myplr].InvList[idx]._iIdentified = true;
	}
	plr[myplr].HoldItem._iIdentified = true;
	TakePlrsMoney(plr[myplr].HoldItem._iIvalue);
	CalcPlrInv(myplr, true);
}

void S_ConfirmEnter()
{
	if (stextsel == 18) {
		switch (stextshold) {
		case STORE_SBUY:
		case STORE_WBUY:
		case STORE_HBUY:
		case STORE_SPBUY:
			BuyItem();
			break;
		case STORE_SSELL:
		case STORE_WSELL:
			StoreSellItem();
			break;
		case STORE_SREPAIR:
			SmithRepairItem();
			break;
		case STORE_WRECHARGE:
			WitchRechargeItem();
			break;
		case STORE_BBOY:
			BoyBuyItem();
			break;
		case STORE_SIDENTIFY:
			StoryIdItem();
			StartStore(STORE_IDSHOW);
			return;
		default:
			break;
		}
	}

	StartStore(stextshold);

	if (stextsel == 22)
		return;

	stextsel = stextlhold;
	stextsval = std::min(stextvhold, stextsmax);

	while (stextsel != -1 && !stext[stextsel]._ssel) {
		stextsel--;
	}
}

void S_HealerEnter()
{
	switch (stextsel) {
	case 12:
		stextlhold = 12;
		talker = TOWN_HEALER;
		stextshold = STORE_HEALER;
		gossipstart = TEXT_PEPIN2;
		gossipend = TEXT_PEPIN11;
		StartStore(STORE_GOSSIP);
		break;
	case 14:
        buyStore = STORE_HBUY;
        storeItem = healitem;
        storeItemCount = sizeof(healitem)/sizeof(healitem[0]); //it's 20 but better not hard code it in 2 places
		sprintf(tempstr, "I have these items for sale:             Your gold: %i", plr[myplr]._pGold);
		StartStore(STORE_HBUY);
		break;
	case 16:
		stextflag = STORE_NONE;
		break;
	}
}

void S_StoryEnter()
{
	switch (stextsel) {
	case 12:
		stextlhold = 12;
		talker = TOWN_STORY;
		stextshold = STORE_STORY;
		gossipstart = TEXT_STORY2;
		gossipend = TEXT_STORY11;
		StartStore(STORE_GOSSIP);
		break;
	case 14:
        storeItem = storehold;
        storeItemCount = sizeof(storehold)/sizeof(storehold[48]); //48 but better not to hard code in two places.
		StartStore(STORE_SIDENTIFY);
		break;
	case 18:
		stextflag = STORE_NONE;
		break;
	}
}

void S_SIDEnter()
{
	int idx;

	if (stextsel == 22) {
		StartStore(STORE_STORY);
		stextsel = 14;
	} else {
		stextshold = STORE_SIDENTIFY;
		stextlhold = stextsel;
		stextvhold = stextsval;
		idx = stextsval + ((stextsel - stextup) >> 2);
		plr[myplr].HoldItem = storehold[idx];
		if (plr[myplr]._pGold < storehold[idx]._iIvalue)
			StartStore(STORE_NOMONEY);
		else
			StartStore(STORE_CONFIRM);
	}
}

void S_TalkEnter()
{
	int i, tq, sn, la;

	if (stextsel == 22) {
		StartStore(stextshold);
		stextsel = stextlhold;
		return;
	}

	sn = 0;
	for (i = 0; i < MAXQUESTS; i++) {
		if (quests[i]._qactive == QUEST_ACTIVE && Qtalklist[talker][i] != TEXT_NONE && quests[i]._qlog)
			sn++;
	}
	if (sn > 6) {
		sn = 14 - (sn >> 1);
		la = 1;
	} else {
		sn = 15 - sn;
		la = 2;
	}

	if (stextsel == sn - 2) {
		SetRndSeed(towner[talker]._tSeed);
		tq = gossipstart + random_(0, gossipend - gossipstart + 1);
		InitQTextMsg(tq);
		return;
	}

	for (i = 0; i < MAXQUESTS; i++) {
		if (quests[i]._qactive == QUEST_ACTIVE && Qtalklist[talker][i] != TEXT_NONE && quests[i]._qlog) {
			if (sn == stextsel) {
				InitQTextMsg(Qtalklist[talker][i]);
			}
			sn += la;
		}
	}
}

void S_TavernEnter()
{
	switch (stextsel) {
	case 12:
		stextlhold = 12;
		talker = TOWN_TAVERN;
		stextshold = STORE_TAVERN;
		gossipstart = TEXT_OGDEN2;
		gossipend = TEXT_OGDEN10;
		StartStore(STORE_GOSSIP);
		break;
	case 18:
		stextflag = STORE_NONE;
		break;
	}
}

void S_BarmaidEnter()
{
	switch (stextsel) {
	case 12:
		stextlhold = 12;
		talker = TOWN_BMAID;
		stextshold = STORE_BARMAID;
		gossipstart = TEXT_GILLIAN2;
		gossipend = TEXT_GILLIAN10;
		StartStore(STORE_GOSSIP);
		break;
	case 18:
		stextflag = STORE_NONE;
		break;
	}
}

void S_DrunkEnter()
{
	switch (stextsel) {
	case 12:
		stextlhold = 12;
		talker = TOWN_DRUNK;
		stextshold = STORE_DRUNK;
		gossipstart = TEXT_FARNHAM2;
		gossipend = TEXT_FARNHAM13;
		StartStore(STORE_GOSSIP);
		break;
	case 18:
		stextflag = STORE_NONE;
		break;
	}
}

} // namespace

ItemStruct golditem;

Uint8 *pSTextBoxCels;
Uint8 *pSPentSpn2Cels;
Uint8 *pSTextSlidCels;

talk_id stextflag;

int storenumh;
char storehidx[48];
ItemStruct storehold[48];

ItemStruct smithitem[SMITH_ITEMS];
int numpremium;
int premiumlevel;
ItemStruct premiumitem[SMITH_PREMIUM_ITEMS];

ItemStruct healitem[20];

ItemStruct witchitem[WITCH_ITEMS];

int boylevel;
ItemStruct boyitem;

void AddStoreHoldRepair(ItemStruct *itm, int i)
{
	ItemStruct *item;
	int v;

	item = &storehold[storenumh];
	storehold[storenumh] = *itm;

	int due = item->_iMaxDur - item->_iDurability;
	if (item->_iMagical != ITEM_QUALITY_NORMAL && item->_iIdentified) {
		v = 30 * item->_iIvalue * due / (item->_iMaxDur * 100 * 2);
		if (v == 0)
			return;
	} else {
		v = item->_ivalue * due / (item->_iMaxDur * 2);
		v = std::max(v, 1);
	}
	item->_iIvalue = v;
	item->_ivalue = v;
	storehidx[storenumh] = i;
	storenumh++;
}

void InitStores()
{
	pSTextBoxCels = LoadFileInMem("Data\\TextBox2.CEL", NULL);
	pSPentSpn2Cels = LoadFileInMem("Data\\PentSpn2.CEL", NULL);
	pSTextSlidCels = LoadFileInMem("Data\\TextSlid.CEL", NULL);
	ClearSText(0, STORE_LINES);
	stextflag = STORE_NONE;
	stextsize = false;
	stextscrl = false;
	numpremium = 0;
	premiumlevel = 1;

	for (int i = 0; i < SMITH_PREMIUM_ITEMS; i++)
		premiumitem[i]._itype = ITYPE_NONE;

	boyitem._itype = ITYPE_NONE;
	boylevel = 0;
}

int PentSpn2Spin()
{
	return (SDL_GetTicks() / 50) % 8 + 1;
}

void SetupTownStores()
{
	int i, l;

	SetRndSeed(glSeedTbl[currlevel] * SDL_GetTicks());
	if (!gbIsMultiplayer) {
		l = 0;
		for (i = 0; i < NUMLEVELS; i++) {
			if (plr[myplr]._pLvlVisited[i])
				l = i;
		}
	} else {
		l = plr[myplr]._pLevel >> 1;
	}
	l += 2;
	if (l < 6)
		l = 6;
	if (l > 16)
		l = 16;
	SpawnStoreGold();
	SpawnSmith(l);
	SpawnWitch(l);
	SpawnHealer(l);
	SpawnBoy(plr[myplr]._pLevel);
	SpawnPremium(myplr);
}

void FreeStoreMem()
{
	MemFreeDbg(pSTextBoxCels);
	MemFreeDbg(pSPentSpn2Cels);
	MemFreeDbg(pSTextSlidCels);
}

void PrintSString(CelOutputBuffer out, int x, int y, bool cjustflag, const char *str, text_color col, int val)
{
	int len, width, sx, sy, i, k, s;
	int xx, yy;
	BYTE c;
	char valstr[32];

	s = y * 12 + stext[y]._syoff;
	if (stextsize)
		xx = PANEL_X + 32;
	else
		xx = PANEL_X + 352;
	sx = xx + x;
	sy = s + 44 + UI_OFFSET_Y;
	len = strlen(str);
	if (stextsize)
		yy = 577;
	else
		yy = 257;
	k = 0;
	if (cjustflag) {
		width = 0;
		for (i = 0; i < len; i++)
			width += fontkern[fontframe[gbFontTransTbl[(BYTE)str[i]]]] + 1;
		if (width < yy)
			k = (yy - width) >> 1;
		sx += k;
	}
	if (stextsel == y) {
		CelDrawTo(out, cjustflag ? xx + x + k - 20 : xx + x - 20, s + 45 + UI_OFFSET_Y, pSPentSpn2Cels, PentSpn2Spin(), 12);
	}
	for (i = 0; i < len; i++) {
		c = fontframe[gbFontTransTbl[(BYTE)str[i]]];
		k += fontkern[c] + 1;
		if (c != 0 && k <= yy) {
			PrintChar(out, sx, sy, c, col);
		}
		sx += fontkern[c] + 1;
	}
	if (!cjustflag && val >= 0) {
		sprintf(valstr, "%i", val);
		sx = PANEL_X + 592 - x;
		len = strlen(valstr);
		for (i = len - 1; i >= 0; i--) {
			c = fontframe[gbFontTransTbl[(BYTE)valstr[i]]];
			sx -= fontkern[c] + 1;
			if (c != 0) {
				PrintChar(out, sx, sy, c, col);
			}
		}
	}
	if (stextsel == y) {
		CelDrawTo(out, cjustflag ? (xx + x + k + 4) : (PANEL_X + 596 - x), s + 45 + UI_OFFSET_Y, pSPentSpn2Cels, PentSpn2Spin(), 12);
	}
}

void DrawSLine(CelOutputBuffer out, int y)
{
	const int sy = y * 12;
	BYTE *src, *dst;
	int width;
	if (stextsize) {
		src = out.at(PANEL_LEFT + 26, 25 + UI_OFFSET_Y);
		dst = out.at(26 + PANEL_X, sy + 38 + UI_OFFSET_Y);
		width = 587; // BUGFIX: should be 587, not 586 (fixed)
	} else {
		src = out.at(PANEL_LEFT + 346, 25 + UI_OFFSET_Y);
		dst = out.at(346 + PANEL_X, sy + 38 + UI_OFFSET_Y);
		width = 267; // BUGFIX: should be 267, not 266 (fixed)
	}

	for (int i = 0; i < 3; i++, src += out.pitch(), dst += out.pitch())
		memcpy(dst, src, width);
}

void DrawSTextHelp()
{
	stextsel = -1;
	stextsize = true;
}

void ClearSText(int s, int e)
{
	int i;

	for (i = s; i < e; i++) {
		stext[i]._sx = 0;
		stext[i]._syoff = 0;
		stext[i]._sstr[0] = 0;
		stext[i]._sjust = false;
		stext[i]._sclr = COL_WHITE;
		stext[i]._sline = 0;
		stext[i]._ssel = false;
		stext[i]._sval = -1;
	}
}

void StartStore(talk_id s)
{
	sbookflag = false;
	invflag = false;
	chrflag = false;
	questlog = false;
	dropGoldFlag = false;
	ClearSText(0, STORE_LINES);
	ReleaseStoreBtn();
	switch (s) {
	case STORE_SMITH:
		S_StartSmith();
		break;
	case STORE_SBUY:
		if (storenumh > 0)
			S_StartBuy();
		else
			S_StartSmith();
		break;
	case STORE_SSELL:
		S_StartSell();
		break;
	case STORE_SREPAIR:
		S_StartSRepair();
		break;
	case STORE_WITCH:
		S_StartWitch();
		break;
	case STORE_WBUY:
		if (storenumh > 0)
			S_StartBuy();
		break;
	case STORE_WSELL:
		S_StartSell();
		break;
	case STORE_WRECHARGE:
		S_StartWRecharge();
		break;
	case STORE_NOMONEY:
		S_StartNoMoney();
		break;
	case STORE_NOROOM:
		S_StartNoRoom();
		break;
	case STORE_CONFIRM:
		S_StartConfirm();
		break;
	case STORE_BOY:
		S_StartBoy();
		break;
	case STORE_BBOY:
		S_StartBBoy();
		break;
	case STORE_HEALER:
		S_StartHealer();
		break;
	case STORE_STORY:
		S_StartStory();
		break;
	case STORE_HBUY:
		if (storenumh > 0)
			S_StartBuy();
		break;
	case STORE_SIDENTIFY:
		S_StartSIdentify();
		break;
	case STORE_SPBUY:
		if (storenumh > 0)
			S_StartBuy();
		break;
	case STORE_GOSSIP:
		S_StartTalk();
		break;
	case STORE_IDSHOW:
		S_StartIdShow();
		break;
	case STORE_TAVERN:
		S_StartTavern();
		break;
	case STORE_DRUNK:
		S_StartDrunk();
		break;
	case STORE_BARMAID:
		S_StartBarMaid();
		break;
	case STORE_NONE:
		break;
	}

	stextsel = -1;
	for (int i = 0; i < STORE_LINES; i++) {
		if (stext[i]._ssel) {
			stextsel = i;
			break;
		}
	}

	stextflag = s;
}

void DrawSText(CelOutputBuffer out)
{
	int i;

	if (!stextsize)
		DrawSTextBack(out);
	else
		DrawQTextBack(out);

	if (stextscrl) {
		switch (stextflag) {
		case STORE_SBUY:
		case STORE_SSELL:
		case STORE_SREPAIR:
		case STORE_WSELL:
		case STORE_WRECHARGE:
		case STORE_SIDENTIFY:
		case STORE_WBUY:
		case STORE_HBUY:
		case STORE_SPBUY:
			S_Scroll(stextsval);
			break;
		default:
			break;
		}
	}

	for (i = 0; i < STORE_LINES; i++) {
		if (stext[i]._sline)
			DrawSLine(out, i);
		if (stext[i]._sstr[0])
			PrintSString(out, stext[i]._sx, i, stext[i]._sjust, stext[i]._sstr, stext[i]._sclr, stext[i]._sval);
	}

	if (stextscrl)
		DrawSSlider(out, 4, 20);
}

void STextESC()
{
	if (qtextflag) {
		qtextflag = false;
		if (leveltype == DTYPE_TOWN)
			stream_stop();
	} else {
		switch (stextflag) {
		case STORE_SMITH:
		case STORE_WITCH:
		case STORE_BOY:
		case STORE_BBOY:
		case STORE_HEALER:
		case STORE_STORY:
		case STORE_TAVERN:
		case STORE_DRUNK:
		case STORE_BARMAID:
			stextflag = STORE_NONE;
			break;
		case STORE_GOSSIP:
			StartStore(stextshold);
			stextsel = stextlhold;
			break;
		case STORE_SBUY:
			StartStore(STORE_SMITH);
			stextsel = 12;
			break;
		case STORE_SPBUY:
			StartStore(STORE_SMITH);
			stextsel = 14;
			break;
		case STORE_SSELL:
			StartStore(STORE_SMITH);
			stextsel = 16;
			break;
		case STORE_SREPAIR:
			StartStore(STORE_SMITH);
			stextsel = 18;
			break;
		case STORE_WBUY:
			StartStore(STORE_WITCH);
			stextsel = 14;
			break;
		case STORE_WSELL:
			StartStore(STORE_WITCH);
			stextsel = 16;
			break;
		case STORE_WRECHARGE:
			StartStore(STORE_WITCH);
			stextsel = 18;
			break;
		case STORE_HBUY:
			StartStore(STORE_HEALER);
			stextsel = 16;
			break;
		case STORE_SIDENTIFY:
			StartStore(STORE_STORY);
			stextsel = 14;
			break;
		case STORE_IDSHOW:
			StartStore(STORE_SIDENTIFY);
			break;
		case STORE_NOMONEY:
		case STORE_NOROOM:
		case STORE_CONFIRM:
			StartStore(stextshold);
			stextsel = stextlhold;
			stextsval = stextvhold;
			break;
		case STORE_NONE:
			break;
		}
	}
}

void STextUp()
{
	PlaySFX(IS_TITLEMOV);
	if (stextsel == -1) {
		return;
	}

	if (stextscrl) {
		if (stextsel == stextup) {
			if (stextsval)
				stextsval--;
			return;
		}

		stextsel--;
		while (!stext[stextsel]._ssel) {
			if (!stextsel)
				stextsel = STORE_LINES - 1;
			else
				stextsel--;
		}
		return;
	}

	if (!stextsel)
		stextsel = STORE_LINES - 1;
	else
		stextsel--;

	while (!stext[stextsel]._ssel) {
		if (!stextsel)
			stextsel = STORE_LINES - 1;
		else
			stextsel--;
	}
}

void STextDown()
{
	PlaySFX(IS_TITLEMOV);
	if (stextsel == -1) {
		return;
	}

	if (stextscrl) {
		if (stextsel == stextdown) {
			if (stextsval < stextsmax)
				stextsval++;
			return;
		}

		stextsel++;
		while (!stext[stextsel]._ssel) {
			if (stextsel == STORE_LINES - 1)
				stextsel = 0;
			else
				stextsel++;
		}
		return;
	}

	if (stextsel == STORE_LINES - 1)
		stextsel = 0;
	else
		stextsel++;

	while (!stext[stextsel]._ssel) {
		if (stextsel == STORE_LINES - 1)
			stextsel = 0;
		else
			stextsel++;
	}
}

void STextPrior()
{
	PlaySFX(IS_TITLEMOV);
	if (stextsel != -1 && stextscrl) {
		if (stextsel == stextup) {
			if (stextsval)
				stextsval -= 4;
			stextsval = stextsval;
			if (stextsval < 0)
				stextsval = 0;
		} else {
			stextsel = stextup;
		}
	}
}

void STextNext()
{
	PlaySFX(IS_TITLEMOV);
	if (stextsel != -1 && stextscrl) {
		if (stextsel == stextdown) {
			if (stextsval < stextsmax)
				stextsval += 4;
			if (stextsval > stextsmax)
				stextsval = stextsmax;
		} else {
			stextsel = stextdown;
		}
	}
}

void SetGoldCurs(int pnum, int i)
{
	SetPlrHandGoldCurs(&plr[pnum].InvList[i]);
}

void SetSpdbarGoldCurs(int pnum, int i)
{
	SetPlrHandGoldCurs(&plr[pnum].SpdList[i]);
}

void TakePlrsMoney(int cost)
{
	int i;

	plr[myplr]._pGold = CalculateGold(myplr) - cost;
	for (i = 0; i < MAXBELTITEMS && cost > 0; i++) {
		if (plr[myplr].SpdList[i]._itype == ITYPE_GOLD && plr[myplr].SpdList[i]._ivalue != MaxGold) {
			if (cost < plr[myplr].SpdList[i]._ivalue) {
				plr[myplr].SpdList[i]._ivalue -= cost;
				SetSpdbarGoldCurs(myplr, i);
				cost = 0;
			} else {
				cost -= plr[myplr].SpdList[i]._ivalue;
				RemoveSpdBarItem(myplr, i);
				i = -1;
			}
		}
	}
	if (cost > 0) {
		for (i = 0; i < MAXBELTITEMS && cost > 0; i++) {
			if (plr[myplr].SpdList[i]._itype == ITYPE_GOLD) {
				if (cost < plr[myplr].SpdList[i]._ivalue) {
					plr[myplr].SpdList[i]._ivalue -= cost;
					SetSpdbarGoldCurs(myplr, i);
					cost = 0;
				} else {
					cost -= plr[myplr].SpdList[i]._ivalue;
					RemoveSpdBarItem(myplr, i);
					i = -1;
				}
			}
		}
	}
	force_redraw = 255;
	if (cost > 0) {
		for (i = 0; i < plr[myplr]._pNumInv && cost > 0; i++) {
			if (plr[myplr].InvList[i]._itype == ITYPE_GOLD && plr[myplr].InvList[i]._ivalue != MaxGold) {
				if (cost < plr[myplr].InvList[i]._ivalue) {
					plr[myplr].InvList[i]._ivalue -= cost;
					SetGoldCurs(myplr, i);
					cost = 0;
				} else {
					cost -= plr[myplr].InvList[i]._ivalue;
					RemoveInvItem(myplr, i);
					i = -1;
				}
			}
		}
		if (cost > 0) {
			for (i = 0; i < plr[myplr]._pNumInv && cost > 0; i++) {
				if (plr[myplr].InvList[i]._itype == ITYPE_GOLD) {
					if (cost < plr[myplr].InvList[i]._ivalue) {
						plr[myplr].InvList[i]._ivalue -= cost;
						SetGoldCurs(myplr, i);
						cost = 0;
					} else {
						cost -= plr[myplr].InvList[i]._ivalue;
						RemoveInvItem(myplr, i);
						i = -1;
					}
				}
			}
		}
	}
}

void STextEnter()
{
	if (qtextflag) {
		qtextflag = false;
		if (leveltype == DTYPE_TOWN)
			stream_stop();

		return;
	}

	PlaySFX(IS_TITLSLCT);
	switch (stextflag) {
	case STORE_SMITH:
		S_SmithEnter();
		break;
	case STORE_SPBUY:
	case STORE_SBUY:
	case STORE_WBUY:
	case STORE_HBUY:
		S_BuyEnter();
		break;
	case STORE_SSELL:
	case STORE_WSELL:
		S_SellEnter();
		break;
	case STORE_SREPAIR:
		S_SRepairEnter();
		break;
	case STORE_WITCH:
		S_WitchEnter();
		break;
	case STORE_WRECHARGE:
		S_WRechargeEnter();
		break;
	case STORE_NOMONEY:
	case STORE_NOROOM:
		StartStore(stextshold);
		stextsel = stextlhold;
		stextsval = stextvhold;
		break;
	case STORE_CONFIRM:
		S_ConfirmEnter();
		break;
	case STORE_BOY:
		S_BoyEnter();
		break;
	case STORE_BBOY:
		S_BBuyEnter();
		break;
	case STORE_HEALER:
		S_HealerEnter();
		break;
	case STORE_STORY:
		S_StoryEnter();
		break;
	case STORE_SIDENTIFY:
		S_SIDEnter();
		break;
	case STORE_GOSSIP:
		S_TalkEnter();
		break;
	case STORE_IDSHOW:
		StartStore(STORE_SIDENTIFY);
		break;
	case STORE_DRUNK:
		S_DrunkEnter();
		break;
	case STORE_TAVERN:
		S_TavernEnter();
		break;
	case STORE_BARMAID:
		S_BarmaidEnter();
		break;
	case STORE_NONE:
		break;
	}
}

void CheckStoreBtn()
{
	int y;

	if (qtextflag) {
		qtextflag = false;
		if (leveltype == DTYPE_TOWN)
			stream_stop();
	} else if (stextsel != -1 && MouseY >= (32 + UI_OFFSET_Y) && MouseY <= (320 + UI_OFFSET_Y)) {
		if (!stextsize) {
			if (MouseX < 344 + PANEL_LEFT || MouseX > 616 + PANEL_LEFT)
				return;
		} else {
			if (MouseX < 24 + PANEL_LEFT || MouseX > 616 + PANEL_LEFT)
				return;
		}
		y = (MouseY - (32 + UI_OFFSET_Y)) / 12;
		if (stextscrl && MouseX > 600 + PANEL_LEFT) {
			if (y == 4) {
				if (stextscrlubtn <= 0) {
					STextUp();
					stextscrlubtn = 10;
				} else {
					stextscrlubtn--;
				}
			}
			if (y == 20) {
				if (stextscrldbtn <= 0) {
					STextDown();
					stextscrldbtn = 10;
				} else {
					stextscrldbtn--;
				}
			}
		} else if (y >= 5) {
			if (y >= 23)
				y = 22;
			if (stextscrl && y < 21 && !stext[y]._ssel) {
				if (stext[y - 2]._ssel) {
					y -= 2;
				} else if (stext[y - 1]._ssel) {
					y--;
				}
			}
			if (stext[y]._ssel || (stextscrl && y == 22)) {
				stextsel = y;
				STextEnter();
			}
		}
	}
}

void ReleaseStoreBtn()
{
	stextscrlubtn = -1;
	stextscrldbtn = -1;
}

} // namespace devilution
