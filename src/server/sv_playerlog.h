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

// sv_playerlog.h -- records when real players join and leave the server
// in an SQLite database (sv_playerLog), one row per visit

#ifndef SV_PLAYERLOG_H
#define SV_PLAYERLOG_H

struct client_t;

// only native dedicated servers carry SQLite
#if defined(DEDICATED) && !defined(EMSCRIPTEN)
#define USE_PLAYERLOG 1
#endif

#ifdef USE_PLAYERLOG

void SV_PlayerLogInit(void);
// a player got a client slot; ends any visit still open in that slot
void SV_PlayerLogJoin(struct client_t *cl);
// the player in this slot left; does nothing if no visit is open there
void SV_PlayerLogLeave(struct client_t *cl, const char *reason);
// keeps last_seen current, so a crash still leaves a usable leave time
void SV_PlayerLogFrame(void);
// the server is going down: ends every open visit and closes the database
void SV_PlayerLogShutdown(const char *reason);

#else

inline void SV_PlayerLogInit(void) {}
inline void SV_PlayerLogJoin(struct client_t *) {}
inline void SV_PlayerLogLeave(struct client_t *, const char *) {}
inline void SV_PlayerLogFrame(void) {}
inline void SV_PlayerLogShutdown(const char *) {}

#endif

#endif
