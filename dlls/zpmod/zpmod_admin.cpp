#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "zpmod.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "shake.h"

#include <time.h>

extern int gmsgSayText;

// Neco ZP admin module
// - password auth via zp_admin_pass cvar (set it in server.cfg)
// - name allowlist via zpmod_admins.ini (one name per line, case-insensitive)
// - ban list via zpmod_bans.ini (lines: <lowername>\t<ip>\t<expiry-unix>)
// - all commands through chat: "!cmd args" or "/cmd args"

cvar_t zp_admin_pass = { "zp_admin_pass", "", FCVAR_SERVER };

#define ZPADMIN_BAN_FILE "zpmod_bans.ini"
#define ZPADMIN_ADMIN_FILE "zpmod_admins.ini"
#define ZPADMIN_MAX_ALLOW 64

static char g_allowNames[ZPADMIN_MAX_ALLOW][32];
static int g_allowCount = 0;
static bool g_authed[33] = { false };

static void ZPAdminLower(char* out, int outSize, const char* in) {
    if (!out || !in) return;
    int i = 0;
    for (; in[i] && i < outSize - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[i] = 0;
}

static void ZPAdminMsg(edict_t* to, const char* text) {
    if (!to) return;
    MESSAGE_BEGIN(MSG_ONE, gmsgSayText, NULL, to);
        WRITE_BYTE(ENTINDEX(to));
        WRITE_STRING(text);
    MESSAGE_END();
}

// loads the name allowlist: admins listed here don't need a password
static void ZPAdminLoadAllowList(void) {
    g_allowCount = 0;
    FILE* f = fopen(ZPADMIN_ADMIN_FILE, "r");
    if (!f) return;

    char line[64];
    while (fgets(line, sizeof(line), f) && g_allowCount < ZPADMIN_MAX_ALLOW) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len == 0 || line[0] == ';') continue; // skip blanks and comments
        ZPAdminLower(g_allowNames[g_allowCount], 32, line);
        g_allowCount++;
    }
    fclose(f);
}

static bool ZPAdminIsAllowListed(int idx) {
    const char* raw = STRING(INDEXENT(idx)->v.netname);
    if (!raw || !raw[0]) return false;

    char lower[64];
    ZPAdminLower(lower, sizeof(lower), raw);
    for (int i = 0; i < g_allowCount; i++) {
        if (strcmp(lower, g_allowNames[i]) == 0)
            return true;
    }
    return false;
}

static bool ZPAdminIsAllowed(int idx) {
    if (g_authed[idx]) return true;
    return ZPAdminIsAllowListed(idx);
}

// removes expired entries from the ban file (called on map init)
static void ZPAdminPruneBans(void) {
    FILE* in = fopen(ZPADMIN_BAN_FILE, "r");
    if (!in) return;

    time_t now = time(NULL);
    char lines[128][160];
    int count = 0;

    char line[160];
    while (fgets(line, sizeof(line), in) && count < 128) {
        char name[64], ip[64];
        long exp = 0;
        if (sscanf(line, "%63[^\t]\t%63[^\t]\t%ld", name, ip, &exp) == 3) {
            if (now < exp)
                strlcpy(lines[count++], line, sizeof(lines[0]));
        }
    }
    fclose(in);

    FILE* out = fopen(ZPADMIN_BAN_FILE, "w");
    if (!out) return;
    for (int i = 0; i < count; i++)
        fprintf(out, "%s", lines[i]);
    fclose(out);
}

static bool ZPAdminAddBan(const char* name, const char* ip, long expire) {
    FILE* f = fopen(ZPADMIN_BAN_FILE, "a");
    if (!f) return false;

    char lower[64];
    ZPAdminLower(lower, sizeof(lower), name);

    char ipBuf[64] = "";
    if (ip && ip[0]) {
        // strip the port if present ("1.2.3.4:27015" -> "1.2.3.4")
        strlcpy(ipBuf, ip, sizeof(ipBuf));
        char* colon = strchr(ipBuf, ':');
        if (colon) *colon = 0;
    }

    fprintf(f, "%s\t%s\t%ld\n", lower, ipBuf, expire);
    fclose(f);
    return true;
}

static bool ZPAdminRemoveBan(const char* name) {
    char lower[64];
    ZPAdminLower(lower, sizeof(lower), name);

    FILE* in = fopen(ZPADMIN_BAN_FILE, "r");
    if (!in) return false;

    char lines[128][160];
    int count = 0;

    char line[160];
    while (fgets(line, sizeof(line), in) && count < 128) {
        char storedName[64];
        if (sscanf(line, "%63[^\t]", storedName) == 1 && strcmp(storedName, lower) != 0)
            strlcpy(lines[count++], line, sizeof(lines[0]));
    }
    fclose(in);

    FILE* out = fopen(ZPADMIN_BAN_FILE, "w");
    if (!out) return false;
    for (int i = 0; i < count; i++)
        fprintf(out, "%s", lines[i]);
    fclose(out);
    return true;
}

// called from CHalfLifeMultiplay::ClientConnected; returns TRUE + reason if
// the joining name/ip is on the active ban list
bool ZPAdminCheckBan(const char* name, const char* address, char reason[128]) {
    if (!name || !name[0]) return false;

    char lower[64];
    ZPAdminLower(lower, sizeof(lower), name);

    char ipBuf[64] = "";
    if (address && address[0]) {
        strlcpy(ipBuf, address, sizeof(ipBuf));
        char* colon = strchr(ipBuf, ':');
        if (colon) *colon = 0;
    }

    time_t now = time(NULL);

    FILE* f = fopen(ZPADMIN_BAN_FILE, "r");
    if (!f) return false;

    bool banned = false;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char storedName[64], storedIp[64];
        long exp = 0;
        if (sscanf(line, "%63[^\t]\t%63[^\t]\t%ld", storedName, storedIp, &exp) == 3) {
            if (now > exp) continue; // expired
            if (strcmp(storedName, lower) == 0 || (ipBuf[0] && strcmp(storedIp, ipBuf) == 0)) {
                banned = true;
                break;
            }
        }
    }
    fclose(f);

    if (banned)
        snprintf(reason, 128, "Banned from this server");

    return banned;
}

void ZPAdminInit(void) {
    g_allowCount = 0;
    ZPAdminLoadAllowList();
    ZPAdminPruneBans();
    for (int i = 1; i <= gpGlobals->maxClients; i++)
        g_authed[i] = false;
}

// finds a connected player whose name starts with the given fragment
static edict_t* ZPAdminFindPlayer(const char* fragment) {
    if (!fragment) return nullptr;

    char frag[64];
    ZPAdminLower(frag, sizeof(frag), fragment);

    for (int i = 1; i <= gpGlobals->maxClients; i++) {
        edict_t* e = INDEXENT(i);
        if (!ZPIsPlayerConnected(e)) continue;

        const char* raw = STRING(e->v.netname);
        if (!raw) continue;

        char lower[64];
        ZPAdminLower(lower, sizeof(lower), raw);
        if (strstr(lower, frag) == lower) // prefix match; also matches exact
            return e;
    }
    return nullptr;
}

bool ZPAdminCommand(edict_t* sender, const char* text) {
    if (!sender || !text) return false;
    if (text[0] != '!' && text[0] != '/') return false;
    if (text[1] == 0) return false;

    int idx = ENTINDEX(sender);
    if (idx < 1 || idx > gpGlobals->maxClients) return false;

    char buf[128];
    strlcpy(buf, text + 1, sizeof(buf));

    // tokenize
    char* argv[8];
    int argc = 0;
    char* p = buf;
    while (*p && argc < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
    if (argc < 1) return false;

    const char* cmd = argv[0];
    bool isAdmin = ZPAdminIsAllowed(idx);
    char outMsg[128];

    // ---- public commands ----
    if (!strcmp(cmd, "help")) {
        ZPAdminMsg(sender,
            "!help !auth <pass> !who\n"
            "Admin: !kick !ban !unban !slap !slay !heal !freeze !unfreeze\n"
            "!tp !bring !zombie !human !noclip !god !map !endround !restart");
        return true;
    }

    if (!strcmp(cmd, "auth") || !strcmp(cmd, "login")) {
        if (argc < 2) {
            ZPAdminMsg(sender, "Usage: !auth <password>");
            return true;
        }
        cvar_t* passcv = CVAR_GET_POINTER("zp_admin_pass");
        const char* pw = passcv ? passcv->string : "";
        if (pw[0] && !strcmp(argv[1], pw)) {
            g_authed[idx] = true;
            ZPAdminMsg(sender, "[Admin] Authenticated. Type !help for commands.");
        } else {
            ZPAdminMsg(sender, "[Admin] Wrong password.");
        }
        return true;
    }

    if (!strcmp(cmd, "who") || !strcmp(cmd, "players")) {
        ZPAdminMsg(sender, "Connected players:");
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* e = INDEXENT(i);
            if (!ZPIsPlayerConnected(e)) continue;
            const char* role = "spectator";
            if (ZPIsZombie(e)) role = "zombie";
            else if (ZPIsHuman(e)) role = "human";
            snprintf(outMsg, sizeof(outMsg), "  %s [%s] %s",
                     isAdmin ? "[ADM]" : "     ", role, STRING(e->v.netname));
            ZPAdminMsg(sender, outMsg);
        }
        return true;
    }

    // ---- admin-only from here ----
    if (!isAdmin) {
        ZPAdminMsg(sender, "[Admin] No permission. Try !auth <password>");
        return true;
    }

    if (!strcmp(cmd, "kick")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !kick <name> [reason]"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        const char* tname = STRING(t->v.netname);
        int uid = GETPLAYERUSERID(t);
        const char* reason = argc >= 3 ? argv[2] : "Kicked by admin";
        SERVER_COMMAND(UTIL_VarArgs("kick #%d \"%s\"\n", uid, reason));
        UTIL_ClientPrintAll(HUD_PRINTNOTIFY, UTIL_VarArgs("%s (admin) kicked %s\n", STRING(sender->v.netname), tname));
        return true;
    }

    if (!strcmp(cmd, "ban")) {
        if (argc < 3) { ZPAdminMsg(sender, "Usage: !ban <name> <minutes> [reason]"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        const char* tname = STRING(t->v.netname);
        long mins = atol(argv[2]);
        if (mins <= 0) mins = 30;
        long expire = (long)time(NULL) + mins * 60;

        ZPAdminAddBan(tname, nullptr, expire);
        int uid = GETPLAYERUSERID(t);
        const char* reason = argc >= 4 ? argv[3] : "Banned by admin";
        SERVER_COMMAND(UTIL_VarArgs("kick #%d \"%s\"\n", uid, reason));
        UTIL_ClientPrintAll(HUD_PRINTNOTIFY, UTIL_VarArgs("%s was banned for %ld minute(s) by %s\n", tname, mins, STRING(sender->v.netname)));
        return true;
    }

    if (!strcmp(cmd, "unban")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !unban <name>"); return true; }
        if (ZPAdminRemoveBan(argv[1]))
            ZPAdminMsg(sender, "[Admin] Ban removed.");
        else
            ZPAdminMsg(sender, "[Admin] No matching ban found.");
        return true;
    }

    if (!strcmp(cmd, "slap")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !slap <name> [damage]"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        CBasePlayer* tp = (CBasePlayer*)GET_PRIVATE(t);
        if (!tp || !tp->IsAlive()) { ZPAdminMsg(sender, "[Admin] Target is not alive."); return true; }

        float dmg = 1.0f;
        if (argc >= 3) dmg = (float)atof(argv[2]);

        Vector dir = t->v.origin - sender->v.origin;
        dir.z = 0;
        if (dir.Length() < 0.1f) dir = gpGlobals->v_forward;
        dir = dir.Normalize();
        tp->pev->velocity = dir * 250.0f + Vector(0, 0, 180);

        tp->TakeDamage(VARS(INDEXENT(0)), VARS(INDEXENT(0)), dmg, DMG_GENERIC);
        EMIT_SOUND(t, CHAN_AUTO, "weapons/cbar_hit1.wav", 1.0, ATTN_NORM);
        return true;
    }

    if (!strcmp(cmd, "slay")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !slay <name>"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        CBasePlayer* tp = (CBasePlayer*)GET_PRIVATE(t);
        if (!tp || !tp->IsAlive()) { ZPAdminMsg(sender, "[Admin] Target is not alive."); return true; }

        tp->TakeDamage(VARS(INDEXENT(0)), VARS(INDEXENT(0)), 10000.0f, DMG_GENERIC);
        UTIL_ClientPrintAll(HUD_PRINTNOTIFY, UTIL_VarArgs("%s was slain by %s\n", STRING(t->v.netname), STRING(sender->v.netname)));
        return true;
    }

    if (!strcmp(cmd, "heal")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !heal <name> [amount]"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        CBasePlayer* tp = (CBasePlayer*)GET_PRIVATE(t);
        if (!tp || !tp->IsAlive()) { ZPAdminMsg(sender, "[Admin] Target is not alive."); return true; }

        float amt = argc >= 3 ? (float)atof(argv[2]) : 100.0f;
        if (amt < 0) amt = 0;
        tp->pev->health = amt;
        UTIL_ScreenFade(tp, Vector(0, 255, 120), 0.3f, 0.1f, 255, FFADE_IN);
        return true;
    }

    if (!strcmp(cmd, "freeze")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !freeze <name> [seconds]"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        CBasePlayer* tp = (CBasePlayer*)GET_PRIVATE(t);
        if (!tp || !tp->IsAlive()) { ZPAdminMsg(sender, "[Admin] Target is not alive."); return true; }

        float secs = argc >= 3 ? (float)atof(argv[2]) : 30.0f;
        if (secs <= 0) secs = 30.0f;
        g_players[ENTINDEX(t)].frozenUntil = gpGlobals->time + secs;
        tp->pev->velocity = Vector(0, 0, 0);
        return true;
    }

    if (!strcmp(cmd, "unfreeze")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !unfreeze <name>"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        g_players[ENTINDEX(t)].frozenUntil = 0.0f;
        t->v.movetype = MOVETYPE_WALK;
        t->v.gravity = 1.0f;
        return true;
    }

    if (!strcmp(cmd, "tp") || !strcmp(cmd, "teleport")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !tp <name>"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        CBasePlayer* me = (CBasePlayer*)GET_PRIVATE(sender);
        if (me && me->pev) {
            me->pev->origin = t->v.origin + Vector(0, 0, 16);
            me->pev->angles = t->v.angles;
            me->pev->velocity = Vector(0, 0, 0);
        }
        return true;
    }

    if (!strcmp(cmd, "bring")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !bring <name>"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        t->v.origin = sender->v.origin + Vector(0, 0, 16);
        t->v.velocity = Vector(0, 0, 0);
        return true;
    }

    if (!strcmp(cmd, "zombie") || !strcmp(cmd, "makezombie")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !zombie <name>"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        if (ZPIsZombie(t)) { ZPAdminMsg(sender, "[Admin] Already a zombie."); return true; }
        ZPInfectPlayer(t, true);
        return true;
    }

    if (!strcmp(cmd, "human") || !strcmp(cmd, "makehuman")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !human <name>"); return true; }
        edict_t* t = ZPAdminFindPlayer(argv[1]);
        if (!t) { ZPAdminMsg(sender, "[Admin] Player not found."); return true; }

        if (ZPIsHuman(t)) { ZPAdminMsg(sender, "[Admin] Already a human."); return true; }
        ZPMakeHuman(t);
        return true;
    }

    if (!strcmp(cmd, "noclip")) {
        g_players[idx].noclip = !g_players[idx].noclip;
        sender->v.velocity = Vector(0, 0, 0);
        if (g_players[idx].noclip) {
            sender->v.movetype = MOVETYPE_NOCLIP;
            sender->v.gravity = 0.0f;
            ZPAdminMsg(sender, "[Admin] Noclip on");
        } else {
            sender->v.movetype = MOVETYPE_WALK;
            sender->v.gravity = 1.0f;
            ZPAdminMsg(sender, "[Admin] Noclip off");
        }
        return true;
    }

    if (!strcmp(cmd, "god")) {
        if (sender->v.takedamage == DAMAGE_NO) {
            sender->v.takedamage = DAMAGE_AIM;
            ZPAdminMsg(sender, "[Admin] Godmode off");
        } else {
            sender->v.takedamage = DAMAGE_NO;
            ZPAdminMsg(sender, "[Admin] Godmode on");
        }
        return true;
    }

    if (!strcmp(cmd, "map") || !strcmp(cmd, "changelevel")) {
        if (argc < 2) { ZPAdminMsg(sender, "Usage: !map <mapname>"); return true; }
        SERVER_COMMAND(UTIL_VarArgs("changelevel %s\n", argv[1]));
        return true;
    }

    if (!strcmp(cmd, "endround")) {
        int winner = 2; // default: zombies win
        if (argc >= 2) {
            if (!strcmp(argv[1], "human") || !strcmp(argv[1], "humans") || !strcmp(argv[1], "1")) winner = 1;
            else if (!strcmp(argv[1], "draw") || !strcmp(argv[1], "0")) winner = 0;
            else winner = 2;
        }
        ZPForceRoundEnd(winner);
        return true;
    }

    if (!strcmp(cmd, "restart")) {
        g_round.state = RS_ROUND_DRAW;
        g_round.resetTime = gpGlobals->time;
        ZPAdminMsg(sender, "[Admin] Round restarting...");
        return true;
    }

    ZPAdminMsg(sender, UTIL_VarArgs("[Admin] Unknown command \"%s\". Try !help", cmd));
    return true;
}