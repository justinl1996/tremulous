/*
===========================================================================
Copyright (C) 2000-2013 Darklegion Development
Copyright (C) 2015-2019 GrangerHub

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

#ifndef QCOMMON_NET_WS_H
#define QCOMMON_NET_WS_H 1

// native dedicated servers can take the web client's websocket connections
// themselves (net_websocket 1) instead of sitting behind websockify
#if defined(DEDICATED) && !defined(EMSCRIPTEN) && !defined(_WIN32)
#define USE_WEBSOCKET 1
#endif

#ifdef USE_WEBSOCKET

#include <sys/select.h>

#include "net.h"

struct msg_t;

bool NET_WS_GetCvars(void);
void NET_WS_Open(const char *ip, const char *ip6, int enabled, int udpPort);
void NET_WS_Close(void);
int NET_WS_FillFdSet(fd_set *fdr, int highestfd);
void NET_WS_Frame(fd_set *fdr);
bool NET_WS_GetPacket(struct netadr_t *from, struct msg_t *msg);
bool NET_WS_SendPacket(int length, const void *data, const struct netadr_t *to);

#endif

#endif
