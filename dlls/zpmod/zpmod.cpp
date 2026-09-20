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
}

void ZPPlayerJoin(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

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
}

void ZPPlayerDisconnect(edict_t* player) {
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

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

void ZPInfectPlayer(edict_t* player, bool wasInfectedBySomeone) {
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].ZMClass = ZM_CLASS_REGULAR;

    int r = RANDOM_LONG(1, 12);
    
    if (r == 6 && wasInfectedBySomeone == false) {
        g_players[idx].ZMClass = ZM_CLASS_BOSS;
        EMIT_SOUND(player, CHAN_AUTO, "ambience/the_horror3.wav", 1.0, ATTN_NONE);
    } else {
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
    player->v.health = 500;
    player->v.gravity = 0.8f;

    if (g_players[idx].ZMClass == ZM_CLASS_BOSS) {
        player->v.health = 4000;
        // player->v.gravity = 0.2f;
    }

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

    pPlayer->RemoveAllItems(false);
    pPlayer->GiveNamedItem("weapon_crowbar");
    pPlayer->SelectItem("weapon_crowbar");

    player->v.team = RoleToInt(ROLE_ZOMBIE);
    player->v.viewmodel = MAKE_STRING("models/zpmod/v_claws.mdl");
    player->v.weaponmodel = iStringNull;
    pPlayer->pev->pain_finished = gpGlobals->time;

    ZPSetPlayerModel(player, "zm");

    player->v.modelindex = MODEL_INDEX("models/player/zm/zm.mdl");
    
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

    if (g_round.state == RS_ACTIVE && ZPIsHuman(player)) {
        ZPInfectPlayer(player, true);
        ZPSendInfection(player, attackerIndex);
        ZPOnInfect(player, attackerIndex);
        return true;
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

    TraceResult tr; 
    CBaseEntity* pHit = ZPZombieCheckHit(pPlayer, 70.0f, &tr);

    if (pHit && pHit->IsPlayer() && ZPIsHuman(pHit->edict())) {
        // ReZombie-style claw: a connecting melee hit converts the victim.
        // Armor is the only protection — it absorbs the claw damage until it breaks.
        float dmg = (g_players[idx].ZMClass == ZM_CLASS_REGULAR) ? 25.0f : 35.0f;

        bool infected = true;
        float armor = pHit->pev->armorvalue;
        if (armor > 0.0f) {
            float newArmor = armor - dmg;
            if (newArmor < 0.0f) newArmor = 0.0f;
            pHit->pev->armorvalue = newArmor;
            infected = (newArmor <= 0.0f);
        }

        if (infected) {
            pPlayer->pev->health += 100.0f;
            ZPInfectPlayer(pHit->edict(), true);
            ZPSendInfection(pHit->edict(), idx);
            ZPOnInfect(pHit->edict(), idx);
        }

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
    char top[80];
    switch (g_round.state) {
        case RS_PREP:
            if (g_round.countdownStarted && gpGlobals->time < g_round.nextStateTime) {
                int sec = (int)ceilf(g_round.nextStateTime - gpGlobals->time);
                snprintf(top, sizeof(top), "ROUND STARTS IN %02d:%02d", sec / 60, sec % 60);
            } else {
                snprintf(top, sizeof(top), "ROUND PREPARING");
            }
            break;
        case RS_ACTIVE: {
            int sec = (int)ceilf((g_round.roundStartTime + g_round.roundDuration) - gpGlobals->time);
            if (sec < 0) sec = 0;
            snprintf(top, sizeof(top), "TIME LEFT %02d:%02d", sec / 60, sec % 60);
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
            className = "Zombie";
            if (g_players[i].ZMClass == ZM_CLASS_BOSS) className = "Boss Zombie";
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
}

void ZPSetPlayerModel(edict_t* player, const char* modelName) {
    if (!player || !modelName) return;

    SET_MODEL(player, modelName);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "model %s\n", modelName);
    CLIENT_COMMAND(player, cmd);
    //char debug[128];
    //snprintf(debug, sizeof(debug), "say %s\n", modelName);
    //CLIENT_COMMAND(player, debug);
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

    if (infectorIndex >= 1 && infectorIndex <= gpGlobals->maxClients) {
        g_players[infectorIndex].infections++;
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
    if (!ZPIsZombie(player)) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer || !pPlayer->IsAlive()) return;

    // keep the crowbar's third-person worldmodel hidden, only the claw viewmodel stays
    player->v.weaponmodel = iStringNull;

    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    if (g_round.state != RS_ACTIVE) return;
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

void ZPRoundThink(ZPRound* round) {
    CVAR_SET_STRING("sv_skyname", "night");

    int connectedCount = ZPCountConnectedPlayers();

    ZPHUD();

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

            UTIL_ClientPrintAll(HUD_PRINTCENTER, "INFECTION!\n");

            if (count > 0) {
                int idx = RANDOM_LONG(0, count - 1);
                edict_t* chosen = INDEXENT(players[idx]);
                ZPInfectPlayer(chosen, false);
                CLIENT_PRINTF(chosen, print_center, "You are the first zombie\n");
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
                pp->pev->maxspeed = isLast ? 300.0f : 260.0f;
            } else {
                g_players[i].lastHuman = false;
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
        if (gpGlobals->time >= round->resetTime) {
            round->state = RS_PREP;
            round->countdownStarted = false;
            round->notEnoughPlayersPrinted = false;
            round->lastAnnounce = -1;
            lastSpokeSecond = -1;
            ZPRoundStopAmbient();

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
    g_engfuncs.pfnAddServerCommand("zpmod_restart", ZPRoundRestart);

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* ed = INDEXENT(i);
        if (ed && !ed->free && (ed->v.flags & FL_CLIENT)) {
            ZPPlayerJoin(ed);
        }
    }
}

void ZPPrecache(void) { // we live in a CRUEL FUCKING WORLD RETARDS..
    PRECACHE_SOUND("zpmod/coming_1.wav");
    PRECACHE_SOUND("zpmod/coming_2.wav");
    PRECACHE_SOUND("zpmod/attack_1.wav");
    PRECACHE_SOUND("zpmod/attack_2.wav");
    PRECACHE_SOUND("zpmod/attack_3.wav");
    PRECACHE_SOUND("zpmod/wall_1.wav");
    PRECACHE_SOUND("zpmod/wall_2.wav");
    PRECACHE_SOUND("zpmod/wall_3.wav");
    PRECACHE_SOUND("zpmod/start.wav");
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

    PRECACHE_MODEL("models/zpmod/v_claws.mdl");
    PRECACHE_GENERIC("models/zpmod/v_claws.mdl");
    PRECACHE_MODEL("models/player/zm/zm.mdl");
    PRECACHE_GENERIC("models/player/zm/zm.mdl");
    PRECACHE_MODEL("models/player/helmet/helmet.mdl");
    PRECACHE_GENERIC("models/player/helmet/helmet.mdl");
    PRECACHE_MODEL("sprites/laserbeam.spr");
    PRECACHE_MODEL("sprites/lgtning.spr");
}