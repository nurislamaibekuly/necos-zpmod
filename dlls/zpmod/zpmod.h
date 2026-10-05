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

// Zombie classes, ordered to match g_zmClasses[] in zpmod.cpp. The table is the
// single source of truth for stats/models/sounds; always index it with these.
enum ZMClasses {
    ZM_CLASS_ZOMBIE,
    ZM_CLASS_SPEED,
    ZM_CLASS_DEIMOS,
    ZM_CLASS_HEAL,
    ZM_CLASS_HEAVY,
    ZM_CLASS_TANK,
    ZM_CLASS_CHINA,
    ZM_CLASS_BOSS,
    ZM_CLASS_COUNT
};

// right-click abilities (only these classes have one)
enum ZMAbility {
    ZMABILITY_NONE,
    ZMABILITY_DASH,   // speed: burst forward
    ZMABILITY_HEAL,   // heal: aura that tops up nearby zombies
    ZMABILITY_SLAM,   // tank: ground slam, area damage
    ZMABILITY_LEAP,   // china: leap at the crosshair
    ZMABILITY_RAGE    // boss: enrage burst
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
    float beamCooldown;
    int clawSwing;
    float nextClawAnim;    // when the claw viewmodel may return to idle
    int aimTarget;
    float healCooldown;
    float adrenalineCooldown;
    float adrenalineUntil;
    float frostCooldown;
    float abilityMenuUntil;
    int menuType;         // which menu this player currently has up (ZPMENU_*)
    bool welcomeMusicPending;
    bool welcomeMusicStarted;
    float frozenUntil;
    bool noclip;
    int steerMode;        // pick a round modifier (see ZPRoundEvent)
    int killStreak;       // consecutive frags this life
    float killIconFadeAt; // when the streak icon starts fading (0 = nothing up)
    float killIconRemoveAt; // when the faded icon gets hidden again
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
    float abilityCooldown; // per-class right-click, see ZMAbility
    float rageUntil;       // deimos/boss enrage window
    float healAuraUntil;   // healer: aura keeps pulsing until this time
    float bossRageSpeed;   // boss: permanent +speed earned this round
    int rageStacks;        // deimos/boss: hits taken while raging
    float nextHealPulse;   // heal: next aura tick
    bool visionTinted;     // red vision fade is up (see ZPSyncZombieState)
    float nextVisionSync;  // when the vision fade is next re-pinned
};

extern ZPRound g_round;
extern ZPPlayer g_players[33];
extern ZPMapVote g_mapVote;

void ZPModInit(void);
void ZPModGrenadeInit(void);
void ZPMolotovBlast(const Vector& origin);
void ZPFrostBlast(const Vector& origin);
void ZPSupplyBoxInit(void);
void ZPSupplyBoxPrecache(void);
void ZPSupplyBoxRoundStart(void);
void ZPSupplyBoxRoundReset(void);
void ZPSupplyBoxThink(void);
void ZPSupplyBoxIconUpdate(void);
void ZPSupplyBoxCommand(void);
void ZPSupplyBoxClearCommand(void);
void ZPCleanupWorld(void);
void ZPRoundThink(ZPRound* round);
void ZPRoundRestart(void);
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
void ZPFeatureKillIconThink(void);
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

// Pushes one player's infection state to their own client: lightstyle 0 as a
// per-client fake fullbright, plus the red vision screenfade while infected.
// Called on every role change and re-asserted from ZPPlayerThink.
void ZPSyncZombieState(edict_t* player);

// Re-applies the model a player's current role requires, if v.modelindex does
// not already point at it. Returns TRUE if the model was changed.
bool ZPEnsurePlayerModel(edict_t* player);

// Re-asserts v.modelindex from v.model when CheckPowerups has stomped the index.
void ZPSyncPlayerModelIndex(edict_t* player);
bool ZPIsDead(edict_t* player);
bool ZPIsHuman(edict_t* player);

// ---- zombie class table (see g_zmClasses[] in zpmod.cpp) ----
bool ZMClassValid(int cls);
const char* ZMClassName(int cls);
const char* ZMClassModel(int cls);        // bare model folder, e.g. "necozpmod_tank"
const char* ZMClawViewModel(int cls);     // models/zpmod/v_claws_<class>.mdl
const char* ZMBombViewModel(int cls);     // models/zpmod/v_infectionbomb_<class>.mdl
const char* ZMClassHurtSound(int cls, int variant);
const char* ZMClassDeathSound(int cls, int variant);
float ZMClassHealth(int cls);
float ZMClassArmor(int cls);
float ZMClassSpeed(int cls);
float ZMClassGravity(int cls);
float ZMClassClawDamage(int cls);
float ZMClassDamageTaken(int cls);        // multiplier applied to incoming damage
int   ZMClassAbility(int cls);            // ZMAbility
int   ZMClassColor(int cls);              // hud + noclip glow, packed 0xRRGGBB
const int* ZMClassClawAnims(int cls, int* count);
const int* ZMClassClawAnimFrames(int cls, int* count);
const char* const* ZMClassBodyClaws(int cls);  // player-model body anims, NULL-terminated

void ZPZombieSwing(edict_t* player);
void ZPSendClawAnim(edict_t* player, int seq);
void ZPZombieHeal(edict_t* player, float amount);
float ZPZombieClawDamage(edict_t* player);
float ZPZombieDamageTaken(edict_t* player);
void ZPZombieOnDamaged(edict_t* player, float damage);
void ZPZombieOnKilled(edict_t* player);
void ZPRoundResetAbilities(int playerIndex);
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
void ZPPlayerDisconnect(edict_t* player);
void ZPSetPlayerModel(edict_t* player, const char* modelName);
void ZPRoundResetPlayer(edict_t* player);

extern cvar_t zpmod_min_players;

// Minimum connected players required for a round to start; the round resets
// every player to a human below this. Defaults to 2. Set to 1 to test solo.
int ZPMinPlayers(void);
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