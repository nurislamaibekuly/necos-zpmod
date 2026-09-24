#pragma once

#include "extdll.h"
#include "edict.h"
#include <stdarg.h>
#include <stdio.h>

// appends to /tmp/zpmod_debug.log; survives console ALERT suppression
void ZP_Trace(const char *fmt, ...);

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
    ZM_CLASS_FAST,
    ZM_CLASS_TANK,
    ZM_CLASS_JUMPER,
    ZM_CLASS_BOSS
};

// how the first infection happens this round (chosen at countdown)
enum ZPRoundMode {
    ZMODE_CLASSIC,      // 1 first zombie
    ZMODE_MULTIPLE,     // 3 first zombies
    ZMODE_SWARM,        // 5 first zombies
    ZMODE_PLAGUE,       // 1 first, infection spreads on its own over time
    ZMODE_ARMAGEDDON,   // everyone but 2 humans starts infected
    ZMODE_NEMESIS       // 1 human survivor vs 1 boss zombie, everyone else zombie
};

// random round modifier (picked rare, announced at countdown)
enum ZPRoundEvent {
    ZEV_NONE,
    ZEV_LOW_GRAVITY,    // sv_gravity 400
    ZEV_BLACKOUT,       // sudden darkness, last 15s
    ZEV_ONE_HIT,        // zombie claws insta-kill
    ZEV_DOUBLE_DAMAGE,  // zombie claw damage x2
    ZEV_SPEED,          // everyone +35 maxspeed
    ZEV_ARMOR,          // humans start at 100 armor
    ZEV_DOUBLE_SPREAD,  // 2x initial infection
    ZEV_SUDDEN_DEATH    // round timer halved
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
    int mode;             // ZPRoundMode for this round
    int eventType;        // ZPRoundEvent for this round
    float eventAnnounced;
    bool suddenDeathActive;
    float suddenDeathUntil;
    bool plagueNextInfectTime;
    float plagueNextSpread;
    float lastHumanMusicUntil;
    bool bossRound;
    bool ambientPlaying;
};

#define ZPMAPVOTE_OPTIONS 3
#define ZPMAPVOTE_INTERVAL 2400.0f
#define ZPMAPVOTE_LENGTH 25.0f

// client menu slots: both the map vote and the human ability menu share the
// engine's single ShowMenu channel, so each player must remember which menu
// is actually on their screen for menuselect to route correctly
#define ZPMENU_NONE     0
#define ZPMENU_VOTE     1
#define ZPMENU_ABILITY  2

struct ZPMapVote {
    bool active;
    bool hasVoted;
    bool tooFewMaps;
    float endTime;
    char options[ZPMAPVOTE_OPTIONS][32];
    int votes[ZPMAPVOTE_OPTIONS];
    int playerVote[33];
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
    int aimTarget;
    float healCooldown;
    float adrenalineCooldown;
    float adrenalineUntil;
    float frostCooldown;
    float abilityMenuUntil;
    int menuType;         // which menu this player currently has up (ZPMENU_*)
    float frozenUntil;
    bool noclip;
    int steerMode;        // pick a round modifier (see ZPRoundEvent)
    int killStreak;       // consecutive frags this life
    int infectStreak;     // consecutive infections this life
    int deathCount;
    int roundsSurvived;
    float playTime;       // seconds connected this session
    int zombieKills;      // kills landed while a zombie
    int roundsAsFirst;    // times became first zombie
    int lastHumanCount;   // times were last human
    bool lastHumanBuffGiven;
    bool bossRageDone;
    bool bossRoundStart;  // forced to spawn as a BOSS zombie when infected
    float slowUntil;      // post-infection slowdown
    float eventSlowUntil;
};

extern ZPRound g_round;
extern ZPPlayer g_players[33];
extern ZPMapVote g_mapVote;

void ZPModInit(void);
void ZPModGrenadeInit(void);
void ZPCleanupWorld(void);
void ZPRoundThink(ZPRound* round);
void ZPFeatureInit(void);
void ZPFeaturePreRound(ZPRound* round);
const char* ZPModeName(int mode);
const char* ZPEventName(int ev);
int ZPFeatureInitialInfections(ZPRound* round, int connected);
void ZPFeatureStartInfections(ZPRound* round, int* players, int count);
void ZPFeatureRoundThink(ZPRound* round);
float ZPFeatureClawMultiplier(void);
float ZPFeatureGravity(void);
float ZPFeatureSpeedMultiplier(void);
int ZPFeatureInitialArmor(void);
float ZPFeatureRoundDuration(void);
void ZPFeatureOnKill(edict_t* killer, bool fromHeadshot);
void ZPFeatureOnInfect(edict_t* victim, int infectorIndex);
void ZPFeatureOnDied(edict_t* player);
void ZPFeatureLastHuman(edict_t* player);
void ZPFeatureOnInfectVictim(edict_t* victim, int infectorIndex);
void ZPFeaturePlayerDisconnect(edict_t* player);
bool ZPStatsCommand(edict_t* sender, int argc, char** argv);
bool ZPTopCommand(edict_t* sender, int argc, char** argv);
void ZPStatsInit(void);
void ZPStatsThink(void);
void ZPStatsOnKill(edict_t* killer, bool fromHeadshot);
void ZPStatsOnInfect(edict_t* victim, int infectorIndex);
void ZPStatsOnDied(edict_t* player);
void ZPStatsPlayerDisconnect(edict_t* player);
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
void ZPMapVoteReset(void);
void ZPMapVoteOpen(void);
void ZPMapVoteThink(void);
void ZPMapVoteSelect(int playerIndex, int slot);
void ZPAbilityMenu(edict_t* player);
void ZPAbilitySelect(int playerIndex, int slot);
void ZPApplyZombieClass(edict_t* player);
int RoleToInt(Role r);
void ZPInfectPlayer(edict_t* player, bool wasInfectedBySomeone);
void ZPMakeHuman(edict_t* player);
void ZPForceRoundEnd(int winner);
void ZPAdminInit(void);
bool ZPAdminCommand(edict_t* sender, const char* text);
bool ZPAdminCheckBan(const char* name, const char* address, char reason[128]);