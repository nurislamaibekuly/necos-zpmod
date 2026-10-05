#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "zpmod.h"
#include "studio.h"
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

void FindHullIntersection( const Vector &vecSrc, TraceResult &tr, float *mins, float *maxs, edict_t *pEntity );

// ---------------------------------------------------------------------------
// Zombie class table.
//
// Single source of truth for the eight classes: stats, models, sounds, melee
// anims and ability. Everything else goes through the ZMClass*() accessors
// below, so rebalancing a class is a one-line edit here.
//
// The claw anim lists are per-class because the viewmodels are not uniform:
// v_claws_deimos.mdl has 9 sequences, the other seven have 8. A shared
// {3,4,5,6,7,8} list would send sequence 8 (out of range) on every other
// class and stall the melee animation mid-swing.
// ---------------------------------------------------------------------------

struct ZMClassDef {
    const char* name;        // display name
    const char* tag;         // lowercase id, also the asset file suffix
    const char* model;       // bare folder under models/player/
    float health;
    float armor;
    float speed;
    float gravity;
    float clawDamage;
    float damageTaken;       // multiplier applied to damage this class takes
    int ability;             // ZMAbility
    int color;               // 0xRRGGBB, used for hud text and noclip glow
    const char* hurtSounds[2];
    const char* deathSounds[2];
    const int* clawAnims;
    int clawAnimCount;
    // Frame count of each entry in clawAnims, same order. See g_kClawFrames*.
    const int* clawAnimFrames;
    // Player-model body sequences, cycled per swing. Looked up by name at
    // runtime and NULL-terminated, because the eight models do NOT ship the
    // same set: the converted CS ones carry "zbs_attack*", while heavy/tank/
    // boss carry no dedicated attack anim at all. A table whose first entry is
    // NULL means "no such anim", and the swing falls back to the generic
    // onehanded shoot.
    const char* const* bodyClaws;
};

// Viewmodel sequence indices, read off the v_claws_*.mdl headers rather than
// guessed. Every one of the eight carries the same 0-7 layout, with deimos
// adding a ninth:
//
//   [0] idle      act=1, flagged looping
//   [1] slash1
//   [2] slash2
//   [3] draw
//   [4] stab
//   [5] stab_miss
//   [6] midslash1
//   [7] midslash2
//   [8] skill      (deimos only)
//
// The hit cycle is the slash pair ONLY: slash1, slash2.
//
// midslash1/midslash2 (6,7) look like they belong in a swing cycle and do not.
// They are a separate long windup -- heal makes it obvious, 31 frames for
// slash against 61 for midslash, so it takes twice as long and reads as a big
// slow overhead instead of a claw hit. An earlier revision interleaved them
// (slash1, midslash1, slash2, midslash2), which meant every other attack
// played the long windup. They are not used for the normal hit.
//
// stab (4) and stab_miss (5) are a thrust and its whiff, not a claw swipe, and
// draw (3) is the weapon-draw pose. None of the three are hit animations.
static const int g_kClawAnims2[] = { 1, 2 };

// deimos is the exception: its slash1/slash2 are 2-frame stubs, the same empty
// placeholder the ref_aim_* clips are on the player models. A 2-frame clip at
// 30fps is a single-frame flash, so it has to fall back to a real clip. midslash
// is the nearest true swipe available and, at 46-48 frames, is the same length
// as the other classes' slashes -- so it is the long-windup complaint above
// that does not apply here.
static const int g_kClawAnimsDeimos[] = { 6, 7 };

// Frame counts for the sequences above, in the same order, read off each
// v_claws_*.mdl header. The server DLL has no pfnGetModel, so the viewmodel's
// studio header is not reachable from here and the clip length cannot be read
// at runtime; it has to travel with the index table. Without it every class
// would use one shared duration and either get cut off or hang on the last
// frame. heal is the outlier (31 against 55-66 elsewhere), so it gets its own.
static const int g_kClawFrames66[] = { 66, 66 };
static const int g_kClawFrames55[] = { 55, 55 };
static const int g_kClawFramesHeal[] = { 31, 31 };
static const int g_kClawFramesDeimos[] = { 46, 48 };

// All eight viewmodels play their attack clips at 30 fps.
static const float ZMCLAW_VIEW_FPS = 30.0f;

// Idle is sequence 0 on all eight, and it is the only one flagged as looping.
// ZPZombieSwing has to return to it or the viewmodel freezes on the last swing
// frame; see the idle restore in ZPPlayerThink.
static const int ZMCLAW_IDLE = 0;

// Highest valid sequence in any v_claws_*.mdl. Seven of the eight have exactly
// 8 sequences (0-7); deimos has 9. Guarding against 8 is deliberate -- a real
// deimos "skill" is at index 8, and clamping at 9 would still let a 0-7 index
// through on the other seven models.
static const int ZMCLAW_MAX_SEQ = 8;

// Body claw anims for the models that have them. Ref_aim holds the windup,
// the two _run entries are the actual swipes (a walk swipe and a run swipe on
// the converted models). Order matters: it is the swing cycle.
static const char* const g_kBodyClaws3[] = { "zbs_attack_walk", "zbs_attack1_run", "zbs_attack2_run" };
static const char* const g_kBodyClaws2[] = { "zbs_attack_walk", "zbs_attack1_run" };
static const char* const g_kBodyClawsNone[] = { NULL };

// Spawn distribution, index-aligned with g_zmClasses[]. Deliberately separate
// from the stat columns: this is how often a class shows up, not how strong it
// is, and the two want to be tuned independently.
static const int g_zmClassWeights[ZM_CLASS_COUNT] = {
    /* zombie */ 36,
    /* speed  */ 18,
    /* deimos */ 12,
    /* heal   */ 12,
    /* heavy  */  9,
    /* tank   */  8,
    /* china  */  3,
    /* boss   */  2
};
static const int kZMClassRollTotal = 100;

static const ZMClassDef g_zmClasses[ZM_CLASS_COUNT] = {
    //  name        tag        model               hp   armor speed grav claw taken  ability        color
    // Only 5 of the 8 models ship a zbs_ clip set. boss/heavy/tank have none at
    // all, so they deliberately fall back to the plain locomotion names.
    { "ZOMBIE",  "zombie",  "necozpmod_zombie", 2000, 200, 290, 0.83f,  70, 1.00f, ZMABILITY_NONE, 0xFF2828,
      { "zpmod/hurt_1.wav", "zpmod/hurt_2.wav" },
      { "zpmod/death_1.wav", "zpmod/death_2.wav" },
      g_kClawAnims2, 2, g_kClawFrames66, g_kBodyClaws2 },

    { "SPEED",   "speed",   "necozpmod_speed",   800, 100, 310, 0.64f,  58, 1.00f, ZMABILITY_DASH,  0x1EDCFF,
      { "zpmod/hurt_female_1.wav", "zpmod/hurt_female_2.wav" },
      { "zpmod/death_female_1.wav", "zpmod/death_female_2.wav" },
      g_kClawAnims2, 2, g_kClawFrames66, g_kBodyClaws3 },

    { "DEIMOS",  "deimos",  "necozpmod_deimos", 2000, 200, 300, 0.72f,  70, 1.00f, ZMABILITY_NONE,  0xB020F0,
      { "zpmod/hurt_heavy_1.wav", "zpmod/hurt_heavy_2.wav" },
      { "zpmod/death_heavy_1.wav", "zpmod/death_heavy_2.wav" },
      g_kClawAnimsDeimos, 2, g_kClawFramesDeimos, g_kBodyClaws3 },

    { "HEALER",  "heal",    "necozpmod_heal",   2000, 200, 290, 0.83f,  70, 1.00f, ZMABILITY_HEAL,  0x30FF60,
      { "zpmod/hurt_1.wav", "zpmod/hurt_2.wav" },
      { "zpmod/death_1.wav", "zpmod/death_2.wav" },
      g_kClawAnims2, 2, g_kClawFramesHeal, g_kBodyClaws3 },

    { "HEAVY",   "heavy",   "necozpmod_heavy",  3000, 300, 250, 0.95f,  85, 0.70f, ZMABILITY_NONE,  0xFF9C20,
      { "zpmod/hurt_heavy_1.wav", "zpmod/hurt_heavy_2.wav" },
      { "zpmod/death_heavy_1.wav", "zpmod/death_heavy_2.wav" },
      g_kClawAnims2, 2, g_kClawFrames66, g_kBodyClawsNone },

    { "TANK",    "tank",    "necozpmod_tank",   3000, 300, 240, 1.05f, 100, 0.75f, ZMABILITY_SLAM,  0xFF8C14,
      { "zpmod/hurt_1.wav", "zpmod/hurt_2.wav" },
      { "zpmod/death_1.wav", "zpmod/death_2.wav" },
      g_kClawAnims2, 2, g_kClawFrames66, g_kBodyClawsNone },

    { "CHINA",   "china",   "necozpmod_china",  3000, 300, 270, 0.95f,  85, 0.75f, ZMABILITY_LEAP,  0xFFC83C,
      { "zpmod/hurt_1.wav", "zpmod/hurt_2.wav" },
      { "zpmod/death_1.wav", "zpmod/death_2.wav" },
      g_kClawAnims2, 2, g_kClawFrames55, g_kBodyClaws3 },

    { "BOSS",    "boss",    "necozpmod_boss",   5000, 500, 250, 0.90f, 120, 0.65f, ZMABILITY_RAGE,  0xFF0F1E,
      { "zpmod/hurt_heavy_1.wav", "zpmod/hurt_heavy_2.wav" },
      { "zpmod/death_heavy_1.wav", "zpmod/death_heavy_2.wav" },
      g_kClawAnims2, 2, g_kClawFrames66, g_kBodyClawsNone }
};

bool ZMClassValid(int cls) {
    return cls >= 0 && cls < ZM_CLASS_COUNT;
}

static const ZMClassDef& ZMClassInfo(int cls) {
    static const ZMClassDef fallback = {
        "ZOMBIE", "zombie", "necozpmod_zombie", 2000, 200, 290, 0.83f, 70, 1.00f,
        ZMABILITY_NONE, 0xFF2828,
        { "zpmod/hurt_1.wav", "zpmod/hurt_2.wav" },
        { "zpmod/death_1.wav", "zpmod/death_2.wav" },
        g_kClawAnims2, 2, g_kClawFrames66, g_kBodyClaws2
    };
    if (!ZMClassValid(cls)) return fallback;
    return g_zmClasses[cls];
}

const char* ZMClassName(int cls) { return ZMClassInfo(cls).name; }
const char* ZMClassModel(int cls) { return ZMClassInfo(cls).model; }
float ZMClassHealth(int cls) { return ZMClassInfo(cls).health; }
float ZMClassArmor(int cls) { return ZMClassInfo(cls).armor; }
float ZMClassSpeed(int cls) { return ZMClassInfo(cls).speed; }
float ZMClassGravity(int cls) { return ZMClassInfo(cls).gravity; }
float ZMClassClawDamage(int cls) { return ZMClassInfo(cls).clawDamage; }
float ZMClassDamageTaken(int cls) { return ZMClassInfo(cls).damageTaken; }
int ZMClassAbility(int cls) { return ZMClassInfo(cls).ability; }
int ZMClassColor(int cls) { return ZMClassInfo(cls).color; }

// The callers pass the result straight to MAKE_STRING/SET_MODEL/PRECACHE_*, so
// a small ring of buffers is enough to keep two classes from clobbering each
// other inside one expression.
static char* ZMClassAssetPath(char* buf, size_t size, const char* prefix, const char* tag) {
    snprintf(buf, size, "%s%s.mdl", prefix, tag);
    return buf;
}

const char* ZMClawViewModel(int cls) {
    static char bufs[4][64];
    static int next = 0;
    char* buf = bufs[next];
    next = (next + 1) & 3;
    return ZMClassAssetPath(buf, 64, "models/zpmod/v_claws_", ZMClassInfo(cls).tag);
}

const char* ZMBombViewModel(int cls) {
    static char bufs[4][64];
    static int next = 0;
    char* buf = bufs[next];
    next = (next + 1) & 3;
    return ZMClassAssetPath(buf, 64, "models/zpmod/v_infectionbomb_", ZMClassInfo(cls).tag);
}

const char* ZMClassHurtSound(int cls, int variant) {
    return ZMClassInfo(cls).hurtSounds[(variant & 1)];
}

const char* ZMClassDeathSound(int cls, int variant) {
    return ZMClassInfo(cls).deathSounds[(variant & 1)];
}

const int* ZMClassClawAnimFrames(int cls, int* count) {
    const ZMClassDef& def = ZMClassInfo(cls);
    if (count) *count = def.clawAnimCount;
    return def.clawAnimFrames;
}

const int* ZMClassClawAnims(int cls, int* count) {
    const ZMClassDef& def = ZMClassInfo(cls);
    if (count) *count = def.clawAnimCount;
    return def.clawAnims;
}

const char* const* ZMClassBodyClaws(int cls) {
    return ZMClassInfo(cls).bodyClaws;
}

// ---------------------------------------------------------------------------
// Per-class ability tuning.
//
// Server-side only, on purpose: pev->viewmodel is already replicated, so the
// per-class claw/bomb viewmodels need no client DLL support.
// ---------------------------------------------------------------------------

#define ZM_DASH_SPEED      900.0f
#define ZM_DASH_LIFT       180.0f
#define ZM_DASH_COOLDOWN   6.0f

#define ZM_LEAP_SPEED      800.0f
#define ZM_LEAP_LIFT       420.0f
#define ZM_LEAP_COOLDOWN   7.0f

// Fallback lunge for the three classes with no named ability (zombie, deimos,
// heavy). Before the class split every zombie lunged on right-click from
// ZPPlayerThink; the per-class ability switch replaced that wholesale, and
// ZMABILITY_NONE fell through to a bare claw swing with no forward movement.
// Restored here as the default so right-click still lunges for them.
//
// Values are the original charge verbatim (750 forward, 220 up, 6s cooldown).
// Note this is deliberately not scaled by ZPFeatureGravity() the way dash and
// leap are, so the feel matches what these classes had before.
#define ZM_LUNGE_SPEED     750.0f
#define ZM_LUNGE_LIFT      220.0f
#define ZM_LUNGE_COOLDOWN  6.0f

#define ZM_HEAL_DURATION   6.0f
#define ZM_HEAL_RADIUS     160.0f
#define ZM_HEAL_INTERVAL   0.5f
#define ZM_HEAL_PER_TICK   10.0f    // x2 per second, so 20 hp/s
#define ZM_HEAL_COOLDOWN   10.0f

#define ZM_SLAM_RADIUS     120.0f
#define ZM_SLAM_DAMAGE     60.0f
#define ZM_SLAM_COOLDOWN   8.0f

// deimos and boss share the "get angry when shot" passive: every hit taken
// stacks a speed ramp for a few seconds, capped so it stays readable
#define ZM_RAGE_DURATION   8.0f
#define ZM_RAGE_COOLDOWN   20.0f
#define ZM_RAGE_DAMAGE     1.2f     // claw multiplier while raging
#define ZM_RAGE_STACK_CAP  6
#define ZM_RAGE_PER_STACK  0.05f    // +5% speed per stack, so +30% at the cap

// deimos's rage is shorter than the boss's, since deimos has no active ability
#define ZM_DEIMOS_RAGE_TIME   5.0f

// boss: each kill banks permanent speed for the rest of the round
#define ZM_BOSS_KILL_SPEED 40.0f
#define ZM_BOSS_MAX_SPEED  160.0f

// china: claw bonus while actually running at someone
#define ZM_CHINA_MOVE_SPEED  200.0f
#define ZM_CHINA_MOVE_DAMAGE 1.25f

// speed multiplier from the current rage stacks, 1.0 when not raging
static float ZMRageSpeedMultiplier(const ZPPlayer& p) {
    if (p.rageUntil <= gpGlobals->time) return 1.0f;
    int stacks = p.rageStacks;
    if (stacks > ZM_RAGE_STACK_CAP) stacks = ZM_RAGE_STACK_CAP;
    if (stacks < 0) stacks = 0;
    return 1.0f + stacks * ZM_RAGE_PER_STACK;
}

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

// Re-assert v.modelindex from v.model when the two have drifted apart.
//
// These are two independent fields: SV_SetModel writes both together, but
// CheckPowerups() in player.cpp resets pev->modelindex back to the stock
// player index every frame to keep the player.mdl eye model off the player
// entity, which leaves v.model still naming the class model. pfnGetModelPtr
// resolves the studio header from v.modelindex, so a class player's header
// silently reverts to the stock table every frame and all sequence indices get
// resolved against the wrong model.
//
// Re-issue SET_MODEL, which rewrites both fields from the path v.model already
// holds. Only runs when they actually disagree.
void ZPSyncPlayerModelIndex( edict_t *player )
{
    if( !player || player->free || !player->v.model )
        return;

    // Zombies only, and deliberately so. CheckPowerups forces the stock player
    // index for humans on purpose: a human's sequences are numbered against
    // player.mdl and remapped client-side, so "repairing" a human's index here
    // would resolve their walk/run/idle against a custom model's own sequence
    // table and break the stock animations.
    if( !ZPIsZombie( player ) )
        return;

    const char *pszModel = STRING( player->v.model );
    if( !pszModel || !pszModel[0] )
        return;

    // Never write an index the engine cannot resolve. modelindex == 0 makes
    // Mod_Handle() return a null model and the engine dereferences it while
    // serialising the entity, which takes the server down.
    int iModel = MODEL_INDEX( pszModel );
    if( iModel == 0 || iModel == player->v.modelindex )
        return;

    player->v.modelindex = iModel;
}

// Re-apply the model this player's role calls for when v.modelindex has drifted
// off it.
//
// pfnGetModelPtr resolves the studio header through v.modelindex, and
// SV_SetModel rewrites both v.model and v.modelindex together. So every call
// that re-applies a model -- ZPRoundResetPlayer on a round reset, Spawn() on
// respawn -- silently puts the modelindex back on the stock player while the
// class is still active. SetAnimation then picks sequence indices against the
// stock table, and the client renders them against the class model. The two
// disagree for as long as nobody corrects it, which is what produced the doll
// pose and the jump playing the swim clip.
//
// Zombies only. It runs from SetAnimation, i.e. every frame for every player,
// so it must never touch humans: CheckPowerups owns the stock player index for
// them on purpose, and a human running a custom model is still numbered against
// player.mdl server-side. Re-asserting here would fight that and break stock
// animations.
//
// It also deliberately uses SET_MODEL rather than ZPSetPlayerModel. The latter
// rewrites the client userinfo key, which is right once at infection time but
// is network traffic and an info-string rebuild if it ever fires on this path.
bool ZPEnsurePlayerModel(edict_t* player) {
    if (!player || player->free) return false;

    if (!ZPIsZombie(player)) return false;

    int idx = ENTINDEX(player);
    if (idx < 1 || idx > (int)gpGlobals->maxClients) return false;

    ZMClasses cls = g_players[idx].ZMClass;
    if (!ZMClassValid(cls)) return false;

    const char* want = ZMClassModel(cls);
    if (!want || !want[0]) return false;

    char szPath[160];
    snprintf(szPath, sizeof(szPath), "models/player/%s/%s.mdl", want, want);

    if (player->v.model && !stricmp(STRING(player->v.model), szPath)) return false;

    ZP_Trace("[model] re-assert ent=%d -> %s (was %s)\n", idx, want,
             player->v.model ? STRING(player->v.model) : "<null>");

    SET_MODEL(player, szPath);
    return true;
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

// svc_lightstyle is [byte style][string pattern] in stock GoldSrc, but Xash's
// client reads a trailing float (the pattern clock) as well whenever the
// connection isn't GoldSrc/Quake, so the payload has to match the engine or
// the stream desyncs. pfnCVarGetPointer only reports cvars the engine really
// has and rcon_enable is Xash-only, so probing it picks the right format.
static bool ZPLightstyleWantsFloat(void) {
    static int cached = -1;
    if (cached < 0)
        cached = g_engfuncs.pfnCVarGetPointer("rcon_enable") ? 1 : 0;
    return cached != 0;
}

// how often the red vision fade is re-pinned while someone stays infected
static const float ZP_VISION_REFRESH = 1.0f;

// Pushes one player's infection state to their own client, no client dll
// involved: lightstyle 0 is driven to "z" (double bright) as a per-client fake
// fullbright and back to "m" (the map's normal light) once they stop being a
// zombie, and the red vision rides the engine's own ScreenFade message.
//
// The fade is sent with FFADE_STAYOUT so it pins at alpha instead of ramping
// and dying (alpha 0 puts the screen back). Because every other ScreenFade the
// mod sends replaces it wholesale -- kill screens, round events, item flashes
// -- ZPPlayerThink re-asserts it while they stay infected.
void ZPSyncZombieState(edict_t* player) {
    if (!ZPIsPlayerConnected(player)) return;

    int idx = ENTINDEX(player);
    bool zombie = ZPIsZombie(player);

    MESSAGE_BEGIN(MSG_ONE, SVC_LIGHTSTYLE, NULL, player);
        WRITE_BYTE(0);
        WRITE_STRING(zombie ? "z" : "m");
        if (ZPLightstyleWantsFloat()) {
            // no pfnWriteFloat in the server API, but MSG_WriteLong and
            // MSG_WriteFloat push the same 32 bits, so the bit pattern of the
            // float goes out identically. The value only seeds the animation
            // clock and single-character patterns ignore it.
            union { float f; int i; } clock;
            clock.f = 1.0f;
            WRITE_LONG(clock.i);
        }
    MESSAGE_END();

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;

    bool inSlot = (idx >= 1 && idx <= gpGlobals->maxClients);

    if (zombie) {
        // (225, 50, 50) at alpha 110: the screen blends that red over the
        // world rather than fading to it, so the map stays readable
        UTIL_ScreenFade(pPlayer, Vector(225, 50, 50), 0.0f, 0.0f, 110, FFADE_STAYOUT);
        if (inSlot) g_players[idx].visionTinted = true;
    } else if (inSlot && g_players[idx].visionTinted) {
        // only clear a tint we actually put up, so a round event's own fade
        // isn't cancelled for someone who never had the vision
        UTIL_ScreenFade(pPlayer, g_vecZero, 0.0f, 0.0f, 0, FFADE_STAYOUT);
        g_players[idx].visionTinted = false;
    }

    if (inSlot)
        g_players[idx].nextVisionSync = gpGlobals->time + ZP_VISION_REFRESH;
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
    g_players[idx].ZMClass = ZM_CLASS_ZOMBIE;
    g_players[idx].isFrozen = false;
    g_players[idx].lastHuman = false;
    g_players[idx].killedByHeadshot = false;
    g_players[idx].kills = 0;
    g_players[idx].infections = 0;
    g_players[idx].headshots = 0;
    g_players[idx].lastInfectKiller = 0;
    g_players[idx].lastInfectTime = 0.0f;
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
    ZPRoundResetAbilities(idx);

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

    ZPSyncZombieState(ed);
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
    g_players[idx].ZMClass = ZM_CLASS_ZOMBIE;
    g_players[idx].menuType = ZPMENU_NONE;
    g_players[idx].abilityMenuUntil = 0;

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

    // fresh join always starts human, so this clears any stale overlay
    ZPSyncZombieState(player);

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
    g_players[idx].ZMClass = ZM_CLASS_ZOMBIE;
    g_players[idx].menuType = ZPMENU_NONE;
    g_players[idx].abilityMenuUntil = 0;
    g_players[idx].welcomeMusicPending = false;
    g_players[idx].welcomeMusicStarted = false;
    player->v.health = 0;
    player->v.team = 0;

    int connected = ZPCountConnectedPlayers();
    if (connected < ZPMinPlayers()) {
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

// the zombie class colour doubles as the noclip/admin glow tint
static void ZMApplyGlow(edict_t* player, int cls, float amt) {
    int rgb = ZMClassColor(cls);
    player->v.rendercolor = Vector((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    player->v.renderamt = amt;
}

// applies per-class speed/gravity/render every frame for zombies so nothing
// can override them; frozen zombies get locked in place with a blue tint
void ZPApplyZombieClass(edict_t* player)
{
    if (!player) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    int cls = g_players[idx].ZMClass;

    // class base, plus the boss's permanent per-kill ramp and any active rage
    float speed = ZMClassSpeed(cls);
    if (g_players[idx].bossRageSpeed > 0.0f)
        speed += g_players[idx].bossRageSpeed;
    speed *= ZMRageSpeedMultiplier(g_players[idx]);

    float gravity = ZMClassGravity(cls);

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
        ZMApplyGlow(player, cls, cls == ZM_CLASS_BOSS ? 80.0f : cls == ZM_CLASS_TANK ? 70.0f : 45.0f);
        return;
    }

    player->v.movetype = MOVETYPE_WALK;
    player->v.maxspeed = speed + ZPFeatureSpeedMultiplier();
    if (g_players[idx].adrenalineUntil > gpGlobals->time)
        player->v.maxspeed = speed * 1.3f + ZPFeatureSpeedMultiplier();
    player->v.gravity = gravity * ZPFeatureGravity();
    player->v.renderfx = kRenderFxGlowShell;
    ZMApplyGlow(player, cls, cls == ZM_CLASS_BOSS ? 80.0f : cls == ZM_CLASS_TANK ? 70.0f : 45.0f);
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
    ZP_Trace("ZPInfectPlayer enter slot=%d state=%d\n",
             ENTINDEX(player), (int)g_round.state);

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].ZMClass = ZM_CLASS_ZOMBIE;
    ZPRoundResetAbilities(idx);

    bool forceBoss = g_players[idx].bossRoundStart;
    g_players[idx].bossRoundStart = false;

    // Weighted roll over the eight classes. Kept as its own table because this
    // is about spawn distribution, not balance: a boss should stay an event
    // (~2% of natural infections) while plain zombies stay the common case.
    if ((!wasInfectedBySomeone && !forceBoss && RANDOM_LONG(1, 50) == 1) ||
        (forceBoss && !wasInfectedBySomeone)) {
        g_players[idx].ZMClass = ZM_CLASS_BOSS;
        EMIT_SOUND(player, CHAN_AUTO, forceBoss ? "zpmod/round_start_boss.wav"
                                                : "ambience/the_horror3.wav",
                   1.0, ATTN_NONE);
    } else {
        int roll = RANDOM_LONG(1, kZMClassRollTotal);
        int cls = ZM_CLASS_ZOMBIE;
        for (int i = 0; i < ZM_CLASS_COUNT; i++) {
            roll -= g_zmClassWeights[i];
            if (roll <= 0) { cls = i; break; }
        }
        g_players[idx].ZMClass = (ZMClasses)cls;

        // One-shot diagnostic. If this line never appears in the console the
        // game is not running this server dll at all, which invalidates every
        // animation theory. Reports the model the client will actually draw and
        // the real frame counts behind the sequences the server picks.
        if (pPlayer && pPlayer->pev->model) {
            int aimSeq = pPlayer->LookupSequence("ref_aim_onehanded");
            int shooSeq = pPlayer->LookupSequence("ref_shoot_onehanded");
            static const char* const kIdle[] = {"idle1", "walk", "run", NULL};
            int idle = pPlayer->LookupFirstUsableSequence(kIdle);
            static const char* const kSwing[] = {"run", "walk", "idle1", NULL};
            int swing = pPlayer->LookupFirstUsableSequence(kSwing);
            ZP_Trace("[anim] idx=%d model=%s\n", idx, STRING(pPlayer->pev->model));
            ZP_Trace("[anim] idx=%d ref_aim_onehanded=%d ref_shoot_onehanded=%d\n",
                     idx, aimSeq, shooSeq);
            ZP_Trace("[anim] idx=%d idle=%d swing=%d seq=%d gait=%d\n",
                     idx, idle, swing, (int)pPlayer->pev->sequence,
                     (int)pPlayer->pev->gaitsequence);
        }

        if (RANDOM_LONG(1, 2) == 1) {
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

    pPlayer->pev->health = ZMClassHealth(g_players[idx].ZMClass);
    pPlayer->pev->armorvalue = ZMClassArmor(g_players[idx].ZMClass);
    pPlayer->pev->armortype = 0.5f;   // absorb half damage until depleted

    ZPApplyZombieClass(player);

    hudtextparms_t params;
    memset(&params, 0, sizeof(params));

    params.channel = 0;
    params.x = -1;
    params.y = 0.1f;
    params.r1 = 255; params.g1 = 0; params.b1 = 0;
    params.a1 = 255;
    params.fadeinTime = 0.3f;
    params.fadeoutTime = 0.3f;
    params.holdTime = 4.0f;
    if (g_players[idx].ZMClass == ZM_CLASS_BOSS)
        UTIL_HudMessageAll(params, "The boss has awakened...");

    // class announce to the freshly infected player, tinted per class
    int rgb = ZMClassColor(g_players[idx].ZMClass);
    char clsMsg[48];
    snprintf(clsMsg, sizeof(clsMsg), "YOU ARE A %s", ZMClassName(g_players[idx].ZMClass));
    params.y = 0.15f;
    params.r1 = (rgb >> 16) & 0xFF;
    params.g1 = (rgb >> 8) & 0xFF;
    params.b1 = rgb & 0xFF;
    UTIL_HudMessage(CBaseEntity::Instance(player), params, clsMsg);

    // red infection burst — heavier for the boss
    UTIL_ParticleEffect(pPlayer->pev->origin + Vector(0, 0, 32), Vector(0, 0, 96), 235, g_players[idx].ZMClass == ZM_CLASS_BOSS ? 60 : 30);

    pPlayer->RemoveAllItems(false);
    pPlayer->GiveNamedItem("weapon_crowbar");
    pPlayer->SelectItem("weapon_crowbar");
    // infection bomb: zombies only, one per infection
    pPlayer->GiveNamedItem("weapon_infectionbomb");

    player->v.team = RoleToInt(ROLE_ZOMBIE);
    // the claw/bomb viewmodels are class specific and get attached by the
    // weapons themselves (CCrowbar::Deploy, CWeaponInfectionBomb::ViewModelPath)
    pPlayer->pev->pain_finished = gpGlobals->time;

    ZPSetPlayerModel(player, ZMClassModel(g_players[idx].ZMClass));

    // Diagnostic must run AFTER ZPSetPlayerModel -- before it, pev->model is
    // still the human's stock player.mdl, which is what an earlier probe here
    // reported and made it look like the class models were never applied.
    {
        CBasePlayer* p = (CBasePlayer*)GET_PRIVATE(player);
        ZP_Trace("[anim] post-set idx=%d class=%s(%s) model=%s\n", idx,
                 ZMClassName(g_players[idx].ZMClass),
                 ZMClassModel(g_players[idx].ZMClass),
                 p && p->pev->model ? STRING(p->pev->model) : "<null>");
        if (p) {
            int mdlIndex = (int)p->pev->model;
            studiohdr_t* h = (studiohdr_t*)GET_MODEL_PTR(ENT(p->pev));
            ZP_Trace("[anim] post-set idx=%d modelindex=%d hdr=%p numseq=%d\n",
                     idx, mdlIndex, (void*)h, h ? h->numseq : -1);
            ZP_Trace("[anim] post-set idx=%d aim1h=%d shoot1h=%d idle1=%d walk=%d run=%d\n",
                     idx, p->LookupSequence("ref_aim_onehanded"),
                     p->LookupSequence("ref_shoot_onehanded"),
                     p->LookupSequence("idle1"),
                     p->LookupSequence("walk"),
                     p->LookupSequence("run"));
            ZP_Trace("[anim] post-set idx=%d seq=%d gait=%d\n",
                     idx, (int)p->pev->sequence, (int)p->pev->gaitsequence);
        }
    }

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

    ZPSyncZombieState(player);
}

// admins can revert a zombie back to a human
void ZPMakeHuman(edict_t* ed) {
    if (!ed) return;
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
    if (!pPlayer) return;
    int idx = ENTINDEX(ed);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    g_players[idx].ZMClass = ZM_CLASS_ZOMBIE;
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

    ZPSyncZombieState(ed);
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
    // claw registration mirrors the crowbar: aim with the *view* angles (not
    // pev->angles, which drops vertical pitch and makes the swing unable to
    // hit anyone above/below you) and fall back to a head-hull trace so
    // grazes still connect instead of whiffing a pixel off
    UTIL_MakeVectors(pPlayer->pev->v_angle);
    Vector vecSrc = pPlayer->pev->origin + pPlayer->pev->view_ofs;
    Vector vecEnd = vecSrc + gpGlobals->v_forward * range;

    UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, pPlayer->edict(), ptr);

    if (ptr->flFraction >= 1.0f)
    {
        UTIL_TraceHull(vecSrc, vecEnd, dont_ignore_monsters, head_hull, pPlayer->edict(), ptr);
        if (ptr->flFraction < 1.0f && ptr->pHit)
        {
            CBaseEntity* pHit = CBaseEntity::Instance(ptr->pHit);
            if (pHit && pHit->IsBSPModel())
                FindHullIntersection(vecSrc, *ptr, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX, pPlayer->edict());
        }
    }

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
        int zidx = ENTINDEX(player);
        EMIT_SOUND(player, CHAN_AUTO,
                   ZMClassDeathSound(g_players[zidx].ZMClass, RANDOM_LONG(0, 1)),
                   1.0, ATTN_NORM);

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

    // death drops the zombie flag: overlay off, lightstyle back to normal
    ZPSyncZombieState(player);

    return false;
}

void ZPHurt(edict_t* player) {
    if (ZPIsZombie(player)) {
        int idx = ENTINDEX(player);
        if (idx < 1 || idx > gpGlobals->maxClients) return;
        EMIT_SOUND(player, CHAN_AUTO, ZMClassHurtSound(g_players[idx].ZMClass, RANDOM_LONG(0, 1)),
                   1.0, ATTN_NORM);
    }
}

void ZPSendClawAnim(edict_t* player, int seq)
{
    if (!player || !player->v.modelindex)
        return;

    // Belt and braces against the crash class, not a substitute for the real
    // fix. The server has no pfnGetModel, so numseq cannot be queried at
    // runtime -- but every v_claws_*.mdl has exactly 8 sequences (deimos 9), and
    // a sequence index past the end makes the client read a pseqdesc from beyond
    // the sequence array and animate off garbage. Clamping here means a bad
    // table entry can never take the server down again.
    if (seq < 0 || seq > ZMCLAW_MAX_SEQ)
    {
        ZP_Trace("[claw] seq %d out of range, clamped to idle\n", seq);
        seq = ZMCLAW_IDLE;
    }

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer)
        return;

    pPlayer->pev->weaponanim = seq;
    MESSAGE_BEGIN(MSG_ONE, SVC_WEAPONANIM, NULL, player);
        WRITE_BYTE(seq);
        WRITE_BYTE(0);
    MESSAGE_END();
}

void ZPZombieSwing(edict_t* player)
{
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    if (g_players[idx].frozenUntil > gpGlobals->time) return;

    // Play the claw swing. The anim list is per-class only because deimos'
    // slash1/slash2 are 2-frame stubs and it has to fall back to midslash;
    // every other class cycles the plain slash pair. See g_kClawAnims2.
    // The body needs a swing anim too (the crafting crowbar swing is skipped
    // entirely for zombies, so do it here).
    int cls = g_players[idx].ZMClass;
    int animCount = 0;
    const int* anims = ZMClassClawAnims(cls, &animCount);
    if (!anims || animCount <= 0) return;

    int seq = anims[g_players[idx].clawSwing % animCount];
    g_players[idx].clawSwing++;

    ZPSendClawAnim(player, seq);

    // Hold the swing clip for roughly its own length, then let ZPPlayerThink
    // put the viewmodel back on idle. Without this the viewmodel would snap
    // back before the swipe is visible.
    //
    // The length comes from the class table rather than a header lookup: the
    // server DLL has no pfnGetModel, and GET_MODEL_PTR(player) would resolve
    // the body model anyway (129 sequences for necozpmod_zombie against the
    // viewmodel's 8), so neither can report the viewmodel's clip length.
    float flLength = 0.5f;
    const int* frames = ZMClassClawAnimFrames(cls, &animCount);
    int iSlot = (g_players[idx].clawSwing - 1) % animCount;
    if (frames && iSlot >= 0 && iSlot < animCount)
        flLength = (frames[iSlot] > 0 ? frames[iSlot] : 1) / ZMCLAW_VIEW_FPS;

    g_players[idx].nextClawAnim = gpGlobals->time + flLength;

    // Drive the body from the class's attack sequences. Two traps here:
    //
    // 1. Do NOT go through SetAnimation(PLAYER_ATTACK1). It resolves through
    //    m_szAnimExtention to "ref_aim_crowbar"/"ref_shoot_crowbar", which none
    //    of the necozpmod_* models ship.
    // 2. Setting pev->sequence on its own is not enough either. PostThink calls
    //    SetAnimation(PLAYER_IDLE/PLAYER_WALK) every single frame, and its
    //    ACT_WALK case only leaves pev->sequence alone while m_Activity is
    //    ACT_RANGE_ATTACK1. Without that, the claw is overwritten on the very
    //    next frame and never plays a frame at all.
    //
    // Verified against the model data: none of the eight models contain a single
    // sequence named zbs_*, attack or claw, so the per-class bodyClaws lists never
    // match anything, and "ref_shoot_onehanded" resolves to a two-frame stub that
    // only moves the root bone -- the model would sit in its raw reference pose for
    // the whole swing. The only clips these models actually animate are the
    // locomotion ones, so the swing falls back to "run", the most aggressive
    // real sequence available, and every candidate is vetted for real frames.
    const char* const* bodyClaws = ZMClassBodyClaws(cls);
    int bodySeq = -1;
    if (bodyClaws && bodyClaws[0]) {
        int n = 0;
        while (bodyClaws[n]) n++;
        const char* const* pick = &bodyClaws[(g_players[idx].clawSwing - 1) % n];
        bodySeq = pPlayer->LookupFirstUsableSequence(pick);
        if (bodySeq < 0) {
            // wrapped to the end of the table: try the remaining entries in order
            for (int i = 0; i < n && bodySeq < 0; i++) {
                if (i == (g_players[idx].clawSwing - 1) % n) continue;
                bodySeq = pPlayer->LookupFirstUsableSequence(&bodyClaws[i]);
            }
        }
    }
    if (bodySeq < 0) {
        static const char* const kSwing[] = { "run", "walk", "idle1", NULL };
        bodySeq = pPlayer->LookupFirstUsableSequence(kSwing);
    }
    if (bodySeq < 0)
        bodySeq = 0;

    pPlayer->m_Activity = ACT_RANGE_ATTACK1;
    pPlayer->m_IdealActivity = ACT_RANGE_ATTACK1;
    pPlayer->pev->sequence = bodySeq;
    pPlayer->pev->frame = 0;
    pPlayer->ResetSequenceInfo();

    TraceResult tr; 
    CBaseEntity* pHit = ZPZombieCheckHit(pPlayer, 70.0f, &tr);

    if (pHit && pHit->IsPlayer() && ZPIsHuman(pHit->edict())) {
        // ReZombie-style claw: deals melee damage per hit (class-dependent).
        // Armor absorbs part of it; a killing blow from a zombie converts the
        // victim (handled in ZPDied).
        float dmg = ZPZombieClawDamage(player);
        pHit->TakeDamage(pPlayer->pev, pPlayer->pev, dmg, DMG_SLASH);

        // splash blood at the wound so a connecting hit reads as a hit
        int bloodColor = pHit->BloodColor();
        UTIL_BloodDrips(tr.vecEndPos, gpGlobals->v_forward, bloodColor, (int)dmg);
        UTIL_BloodDecalTrace(&tr, bloodColor);

        // reward for landing a hit that converted the human
        if (ZPIsZombie(pHit->edict()))
            ZPZombieHeal(player, 100.0f);

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
        if (r == 1) EMIT_SOUND(player, CHAN_AUTO, "zpmod/swing_1.wav", 1.0, ATTN_NORM);
        else if (r == 2) EMIT_SOUND(player, CHAN_AUTO, "zpmod/swing_2.wav", 1.0, ATTN_NORM);
        else EMIT_SOUND(player, CHAN_AUTO, "zpmod/swing_3.wav", 1.0, ATTN_NORM);
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

    // class max is 2000-5000 now, so a raw HP number is unreadable: show the
    // percentage of the target's class maximum for zombies and raw HP for
    // humans, who are always on 100
    int percent = hp;
    if (ZPIsZombie(ed)) {
        float maxHp = ZMClassHealth(g_players[target].ZMClass);
        percent = (maxHp > 0.0f) ? (int)((hp * 100.0f) / maxHp) : 0;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
    }

    if (ZPIsZombie(ed)) {
        int rgb = ZMClassColor(g_players[target].ZMClass);
        aim.r1 = (rgb >> 16) & 0xFF;
        aim.g1 = (rgb >> 8) & 0xFF;
        aim.b1 = rgb & 0xFF;
    } else if (ed->v.team == RoleToInt(ROLE_SPECTATOR)) {
        aim.r1 = aim.g1 = aim.b1 = 190;
    } else {
        aim.r1 = 40; aim.g1 = 255; aim.b1 = 90;
    }

    const char* name = STRING(ed->v.netname);
    if (!name) name = "player";

    char buf[96];
    if (ZPIsZombie(ed))
        snprintf(buf, sizeof(buf), "%s\nHP  %d%%", name, percent);
    else
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
        bool isZombie = ZPIsZombie(ed);
        if (isZombie) {
            className = ZMClassName(g_players[i].ZMClass);
        }
        else if (ed->v.team == RoleToInt(ROLE_SPECTATOR)) className = "Spectator";

        // zombies show a percentage of their class maximum; raw HP is in the
        // thousands and useless at a glance
        char hpText[16];
        if (isZombie) {
            float maxHp = ZMClassHealth(g_players[i].ZMClass);
            int percent = (maxHp > 0.0f) ? (hp * 100) / (int)maxHp : 0;
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            snprintf(hpText, sizeof(hpText), "%d%%", percent);
        } else {
            snprintf(hpText, sizeof(hpText), "%d", hp);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "HP %s  |  %s", hpText, className);

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
    round->roundDuration = 180.0f;
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

void ZPCleanupWorld(void) {
    for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++) {
        edict_t* ed = INDEXENT(i);
        if (!ed || ed->free || FNullEnt(ed))
            continue;

        const char* cls = STRING(ed->v.classname);
        if (!cls || !cls[0])
            continue;

        if (!strncmp(cls, "weaponbox", 9) ||
            !strncmp(cls, "zp_grenade", 10) ||
            !strncmp(cls, "zp_supplybox", 13) ||
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

// Model indices handed back by PRECACHE_MODEL at load time.
//
// Do NOT assign these into pev->v.model. v.model holds a *string* index, not a
// model index: doing so made STRING(pev->model) return the literal "shot" and
// GET_MODEL_PTR fall back to player.mdl (numseq=77), which is visible in the
// debug log. SET_MODEL by path is correct and is what the engine expects.
int g_zmModelIndex[ZM_CLASS_COUNT] = { 0 };
int g_zmHumanModelIndex = 0;
const char* g_zmHumanModel = "";

void ZPSetPlayerModel(edict_t* player, const char* modelName )
{
    if (!player || player->free || !modelName || !modelName[0])
        return;

    char modelPath[160];

    if( strcmp( modelName, "player" ) == 0 )
        snprintf( modelPath, sizeof(modelPath), "models/player.mdl" );
    else
        snprintf( modelPath, sizeof(modelPath), "models/player/%s/%s.mdl", modelName, modelName );

    // Assign by path. This is what pfnSetModel is for and it leaves v.model
    // holding a valid string index, which the model resolver can turn back into
    // the right header.
    SET_MODEL( player, modelPath );

    // Userinfo carries the bare name: that is the form the client uses to
    // resolve its own copy of the player model.
    char* infoBuffer = g_engfuncs.pfnGetInfoKeyBuffer( player );
    if( infoBuffer )
    {
        g_engfuncs.pfnSetClientKeyValue( ENTINDEX( player ), infoBuffer, "model", modelName );
    }
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

    // zombie kills feed the boss's permanent speed ramp
    ZPZombieOnKilled(killer);

    // the old flat 200 cap is meaningless on classes that run to 5000 HP, so
    // zombies heal relative to their own class maximum
    if (ZPIsZombie(killer))
        ZPZombieHeal(killer, fromHeadshot ? 50.0f : 25.0f);
    else {
        pPlayer->pev->health += fromHeadshot ? 50.0f : 25.0f;
        if (pPlayer->pev->health > 200) pPlayer->pev->health = 200;
    }

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

    const char* msg = fromHeadshot ? "HEADSHOT KILL  +50 HP" : "KILL BONUS  +25 HP";
    UTIL_HudMessage(CBaseEntity::Instance(killer), params, msg);
}

void ZPRoundResetAbilities(int playerIndex) {
    if (playerIndex < 0 || playerIndex > 32) return;
    g_players[playerIndex].abilityCooldown = 0.0f;
    g_players[playerIndex].rageUntil = 0.0f;
    g_players[playerIndex].healAuraUntil = 0.0f;
    g_players[playerIndex].bossRageSpeed = 0.0f;
    g_players[playerIndex].rageStacks = 0;
    g_players[playerIndex].nextHealPulse = 0.0f;
}

// Adds health to a zombie, clamped to their own class maximum. Every heal in
// the mod has to go through this: the old code capped at a flat 200, which is
// meaningless now that classes run from 800 to 5000 HP.
void ZPZombieHeal(edict_t* player, float amount) {
    if (!ZPIsZombie(player)) return;
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer) return;

    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    float maxHp = ZMClassHealth(g_players[idx].ZMClass);
    pPlayer->pev->health += amount;
    if (pPlayer->pev->health > maxHp)
        pPlayer->pev->health = maxHp;
}

float ZPZombieDamageTaken(edict_t* player) {
    if (!ZPIsZombie(player)) return 1.0f;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return 1.0f;
    return ZMClassDamageTaken(g_players[idx].ZMClass);
}

// Called from CBasePlayer::TakeDamage when a zombie is hit. Feeds the two
// passive "get angry when shot" abilities.
void ZPZombieOnDamaged(edict_t* player, float damage) {
    if (!ZPIsZombie(player) || damage <= 0.0f) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;

    switch (g_players[idx].ZMClass) {
        case ZM_CLASS_DEIMOS:
            g_players[idx].rageStacks++;
            g_players[idx].rageUntil = gpGlobals->time + ZM_DEIMOS_RAGE_TIME;
            break;

        case ZM_CLASS_BOSS:
            g_players[idx].rageStacks++;
            g_players[idx].rageUntil = gpGlobals->time + ZM_RAGE_DURATION;
            break;

        default:
            break;
    }
}

// Boss kills bank permanent speed for the round.
void ZPZombieOnKilled(edict_t* player) {
    if (!ZPIsZombie(player)) return;
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    if (g_players[idx].ZMClass != ZM_CLASS_BOSS) return;
    if (g_players[idx].bossRageSpeed >= ZM_BOSS_MAX_SPEED) return;

    g_players[idx].bossRageSpeed += ZM_BOSS_KILL_SPEED;
    g_players[idx].rageUntil = gpGlobals->time + ZM_RAGE_DURATION;
    g_players[idx].rageStacks++;

    char buf[64];
    snprintf(buf, sizeof(buf), "BOSS ENRAGED  SPEED +%d", (int)g_players[idx].bossRageSpeed);
    hudtextparms_t params;
    memset(&params, 0, sizeof(params));
    params.channel = 2;
    params.x = -1;
    params.y = 0.2f;
    params.r1 = 255; params.g1 = 0; params.b1 = 40;
    params.a1 = 255;
    params.fadeinTime = 0.1f;
    params.fadeoutTime = 0.5f;
    params.holdTime = 1.5f;
    UTIL_HudMessage(CBaseEntity::Instance(player), params, buf);
}

// current claw damage, including rage, china's speed bonus and round events
float ZPZombieClawDamage(edict_t* player) {
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return 0.0f;

    int cls = g_players[idx].ZMClass;
    float dmg = ZMClassClawDamage(cls);

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (cls == ZM_CLASS_CHINA && pPlayer) {
        Vector flat = pPlayer->pev->velocity;
        flat.z = 0.0f;
        if (flat.Length() >= ZM_CHINA_MOVE_SPEED)
            dmg *= ZM_CHINA_MOVE_DAMAGE;
    }

    if (g_players[idx].rageUntil > gpGlobals->time)
        dmg *= ZM_RAGE_DAMAGE;

    return dmg * ZPFeatureClawMultiplier();
}

// healer aura: tops up every zombie in radius while it runs
static void ZMHealPulse(edict_t* player) {
    int idx = ENTINDEX(player);
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    if (gpGlobals->time < g_players[idx].nextHealPulse) return;
    g_players[idx].nextHealPulse = gpGlobals->time + ZM_HEAL_INTERVAL;

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* target = INDEXENT(i);
        if (!ZPIsZombie(target)) continue;
        CBasePlayer* pTarget = (CBasePlayer*)GET_PRIVATE(target);
        if (!pTarget || !pTarget->IsAlive()) continue;
        if ((pTarget->pev->origin - player->v.origin).Length() > ZM_HEAL_RADIUS) continue;

        ZPZombieHeal(target, ZM_HEAL_PER_TICK);

        MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, pTarget->pev->origin);
            WRITE_BYTE(TE_BLOODSPRITE);
            WRITE_COORD(pTarget->pev->origin.x);
            WRITE_COORD(pTarget->pev->origin.y);
            WRITE_COORD(pTarget->pev->origin.z + 32);
            WRITE_SHORT(MODEL_INDEX("sprites/blood.spr"));
            WRITE_SHORT(MODEL_INDEX("sprites/blood.spr"));
            WRITE_BYTE(120);
            WRITE_BYTE(90);
        MESSAGE_END();
    }
}

static void ZMTankSlam(edict_t* player) {
    EMIT_SOUND(player, CHAN_WEAPON, "zombie/claw_strike1.wav", 1.0, ATTN_NORM);
    EMIT_SOUND(player, CHAN_WEAPON, "debris/bustmetal1.wav", 1.0, ATTN_NORM);
    UTIL_ScreenShake(player->v.origin, 10.0f, 4.0f, 0.5f, 320.0f);

    float dmg = ZM_SLAM_DAMAGE * ZPFeatureClawMultiplier();

    CBaseEntity* pSelf = CBaseEntity::Instance(player);
    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* target = INDEXENT(i);
        if (!ZPIsHuman(target)) continue;
        CBasePlayer* pTarget = (CBasePlayer*)GET_PRIVATE(target);
        if (!pTarget || !pTarget->IsAlive()) continue;

        float dist = (pTarget->pev->origin - player->v.origin).Length();
        if (dist > ZM_SLAM_RADIUS) continue;

        // falloff from full damage at the feet to half at the edge
        pTarget->TakeDamage(pSelf->pev, pSelf->pev, dmg * (1.0f - 0.5f * dist / ZM_SLAM_RADIUS), DMG_SLASH);
    }
}

void ZPPlayerThink(edict_t* player) {
    if (!player) return;
    if (!ZPIsPlayerConnected(player)) return;

    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(player);
    if (!pPlayer || !pPlayer->IsAlive()) return;

    if (ZPIsZombie(player)) {
        // keep the crowbar's third-person worldmodel hidden, only the claw viewmodel stays
        player->v.weaponmodel = iStringNull;

        int idx = ENTINDEX(player);
        if (idx < 1 || idx > gpGlobals->maxClients) return;

        // anything else that flashes this player's screen throws our red
        // vision away, so put it back on a slow timer rather than once
        if (gpGlobals->time >= g_players[idx].nextVisionSync)
            ZPSyncZombieState(player);

        if (g_players[idx].frozenUntil > gpGlobals->time) return;

        int cls = g_players[idx].ZMClass;

        // Return the claw viewmodel to idle once its swing has had time to
        // play. The claws are not a CBasePlayerWeapon -- they are only a
        // viewmodel driven by hand over SVC_WEAPONANIM -- so there is no
        // WeaponIdle() and nothing else would ever put the model back. It sat
        // on the last swing frame indefinitely, which is why the claws looked
        // frozen between attacks.
        //
        // Only while the crowbar is actually held. pev->weaponanim is a single
        // field shared by every weapon the zombie carries, and the infection
        // bomb numbers its own 0-3 against its own viewmodel; forcing 0 here
        // while the bomb was mid-throw yanked it back to idle and cancelled the
        // cook. The bomb manages its own idle in its ItemPostFrame.
        //
        // The restore is guarded on the swing having actually left idle, so
        // idle is sent once per swing rather than every server frame. The
        // infectionbomb does not need this: it is a real weapon and its
        // ItemPostFrame() sends its own idle clip.
        if (FClassnameIs(pPlayer->pev, "weapon_crowbar") &&
            pPlayer->pev->weaponanim != ZMCLAW_IDLE &&
            gpGlobals->time >= g_players[idx].nextClawAnim)
        {
            ZPSendClawAnim(player, ZMCLAW_IDLE);
            g_players[idx].nextClawAnim = gpGlobals->time + 0.25;
        }

        // the healer's aura keeps pulsing on its own, no button needed
        if (g_players[idx].healAuraUntil > gpGlobals->time)
            ZMHealPulse(player);

        if (!(pPlayer->m_afButtonPressed & IN_ATTACK2)) return;
        if (g_players[idx].abilityCooldown > gpGlobals->time) return;

        UTIL_MakeVectors(pPlayer->pev->v_angle);
        Vector dir = gpGlobals->v_forward;
        dir.z = 0.0f;
        if (dir.Length() < 0.1f) return;
        dir = dir.Normalize();

        // leap height follows the round's gravity event so low-gravity rounds
        // actually change how far you travel
        float lift = ZPFeatureGravity();

        switch (ZMClassAbility(cls)) {
            case ZMABILITY_DASH:
                pPlayer->pev->velocity = dir * ZM_DASH_SPEED + Vector(0, 0, ZM_DASH_LIFT * lift);
                g_players[idx].abilityCooldown = gpGlobals->time + ZM_DASH_COOLDOWN;
                EMIT_SOUND(player, CHAN_WEAPON, "zombie/zo_attack1.wav", 1.0, ATTN_NORM);
                UTIL_ScreenShake(pPlayer->pev->origin, 8.0f, 3.0f, 0.5f, 256.0f);
                break;

            case ZMABILITY_LEAP:
                pPlayer->pev->velocity = dir * ZM_LEAP_SPEED + Vector(0, 0, ZM_LEAP_LIFT * lift);
                g_players[idx].abilityCooldown = gpGlobals->time + ZM_LEAP_COOLDOWN;
                EMIT_SOUND(player, CHAN_WEAPON, "zombie/zo_attack1.wav", 1.0, ATTN_NORM);
                break;

            case ZMABILITY_SLAM:
                ZMTankSlam(player);
                g_players[idx].abilityCooldown = gpGlobals->time + ZM_SLAM_COOLDOWN;
                break;

            case ZMABILITY_HEAL:
                g_players[idx].abilityCooldown = gpGlobals->time + ZM_HEAL_COOLDOWN;
                g_players[idx].healAuraUntil = gpGlobals->time + ZM_HEAL_DURATION;
                g_players[idx].nextHealPulse = gpGlobals->time;
                EMIT_SOUND(player, CHAN_WEAPON, "items/suitcharge1.wav", 1.0, ATTN_NORM);
                break;

            case ZMABILITY_RAGE:
                g_players[idx].abilityCooldown = gpGlobals->time + ZM_RAGE_COOLDOWN;
                g_players[idx].rageUntil = gpGlobals->time + ZM_RAGE_DURATION;
                g_players[idx].rageStacks++;
                EMIT_SOUND(player, CHAN_WEAPON, "zombie/zo_alert20.wav", 1.0, ATTN_NORM);
                UTIL_ScreenShake(pPlayer->pev->origin, 8.0f, 3.0f, 0.5f, 256.0f);
                break;

            default:
                // zombie, deimos and heavy have no named ability, but they
                // still lunge on right-click. This used to be unconditional for
                // every zombie; when the per-class switch landed these three
                // were left with a bare claw swing that never moved the player
                // forward. Keep the original lunge as the fallback.
                pPlayer->pev->velocity = dir * ZM_LUNGE_SPEED + Vector(0, 0, ZM_LUNGE_LIFT);
                g_players[idx].abilityCooldown = gpGlobals->time + ZM_LUNGE_COOLDOWN;
                EMIT_SOUND(player, CHAN_WEAPON, "zombie/zo_attack1.wav", 1.0f, ATTN_NORM);
                UTIL_ScreenShake(pPlayer->pev->origin, 8.0f, 3.0f, 0.5f, 256.0f);
                break;
        }
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
    if (g_mapVote.active || g_players[idx].menuType == ZPMENU_VOTE) return;

    // replacing any lingering ability menu from an earlier +use
    g_players[idx].abilityMenuUntil = 0;

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

    g_players[idx].menuType = ZPMENU_ABILITY;
    g_players[idx].abilityMenuUntil = gpGlobals->time + 6.0f;
}

void ZPAbilitySelect(int playerIndex, int slot)
{
    if (playerIndex < 1 || playerIndex > gpGlobals->maxClients) return;
    if (g_players[playerIndex].menuType != ZPMENU_ABILITY) return;
    if (g_players[playerIndex].abilityMenuUntil < gpGlobals->time) return;

    edict_t* ed = INDEXENT(playerIndex);
    if (!ZPIsPlayerConnected(ed)) return;
    CBasePlayer* pPlayer = (CBasePlayer*)GET_PRIVATE(ed);
    if (!pPlayer || !pPlayer->IsAlive()) return;
    if (g_round.state != RS_ACTIVE || !ZPIsHuman(ed)) return;

    g_players[playerIndex].menuType = ZPMENU_NONE;
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

        // the vote takes over this player's menu channel and cancels any
        // lingering ability menu so the two never interleave on-screen
        g_players[i].menuType = ZPMENU_VOTE;
        g_players[i].abilityMenuUntil = 0;

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

    for (int i = 1; i <= gpGlobals->maxClients; i++)
        g_players[i].menuType = ZPMENU_NONE;

    CHANGE_LEVEL(map, NULL);
}

void ZPMapVoteSelect(int playerIndex, int slot) {
    if (!g_mapVote.active)
        return;
    if (playerIndex < 1 || playerIndex > gpGlobals->maxClients)
        return;
    if (g_players[playerIndex].menuType != ZPMENU_VOTE)
        return;
    if (slot < 1 || slot > ZPMAPVOTE_OPTIONS)
        return;
    if (g_mapVote.options[slot - 1][0] == '\0')
        return;
    if (g_mapVote.playerVote[playerIndex] >= 0)
        return;

    g_mapVote.playerVote[playerIndex] = slot - 1;
    g_mapVote.votes[slot - 1] += 1;
    g_players[playerIndex].menuType = ZPMENU_NONE; // ballot cast, menu consumed
}

cvar_t zpmod_advertisementenabled = { "zpmod_advertisementenabled", "0", FCVAR_SERVER };

// Minimum connected players required before a round will start. The round logic
// resets every player back to a human whenever the connected count drops below
// this, and ZPRoundResetPlayer() re-Spawn()s them and restores the stock human
// model. With the default of 2 that makes single-client testing impossible: the
// round never leaves prep, so an infected player is torn back down to
// Helmet.mdl a frame or two after picking a class.
cvar_t zpmod_min_players = { "zpmod_min_players", "2", FCVAR_SERVER };

int ZPMinPlayers(void) {
    cvar_t* cvar = CVAR_GET_POINTER("zpmod_min_players");
    if (!cvar)
        return 2;
    int n = (int)cvar->value;
    if (n < 1)
        n = 1;
    return n;
}

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

    // streak icon fade: fires touch_fade once the streak has stalled
    ZPFeatureKillIconThink();

    // Unconditional so wall markers also track test boxes from zp_supplybox.
    ZPSupplyBoxIconUpdate();

    if (connectedCount < ZPMinPlayers()) {
        if (round->state != RS_PREP || round->countdownStarted) {
            round->state = RS_PREP;
            round->countdownStarted = false;
            round->lastAnnounce = -1;
            lastSpokeSecond = -1;
            ZPRoundStopAmbient();
            ZPSupplyBoxRoundReset();

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
                    EMIT_SOUND(ed, CHAN_AUTO, "zpmod/round_start.wav", 1.0, ATTN_NORM);
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
            ZPSupplyBoxRoundStart();

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
        ZPSupplyBoxThink();

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
            ZPSupplyBoxRoundReset();

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

    // re-roll the mode / boss round / event for the next one right away
    ZPFeaturePreRound(&g_round);
}

// fuck these retards that keeps sending me shit on discord

void ZPModInit(void) {
    memset(g_players, 0, sizeof(g_players));
    ZPRoundInit(&g_round);
    ZPFeatureInit();
    ZPStatsInit();
    ZPModGrenadeInit();
    ZPSupplyBoxInit();
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
    PRECACHE_SOUND("zpmod/swing_1.wav");
    PRECACHE_SOUND("zpmod/swing_2.wav");
    PRECACHE_SOUND("zpmod/swing_3.wav");
    PRECACHE_SOUND("zpmod/wall_1.wav");
    PRECACHE_SOUND("zpmod/wall_2.wav");
    PRECACHE_SOUND("zpmod/wall_3.wav");
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
    // kill announcer, one clip per streak step (see ZPStreakSound)
    PRECACHE_SOUND("zpmod/vox/firstkill.wav");
    PRECACHE_SOUND("zpmod/vox/doublekill.wav");
    PRECACHE_SOUND("zpmod/vox/triplekill.wav");
    PRECACHE_SOUND("zpmod/vox/multikill.wav");
    PRECACHE_SOUND("zpmod/vox/incredible.wav");
    PRECACHE_SOUND("zpmod/vox/cantbelive.wav");
    PRECACHE_SOUND("zpmod/vox/excellent.wav");
    PRECACHE_SOUND("zpmod/vox/crazy.wav");

    // Streak icon textures. Generic resources land in the client resource
    // list, so anyone missing them downloads the file touch_addbutton loads
    // before they spawn.
    for (int i = 1; i <= 8; i++) {
        char icon[64];

        snprintf(icon, sizeof(icon), "gfx/zpmod/%d_kill.tga", i);
        PRECACHE_GENERIC(icon);
    }
    PRECACHE_SOUND("zpmod/hurt_1.wav");
    PRECACHE_SOUND("zpmod/hurt_2.wav");
    PRECACHE_SOUND("zpmod/death_1.wav");
    PRECACHE_SOUND("zpmod/death_2.wav");
    // per-class voice sets: heavy (boss/heavy) and female (speed)
    PRECACHE_SOUND("zpmod/hurt_heavy_1.wav");
    PRECACHE_SOUND("zpmod/hurt_heavy_2.wav");
    PRECACHE_SOUND("zpmod/death_heavy_1.wav");
    PRECACHE_SOUND("zpmod/death_heavy_2.wav");
    PRECACHE_SOUND("zpmod/hurt_female_1.wav");
    PRECACHE_SOUND("zpmod/hurt_female_2.wav");
    PRECACHE_SOUND("zpmod/death_female_1.wav");
    PRECACHE_SOUND("zpmod/death_female_2.wav");
    PRECACHE_SOUND("zombie/zo_alert20.wav");
    PRECACHE_SOUND("zombie/zo_attack2.wav");
    PRECACHE_SOUND("ambience/the_horror3.wav");
    PRECACHE_SOUND("items/smallmedkit1.wav");
    PRECACHE_SOUND("items/suitchargeno1.wav");
    PRECACHE_SOUND("items/suitcharge1.wav");
    PRECACHE_SOUND("debris/bustmetal1.wav");
    PRECACHE_SOUND("debris/bustmetal2.wav");
    PRECACHE_SOUND("player/heartbeat1.wav");

    // Every class model has to be precached: a zombie can roll any of the
    // eight the moment a round starts, and clients that never precached the
    // model would show a null/garbage player.
    for (int cls = 0; cls < ZM_CLASS_COUNT; cls++) {
        char path[64];

        snprintf(path, sizeof(path), "models/player/%s/%s.mdl",
                 ZMClassModel(cls), ZMClassModel(cls));
        g_zmModelIndex[cls] = PRECACHE_MODEL(path);
        PRECACHE_GENERIC(path);

        const char* claw = ZMClawViewModel(cls);
        PRECACHE_MODEL(claw);
        PRECACHE_GENERIC(claw);

        const char* bomb = ZMBombViewModel(cls);
        PRECACHE_MODEL(bomb);
        PRECACHE_GENERIC(bomb);
    }

    // the infection bomb's shared world/player models
    PRECACHE_MODEL("models/zpmod/p_infectionbomb.mdl");
    PRECACHE_GENERIC("models/zpmod/p_infectionbomb.mdl");
    PRECACHE_MODEL("models/zpmod/w_infectionbomb.mdl");
    PRECACHE_GENERIC("models/zpmod/w_infectionbomb.mdl");

    // humans and spectators still use these
    g_zmHumanModelIndex = PRECACHE_MODEL("models/player/zm/zm.mdl");
    PRECACHE_GENERIC("models/player/zm/zm.mdl");
    g_zmHumanModel = "zm";
    PRECACHE_MODEL("models/player/helmet/helmet.mdl");
    PRECACHE_GENERIC("models/player/helmet/helmet.mdl");
    PRECACHE_MODEL("sprites/laserbeam.spr");
    PRECACHE_MODEL("sprites/lgtning.spr");

    ZPSupplyBoxPrecache();
}