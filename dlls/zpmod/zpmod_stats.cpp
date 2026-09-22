// Persistent player stats + motd.txt leaderboard regeneration.
//
// Stats accumulate across maps and server restarts via zpmod_stats.ini.
// Every ZPSTATS_SAVE_INTERVAL the file is rewritten and, when a template
// motd exists at zpmod/motd.txt (relative to the engine base dir), a fresh
// copy with the [globalleaderboard] token replaced by a plain-text ASCII
// leaderboard is dropped into the game folder (<gamedir>/motd.txt). If the
// template is missing nothing is touched.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "enginecallback.h"
#include "game.h"
#include "edict.h"
#include "zpmod.h"
#include "player.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZPSTATS_FILE            "zpmod_stats.ini"   // relative to the engine working dir
#define ZPSTATS_MOTD_TEMPLATE   "zpmod/motd.txt"    //     "     "     "    "     "     "
#define ZPSTATS_MOTD_PLACEHOLDER "[globalleaderboard]"
#define ZPSTATS_SAVE_INTERVAL   60.0f
#define ZPSTATS_MAX_PLAYERS     128
#define ZPSTATS_NAME_LEN        48
#define ZPSTATS_TOP_LIST        5
#define ZPSTATS_TOP_CHAT        5

struct ZPStatEntry {
    char name[ZPSTATS_NAME_LEN];
    int kills;
    int infections;
    int headshots;
    int deaths;
    int playTimeSec;
};

static ZPStatEntry s_stats[ZPSTATS_MAX_PLAYERS];
static int s_statsCount = 0;
static float s_statsSaveAt = 0.0f;
static float s_statsLastThink = 0.0f;
static float s_playtimePad = 0.0f;

static int ZPStatsScore(const ZPStatEntry* e) {
    return e->kills + e->infections * 2;
}

static void ZPStatsLower(const char* src, char* out, int size) {
    int i = 0;
    for (; src[i] && i < size - 1; i++)
        out[i] = (char)tolower((unsigned char)src[i]);
    out[i] = 0;
}

static int ZPStatsFindByName(const char* name) {
    char want[ZPSTATS_NAME_LEN];
    ZPStatsLower(name, want, sizeof(want));
    for (int i = 0; i < s_statsCount; i++) {
        char have[ZPSTATS_NAME_LEN];
        ZPStatsLower(s_stats[i].name, have, sizeof(have));
        if (!strcmp(want, have)) return i;
    }
    return -1;
}

static ZPStatEntry* ZPStatsUpsert(edict_t* player) {
    if (!player) return NULL;
    const char* name = STRING(player->v.netname);
    if (!name || !name[0]) name = "player";

    int j = ZPStatsFindByName(name);
    if (j < 0) {
        if (s_statsCount >= ZPSTATS_MAX_PLAYERS) return NULL;
        j = s_statsCount++;
        memset(&s_stats[j], 0, sizeof(s_stats[j]));
    }
    snprintf(s_stats[j].name, sizeof(s_stats[j].name), "%s", name);
    return &s_stats[j];
}

static void ZPStatsLoad(void) {
    FILE* f = fopen(ZPSTATS_FILE, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f) && s_statsCount < ZPSTATS_MAX_PLAYERS) {
        char name[ZPSTATS_NAME_LEN] = { 0 };
        int kills = 0, infections = 0, headshots = 0, deaths = 0, timeSec = 0;
        if (sscanf(line, "%47[^\t]\t%d\t%d\t%d\t%d\t%d",
                   name, &kills, &infections, &headshots, &deaths, &timeSec) == 6) {
            int j = ZPStatsFindByName(name);
            if (j < 0) {
                j = s_statsCount++;
                memset(&s_stats[j], 0, sizeof(s_stats[j]));
            }
            snprintf(s_stats[j].name, sizeof(s_stats[j].name), "%s", name);
            s_stats[j].kills = kills;
            s_stats[j].infections = infections;
            s_stats[j].headshots = headshots;
            s_stats[j].deaths = deaths;
            s_stats[j].playTimeSec = timeSec;
        }
    }
    fclose(f);
}

static void ZPStatsSave(void) {
    FILE* f = fopen(ZPSTATS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_statsCount; i++) {
        const ZPStatEntry* e = &s_stats[i];
        fprintf(f, "%s\t%d\t%d\t%d\t%d\t%d\n",
                e->name, e->kills, e->infections, e->headshots,
                e->deaths, e->playTimeSec);
    }
    fclose(f);
}

// rank (1-based) of entry index j by score
static int ZPStatsRank(int j) {
    int rank = 1;
    for (int i = 0; i < s_statsCount; i++) {
        if (i == j) continue;
        int sa = ZPStatsScore(&s_stats[i]);
        int sb = ZPStatsScore(&s_stats[j]);
        if (sa > sb || (sa == sb && s_stats[i].kills > s_stats[j].kills))
            rank++;
        else if (sa == sb && s_stats[i].kills == s_stats[j].kills &&
                 strcmp(s_stats[i].name, s_stats[j].name) < 0)
            rank++;
    }
    return rank;
}

static void ZPStatsFormatTime(int secs, char* out, int size) {
    if (secs < 60) { snprintf(out, size, "%ds", secs); return; }
    int m = secs / 60;
    int h = m / 60;
    m = m % 60;
    if (h > 0) snprintf(out, size, "%dh%02dm", h, m);
    else snprintf(out, size, "%dm%02ds", m, secs % 60);
}

static void ZPStatsBuildLeaderboard(char* out, int outSize) {
    int cap = outSize - 1;
    int pos = 0;

    int idx[ZPSTATS_MAX_PLAYERS];
    for (int i = 0; i < s_statsCount; i++) idx[i] = i;

    // selection sort by score desc, then kills desc, then name
    for (int i = 0; i < s_statsCount; i++) {
        for (int j = i + 1; j < s_statsCount; j++) {
            int a = idx[i], b = idx[j];
            int sa = ZPStatsScore(&s_stats[a]);
            int sb = ZPStatsScore(&s_stats[b]);
            bool swap = false;
            if (sb > sa) swap = true;
            else if (sb == sa) {
                if (s_stats[b].kills > s_stats[a].kills) swap = true;
                else if (s_stats[b].kills == s_stats[a].kills &&
                         strcmp(s_stats[b].name, s_stats[a].name) < 0) swap = true;
            }
            if (swap) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        }
    }

    int shown = s_statsCount < ZPSTATS_TOP_LIST ? s_statsCount : ZPSTATS_TOP_LIST;

    pos += snprintf(out + pos, cap - pos, "ZP GLOBAL LEADERBOARD\n");
    pos += snprintf(out + pos, cap - pos, "======================\n");
    pos += snprintf(out + pos, cap - pos,
                    "%-4s%-18s %-6s %-4s %-5s %-5s %-7s\n",
                    "  #", "PLAYER", "KILLS", "INF", "HEADS", "DEAD", "TIME");
    pos += snprintf(out + pos, cap - pos, "----");
    for (int i = 0; i < 18; i++) pos += snprintf(out + pos, cap - pos, "-");
    pos += snprintf(out + pos, cap - pos, " ------ ---- ----- ----- -------\n");

    if (shown == 0) {
        pos += snprintf(out + pos, cap - pos, "    no stats recorded yet, be the first!\n");
    } else {
        for (int i = 0; i < shown && pos < cap; i++) {
            const ZPStatEntry* e = &s_stats[idx[i]];
            char t[16];
            ZPStatsFormatTime(e->playTimeSec, t, sizeof(t));
            pos += snprintf(out + pos, cap - pos,
                            "%2d. %-18.18s %6d %4d %5d %5d %7s\n",
                            i + 1, e->name, e->kills, e->infections,
                            e->headshots, e->deaths, t);
        }
    }

    if (pos > (int)outSize - 1) pos = (int)outSize - 1;
    out[pos] = 0;
}

static void ZPStatsMotdUpdate(void) {
    // no template motd -> leave the game's motd.txt untouched
    FILE* in = fopen(ZPSTATS_MOTD_TEMPLATE, "r");
    if (!in) return;

    const size_t kTemplateCap = 8192;
    char* text = (char*)malloc(kTemplateCap + 1);
    if (!text) { fclose(in); return; }
    size_t n = fread(text, 1, kTemplateCap, in);
    fclose(in);
    text[n] = 0;

    char leaderboard[4096];
    ZPStatsBuildLeaderboard(leaderboard, sizeof(leaderboard));

    // engine reads mod info from the game folder: <gamedir>/motd.txt
    const char* gamedir = "valve";
    const char* gd = CVAR_GET_STRING("gamedir");
    if (gd && gd[0]) gamedir = gd;

    char outPath[256];
    snprintf(outPath, sizeof(outPath), "%s/motd.txt", gamedir);

    FILE* out = fopen(outPath, "w");
    const char* placeholder = ZPSTATS_MOTD_PLACEHOLDER;
    if (!out) { free(text); return; }

    if (!strstr(text, placeholder)) {
        fwrite(text, 1, n, out);
    } else {
        const char* p = text;
        const char* ph;
        while ((ph = strstr(p, placeholder)) != NULL) {
            fwrite(p, 1, (size_t)(ph - p), out);
            fwrite(leaderboard, 1, strlen(leaderboard), out);
            p = ph + strlen(placeholder);
        }
        fwrite(p, 1, strlen(p), out);
    }

    fclose(out);
    free(text);
}

// ---- public API ----

void ZPStatsInit(void) {
    if (s_statsCount > 0) ZPStatsSave(); // flush any deltas from the previous map
    s_statsCount = 0;
    s_statsSaveAt = gpGlobals->time + ZPSTATS_SAVE_INTERVAL;
    s_statsLastThink = gpGlobals->time;
    s_playtimePad = 0.0f;
    ZPStatsLoad();
}

void ZPStatsThink(void) {
    float now = gpGlobals->time;

    float dt = now - s_statsLastThink;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 2.0f) dt = 2.0f; // clamp long hitches so playtime doesn't inflate
    s_statsLastThink = now;

    // accumulate playtime in whole seconds per connected player
    s_playtimePad += dt;
    if (s_playtimePad >= 1.0f) {
        int secs = (int)s_playtimePad;
        s_playtimePad -= (float)secs;
        for (int i = 1; i <= gpGlobals->maxClients; i++) {
            edict_t* e = INDEXENT(i);
            if (!ZPIsPlayerConnected(e)) continue;
            ZPStatEntry* entry = ZPStatsUpsert(e);
            if (entry) entry->playTimeSec += secs;
        }
    }

    // once a minute: persist the file and refresh the leaderboard motd
    if (now >= s_statsSaveAt) {
        s_statsSaveAt = now + ZPSTATS_SAVE_INTERVAL;
        ZPStatsSave();
        ZPStatsMotdUpdate();
    }
}

void ZPStatsOnKill(edict_t* killer, bool fromHeadshot) {
    if (!killer) return;
    ZPStatEntry* e = ZPStatsUpsert(killer);
    if (!e) return;
    e->kills++;
    if (fromHeadshot) e->headshots++;
}

void ZPStatsOnInfect(edict_t* victim, int infectorIndex) {
    (void)victim;
    if (infectorIndex < 1 || infectorIndex > gpGlobals->maxClients) return;
    edict_t* infector = INDEXENT(infectorIndex);
    if (!ZPIsPlayerConnected(infector)) return;
    ZPStatEntry* e = ZPStatsUpsert(infector);
    if (e) e->infections++;
}

void ZPStatsOnDied(edict_t* player) {
    if (!player) return;
    ZPStatEntry* e = ZPStatsUpsert(player);
    if (e) e->deaths++;
}

void ZPStatsPlayerDisconnect(edict_t* player) {
    if (!player) return;
    ZPStatsUpsert(player);
    ZPStatsSave();
}

static void ZPStatsSay(edict_t* to, int senderIndex, const char* msg) {
    extern int gmsgSayText;
    MESSAGE_BEGIN(MSG_ONE, gmsgSayText, NULL, to);
        WRITE_BYTE(senderIndex);
        WRITE_STRING(msg);
    MESSAGE_END();
}

bool ZPStatsCommand(edict_t* sender, int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (!sender) return false;
    int idx = ENTINDEX(sender);
    if (idx < 1 || idx > gpGlobals->maxClients) return false;

    const char* name = STRING(sender->v.netname);
    if (!name || !name[0]) name = "you";

    char msg[192];
    int j = ZPStatsFindByName(name);
    if (j < 0) {
        snprintf(msg, sizeof(msg), "^3[stats]^7 no persistent stats yet for %s", name);
    } else {
        const ZPStatEntry* e = &s_stats[j];
        char t[16];
        ZPStatsFormatTime(e->playTimeSec, t, sizeof(t));
        snprintf(msg, sizeof(msg),
                 "^2[stats]^7 rank ^3#%d^7 | ^1%d kills^7 | ^6%d infections^7 | ^3%d headshots^7 | ^1%d deaths^7 | ^9%s^7",
                 ZPStatsRank(j), e->kills, e->infections, e->headshots, e->deaths, t);
    }

    ZPStatsSay(sender, idx, msg);
    return true;
}

bool ZPTopCommand(edict_t* sender, int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (!sender) return false;
    int idx = ENTINDEX(sender);
    if (idx < 1 || idx > gpGlobals->maxClients) return false;

    char msg[256];
    if (s_statsCount == 0) {
        snprintf(msg, sizeof(msg), "^3[top]^7 no stats recorded yet");
        ZPStatsSay(sender, idx, msg);
        return true;
    }

    int pos = snprintf(msg, sizeof(msg), "^5[top]^7 ");
    int idxList[ZPSTATS_MAX_PLAYERS];
    for (int i = 0; i < s_statsCount; i++) idxList[i] = i;
    for (int i = 0; i < s_statsCount; i++) {
        for (int j = i + 1; j < s_statsCount; j++) {
            int a = idxList[i], b = idxList[j];
            int sa = ZPStatsScore(&s_stats[a]);
            int sb = ZPStatsScore(&s_stats[b]);
            bool swap = false;
            if (sb > sa) swap = true;
            else if (sb == sa) {
                if (s_stats[b].kills > s_stats[a].kills) swap = true;
                else if (s_stats[b].kills == s_stats[a].kills &&
                         strcmp(s_stats[b].name, s_stats[a].name) < 0) swap = true;
            }
            if (swap) { int t = idxList[i]; idxList[i] = idxList[j]; idxList[j] = t; }
        }
    }

    int shown = s_statsCount < ZPSTATS_TOP_CHAT ? s_statsCount : ZPSTATS_TOP_CHAT;
    for (int i = 0; i < shown && pos < (int)sizeof(msg) - 1; i++) {
        const ZPStatEntry* e = &s_stats[idxList[i]];
        const char* rankColor = (i == 0) ? "^8" : (i == 1) ? "^6" : (i == 2) ? "^3" : "^7";
        int n = snprintf(msg + pos, sizeof(msg) - pos,
                         "%s%d.%s %dK/%dI^7  ", rankColor, i + 1, e->name, e->kills, e->infections);
        if (n > 0) pos += n < (int)sizeof(msg) - pos ? n : 0;
    }

    ZPStatsSay(sender, idx, msg);
    return true;
}