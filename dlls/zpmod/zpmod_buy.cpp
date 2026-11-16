#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "weapons.h"
#include "player.h"
#include "shake.h"
#include "zpmod.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int gmsgShowMenu;

// ---------------------------------------------------------------------------
// Classic CS 1.6-style Buy Menu & Cash Economy for Neco Zombie Plague
// ---------------------------------------------------------------------------

static const int kStartMoney       = 2000;
static const int kMaxMoney         = 16000;
static const float kBuyMenuTimeout = 15.0f;

static const int kDmgPerCashStep   = 100;
static const int kCashPerDmgStep   = 150;
static const int kKillCash         = 600;
static const int kHeadshotCash     = 900;
static const int kInfectCash       = 400;
static const int kWinBonus         = 1500;
static const int kParticipateBonus = 800;

static const int kAllAmmoCost      = 350;

enum ZPBuyPage {
	ZPBUY_ROOT = 0,
	ZPBUY_PISTOLS,
	ZPBUY_SMGS,
	ZPBUY_RIFLES,
	ZPBUY_SPECIAL,
	ZPBUY_EQUIP,
	ZPBUY_ZOMBIE
};

enum ZPBuyKind {
	BK_WEAPON = 0,   // unique gun (blocked when already owned)
	BK_CONSUMABLE,   // stackable grenade / bomb up to maxAmmo
	BK_AMMO_PACK,    // gives ammo1 (+ optional ammo2)
	BK_ARMOR_50,
	BK_ARMOR_100,
	BK_LONGJUMP,
	BK_ZM_HEAL,
	BK_ZM_FURY,
	BK_ZM_ANTIDOTE
};

struct ZPBuyItem {
	ZPBuyPage page;
	int slot;                 // 1..4
	const char* label;
	int cost;
	ZPBuyKind kind;
	const char* classname;    // weapon entity classname or NULL
	const char* ammoName;     // primary ammo pool (or required weapon for BK_AMMO_PACK)
	int ammoAmount;
	int ammoMax;
	const char* ammo2Name;
	int ammo2Amount;
	int ammo2Max;
	unsigned int rebuyBit;
};

static const ZPBuyItem kBuyCatalog[] = {
	// --- 1. Pistols ---
	{ ZPBUY_PISTOLS, 1, "9mm Handgun",         300, BK_WEAPON,    "weapon_9mmhandgun", "9mm", 51, _9MM_MAX_CARRY, NULL, 0, 0, (1u << 0) },
	{ ZPBUY_PISTOLS, 2, ".357 Python Magnum",  900, BK_WEAPON,    "weapon_357",        "357", 12, _357_MAX_CARRY, NULL, 0, 0, (1u << 1) },
	{ ZPBUY_PISTOLS, 3, "Pistol Ammo Pack",    150, BK_AMMO_PACK, NULL,                "9mm", 68, _9MM_MAX_CARRY, "357", 12, _357_MAX_CARRY, 0 },

	// --- 2. Shotguns & SMGs ---
	{ ZPBUY_SMGS,    1, "SPAS-12 Shotgun",    1400, BK_WEAPON,    "weapon_shotgun",    "buckshot", 24, BUCKSHOT_MAX_CARRY, NULL, 0, 0, (1u << 2) },
	{ ZPBUY_SMGS,    2, "MP5 Submachine Gun", 1800, BK_WEAPON,    "weapon_9mmAR",      "9mm",      90, _9MM_MAX_CARRY, NULL, 0, 0, (1u << 3) },
	{ ZPBUY_SMGS,    3, "M203 AR Grenades x2", 400, BK_AMMO_PACK, "weapon_9mmAR",      "ARgrenades", 2, M203_GRENADE_MAX_CARRY, NULL, 0, 0, 0 },
	{ ZPBUY_SMGS,    4, "Shotgun & SMG Ammo",  200, BK_AMMO_PACK, NULL,                "buckshot", 24, BUCKSHOT_MAX_CARRY, "9mm", 75, _9MM_MAX_CARRY, 0 },

	// --- 3. Rifles & Heavy ---
	{ ZPBUY_RIFLES,  1, "Crossbow (Sniper)",  2200, BK_WEAPON,    "weapon_crossbow",   "bolts",   15, BOLT_MAX_CARRY,    NULL, 0, 0, (1u << 4) },
	{ ZPBUY_RIFLES,  2, "Tau Cannon (Gauss)", 3400, BK_WEAPON,    "weapon_gauss",      "uranium", 40, URANIUM_MAX_CARRY, NULL, 0, 0, (1u << 5) },
	{ ZPBUY_RIFLES,  3, "RPG Launcher",       3800, BK_WEAPON,    "weapon_rpg",        "rockets",  3, ROCKET_MAX_CARRY,  NULL, 0, 0, (1u << 6) },
	{ ZPBUY_RIFLES,  4, "Gluon Gun (Egon)",   4400, BK_WEAPON,    "weapon_egon",       "uranium", 40, URANIUM_MAX_CARRY, NULL, 0, 0, (1u << 7) },

	// --- 4. Special & Grenades ---
	{ ZPBUY_SPECIAL, 1, "Hivehand",           2400, BK_WEAPON,     "weapon_hornetgun",     "Hornets",      8, HORNET_MAX_CARRY,      NULL, 0, 0, (1u << 8) },
	{ ZPBUY_SPECIAL, 2, "Molotov Cocktail",    500, BK_CONSUMABLE, "weapon_molotov",       "Molotov",      1, MOLOTOV_MAX_CARRY,     NULL, 0, 0, 0 },
	{ ZPBUY_SPECIAL, 3, "Frost Grenade",       600, BK_CONSUMABLE, "weapon_freezebomb",    "Freezebomb",   1, FREEZEBOMB_MAX_CARRY,  NULL, 0, 0, 0 },
	{ ZPBUY_SPECIAL, 4, "Satchel + Frag Pack", 800, BK_CONSUMABLE, "weapon_satchel",       "Satchel Charge", 1, SATCHEL_MAX_CARRY,  "weapon_handgrenade", 1, HANDGRENADE_MAX_CARRY, 0 },

	// --- 5. Equipment & Gear ---
	{ ZPBUY_EQUIP,   1, "Kevlar Vest (+50)",   600, BK_ARMOR_50,  NULL, NULL, 0, 0, NULL, 0, 0, 0 },
	{ ZPBUY_EQUIP,   2, "Assault Suit (100)", 1100, BK_ARMOR_100, NULL, NULL, 0, 0, NULL, 0, 0, (1u << 9) },
	{ ZPBUY_EQUIP,   3, "Long Jump Module",   1500, BK_LONGJUMP,  NULL, NULL, 0, 0, NULL, 0, 0, (1u << 10) },
	{ ZPBUY_EQUIP,   4, "Sentry Turret Kit",  1800, BK_CONSUMABLE,"weapon_turret", "Turret", 1, TURRET_KIT_MAX_CARRY, NULL, 0, 0, (1u << 11) },

	// --- 6. Zombie Extra Shop ---
	{ ZPBUY_ZOMBIE,  1, "Infection Bomb",     1200, BK_CONSUMABLE, "weapon_infectionbomb", "Infectionbomb", 1, INFECTIONBOMB_MAX_CARRY, NULL, 0, 0, 0 },
	{ ZPBUY_ZOMBIE,  2, "Blood Heal (+600 HP)",800, BK_ZM_HEAL,    NULL, NULL, 0, 0, NULL, 0, 0, 0 },
	{ ZPBUY_ZOMBIE,  3, "Fury Boost (8s Spd)",1000, BK_ZM_FURY,    NULL, NULL, 0, 0, NULL, 0, 0, 0 },
	{ ZPBUY_ZOMBIE,  4, "T-Virus Antidote",   5000, BK_ZM_ANTIDOTE,NULL, NULL, 0, 0, NULL, 0, 0, 0 },
};

static const int kNumBuyCatalog = (int)(sizeof(kBuyCatalog) / sizeof(kBuyCatalog[0]));

// ---------------------------------------------------------------------------
// ShowMenu transport helper (chunks long CS 1.6 menus cleanly)
// ---------------------------------------------------------------------------
void ZPSendShowMenu(edict_t* player, int keys, int displayTime, const char* text) {
	if (!player || player->free || (player->v.flags & FL_FAKECLIENT))
		return;

	if (!text || !text[0] || keys == 0) {
		MESSAGE_BEGIN(MSG_ONE, gmsgShowMenu, NULL, player);
			WRITE_SHORT(0);
			WRITE_CHAR(0);
			WRITE_BYTE(0);
			WRITE_STRING("");
		MESSAGE_END();
		return;
	}

	const size_t total = strlen(text);
	const size_t kChunkMax = 100;
	size_t off = 0;

	while (off < total) {
		char chunk[128];
		size_t n = total - off;
		if (n > kChunkMax) n = kChunkMax;
		memcpy(chunk, text + off, n);
		chunk[n] = '\0';
		off += n;

		int needMore = (off < total) ? 1 : 0;
		MESSAGE_BEGIN(MSG_ONE, gmsgShowMenu, NULL, player);
			WRITE_SHORT(keys);
			WRITE_CHAR(displayTime);
			WRITE_BYTE(needMore);
			WRITE_STRING(chunk);
		MESSAGE_END();
	}
}

static void BuyPlayPrivateSound(edict_t* player, const char* sfx) {
	if (!player || (player->v.flags & FL_FAKECLIENT) || !sfx || !sfx[0])
		return;
	CLIENT_COMMAND(player, "spk \"%s\"\n", sfx);
}

static void BuyHudNotice(edict_t* player, const char* text, int r, int g, int b) {
	if (!player || (player->v.flags & FL_FAKECLIENT) || !text || !text[0])
		return;

	hudtextparms_t hp;
	memset(&hp, 0, sizeof(hp));
	hp.channel = 0;
	hp.x = -1.0f;
	hp.y = 0.74f;
	hp.r1 = r; hp.g1 = g; hp.b1 = b; hp.a1 = 255;
	hp.fadeinTime = 0.05f;
	hp.fadeoutTime = 0.35f;
	hp.holdTime = 1.6f;
	UTIL_HudMessage(CBaseEntity::Instance(player), hp, text);
}

static int BuyAmmoCount(CBasePlayer* pPlayer, const char* ammoName) {
	if (!pPlayer || !ammoName) return 0;
	int aIdx = pPlayer->GetAmmoIndex(ammoName);
	if (aIdx < 0 || aIdx >= MAX_AMMO_SLOTS) return 0;
	return pPlayer->m_rgAmmo[aIdx];
}

static bool BuyCanReceiveAmmo(CBasePlayer* pPlayer, const char* name, int maxCarry) {
	return name && name[0] && BuyAmmoCount(pPlayer, name) < maxCarry;
}

static bool BuyAntidoteAllowed(edict_t* player) {
	if (g_round.state != RS_ACTIVE) return false;
	if (g_players[ENTINDEX(player)].ZMClass == ZM_CLASS_BOSS) return false;
	if (g_round.mode == ZMODE_NEMESIS || g_round.mode == ZMODE_ARMAGEDDON || g_round.mode == ZMODE_SWARM)
		return false;

	int zombiesAlive = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t* e = INDEXENT(i);
		if (ZPIsPlayerConnected(e) && ZPIsZombie(e) && e->v.health > 0)
			zombiesAlive++;
	}
	return zombiesAlive > 1;
}

// Returns 0 when available, 1 when insufficient funds, 2 when owned/full/locked.
static int BuyItemStatus(edict_t* player, CBasePlayer* pPlayer, const ZPBuyItem& item) {
	int idx = ENTINDEX(player);
	int money = g_players[idx].money;

	switch (item.kind) {
		case BK_WEAPON:
			if (pPlayer->HasNamedPlayerItem(item.classname)) return 2;
			break;

		case BK_CONSUMABLE:
			if (item.ammo2Name) {
				// Satchel + Frag Pack
				bool canSatchel = BuyCanReceiveAmmo(pPlayer, "Satchel Charge", SATCHEL_MAX_CARRY);
				bool canFrag = BuyCanReceiveAmmo(pPlayer, "Hand Grenade", HANDGRENADE_MAX_CARRY);
				if (!canSatchel && !canFrag) return 2;
			} else if (!BuyCanReceiveAmmo(pPlayer, item.ammoName, item.ammoMax)) {
				return 2;
			}
			break;

		case BK_AMMO_PACK: {
			if (item.classname && !pPlayer->HasNamedPlayerItem(item.classname))
				return 2;
			bool can1 = BuyCanReceiveAmmo(pPlayer, item.ammoName, item.ammoMax);
			bool can2 = item.ammo2Name ? BuyCanReceiveAmmo(pPlayer, item.ammo2Name, item.ammo2Max) : false;
			if (!can1 && !can2) return 2;
			break;
		}

		case BK_ARMOR_50:
		case BK_ARMOR_100:
			if (pPlayer->pev->armorvalue >= 100.0f) return 2;
			break;

		case BK_LONGJUMP:
			if (pPlayer->m_fLongJump) return 2;
			break;

		case BK_ZM_HEAL: {
			float maxHp = ZMClassHealth(g_players[idx].ZMClass);
			if (pPlayer->pev->health >= maxHp) return 2;
			break;
		}

		case BK_ZM_FURY:
			if (g_players[idx].rageUntil > gpGlobals->time) return 2;
			break;

		case BK_ZM_ANTIDOTE:
			if (!BuyAntidoteAllowed(player)) return 2;
			break;
	}

	return (money < item.cost) ? 1 : 0;
}

static bool BuyRefillAllAmmoAvailable(CBasePlayer* pPlayer) {
	static const struct { const char* wpn; const char* ammo; int maxCarry; } kPool[] = {
		{ "weapon_9mmhandgun", "9mm",        _9MM_MAX_CARRY },
		{ "weapon_357",        "357",        _357_MAX_CARRY },
		{ "weapon_shotgun",    "buckshot",   BUCKSHOT_MAX_CARRY },
		{ "weapon_9mmAR",      "9mm",        _9MM_MAX_CARRY },
		{ "weapon_9mmAR",      "ARgrenades", M203_GRENADE_MAX_CARRY },
		{ "weapon_crossbow",   "bolts",      BOLT_MAX_CARRY },
		{ "weapon_gauss",      "uranium",    URANIUM_MAX_CARRY },
		{ "weapon_egon",       "uranium",    URANIUM_MAX_CARRY },
		{ "weapon_rpg",        "rockets",    ROCKET_MAX_CARRY },
	};
	for (size_t i = 0; i < sizeof(kPool) / sizeof(kPool[0]); i++) {
		if (pPlayer->HasNamedPlayerItem(kPool[i].wpn) && BuyCanReceiveAmmo(pPlayer, kPool[i].ammo, kPool[i].maxCarry))
			return true;
	}
	return false;
}

static const char* BuyPageTitle(int page) {
	switch (page) {
		case ZPBUY_PISTOLS: return "Buy Handgun";
		case ZPBUY_SMGS:    return "Buy Shotguns & SMGs";
		case ZPBUY_RIFLES:  return "Buy Rifles & Heavy";
		case ZPBUY_SPECIAL: return "Special & Grenades";
		case ZPBUY_EQUIP:   return "Buy Equipment";
		case ZPBUY_ZOMBIE:  return "Zombie Shop";
		default:            return "Buy Item";
	}
}

static void BuyGiveWeaponWithAmmo(CBasePlayer* pPlayer, const ZPBuyItem& item) {
	pPlayer->GiveNamedItem(item.classname);
	if (item.ammoName && item.ammoAmount > 0)
		pPlayer->GiveAmmo(item.ammoAmount, item.ammoName, item.ammoMax);
	pPlayer->SelectItem(item.classname);
}

static bool BuyExecuteItem(edict_t* player, CBasePlayer* pPlayer, const ZPBuyItem& item, bool silent) {
	int idx = ENTINDEX(player);
	int status = BuyItemStatus(player, pPlayer, item);
	if (status == 2) {
		if (!silent) {
			BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
			const char* why = (item.kind == BK_ZM_ANTIDOTE)
				? "ANTIDOTE UNAVAILABLE RIGHT NOW"
				: "ALREADY OWNED OR AT MAX";
			BuyHudNotice(player, why, 255, 190, 60);
		}
		return false;
	}
	if (status == 1) {
		if (!silent) {
			BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
			char msg[64];
			snprintf(msg, sizeof(msg), "INSUFFICIENT FUNDS ($%d NEEDED)", item.cost);
			BuyHudNotice(player, msg, 255, 80, 80);
		}
		return false;
	}

	g_players[idx].money -= item.cost;
	if (item.rebuyBit)
		g_players[idx].rebuyMask |= item.rebuyBit;

	const char* sfx = "items/gunpickup2.wav";

	switch (item.kind) {
		case BK_WEAPON:
			BuyGiveWeaponWithAmmo(pPlayer, item);
			break;

		case BK_CONSUMABLE:
			if (item.ammo2Name) {
				if (BuyCanReceiveAmmo(pPlayer, "Satchel Charge", SATCHEL_MAX_CARRY))
					pPlayer->GiveNamedItem("weapon_satchel");
				if (BuyCanReceiveAmmo(pPlayer, "Hand Grenade", HANDGRENADE_MAX_CARRY))
					pPlayer->GiveNamedItem("weapon_handgrenade");
			} else {
				pPlayer->GiveNamedItem(item.classname);
			}
			sfx = "items/9mmclip1.wav";
			break;

		case BK_AMMO_PACK:
			if (item.ammoName)
				pPlayer->GiveAmmo(item.ammoAmount, item.ammoName, item.ammoMax);
			if (item.ammo2Name)
				pPlayer->GiveAmmo(item.ammo2Amount, item.ammo2Name, item.ammo2Max);
			sfx = "items/9mmclip1.wav";
			break;

		case BK_ARMOR_50:
			pPlayer->pev->armorvalue += 50.0f;
			if (pPlayer->pev->armorvalue > 100.0f) pPlayer->pev->armorvalue = 100.0f;
			pPlayer->pev->armortype = 0.5f;
			sfx = "items/suitcharge1.wav";
			break;

		case BK_ARMOR_100:
			pPlayer->pev->armorvalue = 100.0f;
			pPlayer->pev->armortype = 0.5f;
			sfx = "items/suitcharge1.wav";
			break;

		case BK_LONGJUMP:
			pPlayer->m_fLongJump = TRUE;
			g_engfuncs.pfnSetPhysicsKeyValue(player, "slj", "1");
			sfx = "items/suitcharge1.wav";
			break;

		case BK_ZM_HEAL:
			ZPZombieHeal(player, 600.0f);
			UTIL_ScreenFade(pPlayer, Vector(80, 255, 90), 0.35f, 0.15f, 180, FFADE_IN);
			sfx = "zpmod/zombi_heal.wav";
			break;

		case BK_ZM_FURY:
			g_players[idx].rageUntil = gpGlobals->time + 8.0f;
			UTIL_ScreenFade(pPlayer, Vector(255, 60, 60), 0.35f, 0.15f, 180, FFADE_IN);
			sfx = "zpmod/zombi_pressure.wav";
			break;

		case BK_ZM_ANTIDOTE:
			ZPMakeHuman(player);
			UTIL_ClientPrintAll(HUD_PRINTNOTIFY, UTIL_VarArgs("[ZP] %s bought an Antidote and returned to Human!\n", STRING(player->v.netname)));
			sfx = "items/suitcharge1.wav";
			break;
	}

	if (!silent) {
		BuyPlayPrivateSound(player, sfx);
		char msg[64];
		snprintf(msg, sizeof(msg), "PURCHASED: %s (-$%d)", item.label, item.cost);
		BuyHudNotice(player, msg, 90, 255, 130);
	}
	return true;
}

static bool BuyRefillAllAmmo(edict_t* player, CBasePlayer* pPlayer) {
	int idx = ENTINDEX(player);
	if (!BuyRefillAllAmmoAvailable(pPlayer)) {
		BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
		BuyHudNotice(player, "AMMO ALREADY FULL", 255, 190, 60);
		return false;
	}
	if (g_players[idx].money < kAllAmmoCost) {
		BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
		BuyHudNotice(player, "INSUFFICIENT FUNDS ($350 NEEDED)", 255, 80, 80);
		return false;
	}

	g_players[idx].money -= kAllAmmoCost;
	static const struct { const char* wpn; const char* ammo; int add; int maxCarry; } kRefill[] = {
		{ "weapon_9mmhandgun", "9mm",        68, _9MM_MAX_CARRY },
		{ "weapon_357",        "357",        18, _357_MAX_CARRY },
		{ "weapon_shotgun",    "buckshot",   32, BUCKSHOT_MAX_CARRY },
		{ "weapon_9mmAR",      "9mm",       100, _9MM_MAX_CARRY },
		{ "weapon_9mmAR",      "ARgrenades",  2, M203_GRENADE_MAX_CARRY },
		{ "weapon_crossbow",   "bolts",      15, BOLT_MAX_CARRY },
		{ "weapon_gauss",      "uranium",    40, URANIUM_MAX_CARRY },
		{ "weapon_egon",       "uranium",    40, URANIUM_MAX_CARRY },
		{ "weapon_rpg",        "rockets",     2, ROCKET_MAX_CARRY },
	};
	for (size_t i = 0; i < sizeof(kRefill) / sizeof(kRefill[0]); i++) {
		if (pPlayer->HasNamedPlayerItem(kRefill[i].wpn))
			pPlayer->GiveAmmo(kRefill[i].add, kRefill[i].ammo, kRefill[i].maxCarry);
	}
	BuyPlayPrivateSound(player, "items/9mmclip1.wav");
	BuyHudNotice(player, "PURCHASED: ALL AMMO (-$350)", 90, 255, 130);
	return true;
}

// ---------------------------------------------------------------------------
// Lifecycle & economy hooks
// ---------------------------------------------------------------------------
void ZPBuyInit(void) {
}

void ZPBuyResetPlayer(int idx, bool resetMoney) {
	if (idx < 1 || idx > gpGlobals->maxClients) return;
	if (resetMoney) {
		g_players[idx].money = kStartMoney;
		g_players[idx].damageMoneyBank = 0.0f;
		g_players[idx].rebuyMask = 0;
	}
	g_players[idx].buyMenuPage = ZPBUY_ROOT;
	g_players[idx].buyMenuUntil = 0.0f;
}

void ZPBuyAddMoney(edict_t* player, int delta, const char* reason) {
	if (!ZPIsPlayerConnected(player) || delta == 0) return;
	int idx = ENTINDEX(player);
	if (idx < 1 || idx > gpGlobals->maxClients) return;

	int m = g_players[idx].money + delta;
	if (m < 0) m = 0;
	if (m > kMaxMoney) m = kMaxMoney;
	g_players[idx].money = m;

	if (reason && reason[0] && !(player->v.flags & FL_FAKECLIENT))
		BuyHudNotice(player, reason, 120, 255, 140);
}

void ZPBuyOnDamage(edict_t* attacker, edict_t* victim, float damage) {
	if (damage <= 0.0f || !attacker || !victim || attacker == victim) return;
	if (!ZPIsPlayerConnected(attacker) || !ZPIsPlayerConnected(victim)) return;
	if (!ZPIsHuman(attacker) || !ZPIsZombie(victim)) return;

	int idx = ENTINDEX(attacker);
	if (idx < 1 || idx > gpGlobals->maxClients) return;

	g_players[idx].damageMoneyBank += damage;
	if (g_players[idx].damageMoneyBank >= (float)kDmgPerCashStep) {
		int steps = (int)(g_players[idx].damageMoneyBank / (float)kDmgPerCashStep);
		g_players[idx].damageMoneyBank -= (float)(steps * kDmgPerCashStep);
		ZPBuyAddMoney(attacker, steps * kCashPerDmgStep);
	}
}

void ZPBuyOnKill(edict_t* killer, bool fromHeadshot) {
	if (!ZPIsPlayerConnected(killer)) return;
	int bonus = fromHeadshot ? kHeadshotCash : kKillCash;
	ZPBuyAddMoney(killer, bonus);
}

void ZPBuyOnInfect(edict_t* infector) {
	if (!ZPIsPlayerConnected(infector)) return;
	ZPBuyAddMoney(infector, kInfectCash);
}

void ZPBuyRoundBonus(bool humansWon) {
	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t* ed = INDEXENT(i);
		if (!ZPIsPlayerConnected(ed)) continue;

		bool won = humansWon ? (ZPIsHuman(ed) && ed->v.health > 0) : ZPIsZombie(ed);
		ZPBuyAddMoney(ed, won ? kWinBonus : kParticipateBonus);
	}
}

// ---------------------------------------------------------------------------
// Menu rendering & selection
// ---------------------------------------------------------------------------
void ZPBuyOpen(edict_t* player, int page) {
	if (!ZPIsPlayerConnected(player) || (player->v.flags & FL_FAKECLIENT))
		return;

	CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
	if (!pPlayer || !pPlayer->IsPlayer())
		return;

	if (!pPlayer->IsAlive() || player->v.team == RoleToInt(ROLE_SPECTATOR)) {
		BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
		BuyHudNotice(player, "YOU MUST BE ALIVE TO BUY", 255, 90, 90);
		return;
	}

	int idx = ENTINDEX(player);
	if (g_mapVote.active || g_players[idx].menuType == ZPMENU_VOTE)
		return;

	bool isZombie = ZPIsZombie(player);
	if (isZombie) {
		page = ZPBUY_ZOMBIE;
	} else if (page < ZPBUY_ROOT || page > ZPBUY_EQUIP) {
		page = ZPBUY_ROOT;
	}

	g_players[idx].abilityMenuUntil = 0.0f;

	char text[512];
	int off = 0;
	int keys = (1 << 9); // 0. Exit

	if (page == ZPBUY_ROOT) {
		bool canAmmo = BuyRefillAllAmmoAvailable(pPlayer) && g_players[idx].money >= kAllAmmoCost;
		bool hasRebuy = (g_players[idx].rebuyMask != 0);

		off += snprintf(text + off, sizeof(text) - off,
			"\\yBuy Item\\w  (\\y$%d\\w)\n\n"
			"\\y1.\\w Pistols\n"
			"\\y2.\\w Shotguns & SMGs\n"
			"\\y3.\\w Rifles & Heavy\n"
			"\\y4.\\w Special & Grenades\n"
			"\\y5.\\w Equipment & Gear\n\n"
			"%s6.%s Refill All Ammo %s\\R$%d\n"
			"%s7.%s Re-Buy Previous\n\n"
			"\\y0.\\w Exit",
			g_players[idx].money,
			canAmmo ? "\\y" : "\\d", canAmmo ? "\\w" : "\\d", canAmmo ? "\\y" : "\\d", kAllAmmoCost,
			hasRebuy ? "\\y" : "\\d", hasRebuy ? "\\w" : "\\d");

		keys |= (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);
	} else {
		const char* hdrColor = isZombie ? "\\r" : "\\y";
		off += snprintf(text + off, sizeof(text) - off,
			"%s%s\\w  (\\y$%d\\w)\n\n",
			hdrColor, BuyPageTitle(page), g_players[idx].money);

		for (int i = 0; i < kNumBuyCatalog; i++) {
			const ZPBuyItem& item = kBuyCatalog[i];
			if (item.page != page) continue;

			int st = BuyItemStatus(player, pPlayer, item);
			const char* numC = (st == 0) ? hdrColor : "\\d";
			const char* lblC = (st == 0) ? "\\w" : "\\d";
			const char* priceC = (st == 0) ? "\\y" : ((st == 1) ? "\\r" : "\\d");

			if (st == 2) {
				const char* tag = (item.kind == BK_WEAPON || item.kind == BK_LONGJUMP) ? "OWNED"
					: (item.kind == BK_ZM_ANTIDOTE) ? "LOCKED" : "FULL";
				off += snprintf(text + off, sizeof(text) - off,
					"%s%d.%s %s \\d\\R[%s]\n",
					numC, item.slot, lblC, item.label, tag);
			} else {
				off += snprintf(text + off, sizeof(text) - off,
					"%s%d.%s %s %s\\R$%d\n",
					numC, item.slot, lblC, item.label, priceC, item.cost);
			}

			keys |= (1 << (item.slot - 1));
		}

		if (!isZombie) {
			off += snprintf(text + off, sizeof(text) - off, "\n\\y5.\\w Back\n\\y0.\\w Exit");
			keys |= (1 << 4); // 5. Back
		} else {
			off += snprintf(text + off, sizeof(text) - off, "\n\\y0.\\w Exit");
		}
	}

	g_players[idx].menuType = ZPMENU_BUY;
	g_players[idx].buyMenuPage = page;
	g_players[idx].buyMenuUntil = gpGlobals->time + kBuyMenuTimeout;

	ZPSendShowMenu(player, keys, (int)kBuyMenuTimeout, text);
}

void ZPBuySelect(int playerIndex, int slot) {
	if (playerIndex < 1 || playerIndex > gpGlobals->maxClients) return;
	if (g_players[playerIndex].menuType != ZPMENU_BUY) return;

	if (g_players[playerIndex].buyMenuUntil < gpGlobals->time) {
		g_players[playerIndex].menuType = ZPMENU_NONE;
		g_players[playerIndex].buyMenuUntil = 0.0f;
		return;
	}

	edict_t* ed = INDEXENT(playerIndex);
	if (!ZPIsPlayerConnected(ed)) return;
	CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
	if (!pPlayer || !pPlayer->IsAlive() || ed->v.team == RoleToInt(ROLE_SPECTATOR)) {
		g_players[playerIndex].menuType = ZPMENU_NONE;
		g_players[playerIndex].buyMenuUntil = 0.0f;
		return;
	}

	int page = g_players[playerIndex].buyMenuPage;
	g_players[playerIndex].menuType = ZPMENU_NONE;
	g_players[playerIndex].buyMenuUntil = 0.0f;

	if (slot == 10) // 0. Exit
		return;

	bool isZombie = ZPIsZombie(ed);
	if (isZombie && page != ZPBUY_ZOMBIE) {
		// Player got infected while holding the human menu open; pivot safely.
		ZPBuyOpen(ed, ZPBUY_ZOMBIE);
		return;
	}
	if (!isZombie && page == ZPBUY_ZOMBIE) {
		ZPBuyOpen(ed, ZPBUY_ROOT);
		return;
	}

	if (page == ZPBUY_ROOT) {
		if (slot >= 1 && slot <= 5) {
			ZPBuyOpen(ed, slot);
			return;
		}
		if (slot == 6) {
			BuyRefillAllAmmo(ed, pPlayer);
			return;
		}
		if (slot == 7) {
			ZPBuyRebuy(ed);
			return;
		}
		return;
	}

	if (!isZombie && slot == 5) {
		ZPBuyOpen(ed, ZPBUY_ROOT);
		return;
	}

	for (int i = 0; i < kNumBuyCatalog; i++) {
		const ZPBuyItem& item = kBuyCatalog[i];
		if (item.page == page && item.slot == slot) {
			bool ok = BuyExecuteItem(ed, pPlayer, item, false);
			// Keep ammo / failed attempts responsive inside the submenu, classic-style
			if (!ok || item.kind == BK_AMMO_PACK)
				ZPBuyOpen(ed, page);
			return;
		}
	}
}

void ZPBuyRebuy(edict_t* player) {
	if (!ZPIsPlayerConnected(player) || ZPIsZombie(player)) return;
	CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
	if (!pPlayer || !pPlayer->IsAlive()) return;

	int idx = ENTINDEX(player);
	unsigned int mask = g_players[idx].rebuyMask;
	if (!mask) {
		BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
		BuyHudNotice(player, "NO PREVIOUS LOADOUT TO RE-BUY", 255, 190, 60);
		return;
	}

	// Buy in descending priority (heavy weapons + armor first) without closing mid-way.
	int bought = 0;
	for (int i = kNumBuyCatalog - 1; i >= 0; i--) {
		const ZPBuyItem& item = kBuyCatalog[i];
		if (item.page == ZPBUY_ZOMBIE || !(mask & item.rebuyBit))
			continue;
		if (BuyExecuteItem(player, pPlayer, item, true))
			bought++;
	}

	if (bought > 0) {
		BuyPlayPrivateSound(player, "items/gunpickup2.wav");
		BuyHudNotice(player, "RE-BOUGHT PREVIOUS LOADOUT", 90, 255, 130);
	} else {
		BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
		BuyHudNotice(player, "NOTHING RE-BOUGHT (ALREADY OWNED OR LOW CASH)", 255, 190, 60);
	}
}

void ZPBuyAutobuy(edict_t* player) {
	if (!ZPIsPlayerConnected(player) || ZPIsZombie(player)) return;
	CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
	if (!pPlayer || !pPlayer->IsAlive()) return;

	bool isBot = (player->v.flags & FL_FAKECLIENT) != 0;
	int bought = 0;

	// Bots pick varied primary archetypes so they don't all look identical.
	static const char* kPrimaryOrder[] = {
		"weapon_egon",
		"weapon_gauss",
		"weapon_crossbow",
		"weapon_9mmAR",
		"weapon_shotgun",
		"weapon_357"
	};

	const int numPrim = (int)(sizeof(kPrimaryOrder) / sizeof(kPrimaryOrder[0]));
	int startPick = isBot ? RANDOM_LONG(0, numPrim - 1) : 0;

	for (int step = 0; step < numPrim; step++) {
		const char* cls = kPrimaryOrder[(startPick + step) % numPrim];
		for (int i = 0; i < kNumBuyCatalog; i++) {
			if (kBuyCatalog[i].classname && !strcmp(kBuyCatalog[i].classname, cls)) {
				if (BuyExecuteItem(player, pPlayer, kBuyCatalog[i], true)) {
					bought++;
					step = numPrim; // grab one primary weapon first
				}
				break;
			}
		}
	}

	// Add armor if affordable
	for (int i = 0; i < kNumBuyCatalog; i++) {
		if (kBuyCatalog[i].kind == BK_ARMOR_100 || kBuyCatalog[i].kind == BK_ARMOR_50) {
			if (BuyExecuteItem(player, pPlayer, kBuyCatalog[i], true)) {
				bought++;
				break;
			}
		}
	}

	if (!isBot) {
		if (bought > 0) {
			BuyPlayPrivateSound(player, "items/gunpickup2.wav");
			BuyHudNotice(player, "AUTO-BUY COMPLETE", 90, 255, 130);
		} else {
			BuyPlayPrivateSound(player, "items/suitchargeno1.wav");
			BuyHudNotice(player, "NOTHING AFFORDABLE TO AUTO-BUY", 255, 190, 60);
		}
	}
}

void ZPBuyAutoOpenPrep(void) {
	for (int i = 1; i <= gpGlobals->maxClients; i++) {
		edict_t* ed = INDEXENT(i);
		if (!ZPIsPlayerConnected(ed) || ed->v.health <= 0 || !ZPIsHuman(ed))
			continue;

		if (ed->v.flags & FL_FAKECLIENT) {
			ZPBuyAutobuy(ed);
			continue;
		}

		if (g_players[i].menuType == ZPMENU_NONE && !g_mapVote.active)
			ZPBuyOpen(ed, ZPBUY_ROOT);
	}
}

bool ZPBuyHandleClientCommand(edict_t* player, const char* cmd) {
	if (!player || !cmd) return false;

	if (FStrEq(cmd, "buy") || FStrEq(cmd, "buymenu") || FStrEq(cmd, "shop") || FStrEq(cmd, "chooseteam")) {
		ZPBuyOpen(player, ZPBUY_ROOT);
		return true;
	}
	if (FStrEq(cmd, "buyequip")) {
		ZPBuyOpen(player, ZPBUY_EQUIP);
		return true;
	}
	if (FStrEq(cmd, "rebuy")) {
		ZPBuyRebuy(player);
		return true;
	}
	if (FStrEq(cmd, "autobuy")) {
		ZPBuyAutobuy(player);
		return true;
	}
	if (FStrEq(cmd, "buyammo1") || FStrEq(cmd, "buyammo2")) {
		if (ZPIsPlayerConnected(player) && ZPIsHuman(player)) {
			CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
			if (pPlayer && pPlayer->IsAlive())
				BuyRefillAllAmmo(player, pPlayer);
		}
		return true;
	}
	return false;
}

bool ZPBuyHandleChatCommand(edict_t* player, const char* text) {
	if (!player || !text || !text[0]) return false;

	if (!stricmp(text, "/buy") || !stricmp(text, "!buy") || !stricmp(text, "buy") ||
	    !stricmp(text, "/shop") || !stricmp(text, "!shop") || !stricmp(text, "shop")) {
		ZPBuyOpen(player, ZPBUY_ROOT);
		return true;
	}
	if (!stricmp(text, "/rebuy") || !stricmp(text, "!rebuy")) {
		ZPBuyRebuy(player);
		return true;
	}
	if (!stricmp(text, "/autobuy") || !stricmp(text, "!autobuy")) {
		ZPBuyAutobuy(player);
		return true;
	}
	return false;
}
