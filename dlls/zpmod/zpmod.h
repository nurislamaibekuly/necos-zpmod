#pragma once

#include "extdll.h"
#include "edict.h"

enum RoundState {
    RS_PREP,
    RS_ACTIVE,
    RS_HUMANS_WIN,
    RS_ZOMBIES_WIN,
    RS_ROUND_DRAW
};

enum Role {
    ROLE_HUMAN,
    ROLE_ZOMBIE,
    ROLE_SPECTATOR
};

enum ZMClasses {
    ZM_CLASS_REGULAR,
    ZM_CLASS_BOSS
};

struct ZPRound {
    int state;
    float resetTime;
    float nextStateTime;
    float roundStartTime;
    float roundDuration;
    int lastAnnounce;
    bool countdownStarted;
    bool notEnoughPlayersPrinted;
    bool playersFrozen;
    bool lastHumanAnnounced;
};

struct ZPPlayer {
    char originalModel[32];
    edict_t* ed;
    ZMClasses ZMClass;
    bool isFrozen;
    bool lastHuman;
    bool killedByHeadshot;
    int kills;
    int infections;
    int headshots;
    int lastInfectKiller;
    float lastInfectTime;
    float chargeCooldown;
    float beamCooldown;
    int clawSwing;
};

extern ZPRound g_round;
extern ZPPlayer g_players[33];

void ZPModInit(void);
void ZPRoundThink(ZPRound* round);
void ZPPrecache(void);
void ZPRoundStartAmbient(void);
void ZPRoundStopAmbient(void);
void ZPHUD();
bool ZPIsPlayerConnected(edict_t* player);
int ZPCountConnectedPlayers(void);
bool ZPIsZombie(edict_t* player);
bool ZPIsDead(edict_t* player);
bool ZPIsHuman(edict_t* player);
void ZPZombieSwing(edict_t* player);
bool ZPDied(edict_t* player, int attackerIndex);
void ZPSendInfection(edict_t* victim, int infectorIndex);
void ZPOnInfect(edict_t* victim, int infectorIndex);
void ZPPlayerThink(edict_t* player);
void ZPKillReward(edict_t* killer, bool fromHeadshot);
void ZPFreezePlayers(bool freeze);
void ZPAnnounceMvp(void);
void ZPThunderStrike(edict_t* player);
void ZPHurt(edict_t* player);
void ZPPlayerJoin(edict_t* player);
void ZPPlayWelcomeMusic(edict_t* player);
void ZPPlayerDisconnect(edict_t* player);
void ZPSetPlayerModel(edict_t* player, const char* modelName);
void ZPRoundResetPlayer(edict_t* player);