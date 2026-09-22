#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "zpmod.h"
#include "player.h"
#include "shake.h"

// Round modes + random event modifiers.
// A mode is picked at countdown (ZPFeaturePreRound), the actual starting
// infection is applied in ZPFeatureStartInfections and per-frame / hook
// behaviour hooks through the rest of the ZP module.

static float s_blackoutAt = 0.0f;      // when the BLACKOUT event triggers
static float s_blackoutUntil = 0.0f;   // when the BLACKOUT event ends
static float s_heartbeatUntil = 0.0f;  // last-human heartbeat cadence
static const float kPlagueInterval = 20.0f;

const char* ZPModeName(int mode) {
    switch (mode) {
        case ZMODE_MULTIPLE:    return "MULTIPLE";
        case ZMODE_SWARM:       return "SWARM";
        case ZMODE_PLAGUE:      return "PLAGUE";
        case ZMODE_ARMAGEDDON:  return "ARMAGEDDON";
        case ZMODE_NEMESIS:     return "SURVIVOR VS NEMESIS";
        case ZMODE_CLASSIC:
        default:                return "CLASSIC";
    }
}

const char* ZPEventName(int ev) {
    switch (ev) {
        case ZEV_LOW_GRAVITY:    return "LOW GRAVITY";
        case ZEV_BLACKOUT:       return "BLACKOUT";
        case ZEV_ONE_HIT:        return "ONE HIT";
        case ZEV_DOUBLE_DAMAGE:  return "DOUBLE DAMAGE";
        case ZEV_SPEED:          return "SPEED";
        case ZEV_ARMOR:          return "ARMOR";
        case ZEV_DOUBLE_SPREAD:  return "DOUBLE SPREAD";
        case ZEV_SUDDEN_DEATH:   return "SUDDEN DEATH";
        default:                 return "";
    }
}

void ZPFeatureInit(void) {
    g_round.mode = ZMODE_CLASSIC;
    g_round.eventType = ZEV_NONE;
    g_round.eventAnnounced = 0.0f;
    g_round.suddenDeathActive = false;
    g_round.suddenDeathUntil = 0.0f;
    g_round.plagueNextInfectTime = false;
    g_round.plagueNextSpread = 0.0f;
    g_round.lastHumanMusicUntil = 0.0f;
    g_round.bossRound = false;

    s_blackoutAt = 0.0f;
    s_blackoutUntil = 0.0f;

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        g_players[i].steerMode = ZEV_NONE;
        g_players[i].bossRoundStart = false;
    }
}

// picks this round's mode + rare boss round + rare event modifier and
// announces them during the countdown
void ZPFeaturePreRound(ZPRound* round) {
    if (!round) return;

    int prevMode = round->mode;
    int mode;
    if (prevMode >= ZMODE_CLASSIC && prevMode <= ZMODE_NEMESIS) {
        do {
            mode = RANDOM_LONG(ZMODE_CLASSIC, ZMODE_NEMESIS);
        } while (mode == prevMode);
    } else {
        mode = RANDOM_LONG(ZMODE_CLASSIC, ZMODE_NEMESIS);
    }
    round->mode = mode;

    // rare boss round (~12%); the nemesis mode is a boss round by design
    round->bossRound = (RANDOM_LONG(1, 100) <= 12);

    // rare random event modifier (~20%)
    int ev = ZEV_NONE;
    if (RANDOM_LONG(1, 100) <= 20)
        ev = RANDOM_LONG(ZEV_LOW_GRAVITY, ZEV_SUDDEN_DEATH);
    round->eventType = ev;

    round->eventAnnounced = gpGlobals->time;
    round->suddenDeathActive = (ev == ZEV_SUDDEN_DEATH);
    round->suddenDeathUntil = 0.0f;
    round->plagueNextInfectTime = false;
    round->plagueNextSpread = gpGlobals->time + kPlagueInterval;
    round->lastHumanMusicUntil = 0.0f;
    round->roundDuration = ZPFeatureRoundDuration();

    s_blackoutAt = 0.0f;
    s_blackoutUntil = 0.0f;
    s_heartbeatUntil = 0.0f;

    for (int i = 1; i <= gpGlobals->maxClients; i++)
        g_players[i].bossRoundStart = false;

    char msg[160];
    const char* evName = ZPEventName(ev);
    if (round->bossRound) {
        if (evName[0])
            snprintf(msg, sizeof(msg), "BOSS ROUND!\nMODE: %s\nEVENT: %s", ZPModeName(mode), evName);
        else
            snprintf(msg, sizeof(msg), "BOSS ROUND!\nMODE: %s", ZPModeName(mode));
    } else {
        if (evName[0])
            snprintf(msg, sizeof(msg), "RANDOM MODE: %s\nEVENT: %s", ZPModeName(mode), evName);
        else
            snprintf(msg, sizeof(msg), "RANDOM MODE: %s", ZPModeName(mode));
    }
    UTIL_ClientPrintAll(HUD_PRINTCENTER, msg);
}

// how many players start infected for the current mode (double spread applies)
int ZPFeatureInitialInfections(ZPRound* round, int connected) {
    if (!round) return 1;

    int base;
    switch (round->mode) {
        case ZMODE_MULTIPLE:    base = 3;                break;
        case ZMODE_SWARM:       base = 5;                break;
        case ZMODE_PLAGUE:      base = 1;                break;
        case ZMODE_ARMAGEDDON:  base = connected - 2;    break;
        case ZMODE_NEMESIS:     base = connected - 1;    break;
        default:                base = 1;                break; // CLASSIC
    }

    if (round->eventType == ZEV_DOUBLE_SPREAD && base > 1)
        base *= 2;

    if (base < 1) base = 1;
    if (base > connected - 1) base = connected - 1;
    if (base < 1) base = 1;
    return base;
}

// applies the starting infection for the chosen round mode
void ZPFeatureStartInfections(ZPRound* round, int* players, int count) {
    if (!round || !players || count < 1) return;

    int mode = round->mode;
    int toInfect = ZPFeatureInitialInfections(round, count);

    if (mode == ZMODE_NEMESIS) {
        // 1 survivor vs 1 boss nemesis, everyone else a zombie
        int survivor = RANDOM_LONG(0, count - 1);
        int nemesis = -1;
        if (count > 1) {
            do {
                nemesis = RANDOM_LONG(0, count - 1);
            } while (nemesis == survivor);
        }

        for (int i = 0; i < count; i++) {
            edict_t* ed = INDEXENT(players[i]);
            if (i == survivor) {
                CLIENT_PRINTF(ed, print_center, "You are the last survivor\n");
                continue;
            }
            if (i == nemesis)
                g_players[players[i]].bossRoundStart = true;
            ZPInfectPlayer(ed, false);
        }
        return;
    }

    if (mode == ZMODE_ARMAGEDDON) {
        // everyone but a couple of humans starts infected
        int humans = count - toInfect;
        if (humans < 1) humans = 1;

        bool survivor[32] = { false };
        int picked = 0;
        while (picked < humans) {
            int r = RANDOM_LONG(0, count - 1);
            if (!survivor[r]) {
                survivor[r] = true;
                picked++;
            }
        }

        bool first = true;
        for (int i = 0; i < count; i++) {
            edict_t* ed = INDEXENT(players[i]);
            if (survivor[i]) {
                CLIENT_PRINTF(ed, print_center, "You survived the armageddon\n");
                continue;
            }
            if (round->bossRound && first) {
                g_players[players[i]].bossRoundStart = true;
                first = false;
            }
            ZPInfectPlayer(ed, false);
        }
        return;
    }

    // CLASSIC / MULTIPLE / SWARM / PLAGUE: N random first zombies
    int n = toInfect < count ? toInfect : count;
    bool picked[32] = { false };
    int infected = 0;
    while (infected < n) {
        int r = RANDOM_LONG(0, count - 1);
        if (picked[r]) continue;
        picked[r] = true;
        infected++;

        edict_t* ed = INDEXENT(players[r]);
        if (round->bossRound && infected == 1)
            g_players[players[r]].bossRoundStart = true;
        ZPInfectPlayer(ed, false);
        CLIENT_PRINTF(ed, print_center, "You are a first zombie\n");
    }
}

// per-frame logic for the active round (plague spread, blackout, cleanup)
void ZPFeatureRoundThink(ZPRound* round) {
    if (!round) return;

    if (round->state != RS_ACTIVE) {
        LIGHT_STYLE(0, "m");
s_blackoutAt = 0.0f;
    s_blackoutUntil = 0.0f;
    s_heartbeatUntil = 0.0f;
        return;
    }

    // BLACKOUT: sudden darkness for a 15s window partway through the round
    if (round->eventType == ZEV_BLACKOUT) {
        if (s_blackoutAt <= 0.0f)
            s_blackoutAt = round->roundStartTime + RANDOM_FLOAT(45.0f, 150.0f);

        if (gpGlobals->time >= s_blackoutAt && gpGlobals->time < s_blackoutAt + 15.0f) {
            if (s_blackoutUntil < s_blackoutAt) {
                s_blackoutUntil = s_blackoutAt + 15.0f;
                UTIL_ClientPrintAll(HUD_PRINTCENTER, "BLACKOUT!\n");
                LIGHT_STYLE(0, "z");
            }
        } else if (gpGlobals->time >= s_blackoutAt + 15.0f && s_blackoutUntil >= s_blackoutAt) {
            s_blackoutUntil = 0.0f;
            LIGHT_STYLE(0, "m");
        }
    }

    // PLAGUE: infection spreads on its own while at least 2 humans remain
    if (round->mode == ZMODE_PLAGUE && gpGlobals->time >= round->plagueNextSpread) {
        round->plagueNextSpread = gpGlobals->time + kPlagueInterval;

        int humans[32];
        int hcount = 0;
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* e = INDEXENT(i);
            if (!ZPIsPlayerConnected(e)) continue;
            if (!ZPIsHuman(e) || e->v.health <= 0) continue;
            humans[hcount++] = i;
        }

        if (hcount > 1) {
            int victim = humans[RANDOM_LONG(0, hcount - 1)];
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "The plague spreads...\n");
            ZPInfectPlayer(INDEXENT(victim), false);
        }
    }

    // HEARTBEAT: steady thump for the last human under pressure
    if (gpGlobals->time >= s_heartbeatUntil) {
        s_heartbeatUntil = gpGlobals->time + 1.2f;
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* e = INDEXENT(i);
            if (!ZPIsPlayerConnected(e) || !g_players[i].lastHuman) continue;
            if (e->v.health <= 0 || !ZPIsHuman(e)) continue;
            EMIT_SOUND(e, CHAN_AUTO, "player/heartbeat1.wav", 1.0, ATTN_NORM);
            break;
        }
    }
}

// ---- event modifiers ----

float ZPFeatureClawMultiplier(void) {
    if (g_round.state != RS_ACTIVE) return 1.0f;
    if (g_round.eventType == ZEV_ONE_HIT) return 200.0f;       // one claw = dead
    if (g_round.eventType == ZEV_DOUBLE_DAMAGE) return 2.0f;
    return 1.0f;
}

float ZPFeatureGravity(void) {
    return (g_round.state == RS_ACTIVE && g_round.eventType == ZEV_LOW_GRAVITY) ? 0.5f : 1.0f;
}

float ZPFeatureSpeedMultiplier(void) {
    return (g_round.state == RS_ACTIVE && g_round.eventType == ZEV_SPEED) ? 35.0f : 0.0f;
}

int ZPFeatureInitialArmor(void) {
    return g_round.eventType == ZEV_ARMOR ? 100 : 0;
}

float ZPFeatureRoundDuration(void) {
    return g_round.suddenDeathActive ? 240.0f : 480.0f;
}

// ---- stat hooks (used by the rest of the ZP module) ----

// personal on-screen streak reward (mirrors ZPKillReward's style)
static void ZPStreakHud(edict_t* player, const char* msg, int r, int g, int b) {
    if (!player || !msg) return;
    hudtextparms_t params;
    memset(&params, 0, sizeof(params));
    params.channel = 6;
    params.x = -1;
    params.y = 0.35f;
    params.r1 = r; params.g1 = g; params.b1 = b;
    params.a1 = 255;
    params.fadeinTime = 0.1f;
    params.fadeoutTime = 0.5f;
    params.holdTime = 1.5f;
    UTIL_HudMessage(CBaseEntity::Instance(player), params, msg);
}

// kill streak: +50 HP at 3, +75 HP + ammo at 5, +125 HP + adr at 10
void ZPFeatureOnKill(edict_t* killer, bool fromHeadshot) {
    if (!killer) return;
    int idx = ENTINDEX(killer);
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    if (g_round.state != RS_ACTIVE) return;

    g_players[idx].killStreak++;
    if (ZPIsZombie(killer))
        g_players[idx].zombieKills++;
    if (fromHeadshot)
        g_players[idx].headshots++;

    int streak = g_players[idx].killStreak;
    if (streak != 3 && streak != 5 && streak != 10) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(killer);
    if (!pPlayer || !pPlayer->IsAlive()) return;

    float hp = 0;
    const char* sfx = "items/smallmedkit1.wav";
    const char* tag = "";
    int r = 0, g = 255, b = 90;

    switch (streak) {
        case 3:
            hp = 50.0f;
            tag = "KILL STREAK x3  +50 HP";
            sfx = "items/smallmedkit1.wav";
            break;
        case 5:
            hp = 75.0f;
            tag = "KILL STREAK x5  +75 HP + AMMO";
            sfx = "items/suitcharge1.wav";
            r = 80; g = 200; b = 255;
            pPlayer->GiveNamedItem("ammo_9mmclip");
            break;
        case 10:
            hp = 125.0f;
            tag = "10 KILL STREAK!  +125 HP + ADRENALINE";
            sfx = "zpmod/round_start_boss.wav";
            r = 255; g = 215; b = 0;
            g_players[idx].adrenalineUntil = gpGlobals->time + 6.0f;
            break;
    }

    pPlayer->pev->health += hp;
    if (pPlayer->pev->health > 200.0f)
        pPlayer->pev->health = 200.0f;

    EMIT_SOUND(killer, CHAN_ITEM, sfx, 1.0, ATTN_NORM);
    UTIL_ScreenFade(pPlayer, Vector((float)r, (float)g, (float)b), 0.2f, 0.1f, 255, FFADE_IN);
    ZPStreakHud(killer, tag, r, g, b);
}

// infection streak: +150 HP at 3, +300 HP at 5 for the infecting zombie
void ZPFeatureOnInfect(edict_t* victim, int infectorIndex) {
    if (!victim) return;
    int v = ENTINDEX(victim);
    if (v < 1 || v > gpGlobals->maxClients) return;

    // the victim died as a human this life
    g_players[v].infectStreak = 0;
    g_players[v].roundsSurvived = 0;

    if (infectorIndex >= 1 && infectorIndex <= gpGlobals->maxClients &&
        ZPIsZombie(INDEXENT(infectorIndex)))
        g_players[infectorIndex].infectStreak++;

    if (g_round.state != RS_ACTIVE || infectorIndex < 1 || infectorIndex > gpGlobals->maxClients)
        return;

    edict_t* infector = INDEXENT(infectorIndex);
    if (!ZPIsPlayerConnected(infector) || !ZPIsZombie(infector))
        return;

    int streak = g_players[infectorIndex].infectStreak;
    if (streak != 3 && streak != 5) return;

    float hp = (streak == 3) ? 150.0f : 300.0f;
    infector->v.health += hp;

    const char* sfx = (streak == 5) ? "zpmod/round_start_boss.wav" : "zpmod/round_start.wav";
    const char* tag = (streak == 5) ? "INFECT STREAK x5  +300 HP" : "INFECT STREAK x3  +150 HP";
    int r = (streak == 5) ? 255 : 255;
    int g = (streak == 5) ? 215 : 90;
    int b = (streak == 5) ? 0 : 90;

    EMIT_SOUND(infector, CHAN_ITEM, sfx, 1.0, ATTN_NORM);
    ZPStreakHud(infector, tag, r, g, b);
}

void ZPFeatureOnDied(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].killStreak = 0;
    g_players[idx].infectStreak = 0;
    g_players[idx].deathCount++;
}

void ZPFeatureLastHuman(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].lastHumanCount++;

    // the buff is handed out once per round
    if (g_players[idx].lastHumanBuffGiven)
        return;
    g_players[idx].lastHumanBuffGiven = true;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer || !pPlayer->IsAlive()) return;

    // armor: full HEV suit
    player->v.armorvalue = 100;
    player->v.armortype = 0.5f;

    // adrenaline: speed surge on top of the built-in last-human speed
    g_players[idx].adrenalineUntil = gpGlobals->time + 8.0f;

    // weapon: upgrade to the assault rifle so the last stand can actually shoot
    pPlayer->GiveNamedItem("weapon_9mmAR");
    pPlayer->GiveNamedItem("ammo_9mmclip");
    pPlayer->GiveNamedItem("ammo_9mmclip");
    pPlayer->SelectItem("weapon_9mmAR");

    // music sting (one burst per round)
    if (g_round.lastHumanMusicUntil < gpGlobals->time) {
        g_round.lastHumanMusicUntil = gpGlobals->time + 45.0f;
        EMIT_SOUND(player, CHAN_AUTO, "zpmod/round_start.wav", 1.0, ATTN_NORM);
    }

    // screen FX: golden flash + last-stand aura so everyone can spot the survivor
    UTIL_ScreenFade(pPlayer, Vector(255, 215, 0), 0.3f, 0.2f, 255, FFADE_IN);
    UTIL_ScreenShake(player->v.origin, 6.0f, 3.0f, 0.6f, 256.0f);
    player->v.renderfx = kRenderFxGlowShell;
    player->v.rendercolor = Vector(255, 215, 0);
    player->v.renderamt = 35;

    // notify all zombies who the last human is
    const char* name = STRING(player->v.netname);
    if (!name || !name[0]) name = "player";
    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* e = INDEXENT(i);
        if (!ZPIsPlayerConnected(e) || !ZPIsZombie(e)) continue;

        hudtextparms_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.channel = 1;
        tp.x = -1;
        tp.y = 0.30f;
        tp.r1 = 255; tp.g1 = 40; tp.b1 = 40;
        tp.a1 = 255;
        tp.fadeinTime = 0.2f;
        tp.fadeoutTime = 0.4f;
        tp.holdTime = 4.0f;

        char huntMsg[80];
        snprintf(huntMsg, sizeof(huntMsg), "HUNT THE LAST HUMAN:\n%s", name);
        UTIL_HudMessage(CBaseEntity::Instance(e), tp, huntMsg);
    }
}

void ZPFeatureOnInfectVictim(edict_t* victim, int infectorIndex) {
    // hook point for the freshly infected victim; stat tracking happens
    // through ZPFeatureOnInfect so this intentionally stays lightweight
    (void)victim;
    (void)infectorIndex;
}

void ZPFeaturePlayerDisconnect(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].steerMode = ZEV_NONE;
    g_players[idx].bossRoundStart = false;
}