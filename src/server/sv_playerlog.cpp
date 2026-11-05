/*
===========================================================================
Copyright (C) 2026 Darklegion Development

This file is part of Tremulous.

Tremulous is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 3 of the License,
or (at your option) any later version.

Tremulous is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Tremulous; if not, see <https://www.gnu.org/licenses/>

===========================================================================
*/

// sv_playerlog.cpp -- one row in player_sessions for each visit a real
// player (not a bot) makes to the server: who, from where, on what, when
// they joined and when they left.
//
// Several servers on one machine can share a database file: rows carry the
// server's name and port, and SQLite's locking keeps their writes apart.

#include "server.h"

#ifdef USE_PLAYERLOG

#include <time.h>

#include <sqlite3.h>

#include "../qcommon/net_ws.h"

static cvar_t *sv_playerLog;  // database file; "" turns the log off
static cvar_t *sv_playerLogServer;  // name stored for this server; "" uses sv_hostname

static sqlite3 *db;
static bool dbFailed;  // don't retry a database that wouldn't open until the cvar changes
static int serverPort;  // the port this database was opened for
static sqlite3_int64 sessionIds[MAX_CLIENTS];  // open player_sessions row per slot, 0 if none
static time_t lastHeartbeat;

static const int HEARTBEAT_SECONDS = 60;

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS player_sessions ("
    "  id           INTEGER PRIMARY KEY,"
    "  server       TEXT    NOT NULL,"  // sv_playerLogServer, or sv_hostname without colours
    "  server_port  INTEGER NOT NULL,"  // net_port
    "  client_num   INTEGER NOT NULL,"
    "  name         TEXT    NOT NULL,"  // name when they joined, colours removed
    "  name_raw     TEXT    NOT NULL,"  // the same with its colour codes
    "  last_name    TEXT,"              // name when they left, colours removed
    "  ip           TEXT    NOT NULL,"  // the browser's own address behind a reverse proxy
    "  transport    TEXT    NOT NULL,"  // 'websocket' (web client) or 'udp' (native client)
    "  user_agent   TEXT,"              // browser User-Agent, web clients only
    "  guid         TEXT,"              // cl_guid
    "  fingerprint  TEXT,"              // RSA key fingerprint
    "  protocol     INTEGER,"           // 71 (1.3), 70 (1.2), 69 (1.1)
    "  map          TEXT,"              // map when they joined
    "  joined_at    INTEGER NOT NULL,"  // unix time, seconds
    "  last_seen    INTEGER NOT NULL,"  // unix time, updated every minute while connected
    "  left_at      INTEGER,"           // unix time, NULL while connected
    "  leave_reason TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS player_sessions_open ON player_sessions (server_port, left_at);"
    "CREATE INDEX IF NOT EXISTS player_sessions_joined ON player_sessions (joined_at);"
    "CREATE INDEX IF NOT EXISTS player_sessions_ip ON player_sessions (ip);"
    "CREATE INDEX IF NOT EXISTS player_sessions_guid ON player_sessions (guid);"
    "CREATE INDEX IF NOT EXISTS player_sessions_name ON player_sessions (name COLLATE NOCASE);"
    // the same with readable UTC times and how long each visit lasted
    "CREATE VIEW IF NOT EXISTS player_sessions_readable AS SELECT"
    "  id, server, server_port, client_num, name, last_name, ip, transport, user_agent,"
    "  guid, fingerprint, protocol, map,"
    "  datetime(joined_at, 'unixepoch') AS joined_utc,"
    "  datetime(left_at, 'unixepoch') AS left_utc,"
    "  COALESCE(left_at, last_seen) - joined_at AS duration_seconds,"
    "  leave_reason"
    "  FROM player_sessions;"
    "PRAGMA user_version = 1;";

static void PlayerLog_Error(const char *what)
{
    Com_Printf(S_COLOR_YELLOW "WARNING: player log: %s: %s\n", what, db ? sqlite3_errmsg(db) : "no database");
}

// runs a statement whose parameters are all integers or text, bound in order
// from a format: 'i' int64, 't' text (NULL becomes SQL NULL)
static bool PlayerLog_Exec(const char *sql, const char *fmt, ...)
{
    sqlite3_stmt *stmt;
    va_list ap;
    bool ok;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        PlayerLog_Error("prepare");
        return false;
    }

    va_start(ap, fmt);
    for (int i = 1; *fmt; fmt++, i++)
    {
        if (*fmt == 'i')
        {
            sqlite3_bind_int64(stmt, i, va_arg(ap, sqlite3_int64));
        }
        else
        {
            const char *s = va_arg(ap, const char *);

            if (s)
                sqlite3_bind_text(stmt, i, s, -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt, i);
        }
    }
    va_end(ap);

    ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) PlayerLog_Error("write");

    sqlite3_finalize(stmt);
    return ok;
}

static void PlayerLog_Close(void)
{
    if (db) sqlite3_close(db);

    db = NULL;
    ::memset(sessionIds, 0, sizeof(sessionIds));
}

static bool PlayerLog_Open(void)
{
    char path[MAX_OSPATH];

    if (sv_playerLog->modified)
    {
        sv_playerLog->modified = false;
        dbFailed = false;
    }

    if (db) return true;
    if (dbFailed || !sv_playerLog->string[0]) return false;

    // relative paths are under the home path, like the server's other files
    const char *file = sv_playerLog->string;

    if (file[0] == '/' || file[0] == '\\' || (file[0] && file[1] == ':'))
        Q_strncpyz(path, file, sizeof(path));
    else
        Com_sprintf(path, sizeof(path), "%s/%s", Cvar_VariableString("fs_homepath"), file);

    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        PlayerLog_Error(va("can't open %s", path));
        PlayerLog_Close();
        dbFailed = true;
        return false;
    }

    // another server sharing the file may be mid-write; wait for it briefly
    // rather than failing, but never long enough to stall the game
    sqlite3_busy_timeout(db, 250);
    sqlite3_exec(db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);

    if (sqlite3_exec(db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK)
    {
        PlayerLog_Error(va("can't set up %s", path));
        PlayerLog_Close();
        dbFailed = true;
        return false;
    }

    serverPort = Cvar_VariableIntegerValue("net_port");

    // visits a previous run of this server never closed (it crashed or was
    // killed) ended around the last time it saw them
    PlayerLog_Exec(
        "UPDATE player_sessions SET left_at = last_seen, leave_reason = 'server stopped unexpectedly'"
        " WHERE server_port = ? AND left_at IS NULL",
        "i", (sqlite3_int64)serverPort);

    Com_Printf("Logging player visits to %s\n", path);
    lastHeartbeat = time(NULL);
    return true;
}

void SV_PlayerLogInit(void)
{
    sv_playerLog = Cvar_Get("sv_playerLog", "players.db", CVAR_ARCHIVE);
    sv_playerLogServer = Cvar_Get("sv_playerLogServer", "", CVAR_ARCHIVE);
}

void SV_PlayerLogJoin(client_t *cl)
{
    int clientNum = cl - svs.clients;
    netadrtype_t type = cl->netchan.remoteAddress.type;
    const char *userAgent = NULL;
    const char *guid;
    char server[MAX_CVAR_VALUE_STRING];
    char name[MAX_COLORFUL_NAME_LENGTH];
    sqlite3_int64 now;

    if (type == NA_BOT || type == NA_LOOPBACK || clientNum < 0 || clientNum >= MAX_CLIENTS) return;
    if (!PlayerLog_Open()) return;

    // a reconnect reuses the slot without dropping the old connection
    SV_PlayerLogLeave(cl, "reconnected");

    Q_strncpyz(server, sv_playerLogServer->string[0] ? sv_playerLogServer->string : sv_hostname->string,
        sizeof(server));
    Q_CleanStr(server);

    Q_strncpyz(name, cl->name, sizeof(name));
    Q_CleanStr(name);

#ifdef USE_WEBSOCKET
    userAgent = NET_WS_UserAgent(&cl->netchan.remoteAddress);
#endif

    guid = Info_ValueForKey(cl->userinfo, "cl_guid");
    now = time(NULL);

    if (!PlayerLog_Exec(
            "INSERT INTO player_sessions (server, server_port, client_num, name, name_raw, ip, transport,"
            " user_agent, guid, fingerprint, protocol, map, joined_at, last_seen)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            "tiitttttttitii", server, (sqlite3_int64)serverPort, (sqlite3_int64)clientNum, name, cl->name,
            NET_AdrToString(cl->netchan.remoteAddress), userAgent ? "websocket" : "udp",
            userAgent && userAgent[0] ? userAgent : NULL,
            guid[0] ? guid : NULL, cl->fingerprint[0] ? cl->fingerprint : NULL,
            (sqlite3_int64)(71 - cl->netchan.alternateProtocol), sv_mapname->string,
            now, now))
        return;

    sessionIds[clientNum] = sqlite3_last_insert_rowid(db);
}

void SV_PlayerLogLeave(client_t *cl, const char *reason)
{
    int clientNum = cl - svs.clients;
    char name[MAX_COLORFUL_NAME_LENGTH];
    char why[MAX_STRING_CHARS];
    sqlite3_int64 now;

    if (!db || clientNum < 0 || clientNum >= MAX_CLIENTS || !sessionIds[clientNum]) return;

    Q_strncpyz(name, cl->name, sizeof(name));
    Q_CleanStr(name);
    Q_strncpyz(why, reason ? reason : "", sizeof(why));
    Q_CleanStr(why);
    now = time(NULL);

    PlayerLog_Exec("UPDATE player_sessions SET last_name = ?, last_seen = ?, left_at = ?, leave_reason = ? WHERE id = ?",
        "tiiti", name[0] ? name : NULL, now, now, why, sessionIds[clientNum]);

    sessionIds[clientNum] = 0;
}

void SV_PlayerLogFrame(void)
{
    time_t now;

    // opening here rather than at the first join closes out a crashed run's
    // visits as soon as the server is back
    if (!PlayerLog_Open()) return;

    now = time(NULL);
    if (now - lastHeartbeat < HEARTBEAT_SECONDS) return;
    lastHeartbeat = now;

    PlayerLog_Exec("UPDATE player_sessions SET last_seen = ? WHERE server_port = ? AND left_at IS NULL",
        "ii", (sqlite3_int64)now, (sqlite3_int64)serverPort);
}

void SV_PlayerLogShutdown(const char *reason)
{
    if (!db) return;

    for (int i = 0; svs.clients && i < sv_maxclients->integer && i < MAX_CLIENTS; i++)
        SV_PlayerLogLeave(&svs.clients[i], reason);

    // anything still open belongs to a slot that's gone
    PlayerLog_Exec("UPDATE player_sessions SET left_at = ?, leave_reason = ? WHERE server_port = ? AND left_at IS NULL",
        "iti", (sqlite3_int64)time(NULL), reason, (sqlite3_int64)serverPort);

    PlayerLog_Close();
}

#endif
