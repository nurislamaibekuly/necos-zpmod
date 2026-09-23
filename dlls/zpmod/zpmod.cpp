#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "zpmod.h"
#include "player.h"
#include "weapons.h"
#include "decals.h"
#include "eiface.h"
#include "gamerules.h"
#include "shake.h"

// #include "zpmod.h" // TODO: eat ass

ZPRound g_round;
TraceResult tr;
int lastSpokeSecond;
ZPPlayer g_players[33];
ZPMapVote g_mapVote;

extern int gmsgShowMenu;
extern int gmsgSayText;
extern int gmsgTextMsg;

void ZPRoundWinSound(edict_t* player, const char* winSound, const char* ambientPrefix, int ambientCount);

int RoleToInt(Role r) {
    switch(r) {
        case ROLE_HUMAN: return 1;
        case ROLE_ZOMBIE: return 2;
        case ROLE_SPECTATOR: return 3;
        default: return 0;
    }
}

const char* NumberWord(int n) {
    switch (n) {
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 6: return "six";
        case 7: return "seven";
        case 8: return "eight";
        case 9: return "nine";
        case 10: return "ten";
        default: return "";
    }
}

bool ZPIsZombie(edict_t* player) {
    if (!player) return false;
    return (player->v.team == RoleToInt(ROLE_ZOMBIE));
}

bool ZPIsHuman(edict_t* player) {
    if (!player) return false;
    if (player->v.team == RoleToInt(ROLE_ZOMBIE)) return false;
    if (player->v.team == RoleToInt(ROLE_SPECTATOR)) return false;
    return true;
}

bool ZPIsDead(edict_t* player) {
    if (!player) return true;
    return (player->v.team == RoleToInt(ROLE_SPECTATOR));
}

bool ZPIsPlayerConnected(edict_t* player) {
    if (!player || player->free) return false;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return false;
    if (g_players[idx].ed != player) return false;
    if (!(player->v.flags & FL_CLIENT)) return false;
    return true;
}

int ZPCountConnectedPlayers(void) {
    int count = 0;
    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (ZPIsPlayerConnected(ed)) {
            count++;
        }
    }
    return count;
}

void ZPRoundResetPlayer(edict_t* ed) {
    if (!ZPIsPlayerConnected(ed)) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
    if (!pPlayer) return;

    int idx = ENTINDEX(ed);
    g_players[idx].ZMClass = ZM_CLASS_REGULAR;
    g_players[idx].isFrozen = false;
    g_players[idx].lastHuman = false;
    g_players[idx].killedByHeadshot = false;
    g_players[idx].kills = 0;
    g_players[idx].infections = 0;
    g_players[idx].headshots = 0;
    g_players[idx].lastInfectKiller = 0;
    g_players[idx].lastInfectTime = 0.0f;
    g_players[idx].chargeCooldown = 0.0f;
    g_players[idx].beamCooldown = 0.0f;
    g_players[idx].clawSwing = 0;
    g_players[idx].bossRoundStart = false;
    g_players[idx].lastHumanBuffGiven = false;
    g_players[idx].killStreak = 0;
    g_players[idx].infectStreak = 0;
    g_players[idx].healCooldown = 0.0f;
    g_players[idx].adrenalineCooldown = 0.0f;
    g_players[idx].adrenalineUntil = 0.0f;
    g_players[idx].frostCooldown = 0.0f;
    g_players[idx].abilityMenuUntil = 0.0f;
    g_players[idx].frozenUntil = 0.0f;
    g_players[idx].aimTarget = 0;
    g_players[idx].noclip = false;

    // clear any class glow/freeze tint from the previous round
    ed->v.renderfx = kRenderFxNone;
    ed->v.rendercolor = Vector(0, 0, 0);
    ed->v.movetype = MOVETYPE_WALK;

    pPlayer->m_iHideHUD = 0;
    pPlayer->pev->iuser1 = 0;
    pPlayer->pev->iuser2 = 0;
    pPlayer->m_hObserverTarget = NULL;
    pPlayer->m_afPhysicsFlags &= ~PFLAG_OBSERVER;

    pPlayer->RemoveAllItems(false);
    ed->v.team = RoleToInt(ROLE_HUMAN);
    ed->v.health = 100;

    pPlayer->pev->flags &= ~FL_SPECTATOR;
    pPlayer->pev->effects &= ~EF_NODRAW;
    pPlayer->pev->deadflag = DEAD_NO;
    pPlayer->pev->gravity = 1.0f;

    CBaseEntity* spawn = UTIL_FindEntityByClassname(nullptr, "info_player_start");
    if (spawn) {
        pPlayer->pev->origin = spawn->pev->origin;
        pPlayer->pev->angles = spawn->pev->angles;
    }
    pPlayer->Spawn();

    if (g_players[idx].originalModel[0]) {
        const char* resetModel = g_players[idx].originalModel;
        if (stricmp(resetModel, "zm") == 0)
            resetModel = "helmet";
        ZPSetPlayerModel(ed, resetModel);
    }

    // every human spawns with the ZP throwables (only humans get them;
    // zombies are stripped of everything when infected)
    pPlayer->GiveNamedItem("weapon_molotov");
    pPlayer->GiveNamedItem("weapon_freezebomb");

    ZP_Trace("ZPRoundResetPlayer gave molotov+freezebomb to slot %d->%d (team=%s weapons=0x%X)\n",
             ed->v.team, ENTINDEX(ed), ed->v.team == RoleToInt(ROLE_HUMAN) ? "human" : "other",
             (unsigned int)ed->v.weapons);
    ALERT(at_console, "ZPDEBUG: gave molotov+freezebomb to slot %d->%d (team=%s weapons=0x%X)\n",
          ed->v.team, ENTINDEX(ed), ed->v.team == RoleToInt(ROLE_HUMAN) ? "human" : "other",
          (unsigned int)ed->v.weapons);
}

static bool joinInProgress = false;

void ZPPlayerJoin(edict_t* player) {
    if (!player) return;
    if (joinInProgress) return;
    joinInProgress = true;

    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) {
        joinInProgress = false;
        return;
    }

    g_players[idx].ed = player;
    g_players[idx].ZMClass = ZM_CLASS_REGULAR;

    char modelName[64] = "player"; // def player model btw

    const char* curModel = g_engfuncs.pfnInfoKeyValue(g_engfuncs.pfnGetInfoKeyBuffer(player), "model");
    if (curModel && curModel[0]) {
        strncpy(modelName, curModel, sizeof(modelName) - 1);
        modelName[sizeof(modelName) - 1] = '\0';
    } else if (player->v.netname) {
        const char* curModelPev = STRING(player->v.model);
        if (curModelPev && curModelPev[0]) {
            strncpy(modelName, curModelPev, sizeof(modelName) - 1);
            modelName[sizeof(modelName) - 1] = '\0';
        }
    }

    strncpy(g_players[idx].originalModel, modelName, sizeof(g_players[idx].originalModel) - 1);
    g_players[idx].originalModel[sizeof(g_players[idx].originalModel) - 1] = '\0';

    g_round.notEnoughPlayersPrinted = false;

    player->v.team = RoleToInt(ROLE_HUMAN);

    if (g_round.state == RS_ACTIVE) {
        CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
        if (pPlayer) {
            pPlayer->pev->deadflag = DEAD_DEAD;
            player->v.team = RoleToInt(ROLE_SPECTATOR);
            pPlayer->pev->health = 0;
            pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->angles);
        }
    }

    joinInProgress = false;
}

void ZPPlayerDisconnect(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    ZPFeaturePlayerDisconnect(player);
    ZPStatsPlayerDisconnect(player);

    g_players[idx].ed = nullptr;
    g_players[idx].originalModel[0] = '\0';
    g_players[idx].ZMClass = ZM_CLASS_REGULAR;
    player->v.health = 0;
    player->v.team = 0;

    int connected = ZPCountConnectedPlayers();
    if (connected < 2) {
        g_round.state = RS_PREP;
        g_round.countdownStarted = false;
        g_round.lastAnnounce = -1;
        lastSpokeSecond = -1;
        ZPRoundStopAmbient();

        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* ed = INDEXENT(i);
            if (ZPIsPlayerConnected(ed)) {
                ZPRoundResetPlayer(ed);
            }
        }

        if (!g_round.notEnoughPlayersPrinted) {
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Not enough players to start the infection\n");
            g_round.notEnoughPlayersPrinted = true;
        }
    }
}

void ZPThunderStrike(edict_t* player) {
    if (!player) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;

    Vector head = pPlayer->pev->origin + pPlayer->pev->view_ofs;
    if (head.z < -9000) return;

    // main jagged bolt from the sky down to the head
    Vector sky1 = head + Vector(RANDOM_FLOAT(-80, 80), RANDOM_FLOAT(-80, 80), 800);
    MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, head);
        WRITE_BYTE(TE_BEAMPOINTS);
        WRITE_COORD(sky1.x); WRITE_COORD(sky1.y); WRITE_COORD(sky1.z);
        WRITE_COORD(head.x); WRITE_COORD(head.y); WRITE_COORD(head.z);
        WRITE_SHORT(MODEL_INDEX("sprites/lgtning.spr"));
        WRITE_BYTE(0);    // frame
        WRITE_BYTE(0);    // framerate
        WRITE_BYTE(8);    // life (0.8s)
        WRITE_BYTE(14);   // width
        WRITE_BYTE(80);   // noise -> lightning jags
        WRITE_BYTE(255);  // r
        WRITE_BYTE(255);  // g
        WRITE_BYTE(255);  // b
        WRITE_BYTE(255);  // brightness
        WRITE_BYTE(100);  // speed
    MESSAGE_END();

    // secondary thinner bolt offset for variety
    Vector sky2 = head + Vector(RANDOM_FLOAT(-240, 240), RANDOM_FLOAT(-240, 240), 700);
    Vector tip2 = head + Vector(RANDOM_FLOAT(-30, 30), RANDOM_FLOAT(-30, 30), 40);
    MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, head);
        WRITE_BYTE(TE_BEAMPOINTS);
        WRITE_COORD(sky2.x); WRITE_COORD(sky2.y); WRITE_COORD(sky2.z);
        WRITE_COORD(tip2.x); WRITE_COORD(tip2.y); WRITE_COORD(tip2.z);
        WRITE_SHORT(MODEL_INDEX("sprites/lgtning.spr"));
        WRITE_BYTE(0);    // frame
        WRITE_BYTE(0);    // framerate
        WRITE_BYTE(6);    // life (0.6s)
        WRITE_BYTE(8);    // width
        WRITE_BYTE(60);   // noise
        WRITE_BYTE(255);  // r
        WRITE_BYTE(210);  // g
        WRITE_BYTE(80);   // b
        WRITE_BYTE(220);  // brightness
        WRITE_BYTE(80);   // speed
    MESSAGE_END();

    // white flash at the struck head
    Vector headPos = head;
    MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, head);
        WRITE_BYTE(TE_DLIGHT);
        WRITE_COORD(headPos.x); WRITE_COORD(headPos.y); WRITE_COORD(headPos.z);
        WRITE_BYTE(30);   // radius
        WRITE_BYTE(255);  // r
        WRITE_BYTE(255);  // g
        WRITE_BYTE(255);  // b
        WRITE_BYTE(8);    // time (0.8s)
        WRITE_BYTE(5);    // decay
    MESSAGE_END();

    // brief white screen flash for the victim
    Vector white = Vector(255, 255, 255);
    UTIL_ScreenFade(pPlayer, white, 0.15f, 0.05f, 255, FFADE_IN);
}

const char* ZMClassName(int cls) {
    switch (cls) {
        case ZM_CLASS_FAST:   return "FAST ZOMBIE";
        case ZM_CLASS_TANK:   return "TANK ZOMBIE";
        case ZM_CLASS_JUMPER: return "JUMPER ZOMBIE";
        case ZM_CLASS_BOSS:   return "BOSS ZOMBIE";
        default:              return "ZOMBIE";
    }
}

// applies per-class speed/gravity/render every frame for zombies so nothing
// can override them; frozen zombies get locked in place with a blue tint
void ZPApplyZombieClass(edict_t* player)
{
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    int cls = g_players[idx].ZMClass;

    float speed = 240.0f, gravity = 0.8f;
    switch (cls) {
        case ZM_CLASS_FAST:    speed = 320.0f; gravity = 0.8f;  break;
        case ZM_CLASS_TANK:    speed = 205.0f; gravity = 1.0f;  break;
        case ZM_CLASS_JUMPER:  speed = 250.0f; gravity = 0.42f; break;
        case ZM_CLASS_BOSS:    speed = 240.0f; gravity = 0.8f;  break;
        default:               speed = 240.0f; gravity = 0.8f;  break;
    }

    if (g_players[idx].frozenUntil > gpGlobals->time) {
        player->v.movetype = MOVETYPE_NONE;
        player->v.velocity = Vector(0, 0, 0);
        player->v.gravity = 0.0f;
        player->v.renderfx = kRenderFxGlowShell;
        player->v.rendercolor = Vector(120, 200, 255);
        player->v.renderamt = 70;
        return;
    }

    if (g_players[idx].noclip) {
        player->v.movetype = MOVETYPE_NOCLIP;
        player->v.gravity = 0.0f;
        player->v.renderfx = kRenderFxGlowShell;
        player->v.renderamt = cls == ZM_CLASS_BOSS ? 80 : cls == ZM_CLASS_TANK ? 70 : 45;
        switch (cls) {
            case ZM_CLASS_FAST:    player->v.rendercolor = Vector(30, 220, 255);  break;
            case ZM_CLASS_TANK:    player->v.rendercolor = Vector(255, 140, 20);  break;
            case ZM_CLASS_JUMPER:  player->v.rendercolor = Vector(80, 255, 70);   break;
            case ZM_CLASS_BOSS:    player->v.rendercolor = Vector(255, 15, 30);   break;
            default:               player->v.rendercolor = Vector(255, 40, 40);   break;
        }
        return;
    }

    player->v.movetype = MOVETYPE_WALK;
    player->v.maxspeed = speed + ZPFeatureSpeedMultiplier();
    player->v.gravity = gravity * ZPFeatureGravity();
    player->v.renderfx = kRenderFxGlowShell;
    player->v.renderamt = cls == ZM_CLASS_BOSS ? 80 : cls == ZM_CLASS_TANK ? 70 : 45;
    switch (cls) {
        case ZM_CLASS_FAST:    player->v.rendercolor = Vector(30, 220, 255);  break;
        case ZM_CLASS_TANK:    player->v.rendercolor = Vector(255, 140, 20);  break;
        case ZM_CLASS_JUMPER:  player->v.rendercolor = Vector(80, 255, 70);   break;
        case ZM_CLASS_BOSS:    player->v.rendercolor = Vector(255, 15, 30);   break;
        default:               player->v.rendercolor = Vector(255, 40, 40);   break;
    }
}

// shared overlay for humans (admin noclip / freeze) so movement stays stable
// during an active round without stomping the class logic
void ZPApplyHumanOverlay(edict_t* player)
{
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    if (g_players[idx].frozenUntil > gpGlobals->time) {
        player->v.movetype = MOVETYPE_NONE;
        player->v.velocity = Vector(0, 0, 0);
        player->v.gravity = 0.0f;
        return;
    }

    if (g_players[idx].noclip) {
        player->v.movetype = MOVETYPE_NOCLIP;
        player->v.gravity = 0.0f;
        return;
    }

    player->v.movetype = MOVETYPE_WALK;
    player->v.gravity = 1.0f * ZPFeatureGravity();
}

void ZPInfectPlayer(edict_t* player, bool wasInfectedBySomeone) {
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].ZMClass = ZM_CLASS_REGULAR;

    bool forceBoss = g_players[idx].bossRoundStart;
    g_players[idx].bossRoundStart = false;

    int r = RANDOM_LONG(1, 12);
    
    if (r == 6 && wasInfectedBySomeone == false) {
        g_players[idx].ZMClass = ZM_CLASS_BOSS;
        EMIT_SOUND(player, CHAN_AUTO, "ambience/the_horror3.wav", 1.0, ATTN_NONE);
    } else if (forceBoss && !wasInfectedBySomeone) {
        g_players[idx].ZMClass = ZM_CLASS_BOSS;
        EMIT_SOUND(player, CHAN_AUTO, "ambience/the_horror3.wav", 1.0, ATTN_NONE);
    } else {
        r = RANDOM_LONG(1, 4);
        if (r == 1) g_players[idx].ZMClass = ZM_CLASS_FAST;
        else if (r == 2) g_players[idx].ZMClass = ZM_CLASS_TANK;
        else if (r == 3) g_players[idx].ZMClass = ZM_CLASS_JUMPER;
        else g_players[idx].ZMClass = ZM_CLASS_REGULAR;

        r = RANDOM_LONG(1, 2);
        if (r == 1) {
            EMIT_SOUND(player, CHAN_AUTO, "zpmod/coming_1.wav", 1.0, 0.2f);
            EMIT_SOUND(player, CHAN_AUTO, "zpmod/human_death_1.wav", 1.0, ATTN_NORM);
        } else {
            EMIT_SOUND(player, CHAN_AUTO, "zpmod/coming_2.wav", 1.0, 0.2f);
            EMIT_SOUND(player, CHAN_AUTO, "zpmod/human_death_2.wav", 1.0, ATTN_NORM);
        }
    }

    // yay effects
    UTIL_ScreenShake(pPlayer->pev->origin, 10.0f, 5.0f, 1.0f, 512.0f);

    Vector red = Vector(255,0,0);
    UTIL_ScreenFade(pPlayer, red, 1.0f, 0.2f, 255, FFADE_IN);

    // zombie stuff
    pPlayer->pev->deadflag = DEAD_NO;

    float clsHp = 500.0f;
    switch (g_players[idx].ZMClass) {
        case ZM_CLASS_FAST:   clsHp = 350.0f; break;
        case ZM_CLASS_TANK:   clsHp = 900.0f; break;
        case ZM_CLASS_JUMPER: clsHp = 400.0f; break;
        case ZM_CLASS_BOSS:   clsHp = 4000.0f; break;
        default:              clsHp = 500.0f; break;
    }
    player->v.health = clsHp;

    ZPApplyZombieClass(player);

    hudtextparms_t params;
    memset(&params, 0, sizeof(params));

    char msg[64] = "The boss has awakened...";

    params.channel = 0;
    params.x = -1;
    params.y = 0.1f;
    params.r1 = 255; params.g1 = 0; params.b1 = 0;
    params.a1 = 255;
    params.fadeinTime = 0.3f;
    params.fadeoutTime = 0.3f;
    params.holdTime = 4.0f;
    if (g_players[idx].ZMClass == ZM_CLASS_BOSS) UTIL_HudMessageAll(params, msg);

    // class announce to the freshly infected player
    char clsMsg[48];
    snprintf(clsMsg, sizeof(clsMsg), "YOU ARE A %s", ZMClassName(g_players[idx].ZMClass));
    params.y = 0.15f;
    if (g_players[idx].ZMClass == ZM_CLASS_BOSS) {
        params.r1 = 255; params.g1 = 0; params.b1 = 255;
    } else if (g_players[idx].ZMClass == ZM_CLASS_TANK) {
        params.r1 = 255; params.g1 = 140; params.b1 = 20;
    } else if (g_players[idx].ZMClass == ZM_CLASS_JUMPER) {
        params.r1 = 80; params.g1 = 255; params.b1 = 70;
    } else if (g_players[idx].ZMClass == ZM_CLASS_FAST) {
        params.r1 = 30; params.g1 = 220; params.b1 = 255;
    } else {
        params.r1 = 255; params.g1 = 40; params.b1 = 40;
    }
    UTIL_HudMessage(CBaseEntity::Instance(player), params, clsMsg);

    // red infection burst — heavier for the boss
    UTIL_ParticleEffect(pPlayer->pev->origin + Vector(0, 0, 32), Vector(0, 0, 96), 235, g_players[idx].ZMClass == ZM_CLASS_BOSS ? 60 : 30);

    pPlayer->RemoveAllItems(false);
    pPlayer->GiveNamedItem("weapon_crowbar");
    pPlayer->SelectItem("weapon_crowbar");
    // infection bomb: zombies only, one per infection
    pPlayer->GiveNamedItem("weapon_infectionbomb");

    player->v.team = RoleToInt(ROLE_ZOMBIE);
    /* player->v.viewmodel = MAKE_STRING("models/zpmod/v_claws.mdl");
    player->v.weaponmodel = iStringNull; */
    pPlayer->pev->pain_finished = gpGlobals->time;

    ZPSetPlayerModel(player, "zm");

    // UTIL_Sparks(pPlayer->pev->origin);
    MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, pPlayer->pev->origin);
        WRITE_BYTE(TE_BLOODSPRITE);
        WRITE_COORD(pPlayer->pev->origin.x);
        WRITE_COORD(pPlayer->pev->origin.y);
        WRITE_COORD(pPlayer->pev->origin.z + 32);
        WRITE_SHORT(MODEL_INDEX("sprites/blood.spr"));
        WRITE_SHORT(MODEL_INDEX("sprites/blood.spr"));
        WRITE_BYTE(248);
        WRITE_BYTE(20);
    MESSAGE_END();

    ZPThunderStrike(player);
}

// admins can revert a zombie back to a human
void ZPMakeHuman(edict_t* ed) {
    if (!ed) return;
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
    if (!pPlayer) return;
    int idx = ENTINDEX(ed);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].ZMClass = ZM_CLASS_REGULAR;
    g_players[idx].frozenUntil = 0.0f;
    g_players[idx].lastHuman = false;
    g_players[idx].lastInfectKiller = 0;
    g_players[idx].bossRoundStart = false;
    g_players[idx].lastHumanBuffGiven = false;

    ed->v.team = RoleToInt(ROLE_HUMAN);
    ed->v.deadflag = DEAD_NO;
    ed->v.gravity = 1.0f;
    ed->v.maxspeed = 260.0f;
    ed->v.renderfx = kRenderFxNone;
    ed->v.rendercolor = Vector(0, 0, 0);
    ed->v.movetype = MOVETYPE_WALK;
    ed->v.health = 100;

    pPlayer->RemoveAllItems(false);
    pPlayer->GiveNamedItem("weapon_crowbar");
    pPlayer->GiveNamedItem("weapon_9mmhandgun");
    pPlayer->GiveNamedItem("ammo_9mmclip");
    pPlayer->SelectItem("weapon_9mmhandgun");

    if (g_players[idx].originalModel[0]) {
        const char* m = g_players[idx].originalModel;
        if (stricmp(m, "zm") == 0) m = "helmet";
        ZPSetPlayerModel(ed, m);
    }

    pPlayer->GiveNamedItem("weapon_molotov");
    pPlayer->GiveNamedItem("weapon_freezebomb");

    ZP_Trace("ZPMakeHuman gave molotov+freezebomb to %d (weapons=0x%X)\n",
             ENTINDEX(ed), (unsigned int)ed->v.weapons);
    ALERT(at_console, "ZPDEBUG: ZPMakeHuman gave molotov+freezebomb to %d (weapons=0x%X)\n",
          ENTINDEX(ed), (unsigned int)ed->v.weapons);

    UTIL_ScreenFade(pPlayer, Vector(0, 255, 120), 0.4f, 0.2f, 255, FFADE_IN);
}

// forces the current round to end with a chosen winner (0 = draw, 1 = humans, 2 = zombies)
void ZPForceRoundEnd(int winner) {
    if (g_round.state == RS_PREP) return;

    g_round.resetTime = gpGlobals->time + 5.0f;

    if (winner == 2) {
        g_round.state = RS_ZOMBIES_WIN;
        UTIL_ClientPrintAll(HUD_PRINTCENTER, "Zombies win\n");
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* ed = INDEXENT(i);
            if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                ZPRoundWinSound(ed, "zpmod/win_zombi.wav", "zpmod/win_zombi", 3);
        }
    } else if (winner == 1) {
        g_round.state = RS_HUMANS_WIN;
        UTIL_ClientPrintAll(HUD_PRINTCENTER, "Humans win\n");
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* ed = INDEXENT(i);
            if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                ZPRoundWinSound(ed, "zpmod/win_human.wav", "zpmod/win_human", 2);
        }
    } else {
        g_round.state = RS_ROUND_DRAW;
        UTIL_ClientPrintAll(HUD_PRINTCENTER, "Round draw\n");
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* ed = INDEXENT(i);
            if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                EMIT_SOUND(ed, CHAN_AUTO, "zpmod/round_draw.wav", 1.0, ATTN_NORM);
        }
    }

    ZPAnnounceMvp();
}

CBaseEntity* ZPZombieCheckHit(CBasePlayer* pPlayer, float range, TraceResult* ptr)
{
    UTIL_MakeVectors(pPlayer->pev->angles);
    Vector vecSrc = pPlayer->pev->origin + pPlayer->pev->view_ofs;
    Vector vecEnd = vecSrc + gpGlobals->v_forward * range;

    // trace that hits monsters players too
    UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, pPlayer->edict(), ptr);

    if (ptr->flFraction < 1.0f && ptr->pHit && ENTINDEX(ptr->pHit) != 0) {
        return CBaseEntity::Instance(ptr->pHit); // not world
    }
    return nullptr; // world or nothing
}

bool ZPDied(edict_t* player, int attackerIndex) {
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return false;

    ZPFeatureOnDied(player);
    ZPStatsOnDied(player);

    // only a zombie's killing blow converts a human — suicides, fall damage
    // and teamkills are real deaths
    if (g_round.state == RS_ACTIVE && ZPIsHuman(player)) {
        if (attackerIndex >= 1 && attackerIndex <= gpGlobals->maxClients) {
            edict_t* attacker = INDEXENT(attackerIndex);
            if (ZPIsPlayerConnected(attacker) && ZPIsZombie(attacker)) {
                ZPInfectPlayer(player, true);
                ZPSendInfection(player, attackerIndex);
                ZPOnInfect(player, attackerIndex);
                return true;
            }
        }
    }

    if (ZPIsZombie(player)) {
        int r = RANDOM_LONG(1, 2);
        if (r == 1) EMIT_SOUND(player, CHAN_AUTO, "zpmod/death_1.wav", 1.0, ATTN_NORM);
        else if (r == 2) EMIT_SOUND(player, CHAN_AUTO, "zpmod/death_2.wav", 1.0, ATTN_NORM);

        bool headshot = g_players[ENTINDEX(player)].killedByHeadshot;
        g_players[ENTINDEX(player)].killedByHeadshot = false;

        if (attackerIndex >= 1 && attackerIndex <= gpGlobals->maxClients) {
            edict_t* killer = INDEXENT(attackerIndex);
            if (ZPIsPlayerConnected(killer) && ZPIsHuman(killer)) {
                ZPKillReward(killer, headshot);
                ZPFeatureOnKill(killer, headshot);
                ZPStatsOnKill(killer, headshot);

                // headshot blast: white flash + sparks at the brain
                if (headshot) {
                    Vector head = pPlayer->pev->origin + Vector(0, 0, 36);
                    UTIL_Sparks(head);
                    MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, head);
                        WRITE_BYTE(TE_DLIGHT);
                        WRITE_COORD(head.x); WRITE_COORD(head.y); WRITE_COORD(head.z);
                        WRITE_BYTE(32);   // radius
                        WRITE_BYTE(255);  // r
                        WRITE_BYTE(255);  // g
                        WRITE_BYTE(255);  // b
                        WRITE_BYTE(10);   // life
                        WRITE_BYTE(6);    // decay
                    MESSAGE_END();
                    UTIL_ScreenFade((CBaseEntity*)GET_PRIVATE(killer), Vector(255, 255, 255), 0.2f, 0.1f, 255, FFADE_IN);
                }
            }
        }
    }

    pPlayer->pev->deadflag = DEAD_DEAD;
    player->v.team = RoleToInt(ROLE_SPECTATOR);
    pPlayer->pev->health = 0;
    pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->angles);

    return false;
}

void ZPHurt(edict_t* player) {
    if (ZPIsZombie(player)) {
        int r = RANDOM_LONG(1, 2);
        if (r == 1) EMIT_SOUND(player, CHAN_AUTO, "zpmod/hurt_1.wav", 1.0, ATTN_NORM);
        else if (r == 2) EMIT_SOUND(player, CHAN_AUTO, "zpmod/hurt_2.wav", 1.0, ATTN_NORM);
    }
}

void ZPZombieSwing(edict_t* player)
{
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    if (g_players[idx].frozenUntil > gpGlobals->time) return;

    // play the claw swing: v_claws.mdl sequences 3-8 are the attack anims,
    // and the body needs a swing anim too (the crafting crowbar swing is
    // skipped entirely for zombies, so do it here)
    static const int kClawAttacks[] = { 3, 4, 5, 6, 7, 8 };
    int seq = kClawAttacks[g_players[idx].clawSwing % 6];
    g_players[idx].clawSwing++;

    pPlayer->pev->weaponanim = seq;
    MESSAGE_BEGIN(MSG_ONE, SVC_WEAPONANIM, NULL, player);
        WRITE_BYTE(seq);
        WRITE_BYTE(0);
    MESSAGE_END();
    pPlayer->SetAnimation(PLAYER_ATTACK1);

    TraceResult tr; 
    CBaseEntity* pHit = ZPZombieCheckHit(pPlayer, 70.0f, &tr);

    if (pHit && pHit->IsPlayer() && ZPIsHuman(pHit->edict())) {
        // ReZombie-style claw: deals melee damage per hit (class-dependent).
        // Armor absorbs part of it; a killing blow from a zombie converts the
        // victim (handled in ZPDied).
        float dmg = 40.0f * ZPFeatureClawMultiplier();
        switch (g_players[idx].ZMClass) {
            case ZM_CLASS_FAST:   dmg = 30.0f * ZPFeatureClawMultiplier(); break;
            case ZM_CLASS_TANK:   dmg = 55.0f * ZPFeatureClawMultiplier(); break;
            case ZM_CLASS_JUMPER: dmg = 32.0f * ZPFeatureClawMultiplier(); break;
            case ZM_CLASS_BOSS:   dmg = 60.0f * ZPFeatureClawMultiplier(); break;
            default:              dmg = 40.0f * ZPFeatureClawMultiplier(); break;
        }
        pHit->TakeDamage(pPlayer->pev, pPlayer->pev, dmg, DMG_SLASH);

        // splash blood at the wound so a connecting hit reads as a hit
        int bloodColor = pHit->BloodColor();
        UTIL_BloodDrips(tr.vecEndPos, gpGlobals->v_forward, bloodColor, (int)dmg);
        UTIL_BloodDecalTrace(&tr, bloodColor);

        // reward for landing a hit that converted the human
        if (ZPIsZombie(pHit->edict()))
            pPlayer->pev->health += 100.0f;

        // fuckass sounds
        int r = RANDOM_LONG(1, 3);
        if (r == 1) EMIT_SOUND(player, CHAN_AUTO, "zpmod/attack_1.wav", 1.0, ATTN_NORM);
        else if (r == 2) EMIT_SOUND(player, CHAN_AUTO, "zpmod/attack_2.wav", 1.0, ATTN_NORM);
        else EMIT_SOUND(player, CHAN_AUTO, "zpmod/attack_3.wav", 1.0, ATTN_NORM);
    }
    else if (tr.flFraction < 1.0f && tr.pHit && tr.pHit == ENT(0)) {
        // me when wall :3
        int r = RANDOM_LONG(1, 3);
        if (r == 1) EMIT_SOUND(player, CHAN_AUTO, "zpmod/wall_1.wav", 1.0, ATTN_NORM);
        else if (r == 2) EMIT_SOUND(player, CHAN_AUTO, "zpmod/wall_2.wav", 1.0, ATTN_NORM);
        else EMIT_SOUND(player, CHAN_AUTO, "zpmod/wall_3.wav", 1.0, ATTN_NORM);

        // draw some cool motherfucking decals for example idk fuck this bullshit
        UTIL_DecalTrace(&tr, DECAL_GUNSHOT1);
    }
    else {
        // clean miss: still give the swing a woosh
        int r = RANDOM_LONG(1, 3);
        if (r == 1) EMIT_SOUND(player, CHAN_AUTO, "zpmod/attack_1.wav", 1.0, ATTN_NORM);
        else if (r == 2) EMIT_SOUND(player, CHAN_AUTO, "zpmod/attack_2.wav", 1.0, ATTN_NORM);
        else EMIT_SOUND(player, CHAN_AUTO, "zpmod/attack_3.wav", 1.0, ATTN_NORM);
    }
}

void ZPCountTeams(int& humans, int& zombies)
{
    humans = 0;
    zombies = 0;

    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t* e = INDEXENT(i);
        if (!ZPIsPlayerConnected(e)) continue;

        CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(e);
        if (!pPlayer || !pPlayer->IsPlayer() || !pPlayer->IsAlive())
            continue;

        if (ZPIsZombie(e)) zombies++;
        else if (ZPIsHuman(e)) humans++;
    }
}

// shows the name and health of the player under the crosshair, centered a bit below the middle
// color depends on the target's role: zombie red, boss magenta, human green, spectator gray
void ZPPlayerAimDisplay(edict_t* player) {
    if (!player) return;

    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer || !pPlayer->IsPlayer()) return;

    int target = 0;

    if (pPlayer->IsAlive()) {
        UTIL_MakeVectors(pPlayer->pev->v_angle);
        Vector eye = pPlayer->pev->origin + pPlayer->pev->view_ofs;
        Vector end = eye + gpGlobals->v_forward * 512.0f;

        TraceResult tr;
        UTIL_TraceLine(eye, end, dont_ignore_monsters, player, &tr);

        if (!FNullEnt(tr.pHit)) {
            CBaseEntity* pHit = CBaseEntity::Instance(tr.pHit);
            if (pHit && pHit->IsPlayer()) {
                int t = ENTINDEX(tr.pHit);
                if (t >= 1 && t <= gpGlobals->maxClients && t != idx && ZPIsPlayerConnected(INDEXENT(t)))
                    target = t;
            }
        }
    }

    int prev = g_players[idx].aimTarget;
    if (target == 0 && prev == 0)
        return;

    g_players[idx].aimTarget = target;

    hudtextparms_t aim;
    memset(&aim, 0, sizeof(aim));
    aim.channel = 0;
    aim.x = -1;
    aim.y = 0.55f;
    aim.r1 = aim.g1 = aim.b1 = 255;
    aim.a1 = 255;
    aim.fadeinTime = 0;
    aim.fadeoutTime = 0;
    aim.holdTime = 0.5f;

    if (target == 0) {
        UTIL_HudMessage(CBaseEntity::Instance(player), aim, "");
        return;
    }

    edict_t* ed = INDEXENT(target);
    int hp = (int)ed->v.health;
    if (hp < 0) hp = 0;

    if (ZPIsZombie(ed)) {
        if (g_players[target].ZMClass == ZM_CLASS_BOSS) {
            aim.r1 = 255; aim.g1 = 60; aim.b1 = 200;
        } else {
            aim.r1 = 255; aim.g1 = 40; aim.b1 = 40;
        }
    } else if (ed->v.team == RoleToInt(ROLE_SPECTATOR)) {
        aim.r1 = aim.g1 = aim.b1 = 190;
    } else {
        aim.r1 = 40; aim.g1 = 255; aim.b1 = 90;
    }

    const char* name = STRING(ed->v.netname);
    if (!name) name = "player";

    char buf[96];
    snprintf(buf, sizeof(buf), "%s\nHP  %d", name, hp);
    UTIL_HudMessage(CBaseEntity::Instance(player), aim, buf);
}

void ZPHUD()
{
    int h, z;
    ZPCountTeams(h, z);

    hudtextparms_t params;
    memset(&params, 0, sizeof(params));
    params.fadeinTime = 0;
    params.fadeoutTime = 0;
    params.holdTime = 1.0f;

    // top center: round status / timer
    char top[128];
    switch (g_round.state) {
        case RS_PREP:
            if (g_round.countdownStarted && gpGlobals->time < g_round.nextStateTime) {
                int sec = (int)ceilf(g_round.nextStateTime - gpGlobals->time);
                snprintf(top, sizeof(top), "ROUND STARTS IN %02d:%02d  [%s]", sec / 60, sec % 60, ZPModeName(g_round.mode));
            } else {
                snprintf(top, sizeof(top), "ROUND PREPARING");
            }
            break;
        case RS_ACTIVE: {
            int sec = (int)ceilf((g_round.roundStartTime + g_round.roundDuration) - gpGlobals->time);
            if (sec < 0) sec = 0;
            const char* ev = ZPEventName(g_round.eventType);
            if (ev[0])
                snprintf(top, sizeof(top), "TIME LEFT %02d:%02d  [%s / %s]", sec / 60, sec % 60, ZPModeName(g_round.mode), ev);
            else
                snprintf(top, sizeof(top), "TIME LEFT %02d:%02d  [%s]", sec / 60, sec % 60, ZPModeName(g_round.mode));
            break;
        }
        case RS_HUMANS_WIN:
            snprintf(top, sizeof(top), "HUMANS WIN");
            break;
        case RS_ZOMBIES_WIN:
            snprintf(top, sizeof(top), "ZOMBIES WIN");
            break;
        case RS_ROUND_DRAW:
            snprintf(top, sizeof(top), "ROUND DRAW");
            break;
        default:
            snprintf(top, sizeof(top), "ROUND PREPARING");
            break;
    }

    params.channel = 2;
    params.x = -1;
    params.y = 0.03f;
    params.a1 = 255;

    if (g_round.state == RS_HUMANS_WIN) {
        params.r1 = 0; params.g1 = 255; params.b1 = 0;
    } else if (g_round.state == RS_ZOMBIES_WIN) {
        params.r1 = 255; params.g1 = 0; params.b1 = 0;
    } else if (g_round.state == RS_ROUND_DRAW) {
        params.r1 = 255; params.g1 = 255; params.b1 = 255;
    } else {
        params.r1 = 255; params.g1 = 220; params.b1 = 0;
    }
    UTIL_HudMessageAll(params, top);

    // top left: team counters
    char humbuf[32];
    snprintf(humbuf, sizeof(humbuf), "HUMANS: %d", h);

    hudtextparms_t hum = params;
    hum.channel = 3;
    hum.x = 0.01f;
    hum.y = 0.06f;
    hum.r1 = 0; hum.g1 = 255; hum.b1 = 0;
    hum.a1 = 255;
    UTIL_HudMessageAll(hum, humbuf);

    char zombuf[32];
    snprintf(zombuf, sizeof(zombuf), "ZOMBIES: %d", z);

    hudtextparms_t zomb = params;
    zomb.channel = 4;
    zomb.x = 0.01f;
    zomb.y = 0.10f;
    zomb.r1 = 255; zomb.g1 = 0; zomb.b1 = 0;
    zomb.a1 = 255;
    UTIL_HudMessageAll(zomb, zombuf);

    // bottom left: per-player info
    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (!ZPIsPlayerConnected(ed)) continue;

        CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
        if (!pPlayer || !pPlayer->IsPlayer()) continue;

        int hp = (int)pPlayer->pev->health;
        if (hp < 0) hp = 0;
        const char* className = "Human";
        if (ZPIsZombie(ed)) {
            className = ZMClassName(g_players[i].ZMClass);
        }
        else if (ed->v.team == RoleToInt(ROLE_SPECTATOR)) className = "Spectator";

        char buf[80];
        snprintf(buf, sizeof(buf), "HP %d  |  %s", hp, className);

        hudtextparms_t info;
        memset(&info, 0, sizeof(info));
        info.channel = 5;
        info.x = 0.02f;
        info.y = 0.88f;
        info.a1 = 255;
        info.fadeinTime = 0;
        info.fadeoutTime = 0;
        info.holdTime = 1.0f;

        if (ed->v.team == RoleToInt(ROLE_SPECTATOR)) {
            info.r1 = 255; info.g1 = 150; info.b1 = 0;
        } else if (ZPIsZombie(ed)) {
            info.r1 = 255; info.g1 = 70; info.b1 = 70;
        } else {
            info.r1 = 0; info.g1 = 255; info.b1 = 120;
        }

        UTIL_HudMessage(CBaseEntity::Instance(ed), info, buf);

        ZPPlayerAimDisplay(ed);
    }
}

void ZPRoundInit(ZPRound* round) {
    round->state = RS_PREP;
    round->resetTime = 0.0f;
    round->nextStateTime = 0.0f;
    round->roundStartTime = 0.0f;
    round->roundDuration = 480.0f;
    round->lastAnnounce = -1;
    lastSpokeSecond = -1;
    round->countdownStarted = false;
    round->notEnoughPlayersPrinted = false;
    round->playersFrozen = false;
    round->lastHumanAnnounced = false;
    round->mode = ZMODE_CLASSIC;
    round->eventType = ZEV_NONE;
    round->eventAnnounced = 0.0f;
    round->suddenDeathActive = false;
    round->suddenDeathUntil = 0.0f;
    round->plagueNextInfectTime = false;
    round->plagueNextSpread = 0.0f;
    round->lastHumanMusicUntil = 0.0f;
    round->bossRound = false;
    round->ambientPlaying = false;
}

// every-round arena wipe: remove world clutter spawned during play
// (dropped/created weapons, ammo, item healthkits/batteries, weaponboxes
// and any leftover ZP grenades) so each round starts on a clean map.
// Blood pools and shell casings are client-side surface effects, so the
// server can't clear those - clients can drop their decal count with
// r_decals to reduce them.
void ZPCleanupWorld(void) {
    for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++) {
        edict_t* ed = INDEXENT(i);
        if (!ed || ed->free || FNullEnt(ed))
            continue;

        const char* cls = STRING(ed->v.classname);
        if (!cls || !cls[0])
            continue;

        if (!strncmp(cls, "weapon_", 7) ||
            !strncmp(cls, "ammo_", 5) ||
            !strncmp(cls, "item_", 5) ||
            !strncmp(cls, "weaponbox", 9) ||
            !strncmp(cls, "zp_grenade", 10) ||
            !strncmp(cls, "grenade", 7))
            UTIL_Remove(CBaseEntity::Instance(ed));
    }
}

void ZPRoundStartAmbient() {
    EMIT_AMBIENT_SOUND(
        ENT(0),
        Vector(0,0,0),
        "ambience/wind1.wav",
        1.0,
        ATTN_NONE,
        0,
        100
    );
    g_round.ambientPlaying = true;
}

void ZPRoundStopAmbient() {
    EMIT_AMBIENT_SOUND(
        ENT(0),
        Vector(0,0,0),
        "ambience/wind1.wav",
        0,
        ATTN_NONE,
        SND_STOP,
        100
    );
    g_round.ambientPlaying = false;
}

void ZPSetPlayerModel(edict_t* player, const char* modelName)
{
    if (!player || player->free || !modelName || !modelName[0])
        return;

    // modelName must be the bare folder name:
    // "zm" -> models/player/zm/zm.mdl
    const char* bareModel = modelName;

    // Update the player's model userinfo.
    // This is the method used by the existing CMultiplayBusters code.
    char* infoBuffer = g_engfuncs.pfnGetInfoKeyBuffer(player);

    if (infoBuffer)
    {
        g_engfuncs.pfnSetClientKeyValue(
            ENTINDEX(player),
            infoBuffer,
            "model",
            bareModel
        );
    }

    // Apply the visible model immediately.
    char modelPath[160];

    if (strcmp(bareModel, "player") == 0)
    {
        snprintf(
            modelPath,
            sizeof(modelPath),
            "models/player.mdl"
        );
    }
    else
    {
        snprintf(
            modelPath,
            sizeof(modelPath),
            "models/player/%s/%s.mdl",
            bareModel,
            bareModel
        );
    }

    SET_MODEL(player, modelPath);
}

void ZPPlayWelcomeMusic(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    CLIENT_COMMAND(player, "cd play media/Half-Life17.mp3\n");
}

void ZPSendInfection(edict_t* victim, int infectorIndex) {
    if (!victim) return;

    extern int gmsgDeathMsg;
    MESSAGE_BEGIN(MSG_ALL, gmsgDeathMsg);
        WRITE_BYTE(infectorIndex);
        WRITE_BYTE(ENTINDEX(victim));
        WRITE_STRING("zpmod_infection");
    MESSAGE_END();
}

void ZPOnInfect(edict_t* victim, int infectorIndex) {
    int v = ENTINDEX(victim);
    if (v < 1 || v > gpGlobals->maxClients) return;

    g_players[v].lastInfectKiller = infectorIndex;
    g_players[v].lastInfectTime = gpGlobals->time;
    g_players[v].killedByHeadshot = false;

    ZPFeatureOnInfect(victim, infectorIndex);
    ZPFeatureOnInfectVictim(victim, infectorIndex);
    ZPStatsOnInfect(victim, infectorIndex);

    if (infectorIndex >= 1 && infectorIndex <= gpGlobals->maxClients) {
        g_players[infectorIndex].infections++;

        // credit the infecting zombie on the scoreboard. AddPoints bumps
        // pev->frags and broadcasts ScoreInfo so everyone's scoreboard updates.
        edict_t* infector = INDEXENT(infectorIndex);
        if (ZPIsPlayerConnected(infector)) {
            CBasePlayer* pInfector = (CBasePlayer*)GET_PRIVATE(infector);
            if (pInfector) {
                pInfector->AddPoints(1, FALSE);
            }
        }
    }
}

void ZPKillReward(edict_t* killer, bool fromHeadshot) {
    if (!killer) return;
    int idx = ENTINDEX(killer);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(killer);
    if (!pPlayer || !pPlayer->IsAlive()) return;

    g_players[idx].kills++;

    pPlayer->pev->health += fromHeadshot ? 50.0f : 25.0f;
    if (pPlayer->pev->health > 200) pPlayer->pev->health = 200;

    hudtextparms_t params;
    memset(&params, 0, sizeof(params));
    params.channel = 6;
    params.x = -1;
    params.y = 0.35f;
    params.r1 = 0; params.g1 = 255; params.b1 = 0;
    params.a1 = 255;
    params.fadeinTime = 0.1f;
    params.fadeoutTime = 0.5f;
    params.holdTime = 1.5f;

    char msg[48];
    if (fromHeadshot) snprintf(msg, sizeof(msg), "HEADSHOT KILL  +50 HP");
    else snprintf(msg, sizeof(msg), "KILL BONUS  +25 HP");
    UTIL_HudMessage(CBaseEntity::Instance(killer), params, msg);
}

void ZPPlayerThink(edict_t* player) {
    if (!player) return;
    if (!ZPIsPlayerConnected(player)) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer || !pPlayer->IsAlive()) return;

    if (g_round.state != RS_ACTIVE) return;

    if (ZPIsZombie(player)) {
        // keep the crowbar's third-person worldmodel hidden, only the claw viewmodel stays
        player->v.weaponmodel = iStringNull;

        int idx = ENTINDEX(player);
        if (idx < 1 || idx > gpGlobals->maxClients) return;
        if (g_players[idx].frozenUntil > gpGlobals->time) return;
        if (g_players[idx].chargeCooldown > gpGlobals->time) return;
        if (!(pPlayer->m_afButtonPressed & IN_ATTACK2)) return;

        UTIL_MakeVectors(pPlayer->pev->v_angle);
        Vector dir = gpGlobals->v_forward;
        dir.z = 0;
        if (dir.Length() < 0.1f) return;

        pPlayer->pev->velocity = dir * 750.0f + Vector(0, 0, 220);
        g_players[idx].chargeCooldown = gpGlobals->time + 6.0f;

        EMIT_SOUND(player, CHAN_WEAPON, "zombie/zo_attack1.wav", 1.0, ATTN_NORM);
        UTIL_ScreenShake(pPlayer->pev->origin, 8.0f, 3.0f, 0.5f, 256.0f);
        return;
    }

    // human kit: press +use (E) to open the ability menu
    if (ZPIsHuman(player) && (pPlayer->m_afButtonPressed & IN_USE))
        ZPAbilityMenu(player);
}

void ZPAbilityMenu(edict_t* player)
{
    if (!player) return;
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer || !pPlayer->IsAlive()) return;
    int idx = ENTINDEX(player);

    // the map vote owns menuselect while it is running
    if (g_mapVote.active) return;

    int bits = 0;
    if (g_players[idx].healCooldown <= gpGlobals->time) bits |= (1 << 0);
    if (g_players[idx].adrenalineCooldown <= gpGlobals->time) bits |= (1 << 1);
    if (g_players[idx].frostCooldown <= gpGlobals->time) bits |= (1 << 2);

    if (bits == 0) {
        hudtextparms_t cd;
        memset(&cd, 0, sizeof(cd));
        cd.channel = 0;
        cd.x = -1;
        cd.y = 0.3f;
        cd.r1 = cd.g1 = cd.b1 = 255;
        cd.a1 = 255;
        cd.fadeinTime = 0.05f;
        cd.fadeoutTime = 0.3f;
        cd.holdTime = 1.0f;
        UTIL_HudMessage(CBaseEntity::Instance(player), cd, "ALL ABILITIES ON COOLDOWN");
        return;
    }

    MESSAGE_BEGIN(MSG_ONE, gmsgShowMenu, NULL, player);
        WRITE_SHORT(bits);
        WRITE_CHAR(-1);   // display until a key is pressed
        WRITE_BYTE(1);    // keep the menu up on click
        WRITE_STRING("HUMAN ABILITIES\n\n1. Heal +50 HP\n2. Adrenaline (6s speed)\n3. Frost Nova (freeze)");
    MESSAGE_END();

    g_players[idx].abilityMenuUntil = gpGlobals->time + 6.0f;
}

void ZPAbilitySelect(int playerIndex, int slot)
{
    if (playerIndex < 1 || playerIndex > gpGlobals->maxClients) return;
    if (g_players[playerIndex].abilityMenuUntil < gpGlobals->time) return;

    edict_t* ed = INDEXENT(playerIndex);
    if (!ZPIsPlayerConnected(ed)) return;
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
    if (!pPlayer || !pPlayer->IsAlive()) return;
    if (g_round.state != RS_ACTIVE || !ZPIsHuman(ed)) return;

    g_players[playerIndex].abilityMenuUntil = 0;

    hudtextparms_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = 0;
    fb.x = -1;
    fb.y = 0.3f;
    fb.a1 = 255;
    fb.fadeinTime = 0.05f;
    fb.fadeoutTime = 0.4f;
    fb.holdTime = 1.2f;

    char fmsg[64];

    if (slot == 1 && g_players[playerIndex].healCooldown <= gpGlobals->time) {
        pPlayer->pev->health += 50.0f;
        if (pPlayer->pev->health > 100.0f) pPlayer->pev->health = 100.0f;
        g_players[playerIndex].healCooldown = gpGlobals->time + 25.0f;

        EMIT_SOUND(ed, CHAN_ITEM, "items/smallmedkit1.wav", 1.0, ATTN_NORM);
        UTIL_ParticleEffect(pPlayer->pev->origin + Vector(0, 0, 40), Vector(0, 0, 70), 80, 24);
        UTIL_ScreenFade(pPlayer, Vector(80, 255, 90), 0.4f, 0.2f, 255, FFADE_IN);

        fb.r1 = 80; fb.g1 = 255; fb.b1 = 90;
        snprintf(fmsg, sizeof(fmsg), "HEALED +50 HP");
    } else if (slot == 2 && g_players[playerIndex].adrenalineCooldown <= gpGlobals->time) {
        g_players[playerIndex].adrenalineCooldown = gpGlobals->time + 30.0f;
        g_players[playerIndex].adrenalineUntil = gpGlobals->time + 6.0f;

        EMIT_SOUND(ed, CHAN_ITEM, "items/suitchargeno1.wav", 1.0, ATTN_NORM);
        UTIL_ParticleEffect(pPlayer->pev->origin + Vector(0, 0, 40), Vector(0, 0, 80), 14, 30);
        UTIL_ScreenFade(pPlayer, Vector(255, 210, 80), 0.3f, 0.1f, 255, FFADE_IN);

        fb.r1 = 255; fb.g1 = 200; fb.b1 = 60;
        snprintf(fmsg, sizeof(fmsg), "ADRENALINE +6s SPEED");
    } else if (slot == 3 && g_players[playerIndex].frostCooldown <= gpGlobals->time) {
        g_players[playerIndex].frostCooldown = gpGlobals->time + 45.0f;

        Vector origin = pPlayer->pev->origin;
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* e = INDEXENT(i);
            if (!ZPIsPlayerConnected(e) || !ZPIsZombie(e)) continue;
            if (g_players[i].ZMClass == ZM_CLASS_BOSS) continue;
            CBasePlayer* pz = (CBasePlayer*)GET_PRIVATE(e);
            if (!pz || !pz->IsAlive()) continue;
            if ((pz->pev->origin - origin).Length() > 480.0f) continue;

            g_players[i].frozenUntil = gpGlobals->time + 4.0f;
            pz->pev->velocity = Vector(0, 0, 0);
            EMIT_SOUND(e, CHAN_ITEM, "debris/bustmetal2.wav", 1.0, ATTN_NORM);
            UTIL_ScreenFade(pz, Vector(120, 200, 255), 0.4f, 0.2f, 255, FFADE_IN);
            UTIL_ParticleEffect(pz->pev->origin + Vector(0, 0, 32), Vector(0, 0, 20), 140, 16);
        }

        UTIL_ScreenShake(origin, 9.0f, 4.0f, 0.6f, 320.0f);
        UTIL_ParticleEffect(origin + Vector(0, 0, 32), Vector(0, 0, 120), 140, 40);
        EMIT_SOUND(ed, CHAN_WEAPON, "debris/bustmetal1.wav", 1.0, ATTN_NORM);

        fb.r1 = 120; fb.g1 = 200; fb.b1 = 255;
        snprintf(fmsg, sizeof(fmsg), "FROST NOVA");
    } else {
        return;
    }

    UTIL_HudMessage(CBaseEntity::Instance(ed), fb, fmsg);
}

void ZPFreezePlayers(bool freeze) {
    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (!ZPIsPlayerConnected(ed)) continue;

        CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
        if (!pPlayer || !pPlayer->IsAlive()) continue;

        if (freeze && !g_players[i].isFrozen) {
            g_players[i].isFrozen = true;
            ed->v.velocity = Vector(0, 0, 0);
            ed->v.movetype = MOVETYPE_NONE;
        } else if (!freeze && g_players[i].isFrozen) {
            g_players[i].isFrozen = false;
            ed->v.movetype = MOVETYPE_WALK;
        }
    }
}

void ZPAnnounceMvp(void) {
    int bestHuman = -1, bestZombie = -1;
    int bestKills = 0, bestInfections = 0;

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (!ZPIsPlayerConnected(ed)) continue;
        if (g_players[i].kills > bestKills) {
            bestKills = g_players[i].kills;
            bestHuman = i;
        }
        if (g_players[i].infections > bestInfections) {
            bestInfections = g_players[i].infections;
            bestZombie = i;
        }
    }

    if (bestKills <= 0 && bestInfections <= 0) return;

    char msg[160];
    msg[0] = 0;
    size_t off = 0;

    if (bestHuman != -1 && bestKills > 0) {
        int n = snprintf(msg + off, sizeof(msg) - off, "BEST HUMAN: %s (%d kills)\n",
                         STRING(INDEXENT(bestHuman)->v.netname), bestKills);
        if (n > 0) off += n;
        if (off > sizeof(msg)) off = sizeof(msg);
    }
    if (bestZombie != -1 && bestInfections > 0) {
        int n = snprintf(msg + off, sizeof(msg) - off, "BEST ZOMBIE: %s (%d infections)",
                         STRING(INDEXENT(bestZombie)->v.netname), bestInfections);
        if (n > 0) off += n;
    }

    hudtextparms_t params;
    memset(&params, 0, sizeof(params));
    params.channel = 1;
    params.x = -1;
    params.y = 0.22f;
    params.r1 = 255; params.g1 = 220; params.b1 = 0;
    params.a1 = 255;
    params.fadeinTime = 0.3f;
    params.fadeoutTime = 0.3f;
    params.holdTime = 5.0f;
    UTIL_HudMessageAll(params, msg);
}

void ZPRoundWinSound(edict_t* player, const char* winSound, const char* ambientPrefix, int ambientCount) {
    EMIT_SOUND(player, CHAN_AUTO, winSound, 1.0, ATTN_NORM);

    if (ambientCount > 0) {
        char path[64];
        snprintf(path, sizeof(path), "%s_ambient_%02d.wav", ambientPrefix, RANDOM_LONG(1, ambientCount));
        EMIT_SOUND(player, CHAN_AUTO, path, 1.0, ATTN_NORM);
    }
}

void ZPMapVoteReset(void) {
    g_mapVote.active = false;
    g_mapVote.hasVoted = false;
    g_mapVote.tooFewMaps = false;
    g_mapVote.endTime = 0.0f;

    for (int i = 0; i < ZPMAPVOTE_OPTIONS; i++) {
        g_mapVote.options[i][0] = '\0';
        g_mapVote.votes[i] = 0;
    }

    for (int i = 0; i < 33; i++)
        g_mapVote.playerVote[i] = -1;
}

void ZPMapVoteOpen(void) {
    if (g_mapVote.active || g_mapVote.hasVoted)
        return;

    char pool[32][32];
    int poolCount = 0;
    int size = 0;
    char* buf = (char*)LOAD_FILE_FOR_ME("mapcycle.txt", &size);

    if (buf && size > 0) {
        char* p = buf;

        while (*p && poolCount < 32) {
            char line[128];
            int ll = 0;

            while (*p && *p != '\n' && ll < (int)sizeof(line) - 1)
                line[ll++] = *p++;
            if (*p == '\n')
                p++;
            line[ll] = '\0';

            char* tok = line;
            while (*tok == ' ' || *tok == '\t' || *tok == '\r')
                tok++;

            if (!*tok || *tok == ';' || (*tok == '/' && tok[1] == '/'))
                continue;

            const char* mapName = tok;
            if (strncmp(mapName, "map ", 4) == 0)
                mapName += 4;
            else if (strncmp(mapName, "defaultmap ", 11) == 0)
                mapName += 11;

            char name[32];
            int n = 0;
            while (*mapName && *mapName != ' ' && *mapName != '\t' && *mapName != ':' && n < 31)
                name[n++] = *mapName++;
            name[n] = '\0';

            if (!n)
                continue;
            if (stricmp(name, STRING(gpGlobals->mapname)) == 0)
                continue;

            bool dup = false;
            for (int i = 0; i < poolCount; i++) {
                if (stricmp(pool[i], name) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;

            strncpy(pool[poolCount], name, sizeof(pool[0]) - 1);
            pool[poolCount][sizeof(pool[0]) - 1] = '\0';
            poolCount++;
        }
    }

    if (buf)
        FREE_FILE(buf);

    if (poolCount < 2) {
        if (!g_mapVote.tooFewMaps) {
            g_mapVote.tooFewMaps = true;
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Not enough maps in mapcycle.txt for a vote\n");
        }
        return;
    }

    int options = poolCount < ZPMAPVOTE_OPTIONS ? poolCount : ZPMAPVOTE_OPTIONS;
    bool used[32] = { false };

    for (int i = 0; i < options; i++) {
        int pick;
        do {
            pick = RANDOM_LONG(0, poolCount - 1);
        } while (used[pick]);

        used[pick] = true;
        strcpy(g_mapVote.options[i], pool[pick]);
        g_mapVote.votes[i] = 0;
    }

    g_mapVote.active = true;
    g_mapVote.endTime = gpGlobals->time + ZPMAPVOTE_LENGTH;

    char menuText[256];
    snprintf(menuText, sizeof(menuText), "MAP VOTE!\n1. %s\n2. %s\n3. %s",
             g_mapVote.options[0],
             options > 1 ? g_mapVote.options[1] : "",
             options > 2 ? g_mapVote.options[2] : "");

    unsigned short bits = 0;
    for (int i = 0; i < options; i++)
        bits |= (1 << i);

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (!ZPIsPlayerConnected(ed))
            continue;

        MESSAGE_BEGIN(MSG_ONE, gmsgShowMenu, NULL, ed);
            WRITE_SHORT(bits);
            WRITE_CHAR((int)ZPMAPVOTE_LENGTH);
            WRITE_BYTE(0);
            WRITE_STRING(menuText);
        MESSAGE_END();
    }

    UTIL_ClientPrintAll(HUD_PRINTCENTER, "Map vote started! Press 1, 2 or 3\n");
}

void ZPMapVoteThink(void) {
    if (!g_mapVote.active)
        return;
    if (gpGlobals->time < g_mapVote.endTime)
        return;

    int winner = 0;
    int best = g_mapVote.votes[0];

    for (int i = 1; i < ZPMAPVOTE_OPTIONS; i++) {
        if (g_mapVote.votes[i] > best) {
            best = g_mapVote.votes[i];
            winner = i;
        }
    }

    if (best <= 0) {
        int valid = 0;
        for (int i = 0; i < ZPMAPVOTE_OPTIONS; i++) {
            if (g_mapVote.options[i][0] != '\0')
                valid++;
        }
        winner = RANDOM_LONG(0, valid - 1) % ZPMAPVOTE_OPTIONS;

        while (g_mapVote.options[winner][0] == '\0')
            winner = (winner + 1) % ZPMAPVOTE_OPTIONS;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Next map: %s", g_mapVote.options[winner]);
    UTIL_ClientPrintAll(HUD_PRINTCENTER, msg);

    const char* map = g_mapVote.options[winner];
    g_mapVote.active = false;
    g_mapVote.hasVoted = true;
    g_mapVote.endTime = 0.0f;
    CHANGE_LEVEL(map, NULL);
}

void ZPMapVoteSelect(int playerIndex, int slot) {
    if (!g_mapVote.active)
        return;
    if (playerIndex < 1 || playerIndex > gpGlobals->maxClients)
        return;
    if (slot < 1 || slot > ZPMAPVOTE_OPTIONS)
        return;
    if (g_mapVote.options[slot - 1][0] == '\0')
        return;
    if (g_mapVote.playerVote[playerIndex] >= 0)
        return;

    g_mapVote.playerVote[playerIndex] = slot - 1;
    g_mapVote.votes[slot - 1] += 1;
}

cvar_t zpmod_advertisementenabled = { "zpmod_advertisementenabled", "0", FCVAR_SERVER };

static const char* const kZPAds[] = {
    "^3[zp] ^7don't forget to join our discord: ^4necois.fun/discord",
    "^6[zp] ^7got a suggestion or found a bug? let us know: ^4necois.fun/discord",
    "^2[zp] ^7new maps, classes and updates are coming, join us: ^4necois.fun/discord",
    "^5[zp] ^7bring your friends and join the chaos: ^4necois.fun/discord",
    "^1[zp] ^7need help or want to report someone? ^4necois.fun/discord",
    "^3[zp] ^7wanna keep up with the server? join our discord: ^4necois.fun/discord",
    "^6[zp] ^7found something broken? tell us before it eats the server: ^4necois.fun/discord",
    "^2[zp] ^7we're working on new stuff, come hang out: ^4necois.fun/discord",
    "^5[zp] ^7got friends who think they can survive? bring them: ^4necois.fun/discord",
    "^1[zp] ^7server updates and random chaos live here: ^4necois.fun/discord"
};
static int s_adIndex = 0;
static float s_nextAdvertTime = 0.0f;

// rotates through kZPAds in chat every 3-5 min; enabled via
// zpmod_advertisementenabled (default off)
void ZPAdvertThink(void) {
    cvar_t* enabled = CVAR_GET_POINTER("zpmod_advertisementenabled");
    if (!enabled || enabled->value == 0.0f)
        return;

    if (s_nextAdvertTime > gpGlobals->time)
        return;

    s_nextAdvertTime = gpGlobals->time + (180.0f + (float)RANDOM_LONG(0, 120));
    const char* msg = kZPAds[s_adIndex % (int)ARRAYSIZE(kZPAds)];
    s_adIndex++;

    MESSAGE_BEGIN(MSG_ALL, gmsgTextMsg);
        WRITE_BYTE(HUD_PRINTTALK);
        WRITE_STRING(msg);
    MESSAGE_END();
}

void ZPRoundThink(ZPRound* round) {
    CVAR_SET_STRING("sv_skyname", "night");

    ZPAdvertThink();

    ZPStatsThink();

    int connectedCount = ZPCountConnectedPlayers();

    ZPHUD();

    ZPFeatureRoundThink(round);

    if (connectedCount < 2) {
        if (round->state != RS_PREP || round->countdownStarted) {
            round->state = RS_PREP;
            round->countdownStarted = false;
            round->lastAnnounce = -1;
            lastSpokeSecond = -1;
            ZPRoundStopAmbient();

            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (ZPIsPlayerConnected(ed)) {
                    ZPRoundResetPlayer(ed);
                }
            }
        }

        if (!round->notEnoughPlayersPrinted) {
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Not enough players to start the infection\n");
            round->notEnoughPlayersPrinted = true;
        }
        return;
    }

    round->notEnoughPlayersPrinted = false;

    // if preparing
    if (round->state == RS_PREP) {
        // stop the round ambient only while it's actually playing; emitting a
        // SND_STOP for wind1 every frame otherwise spams the engine sound
        // system for the whole countdown (and shows up as "wind1 late-precache")
        if (round->ambientPlaying)
            ZPRoundStopAmbient();

        int players[32];
        int count = 0;
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* ed = INDEXENT(i);
            if (ZPIsPlayerConnected(ed) && ed->v.health > 0) {
                players[count++] = i;
            }
        }

        if (count < 2) {
            if (!round->notEnoughPlayersPrinted) {
                UTIL_ClientPrintAll(HUD_PRINTCENTER, "Not enough players to start the infection\n");
                round->notEnoughPlayersPrinted = true;
            }
            round->countdownStarted = false;
            round->lastAnnounce = -1;
            lastSpokeSecond = -1;
            return;
        }

        if (!round->countdownStarted) {
            round->countdownStarted = true;
            round->nextStateTime = gpGlobals->time + 20.0f;
            round->lastAnnounce = -1;
            lastSpokeSecond = -1;

            /* pick + announce the random mode / boss round / event for this round */
            ZPFeaturePreRound(round);

            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (ZPIsPlayerConnected(ed) && ed->v.health > 0) {
                    EMIT_SOUND(ed, CHAN_AUTO, "zpmod/start.wav", 1.0, ATTN_NORM);
                }
            }
        }

        float timeLeft = round->nextStateTime - gpGlobals->time;
        int secLeft = (int)ceilf(timeLeft);

        // freeze everyone for the final seconds of the countdown
        ZPFreezePlayers(secLeft <= 6 && secLeft > 0);

        if (secLeft > 0 && secLeft != round->lastAnnounce) {
            round->lastAnnounce = secLeft;

            char msg[64];
            snprintf(msg, sizeof(msg), "Infection in %d", secLeft);
            UTIL_ClientPrintAll(HUD_PRINTCENTER, msg);

            if (secLeft <= 10 && lastSpokeSecond != secLeft) {
                lastSpokeSecond = secLeft;

                const char* word = NumberWord(secLeft);
                if (word[0]) {
                    char path[64];
                    snprintf(path, sizeof(path), "zpmod/vox/%s.wav", word);

                    for (int i = 1; i <= gpGlobals->maxClients; i++) {
                        edict_t* ed = INDEXENT(i);
                        if (!ZPIsPlayerConnected(ed) || ed->v.health <= 0) continue;
                        EMIT_SOUND(ed, CHAN_AUTO, path, 1.0, ATTN_NORM);
                    }
                }
            }
        }

        // time to start infection
        if (gpGlobals->time >= round->nextStateTime) {
            round->state = RS_ACTIVE;
            round->roundStartTime = gpGlobals->time;
            round->lastHumanAnnounced = false;

            ZPFreezePlayers(false);
            ZPRoundStartAmbient();
            ZPCleanupWorld();

            UTIL_ClientPrintAll(HUD_PRINTCENTER, "INFECTION!\n");

            const char* activeEvent = ZPEventName(round->eventType);
            if (activeEvent[0]) {
                char evMsg[96];
                snprintf(evMsg, sizeof(evMsg), "EVENT: %s!", activeEvent);
                UTIL_ClientPrintAll(HUD_PRINTCENTER, evMsg);
            }

            if (count > 0) {
                /* apply the round mode's starting infection */
                ZPFeatureStartInfections(round, players, count);

                /* hand every surviving human the ZP throwables (first round
                   has no prior round-end reset to inherit them from) */
                for (int i = 1; i <= gpGlobals->maxClients; i++) {
                    edict_t* ed = INDEXENT(i);
                    if (!ZPIsPlayerConnected(ed) || !ZPIsHuman(ed)) continue;
                    CBasePlayer* pp = (CBasePlayer*)GET_PRIVATE(ed);
                    if (!pp || !pp->IsAlive()) continue;
                    pp->GiveNamedItem("weapon_molotov");
                    pp->GiveNamedItem("weapon_freezebomb");
                }

                /* ARMOR event: humans get a full suit at spawn */
                int armor = ZPFeatureInitialArmor();
                if (armor > 0) {
                    for (int i = 1; i <= gpGlobals->maxClients; i++) {
                        edict_t* ed = INDEXENT(i);
                        if (!ZPIsPlayerConnected(ed) || !ZPIsHuman(ed)) continue;
                        CBasePlayer* pp = (CBasePlayer*)GET_PRIVATE(ed);
                        if (!pp || !pp->IsAlive()) continue;
                        pp->pev->armorvalue = (float)armor;
                        pp->pev->armortype = 0.5f;
                    }
                }
            }
        }
    }

    // if round active
    else if (round->state == RS_ACTIVE) {
        int humans = 0, zombies = 0;
        ZPCountTeams(humans, zombies);

        // last human alive: buff + warning
        int lastHuman = -1;
        if (humans == 1) {
            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* e = INDEXENT(i);
                if (!ZPIsPlayerConnected(e)) continue;
                CBasePlayer* pp = (CBasePlayer*)GET_PRIVATE(e);
                if (!pp || !pp->IsAlive() || !ZPIsHuman(e)) continue;
                lastHuman = i;
                break;
            }

            if (lastHuman >= 1 && !round->lastHumanAnnounced) {
                round->lastHumanAnnounced = true;
                char lastMsg[96];
                snprintf(lastMsg, sizeof(lastMsg), "LAST HUMAN: %s", STRING(INDEXENT(lastHuman)->v.netname));
                UTIL_ClientPrintAll(HUD_PRINTCENTER, lastMsg);
                ZPFeatureLastHuman(INDEXENT(lastHuman));
            }
        } else {
            round->lastHumanAnnounced = false;
        }

        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* e = INDEXENT(i);
            if (!ZPIsPlayerConnected(e)) continue;
            CBasePlayer* pp = (CBasePlayer*)GET_PRIVATE(e);
            if (!pp || !pp->IsAlive()) continue;

            if (ZPIsHuman(e)) {
                bool isLast = (humans == 1 && i == lastHuman);
                g_players[i].lastHuman = isLast;

                ZPApplyHumanOverlay(e);
                if (g_players[i].frozenUntil > gpGlobals->time)
                    continue;

                pp->pev->maxspeed = (isLast ? 300.0f : 260.0f) + ZPFeatureSpeedMultiplier();
                // adrenaline burst temporarily lifts human speed
                if (g_players[i].adrenalineUntil > gpGlobals->time)
                    pp->pev->maxspeed = 335.0f + ZPFeatureSpeedMultiplier();
            } else {
                g_players[i].lastHuman = false;
                // keep class stats applied every frame (freeze handled inside)
                ZPApplyZombieClass(e);
            }
        }

        // payback marker: red beam toward whoever infected you
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* ed = INDEXENT(i);
            if (!ZPIsPlayerConnected(ed)) continue;

            CBasePlayer* pp = (CBasePlayer*)GET_PRIVATE(ed);
            if (!pp || !pp->IsAlive()) continue;
            if (g_players[i].lastInfectKiller < 1 || g_players[i].lastInfectKiller > gpGlobals->maxClients) continue;
            if (gpGlobals->time - g_players[i].lastInfectTime > 8.0f) continue;
            if (g_players[i].beamCooldown > gpGlobals->time) continue;

            edict_t* infector = INDEXENT(g_players[i].lastInfectKiller);
            if (!ZPIsPlayerConnected(infector) || infector->v.health <= 0) continue;

            g_players[i].beamCooldown = gpGlobals->time + 0.3f;

            MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, NULL, ed);
                WRITE_BYTE(TE_BEAMENTS);
                WRITE_SHORT(i);
                WRITE_SHORT(g_players[i].lastInfectKiller);
                WRITE_SHORT(MODEL_INDEX("sprites/laserbeam.spr"));
                WRITE_BYTE(0);    // frame
                WRITE_BYTE(0);    // framerate
                WRITE_BYTE(6);    // life (0.6s)
                WRITE_BYTE(10);   // width
                WRITE_BYTE(0);    // noise
                WRITE_BYTE(255);  // r
                WRITE_BYTE(30);   // g
                WRITE_BYTE(30);   // b
                WRITE_BYTE(200);  // brightness
                WRITE_BYTE(0);    // speed
            MESSAGE_END();
        }

        if (humans == 0 && zombies > 0) {
            round->state = RS_ZOMBIES_WIN;
            round->resetTime = gpGlobals->time + 5.0f;
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Zombies win\n");
            ZPAnnounceMvp();
            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                    ZPRoundWinSound(ed, "zpmod/win_zombi.wav", "zpmod/win_zombi", 3);
            }
        }
        else if (zombies == 0 && humans > 0) {
            round->state = RS_HUMANS_WIN;
            round->resetTime = gpGlobals->time + 5.0f;
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Humans win\n");
            ZPAnnounceMvp();
            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                    ZPRoundWinSound(ed, "zpmod/win_human.wav", "zpmod/win_human", 2);
            }
        }
        else if (zombies == 0 && humans == 0) {
            round->state = RS_ROUND_DRAW;
            round->resetTime = gpGlobals->time + 5.0f;
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Round draw\n");
            ZPAnnounceMvp();
            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                    EMIT_SOUND(ed, CHAN_AUTO, "zpmod/round_draw.wav", 1.0, ATTN_NORM);
            }
        }
        else if (gpGlobals->time >= round->roundStartTime + round->roundDuration) {
            round->state = RS_HUMANS_WIN;
            round->resetTime = gpGlobals->time + 5.0f;
            UTIL_ClientPrintAll(HUD_PRINTCENTER, "Time is up! Humans win\n");
            ZPAnnounceMvp();
            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (ZPIsPlayerConnected(ed) && ed->v.health > 0)
                    ZPRoundWinSound(ed, "zpmod/win_human.wav", "zpmod/win_human", 2);
            }
        }
    }

    // if win / draw
    else if (round->state == RS_ZOMBIES_WIN || round->state == RS_HUMANS_WIN || round->state == RS_ROUND_DRAW) {
        if (gpGlobals->time >= ZPMAPVOTE_INTERVAL)
            ZPMapVoteOpen();

        ZPMapVoteThink();

        if (gpGlobals->time >= round->resetTime && !g_mapVote.active) {
            round->state = RS_PREP;
            round->countdownStarted = false;
            round->notEnoughPlayersPrinted = false;
            round->lastAnnounce = -1;
            lastSpokeSecond = -1;
            ZPRoundStopAmbient();
            ZPCleanupWorld();

            for (int i = 1; i <= gpGlobals->maxClients; i++) {
                edict_t* ed = INDEXENT(i);
                if (!ZPIsPlayerConnected(ed)) continue;

                ZPRoundResetPlayer(ed);
            }
        }
    }
}

void ZPRoundRestart() {
    g_round.state = RS_ROUND_DRAW;
    g_round.resetTime = gpGlobals->time;
}

// fuck these retards that keeps sending me shit on discord

void ZPModInit(void) {
    memset(g_players, 0, sizeof(g_players));
    ZPRoundInit(&g_round);
    ZPFeatureInit();
    ZPStatsInit();
    ZPModGrenadeInit();
    ZPMapVoteReset();
    ZPAdminInit();
    s_adIndex = 0;
    s_nextAdvertTime = gpGlobals->time + (180.0f + (float)RANDOM_LONG(0, 120));
    g_engfuncs.pfnAddServerCommand("zpmod_restart", ZPRoundRestart);

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (ed && !ed->free && (ed->v.flags & FL_CLIENT)) {
            ZPPlayerJoin(ed);
        }
    }
}

void ZPPrecache(void) { // we live in a CRUEL FUCKING WORLD RETARDS..
    PRECACHE_SOUND("ambience/wind1.wav");
    PRECACHE_SOUND("zpmod/coming_1.wav");
    PRECACHE_SOUND("zpmod/coming_2.wav");
    PRECACHE_SOUND("zpmod/attack_1.wav");
    PRECACHE_SOUND("zpmod/attack_2.wav");
    PRECACHE_SOUND("zpmod/attack_3.wav");
    PRECACHE_SOUND("zpmod/wall_1.wav");
    PRECACHE_SOUND("zpmod/wall_2.wav");
    PRECACHE_SOUND("zpmod/wall_3.wav");
    PRECACHE_SOUND("zpmod/start.wav");
    PRECACHE_SOUND("zpmod/round_start.wav");
    PRECACHE_SOUND("zpmod/round_start_boss.wav");
    PRECACHE_SOUND("zpmod/human_death_1.wav");
    PRECACHE_SOUND("zpmod/human_death_2.wav");
    PRECACHE_SOUND("zombie/zo_attack1.wav");
    PRECACHE_SOUND("zombie/claw_strike1.wav");
    PRECACHE_SOUND("zpmod/win_human.wav");
    PRECACHE_SOUND("zpmod/win_human_ambient_01.wav");
    PRECACHE_SOUND("zpmod/win_human_ambient_02.wav");
    PRECACHE_SOUND("zpmod/win_zombi.wav");
    PRECACHE_SOUND("zpmod/win_zombi_ambient_01.wav");
    PRECACHE_SOUND("zpmod/win_zombi_ambient_02.wav");
    PRECACHE_SOUND("zpmod/win_zombi_ambient_03.wav");
    PRECACHE_SOUND("zpmod/round_draw.wav");
    PRECACHE_SOUND("zpmod/vox/ten.wav");
    PRECACHE_SOUND("zpmod/vox/nine.wav");
    PRECACHE_SOUND("zpmod/vox/eight.wav");
    PRECACHE_SOUND("zpmod/vox/seven.wav");
    PRECACHE_SOUND("zpmod/vox/six.wav");
    PRECACHE_SOUND("zpmod/vox/five.wav");
    PRECACHE_SOUND("zpmod/vox/four.wav");
    PRECACHE_SOUND("zpmod/vox/three.wav");
    PRECACHE_SOUND("zpmod/vox/two.wav");
    PRECACHE_SOUND("zpmod/vox/one.wav");
    PRECACHE_SOUND("zpmod/hurt_1.wav");
    PRECACHE_SOUND("zpmod/hurt_2.wav");
    PRECACHE_SOUND("zpmod/death_1.wav");
    PRECACHE_SOUND("zpmod/death_2.wav");
    PRECACHE_SOUND("ambience/the_horror3.wav");
    PRECACHE_SOUND("items/smallmedkit1.wav");
    PRECACHE_SOUND("items/suitchargeno1.wav");
    PRECACHE_SOUND("items/suitcharge1.wav");
    PRECACHE_SOUND("debris/bustmetal1.wav");
    PRECACHE_SOUND("debris/bustmetal2.wav");
    PRECACHE_SOUND("player/heartbeat1.wav");

    PRECACHE_MODEL("models/zpmod/v_claws.mdl");
    PRECACHE_GENERIC("models/zpmod/v_claws.mdl");
    PRECACHE_MODEL("models/player/zm/zm.mdl");
    PRECACHE_GENERIC("models/player/zm/zm.mdl");
    PRECACHE_MODEL("models/player/helmet/helmet.mdl");
    PRECACHE_GENERIC("models/player/helmet/helmet.mdl");
    PRECACHE_MODEL("sprites/laserbeam.spr");
    PRECACHE_MODEL("sprites/lgtning.spr");
}