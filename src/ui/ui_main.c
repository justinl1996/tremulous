/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
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

/*
=======================================================================

USER INTERFACE MAIN

=======================================================================
*/

#include "ui_local.h"

uiInfo_t uiInfo;

#ifdef MODULE_INTERFACE_11
#undef AS_GLOBAL
#undef AS_LOCAL
#define AS_GLOBAL 2
#define AS_LOCAL 0
#endif

static const char *netSources[] = {
#ifdef MODULE_INTERFACE_11
    "LAN", "Mplayer", "Internet",
#else
    "Internet", "Mplayer", "LAN",
#endif
    "Favorites"};

static const size_t numNetSources = ARRAY_LEN(netSources);

static const char *netnames[] = {"???", "UDP", "IPX", NULL};

/*
================
cvars
================
*/

typedef struct {
    vmCvar_t *vmCvar;
    char *cvarName;
    char *defaultString;
    int cvarFlags;
}

cvarTable_t;

vmCvar_t ui_browserShowFull;
vmCvar_t ui_browserShowEmpty;

vmCvar_t ui_dedicated;
vmCvar_t ui_netSource;
vmCvar_t ui_selectedMap;
vmCvar_t ui_lastServerRefresh_0;
vmCvar_t ui_lastServerRefresh_1;
vmCvar_t ui_lastServerRefresh_2;
vmCvar_t ui_lastServerRefresh_3;
vmCvar_t ui_lastServerRefresh_0_time;
vmCvar_t ui_lastServerRefresh_1_time;
vmCvar_t ui_lastServerRefresh_2_time;
vmCvar_t ui_lastServerRefresh_3_time;
vmCvar_t ui_mainMenuSelectedMusic;
vmCvar_t ui_smallFont;
vmCvar_t ui_bigFont;
vmCvar_t ui_findPlayer;
vmCvar_t ui_serverStatusTimeOut;
vmCvar_t ui_textWrapCache;
vmCvar_t ui_developer;

vmCvar_t ui_emoticons;
vmCvar_t ui_winner;
vmCvar_t ui_chatCommands;
vmCvar_t ui_clantag;

static cvarTable_t cvarTable[] = {{&ui_browserShowFull, "ui_browserShowFull", "1", CVAR_ARCHIVE},
    {&ui_browserShowEmpty, "ui_browserShowEmpty", "1", CVAR_ARCHIVE},

    {&ui_dedicated, "ui_dedicated", "0", CVAR_ARCHIVE}, {&ui_netSource, "ui_netSource", "0", CVAR_ARCHIVE},
    {&ui_selectedMap, "ui_selectedMap", "0", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_0, "ui_lastServerRefresh_0", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_1, "ui_lastServerRefresh_1", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_2, "ui_lastServerRefresh_2", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_3, "ui_lastServerRefresh_3", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_0, "ui_lastServerRefresh_0_time", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_1, "ui_lastServerRefresh_1_time", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_2, "ui_lastServerRefresh_2_time", "", CVAR_ARCHIVE},
    {&ui_lastServerRefresh_3, "ui_lastServerRefresh_3_time", "", CVAR_ARCHIVE},
    {&ui_mainMenuSelectedMusic, "ui_mainMenuSelectedMusic", "", CVAR_ARCHIVE},
    {&ui_smallFont, "ui_smallFont", "0.2", CVAR_ARCHIVE | CVAR_LATCH},
    {&ui_bigFont, "ui_bigFont", "0.5", CVAR_ARCHIVE | CVAR_LATCH}, {&ui_findPlayer, "ui_findPlayer", "", CVAR_ARCHIVE},
    {&ui_serverStatusTimeOut, "ui_serverStatusTimeOut", "7000", CVAR_ARCHIVE},
    {&ui_textWrapCache, "ui_textWrapCache", "1", CVAR_ARCHIVE},
    {&ui_developer, "ui_developer", "0", CVAR_ARCHIVE | CVAR_CHEAT},
    {&ui_emoticons, "cg_emoticons", "1", CVAR_LATCH | CVAR_ARCHIVE}, {&ui_winner, "ui_winner", "", CVAR_ROM},
    { &ui_chatCommands, "ui_chatCommands", "1", CVAR_ARCHIVE },
    { &ui_clantag, "ui_clantag", "", CVAR_ARCHIVE }};

static size_t cvarTableSize = ARRAY_LEN(cvarTable);

/*
================
vmMain

This is the only way control passes into the module.
This must be the very first function compiled into the .qvm file
================
*/
void UI_Init(qboolean);
void UI_Shutdown(void);
void UI_KeyEvent(int key, qboolean down);
void UI_MouseEvent(int dx, int dy);
int UI_MousePosition(void);
void UI_SetMousePosition(int x, int y);
void UI_Refresh(int realtime);
qboolean UI_IsFullscreen(void);
void UI_SetActiveMenu(uiMenuCommand_t menu);

Q_EXPORT intptr_t vmMain(int command, int arg0, int arg1, int arg2)
{
    switch (command)
    {
        case UI_GETAPIVERSION:
            return UI_API_VERSION;

        case UI_INIT:
            UI_Init(arg0);
            return 0;

        case UI_SHUTDOWN:
            UI_Shutdown();
            return 0;

        case UI_KEY_EVENT:
            UI_KeyEvent(arg0, arg1);
            return 0;

        case UI_MOUSE_EVENT:
            UI_MouseEvent(arg0, arg1);
            return 0;

#ifndef MODULE_INTERFACE_11
        case UI_MOUSE_POSITION:
            return UI_MousePosition();

        case UI_SET_MOUSE_POSITION:
            UI_SetMousePosition(arg0, arg1);
            return 0;
#endif

        case UI_REFRESH:
            UI_Refresh(arg0);
            return 0;

        case UI_IS_FULLSCREEN:
            return UI_IsFullscreen();

        case UI_SET_ACTIVE_MENU:
            UI_SetActiveMenu(arg0);
            return 0;

        case UI_CONSOLE_COMMAND:
            return UI_ConsoleCommand(arg0);

        case UI_DRAW_CONNECT_SCREEN:
            UI_DrawConnectScreen();
            return 0;
    }

    return -1;
}

void AssetCache(void)
{
    int i;

    uiInfo.uiDC.Assets.gradientBar = trap_R_RegisterShaderNoMip(ASSET_GRADIENTBAR);
    uiInfo.uiDC.Assets.scrollBar = trap_R_RegisterShaderNoMip(ASSET_SCROLLBAR);
    uiInfo.uiDC.Assets.scrollBarArrowDown = trap_R_RegisterShaderNoMip(ASSET_SCROLLBAR_ARROWDOWN);
    uiInfo.uiDC.Assets.scrollBarArrowUp = trap_R_RegisterShaderNoMip(ASSET_SCROLLBAR_ARROWUP);
    uiInfo.uiDC.Assets.scrollBarArrowLeft = trap_R_RegisterShaderNoMip(ASSET_SCROLLBAR_ARROWLEFT);
    uiInfo.uiDC.Assets.scrollBarArrowRight = trap_R_RegisterShaderNoMip(ASSET_SCROLLBAR_ARROWRIGHT);
    uiInfo.uiDC.Assets.scrollBarThumb = trap_R_RegisterShaderNoMip(ASSET_SCROLL_THUMB);
    uiInfo.uiDC.Assets.showMoreArrow = trap_R_RegisterShaderNoMip(ASSET_SHOWMORE_ARROW);
    uiInfo.uiDC.Assets.sliderBar = trap_R_RegisterShaderNoMip(ASSET_SLIDER_BAR);
    uiInfo.uiDC.Assets.sliderThumb = trap_R_RegisterShaderNoMip(ASSET_SLIDER_THUMB);

    uiInfo.uiDC.Assets.cornerIn[BORDER_SQUARE]         = trap_R_RegisterShaderNoMip( ASSET_CORNERIN_SQUARE );
    uiInfo.uiDC.Assets.cornerOut[BORDER_SQUARE]        = trap_R_RegisterShaderNoMip( ASSET_CORNEROUT_SQUARE );
    uiInfo.uiDC.Assets.cornerIn[BORDER_ROUNDED]        = trap_R_RegisterShaderNoMip( ASSET_CORNERIN_ROUNDED );
    uiInfo.uiDC.Assets.cornerOut[BORDER_ROUNDED]       = trap_R_RegisterShaderNoMip( ASSET_CORNEROUT_ROUNDED );
    uiInfo.uiDC.Assets.cornerIn[BORDER_FOLD]           = trap_R_RegisterShaderNoMip( ASSET_CORNERIN_FOLD );
    uiInfo.uiDC.Assets.cornerOut[BORDER_FOLD]          = trap_R_RegisterShaderNoMip( ASSET_CORNEROUT_FOLD );

    if (ui_emoticons.integer)
    {
        uiInfo.uiDC.Assets.emoticonCount = BG_LoadEmoticons(uiInfo.uiDC.Assets.emoticons, MAX_EMOTICONS);
    }
    else
        uiInfo.uiDC.Assets.emoticonCount = 0;

    for (i = 0; i < uiInfo.uiDC.Assets.emoticonCount; i++)
    {
        uiInfo.uiDC.Assets.emoticons[i].shader = trap_R_RegisterShaderNoMip(
            va("emoticons/%s_%dx1.tga", uiInfo.uiDC.Assets.emoticons[i].name, uiInfo.uiDC.Assets.emoticons[i].width));
    }
}

void UI_DrawSides(float x, float y, float w, float h, float size)
{
    UI_AdjustFrom640(&x, &y, &w, &h);
    size *= uiInfo.uiDC.xscale;

    trap_R_DrawStretchPic(x, y, size, h, 0, 0, 0, 0, uiInfo.uiDC.whiteShader);
    trap_R_DrawStretchPic(x + w - size, y, size, h, 0, 0, 0, 0, uiInfo.uiDC.whiteShader);
}

void UI_DrawTopBottom(float x, float y, float w, float h, float size)
{
    UI_AdjustFrom640(&x, &y, &w, &h);
    size *= uiInfo.uiDC.yscale;
    trap_R_DrawStretchPic(x, y, w, size, 0, 0, 0, 0, uiInfo.uiDC.whiteShader);
    trap_R_DrawStretchPic(x, y + h - size, w, size, 0, 0, 0, 0, uiInfo.uiDC.whiteShader);
}

void UI_DrawCorners( float x, float y, float w, float h, float size, const float *style, qhandle_t *pic )
{
  float hs, vs;
  UI_AdjustFrom640( &x, &y, &w, &h );
  hs = size * uiInfo.uiDC.xscale;
  vs = size * uiInfo.uiDC.yscale;

  trap_R_DrawStretchPic( x, y, hs, vs, 0, 0, 0.5, 0.5, pic[(int)(style[0])] );
  trap_R_DrawStretchPic( x + w - hs, y, hs, vs, 0.5, 0, 1, 0.5, pic[(int)(style[1])] );
  trap_R_DrawStretchPic( x + w - hs, y + h - vs, hs, vs, 0.5, 0.5, 1, 1, pic[(int)(style[2])] );
  trap_R_DrawStretchPic( x, y + h - vs, hs, vs, 0, 0.5, 0.5, 1, pic[(int)(style[3])] );
}

/*
================
UI_DrawRect

Coordinates are 640*480 virtual values
=================
*/
void UI_DrawRect(float x, float y, float w, float h, float size, const float *color)
{
    float sizeY, sizeX;

    UI_AdjustFrom640(&x, &y, &w, &h);
    sizeY = size * uiInfo.uiDC.yscale;
    sizeX = size * uiInfo.uiDC.xscale;

    trap_R_SetColor(color);

    trap_R_DrawStretchPic(x, y, w, sizeY, 0, 0, 0, 0, uiInfo.uiDC.whiteShader);
    trap_R_DrawStretchPic(x, y + h - sizeY, w, sizeY, 0, 0, 0, 0, uiInfo.uiDC.whiteShader);

    trap_R_DrawStretchPic(x, y + sizeY, sizeX, h - (sizeY * 2.0f), 0, 0, 0, 0, uiInfo.uiDC.whiteShader);
    trap_R_DrawStretchPic(x + w - sizeX, y + sizeY, sizeX, h - (sizeY * 2.0f), 0, 0, 0, 0, uiInfo.uiDC.whiteShader);

    trap_R_SetColor(NULL);
}

/*
================
UI_DrawRoundedRect

Coordinates are 640*480 virtual values
=================
*/
void UI_DrawRoundedRect( float x, float y, float width, float height, float size, const float *style, const float *color )
{
  trap_R_SetColor( color );

  UI_DrawTopBottom( x + size * 4, y, width - size * 8, height, size );
  UI_DrawSides( x, y + size * 4, width, height - size * 8, size );
  UI_DrawCorners( x, y, width, height, size * 4, style, uiInfo.uiDC.Assets.cornerOut );

  trap_R_SetColor( NULL );
}

/*
==================
UI_ServerInfoIsValid

Return false if the infostring contains nonprinting characters,
 or if the hostname is blank/undefined
==================
*/
static qboolean UI_ServerInfoIsValid(char *info)
{
    char *c;
    int len = 0;

    for (c = info; *c; c++)
    {
        if (!isprint(*c))
            return qfalse;
    }

    for (c = Info_ValueForKey(info, "hostname"); *c; c++)
    {
        if (isgraph(*c))
            len++;
    }

    if (len)
        return qtrue;
    else
        return qfalse;
}

/*
==================
UI_SanitiseString

Remove color codes and non-alphanumeric characters from a string
==================
*/
void UI_SanitiseString( char *in, char *out, int len )
{
    len--;

    while( *in && len > 0 )
    {
        if( Q_IsColorString( in ) )
        {
            in += 2;    // skip color code
            continue;
        }

        if( isalnum( *in ) )
        {
            *out++ = tolower( *in );
            len--;
        }
        in++;
    }
    *out = 0;
}

/*
==================
UI_PortFromAddress
==================
*/
static int UI_PortFromAddress(const char *adrStr) {
    int i;
    int portLength = 0;
    char portStr[MAX_ADDRESSLENGTH] = "";
    qboolean foundPort = qfalse;

    if (!adrStr || !adrStr[0]) {
      return -1;
    }

    for (i = 0; adrStr[i] && (adrStr[i] != ' '); i++) {
        if (!foundPort) {
            if (adrStr[i] == ':') {
                foundPort = qtrue;
            }

            continue;
        }

        portStr[portLength] = adrStr[i];
        portLength++;
    }

    if (portLength) {
        return atoi(portStr);
    } else {
        return -1;
    }
}

/*
==================
UI_ProtocolFromAddress

returns 2 if 1.1 is detected, returns 1 if gpp is detected, otherwise returns 0
==================
*/
static int UI_ProtocolFromAddress(const char *adrStr) {
    int i;

    if (!adrStr || !adrStr[0]) {
        return 0;
    }

    for (i = 0; adrStr[i]; i++) {
        if (adrStr[i] == '-') {
            if (adrStr[i+1]) {
                switch (adrStr[i+1]) {
                    case '1':
                        return 2;

                    case 'g':
                        return 1;

                    default:
                        return 0;
                }
            }
        }
    }

    return 0;
}

/*
==================
UI_RemoveServerFromDisplayList
==================
*/
static void UI_RemoveServerFromDisplayList(int num)
{
    int i, j;
    static char info[MAX_STRING_CHARS];

    for (i = 0; i < uiInfo.serverStatus.numDisplayServers; i++)
    {
        if (uiInfo.serverStatus.displayServers[i] == num)
        {
            uiInfo.serverStatus.numDisplayServers--;

            trap_LAN_GetServerInfo(ui_netSource.integer, num, info, MAX_STRING_CHARS);

            for (j = i; j < uiInfo.serverStatus.numDisplayServers; j++)
                uiInfo.serverStatus.displayServers[j] = uiInfo.serverStatus.displayServers[j+1];

            return;
        }
    }
}

/*
==================
UI_InsertServerIntoDisplayList
==================
*/
static qboolean UI_InsertServerIntoDisplayList(int num, int position)
{
    int i;
    int hostnameLen;
    int protocol;
    int port;
    char adrstr[MAX_ADDRESSLENGTH];
    char hostname[MAX_HOSTNAME_LENGTH];
    char basehostname[MAX_HOSTNAME_LENGTH];
    static char info[MAX_STRING_CHARS];

    if (position < 0 || position > uiInfo.serverStatus.numDisplayServers)
        return qfalse;

    trap_LAN_GetServerInfo(ui_netSource.integer, num, info, MAX_STRING_CHARS);

    if (!UI_ServerInfoIsValid(info))  // don't list servers with invalid info
        return qfalse;

    Q_strncpyz(hostname, Info_ValueForKey(info, "hostname"), MAX_HOSTNAME_LENGTH);

    hostnameLen = strlen(hostname);

    trap_LAN_GetServerAddressString(
        ui_netSource.integer, num, adrstr, MAX_ADDRESSLENGTH);

    protocol = UI_ProtocolFromAddress(adrstr);

    port = UI_PortFromAddress(adrstr);

    if (protocol && hostnameLen > 6) {
        // strip the protocol tags from the hostname
        hostname[hostnameLen - 6] = '\0';
    }

    UI_SanitiseString(hostname, basehostname, sizeof(basehostname));

    // check if this is a duplicate listing of a multiprotocol server
    for (i = 0; i < uiInfo.serverStatus.numDisplayServers; i++) {
        int j;
        int clients;
        int protocol2;
        int port2;
        char info2[MAX_STRING_CHARS];
        char adrstr2[MAX_ADDRESSLENGTH];

        trap_LAN_GetServerAddressString(
            ui_netSource.integer,
            uiInfo.serverStatus.displayServers[i], adrstr2, MAX_ADDRESSLENGTH);

        protocol2 = UI_ProtocolFromAddress(adrstr2);

        port2 = UI_PortFromAddress(adrstr2);

        //compare the addresses
        if (adrstr[0] != adrstr2[0]) {
            continue;
        } else {
            qboolean skip = qfalse;

            for (j = 1; adrstr[j] && adrstr2[j]; j++) {
                if(adrstr[j] != adrstr2[j]) {
                    skip = qtrue;
                    break;
                }

                //don't compare ports
                if (adrstr[j] == ':') {
                    break;
                }
            }

            if (skip) {
                continue;
            }
        }

        trap_LAN_GetServerInfo(
            ui_netSource.integer,
             uiInfo.serverStatus.displayServers[i], info2, MAX_STRING_CHARS);

        // if the ports are not the same, check to see if the host names are the
        // same for older multiprotocol servers
        if(port != port2) {
            int hostnameLen2;
            char hostname2[MAX_HOSTNAME_LENGTH];
            char basehostname2[MAX_HOSTNAME_LENGTH];

            Q_strncpyz(hostname2, Info_ValueForKey(info2, "hostname"), MAX_HOSTNAME_LENGTH);

            hostnameLen2 = strlen(hostname2);

            if (protocol2 && hostnameLen2 > 6) {
                 // strip the protocol tags from the hostname
                 hostname2[hostnameLen2 - 7] = '\0';
            }

            UI_SanitiseString(hostname2, basehostname2, sizeof(basehostname2));

            //compare the hostnames
            if (Q_stricmp(basehostname, basehostname2)) {
                continue;
            }
        }

        uiInfo.serverStatus.numDuplicateMultiprotocolServers++;

        //show only the most recent protocol for a given server
        if (protocol >= protocol2) {
            clients = atoi(Info_ValueForKey(info, "clients"));
            uiInfo.serverStatus.numDuplicateMultiprotocolServerClients += clients;
            return qfalse;
        } else {
            clients = atoi(Info_ValueForKey(info2, "clients"));
            uiInfo.serverStatus.numDuplicateMultiprotocolServerClients += clients;
            UI_RemoveServerFromDisplayList(uiInfo.serverStatus.displayServers[i]);
            i--;
            continue;
        }
    }

    //insert the server
    uiInfo.serverStatus.numDisplayServers++;

    for (i = uiInfo.serverStatus.numDisplayServers; i > position; i--)
        uiInfo.serverStatus.displayServers[i] = uiInfo.serverStatus.displayServers[i - 1];

    uiInfo.serverStatus.displayServers[position] = num;

    return qtrue;
}

/*
==================
UI_BinaryServerInsertion
==================
*/
static qboolean UI_BinaryServerInsertion(int num)
{
    int mid, offset, res, len;

    // use binary search to insert server
    len = uiInfo.serverStatus.numDisplayServers;
    mid = len;
    offset = 0;
    res = 0;

    while (mid > 0)
    {
        mid = len >> 1;
        //
        res = trap_LAN_CompareServers(ui_netSource.integer, uiInfo.serverStatus.sortKey, uiInfo.serverStatus.sortDir,
            num, uiInfo.serverStatus.displayServers[offset + mid]);
        // if equal

        if (res == 0)
        {
            return UI_InsertServerIntoDisplayList(num, offset + mid);
        }

        // if larger
        else if (res == 1)
        {
            offset += mid;
            len -= mid;
        }

        // if smaller
        else
            len -= mid;
    }

    if (res == 1)
        offset++;

    return UI_InsertServerIntoDisplayList(num, offset);
}

typedef struct {
    char *name, *altName;
}

serverStatusCvar_t;

serverStatusCvar_t serverStatusCvars[] = {{"sv_hostname", "Name"}, {"Address", ""}, {"gamename", "Game name"},
    {"mapname", "Map"}, {"version", ""}, {"protocol", ""}, {"timelimit", ""}, {NULL, NULL}};

/*
==================
UI_SortServerStatusInfo
==================
*/

static int UI_SortServerStatusCompare(const void *a, const void *b)
{
    const char **la = (const char **)a;
    const char **lb = (const char **)b;

    return strcmp(la[0], lb[0]);
}

static void UI_SortServerStatusInfo(serverStatusInfo_t *info)
{
    int i, j, index;
    char *tmp1, *tmp2;

    index = 0;

    for (i = 0; serverStatusCvars[i].name; i++)
    {
        for (j = 0; j < info->numLines; j++)
        {
            if (!info->lines[j][1] || info->lines[j][1][0])
                continue;

            if (!Q_stricmp(serverStatusCvars[i].name, info->lines[j][0]))
            {
                // swap lines
                tmp1 = info->lines[index][0];
                tmp2 = info->lines[index][3];
                info->lines[index][0] = info->lines[j][0];
                info->lines[index][3] = info->lines[j][3];
                info->lines[j][0] = tmp1;
                info->lines[j][3] = tmp2;
                //

                if (strlen(serverStatusCvars[i].altName))
                    info->lines[index][0] = serverStatusCvars[i].altName;

                index++;
            }
        }
    }

    // sort remaining cvars
    qsort(info->lines + index, info->numLines - index, sizeof(info->lines[0]), UI_SortServerStatusCompare);
}

/*
==================
UI_GetServerStatusInfo
==================
*/
static int UI_GetServerStatusInfo(const char *serverAddress, serverStatusInfo_t *info)
{
    char *p, *score, *ping, *name;
    int i, len;

    if (!info)
    {
        trap_LAN_ServerStatus(serverAddress, NULL, 0);
        return qfalse;
    }

    memset(info, 0, sizeof(*info));

    if (trap_LAN_ServerStatus(serverAddress, info->text, sizeof(info->text)))
    {
        Q_strncpyz(info->address, serverAddress, sizeof(info->address));
        p = info->text;
        info->numLines = 0;
        info->lines[info->numLines][0] = "Address";
        info->lines[info->numLines][1] = "";
        info->lines[info->numLines][2] = "";
        info->lines[info->numLines][3] = info->address;
        info->numLines++;
        // get the cvars

        while (p && *p)
        {
            p = strchr(p, '\\');

            if (!p)
                break;

            *p++ = '\0';

            if (*p == '\\')
                break;

            info->lines[info->numLines][0] = p;
            info->lines[info->numLines][1] = "";
            info->lines[info->numLines][2] = "";

            p = strchr(p, '\\');

            if (!p)
                break;

            *p++ = '\0';

            info->lines[info->numLines][3] = p;
            info->numLines++;

            if (info->numLines >= MAX_SERVERSTATUS_LINES)
                break;
        }

        UI_SortServerStatusInfo(info);

        // get the player list
        if (info->numLines < MAX_SERVERSTATUS_LINES - 3)
        {
            // empty line
            info->lines[info->numLines][0] = "";
            info->lines[info->numLines][1] = "";
            info->lines[info->numLines][2] = "";
            info->lines[info->numLines][3] = "";
            info->numLines++;
            // header
            info->lines[info->numLines][0] = "num";
            info->lines[info->numLines][1] = "score";
            info->lines[info->numLines][2] = "ping";
            info->lines[info->numLines][3] = "name";
            info->numLines++;
            // parse players
            i = 0;
            len = 0;

            while (p && *p)
            {
                if (*p == '\\')
                    *p++ = '\0';

                if (!p)
                    break;

                score = p;

                p = strchr(p, ' ');

                if (!p)
                    break;

                *p++ = '\0';

                ping = p;

                p = strchr(p, ' ');

                if (!p)
                    break;

                *p++ = '\0';

                name = p;

                Com_sprintf(&info->pings[len], sizeof(info->pings) - len, "%d", i);

                info->lines[info->numLines][0] = &info->pings[len];

                len += strlen(&info->pings[len]) + 1;

                info->lines[info->numLines][1] = score;
                info->lines[info->numLines][2] = ping;
                info->lines[info->numLines][3] = name;
                info->numLines++;

                if (info->numLines >= MAX_SERVERSTATUS_LINES)
                    break;

                p = strchr(p, '\\');

                if (!p)
                    break;

                *p++ = '\0';

                //
                i++;
            }
        }

        return qtrue;
    }

    return qfalse;
}

/*
==================
stristr
==================
*/
static char *stristr(char *str, char *charset)
{
    int i;

    while (*str)
    {
        for (i = 0; charset[i] && str[i]; i++)
            if (toupper(charset[i]) != toupper(str[i]))
                break;

        if (!charset[i])
            return str;

        str++;
    }

    return NULL;
}

/*
==================
UI_BuildFindPlayerList
==================
*/
static void UI_FeederSelection(int feederID, int index);

static void UI_BuildFindPlayerList(qboolean force)
{
    static int numFound, numTimeOuts;
    int i, j, k, resend;
    serverStatusInfo_t info;
    char name[MAX_COLORFUL_NAME_LENGTH + 2];
    char infoString[MAX_STRING_CHARS];
    qboolean duplicate;

    if (!force)
    {
        if (!uiInfo.nextFindPlayerRefresh || uiInfo.nextFindPlayerRefresh > uiInfo.uiDC.realTime)
            return;
    }
    else
    {
        memset(&uiInfo.pendingServerStatus, 0, sizeof(uiInfo.pendingServerStatus));
        uiInfo.numFoundPlayerServers = 0;
        uiInfo.currentFoundPlayerServer = 0;
        trap_Cvar_VariableStringBuffer("ui_findPlayer", uiInfo.findPlayerName, sizeof(uiInfo.findPlayerName));
        Q_CleanStr(uiInfo.findPlayerName);
        // should have a string of some length

        if (!strlen(uiInfo.findPlayerName))
        {
            uiInfo.nextFindPlayerRefresh = 0;
            return;
        }

        // set resend time
        resend = ui_serverStatusTimeOut.integer / 2 - 10;

        if (resend < 50)
            resend = 50;

        trap_Cvar_Set("cl_serverStatusResendTime", va("%d", resend));
        // reset all server status requests
        trap_LAN_ServerStatus(NULL, NULL, 0);
        //
        uiInfo.numFoundPlayerServers = 1;
        Com_sprintf(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1],
            sizeof(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1]), "searching %d...",
            uiInfo.pendingServerStatus.num);
        numFound = 0;
        numTimeOuts++;
    }

    for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++)
    {
        // if this pending server is valid

        if (uiInfo.pendingServerStatus.server[i].valid)
        {
            // try to get the server status for this server

            if (UI_GetServerStatusInfo(uiInfo.pendingServerStatus.server[i].adrstr, &info))
            {
                //
                numFound++;
                // parse through the server status lines

                for (j = 0; j < info.numLines; j++)
                {
                    // should have ping info

                    if (!info.lines[j][2] || !info.lines[j][2][0])
                        continue;

                    // clean string first
                    Q_strncpyz(name, info.lines[j][3], sizeof(name));

                    Q_CleanStr(name);

                    duplicate = qfalse;

                    for (k = 0; k < uiInfo.numFoundPlayerServers - 1; k++)
                    {
                        if (Q_strncmp(uiInfo.foundPlayerServerAddresses[k], uiInfo.pendingServerStatus.server[i].adrstr,
                                MAX_ADDRESSLENGTH) == 0)
                            duplicate = qtrue;
                    }

                    // if the player name is a substring
                    if (stristr(name, uiInfo.findPlayerName) && !duplicate)
                    {
                        // add to found server list if we have space (always leave space for a line with the number
                        // found)

                        if (uiInfo.numFoundPlayerServers < MAX_FOUNDPLAYER_SERVERS - 1)
                        {
                            //
                            Q_strncpyz(uiInfo.foundPlayerServerAddresses[uiInfo.numFoundPlayerServers - 1],
                                uiInfo.pendingServerStatus.server[i].adrstr,
                                sizeof(uiInfo.foundPlayerServerAddresses[0]));
                            Q_strncpyz(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1],
                                uiInfo.pendingServerStatus.server[i].name, sizeof(uiInfo.foundPlayerServerNames[0]));
                            uiInfo.numFoundPlayerServers++;
                        }
                        else
                        {
                            // can't add any more so we're done
                            uiInfo.pendingServerStatus.num = uiInfo.serverStatus.numDisplayServers;
                        }
                    }
                }

                Com_sprintf(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1],
                    sizeof(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1]), "searching %d/%d...",
                    numFound, uiInfo.pendingServerStatus.num);
                // retrieved the server status so reuse this spot
                uiInfo.pendingServerStatus.server[i].valid = qfalse;
            }
        }

        // if empty pending slot or timed out
        if (!uiInfo.pendingServerStatus.server[i].valid ||
            uiInfo.pendingServerStatus.server[i].startTime < uiInfo.uiDC.realTime - ui_serverStatusTimeOut.integer)
        {
            if (uiInfo.pendingServerStatus.server[i].valid)
                numTimeOuts++;

            // reset server status request for this address
            UI_GetServerStatusInfo(uiInfo.pendingServerStatus.server[i].adrstr, NULL);

            // reuse pending slot
            uiInfo.pendingServerStatus.server[i].valid = qfalse;

            // if we didn't try to get the status of all servers in the main browser yet
            if (uiInfo.pendingServerStatus.num < uiInfo.serverStatus.numDisplayServers)
            {
                uiInfo.pendingServerStatus.server[i].startTime = uiInfo.uiDC.realTime;
                trap_LAN_GetServerAddressString(ui_netSource.integer,
                    uiInfo.serverStatus.displayServers[uiInfo.pendingServerStatus.num],
                    uiInfo.pendingServerStatus.server[i].adrstr, sizeof(uiInfo.pendingServerStatus.server[i].adrstr));

                trap_LAN_GetServerInfo(ui_netSource.integer,
                    uiInfo.serverStatus.displayServers[uiInfo.pendingServerStatus.num], infoString, sizeof(infoString));

                Q_strncpyz(uiInfo.pendingServerStatus.server[i].name, Info_ValueForKey(infoString, "hostname"),
                    sizeof(uiInfo.pendingServerStatus.server[0].name));

                uiInfo.pendingServerStatus.server[i].valid = qtrue;
                uiInfo.pendingServerStatus.num++;
                Com_sprintf(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1],
                    sizeof(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1]), "searching %d/%d...",
                    numFound, uiInfo.pendingServerStatus.num);
            }
        }
    }

    for (i = 0; i < MAX_SERVERSTATUSREQUESTS; i++)
    {
        if (uiInfo.pendingServerStatus.server[i].valid)
            break;
    }

    // if still trying to retrieve server status info
    if (i < MAX_SERVERSTATUSREQUESTS)
        uiInfo.nextFindPlayerRefresh = uiInfo.uiDC.realTime + 25;
    else
    {
        // add a line that shows the number of servers found

        if (!uiInfo.numFoundPlayerServers)
        {
            Com_sprintf(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1],
                sizeof(uiInfo.foundPlayerServerAddresses[0]), "no servers found");
        }
        else
        {
            Com_sprintf(uiInfo.foundPlayerServerNames[uiInfo.numFoundPlayerServers - 1],
                sizeof(uiInfo.foundPlayerServerAddresses[0]), "%d server%s found with player %s",
                uiInfo.numFoundPlayerServers - 1, uiInfo.numFoundPlayerServers == 2 ? "" : "s", uiInfo.findPlayerName);
        }

        uiInfo.nextFindPlayerRefresh = 0;
        // show the server status info for the selected server
        UI_FeederSelection(FEEDER_FINDPLAYER, uiInfo.currentFoundPlayerServer);
    }
}

/*
==================
UI_ClientNumbersFromString

Sets plist to an array of integers that represent client numbers that have
names that are a partial match for s.

Returns number of matching clientids up to max.
==================
*/
static void UI_BuildPlayerList();
int UI_ClientNumbersFromString(char *s, int *plist, int max) {
  int i, found = 0;
  char n2[MAX_COLORFUL_NAME_LENGTH] = {""};
  char s2[MAX_COLORFUL_NAME_LENGTH] = {""};
  char n2_temp[MAX_COLORFUL_NAME_LENGTH] = {""};
  char s2_temp[MAX_COLORFUL_NAME_LENGTH] = {""};

  if(max == 0) {
    return 0;
  }

  UI_BuildPlayerList();

  if(!s[0]) {
    for(i = 0; i < uiInfo.playerCount && found < max; i++) {
      *plist++ = i;
      found++;
    }
    return found;
  }

  // now look for name matches
  Q_strncpyz(s2_temp, s, sizeof(s2_temp));
  Q_CleanStr(s2_temp);
  Q_StringToLower(s2_temp, s2, sizeof(s2));
  if(!s2[0]) {
    return 0;
  }

    for(i = 0; i < uiInfo.playerCount && found < max; i++) {
        Q_strncpyz(n2_temp, uiInfo.playerNames[i], sizeof(n2_temp));
        Q_CleanStr(n2_temp);
        Q_StringToLower(n2_temp, n2, sizeof(n2));
        if(strstr(n2, s2)) {
            *plist++ = i;
            found++;
        }
    }

    return found;
}

/*
==================
UI_BuildServerStatus
==================
*/
static void UI_BuildServerStatus(qboolean force)
{
    if (uiInfo.nextFindPlayerRefresh)
        return;

    if (!force)
    {
        if (!uiInfo.nextServerStatusRefresh || uiInfo.nextServerStatusRefresh > uiInfo.uiDC.realTime)
            return;
    }
    else
    {
        Menu_SetFeederSelection(NULL, FEEDER_SERVERSTATUS, 0, NULL);
        uiInfo.serverStatusInfo.numLines = 0;
        // reset all server status requests
        trap_LAN_ServerStatus(NULL, NULL, 0);
    }

    if (uiInfo.serverStatus.currentServer < 0 ||
        uiInfo.serverStatus.currentServer > uiInfo.serverStatus.numDisplayServers ||
        uiInfo.serverStatus.numDisplayServers == 0)
        return;

    if (UI_GetServerStatusInfo(uiInfo.serverStatusAddress, &uiInfo.serverStatusInfo))
    {
        uiInfo.nextServerStatusRefresh = 0;
        UI_GetServerStatusInfo(uiInfo.serverStatusAddress, NULL);
    }
    else
        uiInfo.nextServerStatusRefresh = uiInfo.uiDC.realTime + 500;
}

/*
==================
UI_BuildServerDisplayList
==================
*/
static void UI_BuildServerDisplayList(int force)
{
    int i, count, clients, maxClients, ping, len, visible;
    char info[MAX_STRING_CHARS];
    static int numinvisible;

    if (!(force || uiInfo.uiDC.realTime > uiInfo.serverStatus.nextDisplayRefresh))
        return;

    // if we shouldn't reset
    if (force == 2)
        force = 0;

    // do motd updates here too
    trap_Cvar_VariableStringBuffer("cl_motdString", uiInfo.serverStatus.motd, sizeof(uiInfo.serverStatus.motd));

    len = strlen(uiInfo.serverStatus.motd);

    if (len != uiInfo.serverStatus.motdLen)
    {
        uiInfo.serverStatus.motdLen = len;
        uiInfo.serverStatus.motdWidth = -1;
    }

    if (force)
    {
        numinvisible = 0;
        // clear number of displayed servers
        uiInfo.serverStatus.numDisplayServers = 0;
        uiInfo.serverStatus.numPlayersOnServers = 0;
        uiInfo.serverStatus.numDuplicateMultiprotocolServers = 0;
        uiInfo.serverStatus.numDuplicateMultiprotocolServerClients = 0;
        // set list box index to zero
        Menu_SetFeederSelection(NULL, FEEDER_SERVERS, 0, NULL);
        // mark all servers as visible so we store ping updates for them
        trap_LAN_MarkServerVisible(ui_netSource.integer, -1, qtrue);
    }

    // get the server count (comes from the master)
    count = trap_LAN_GetServerCount(ui_netSource.integer);

    if (count == -1 || (ui_netSource.integer == AS_LOCAL && count == 0))
    {
        // still waiting on a response from the master
        uiInfo.serverStatus.numDisplayServers = 0;
        uiInfo.serverStatus.numPlayersOnServers = 0;
        uiInfo.serverStatus.numDuplicateMultiprotocolServers = 0;
        uiInfo.serverStatus.numDuplicateMultiprotocolServerClients = 0;
        uiInfo.serverStatus.nextDisplayRefresh = uiInfo.uiDC.realTime + 500;
        return;
    }

    visible = qfalse;

    for (i = 0; i < count; i++)
    {
        // if we already got info for this server

        if (!trap_LAN_ServerIsVisible(ui_netSource.integer, i))
            continue;

        visible = qtrue;
        // get the ping for this server
        ping = trap_LAN_GetServerPing(ui_netSource.integer, i);

        if (ping > 0 || ui_netSource.integer == AS_FAVORITES)
        {
            trap_LAN_GetServerInfo(ui_netSource.integer, i, info, MAX_STRING_CHARS);

            clients = atoi(Info_ValueForKey(info, "clients"));
            uiInfo.serverStatus.numPlayersOnServers += clients;

            if (ui_browserShowEmpty.integer == 0)
            {
                if (clients == 0)
                {
                    trap_LAN_MarkServerVisible(ui_netSource.integer, i, qfalse);
                    continue;
                }
            }

            if (ui_browserShowFull.integer == 0)
            {
                maxClients = atoi(Info_ValueForKey(info, "sv_maxclients"));

                if (clients == maxClients)
                {
                    trap_LAN_MarkServerVisible(ui_netSource.integer, i, qfalse);
                    continue;
                }
            }

            // make sure we never add a favorite server twice
            if (ui_netSource.integer == AS_FAVORITES)
                UI_RemoveServerFromDisplayList(i);

            // insert the server into the list
            UI_BinaryServerInsertion(i);

            // done with this server
            if (ping > 0)
            {
                trap_LAN_MarkServerVisible(ui_netSource.integer, i, qfalse);
                numinvisible++;
            }
        }
    }

    uiInfo.serverStatus.refreshtime = uiInfo.uiDC.realTime;

    // if there were no servers visible for ping updates

    if (!visible)
    {
        //    UI_StopServerRefresh();
        //    uiInfo.serverStatus.nextDisplayRefresh = 0;
    }
}

/*
=================
UI_StopServerRefresh
=================
*/
static void UI_StopServerRefresh(void)
{
    int count;

    if (!uiInfo.serverStatus.refreshActive)
    {
        // not currently refreshing
        return;
    }

    uiInfo.serverStatus.refreshActive = qfalse;
    Com_Printf("%d servers listed in browser with %d players.\n", uiInfo.serverStatus.numDisplayServers,
        uiInfo.serverStatus.numPlayersOnServers -
            uiInfo.serverStatus.numDuplicateMultiprotocolServerClients);
    count = trap_LAN_GetServerCount(ui_netSource.integer);

    if (count - uiInfo.serverStatus.numDisplayServers - uiInfo.serverStatus.numDuplicateMultiprotocolServers > 0)
    {
        Com_Printf(
            "%d servers not listed due to packet loss, invalid info,"
            " or pings higher than %d\n",
            count - uiInfo.serverStatus.numDisplayServers - uiInfo.serverStatus.numDuplicateMultiprotocolServers,
            (int)trap_Cvar_VariableValue("cl_maxPing"));
    }
}

/*
=================
UI_DoServerRefresh
=================
*/
static void UI_DoServerRefresh(void)
{
    qboolean wait = qfalse;

    if (!uiInfo.serverStatus.refreshActive)
        return;

    if (ui_netSource.integer != AS_FAVORITES)
    {
        if (ui_netSource.integer == AS_LOCAL)
        {
            if (!trap_LAN_GetServerCount(ui_netSource.integer))
                wait = qtrue;
        }
        else
        {
            if (trap_LAN_GetServerCount(ui_netSource.integer) < 0)
                wait = qtrue;
        }
    }

    if (uiInfo.uiDC.realTime < uiInfo.serverStatus.refreshtime)
    {
        if (wait)
            return;
    }

    // if still trying to retrieve pings
    if (trap_LAN_UpdateVisiblePings(ui_netSource.integer))
        uiInfo.serverStatus.refreshtime = uiInfo.uiDC.realTime + 1000;
    else if (!wait)
    {
        // get the last servers in the list
        UI_BuildServerDisplayList(2);
        // stop the refresh
        UI_StopServerRefresh();
    }

    //
    UI_BuildServerDisplayList(qfalse);
}

/*
=================
UI_UpdatePendingPings
=================
*/
static void UI_UpdatePendingPings(void)
{
    trap_LAN_ResetPings(ui_netSource.integer);
    uiInfo.serverStatus.refreshActive = qtrue;
    uiInfo.serverStatus.refreshtime = uiInfo.uiDC.realTime + 1000;
}

/*
=================
UI_StartServerRefresh
=================
*/
static void UI_StartServerRefresh(qboolean full)
{
    int time;
    qtime_t q;

    time = trap_RealTime(&q);
    trap_Cvar_Set(va("ui_lastServerRefresh_%i_time", ui_netSource.integer), va("%i", time));
    trap_Cvar_Set(va("ui_lastServerRefresh_%i", ui_netSource.integer),
        va("%04i-%02i-%02i %02i:%02i:%02i", q.tm_year + 1900, q.tm_mon + 1, q.tm_mday, q.tm_hour, q.tm_min, q.tm_sec));

    if (!full)
    {
        UI_UpdatePendingPings();
        return;
    }

    uiInfo.serverStatus.refreshActive = qtrue;
    uiInfo.serverStatus.nextDisplayRefresh = uiInfo.uiDC.realTime + 1000;
    // clear number of displayed servers
    uiInfo.serverStatus.numDisplayServers = 0;
    uiInfo.serverStatus.numPlayersOnServers = 0;
    uiInfo.serverStatus.numDuplicateMultiprotocolServers = 0;
    uiInfo.serverStatus.numDuplicateMultiprotocolServerClients = 0;
    // mark all servers as visible so we store ping updates for them
    trap_LAN_MarkServerVisible(ui_netSource.integer, -1, qtrue);
    // reset all the pings
    trap_LAN_ResetPings(ui_netSource.integer);
    //

    if (ui_netSource.integer == AS_LOCAL)
    {
        trap_Cmd_ExecuteText(EXEC_APPEND, "localservers\n");
        uiInfo.serverStatus.refreshtime = uiInfo.uiDC.realTime + 1000;
        return;
    }

    uiInfo.serverStatus.refreshtime = uiInfo.uiDC.realTime + 5000;

    if (ui_netSource.integer == AS_GLOBAL || ui_netSource.integer == AS_MPLAYER)
    {
        qboolean global = ui_netSource.integer == AS_GLOBAL;

#ifdef MODULE_INTERFACE_11
        trap_Cmd_ExecuteText(EXEC_APPEND, va("globalservers %d 69 full empty\n",
#else
        trap_Cmd_ExecuteText(EXEC_APPEND, va("globalservers %d 70 full empty\n",
#endif
                                              global ? 0 : 1));
    }
}

int frameCount = 0;
int startTime;

#define UI_FPS_FRAMES 4
void UI_Refresh(int realtime)
{
    static int index;
    static int previousTimes[UI_FPS_FRAMES];

    // if( !( trap_Key_GetCatcher() & KEYCATCH_UI ) ) {
    //  return;
    //}

    uiInfo.uiDC.frameTime = realtime - uiInfo.uiDC.realTime;
    uiInfo.uiDC.realTime = realtime;

    previousTimes[index % UI_FPS_FRAMES] = uiInfo.uiDC.frameTime;
    index++;

    if (index > UI_FPS_FRAMES)
    {
        int i, total;
        // average multiple frames together to smooth changes out a bit
        total = 0;

        for (i = 0; i < UI_FPS_FRAMES; i++)
            total += previousTimes[i];

        if (!total)
            total = 1;

        uiInfo.uiDC.FPS = 1000 * UI_FPS_FRAMES / total;
    }

    UI_UpdateCvars();

    if (Menu_Count() > 0)
    {
        Menu_UpdateAll();
        Menu_PaintAll();
        UI_DoServerRefresh();
        UI_BuildServerStatus(qfalse);
        UI_BuildFindPlayerList(qfalse);
        UI_UpdateNews(qfalse);
        // FIXME: CHECK FOR "AUTOMATICALLLY CHECK FOR UPDATES == true"
        // UI_UpdateGithubRelease( );
    }

    // draw cursor
    UI_SetColor(NULL);

    if (trap_Key_GetCatcher() == KEYCATCH_UI && !trap_Cvar_VariableValue("ui_hideCursor"))
    {
        UI_DrawHandlePic(uiInfo.uiDC.cursorx - (16.0f * uiInfo.uiDC.aspectScale), uiInfo.uiDC.cursory - 16.0f,
            32.0f * uiInfo.uiDC.aspectScale, 32.0f, uiInfo.uiDC.Assets.cursor);
    }
}

/*
=================
UI_Shutdown
=================
*/
void UI_Shutdown(void) { 
  BG_Bucket_Destroy_All_Buckets( );
  trap_LAN_SaveCachedServers();
}

qboolean Asset_Parse(int handle)
{
    pc_token_t token;
    const char *tempStr;

    if (!trap_Parse_ReadToken(handle, &token))
        return qfalse;

    if (Q_stricmp(token.string, "{") != 0)
        return qfalse;

    while (1)
    {
        memset(&token, 0, sizeof(pc_token_t));

        if (!trap_Parse_ReadToken(handle, &token))
            return qfalse;

        if (Q_stricmp(token.string, "}") == 0)
            return qtrue;

        // font
        if (Q_stricmp(token.string, "font") == 0)
        {
            int pointSize;

            if (!PC_String_Parse(handle, &tempStr) || !PC_Int_Parse(handle, &pointSize))
                return qfalse;

            trap_R_RegisterFont(tempStr, pointSize, &uiInfo.uiDC.Assets.textFont);
            uiInfo.uiDC.Assets.fontRegistered = qtrue;
            continue;
        }

        if (Q_stricmp(token.string, "smallFont") == 0)
        {
            int pointSize;

            if (!PC_String_Parse(handle, &tempStr) || !PC_Int_Parse(handle, &pointSize))
                return qfalse;

            trap_R_RegisterFont(tempStr, pointSize, &uiInfo.uiDC.Assets.smallFont);
            continue;
        }

        if (Q_stricmp(token.string, "bigFont") == 0)
        {
            int pointSize;

            if (!PC_String_Parse(handle, &tempStr) || !PC_Int_Parse(handle, &pointSize))
                return qfalse;

            trap_R_RegisterFont(tempStr, pointSize, &uiInfo.uiDC.Assets.bigFont);
            continue;
        }

        // gradientbar
        if (Q_stricmp(token.string, "gradientbar") == 0)
        {
            if (!PC_String_Parse(handle, &tempStr))
                return qfalse;

            uiInfo.uiDC.Assets.gradientBar = trap_R_RegisterShaderNoMip(tempStr);
            continue;
        }

        // enterMenuSound
        if (Q_stricmp(token.string, "menuEnterSound") == 0)
        {
            if (!PC_String_Parse(handle, &tempStr))
                return qfalse;

            uiInfo.uiDC.Assets.menuEnterSound = trap_S_RegisterSound(tempStr, qfalse);
            continue;
        }

        // exitMenuSound
        if (Q_stricmp(token.string, "menuExitSound") == 0)
        {
            if (!PC_String_Parse(handle, &tempStr))
                return qfalse;

            uiInfo.uiDC.Assets.menuExitSound = trap_S_RegisterSound(tempStr, qfalse);
            continue;
        }

        // itemFocusSound
        if (Q_stricmp(token.string, "itemFocusSound") == 0)
        {
            if (!PC_String_Parse(handle, &tempStr))
                return qfalse;

            uiInfo.uiDC.Assets.itemFocusSound = trap_S_RegisterSound(tempStr, qfalse);
            continue;
        }

        // menuBuzzSound
        if (Q_stricmp(token.string, "menuBuzzSound") == 0)
        {
            if (!PC_String_Parse(handle, &tempStr))
                return qfalse;

            uiInfo.uiDC.Assets.menuBuzzSound = trap_S_RegisterSound(tempStr, qfalse);
            continue;
        }

        if (Q_stricmp(token.string, "cursor") == 0)
        {
            if (!PC_String_Parse(handle, &uiInfo.uiDC.Assets.cursorStr))
                return qfalse;

            uiInfo.uiDC.Assets.cursor = trap_R_RegisterShaderNoMip(uiInfo.uiDC.Assets.cursorStr);
            continue;
        }

        if (Q_stricmp(token.string, "fadeClamp") == 0)
        {
            if (!PC_Float_Parse(handle, &uiInfo.uiDC.Assets.fadeClamp))
                return qfalse;

            continue;
        }

        if (Q_stricmp(token.string, "fadeCycle") == 0)
        {
            if (!PC_Int_Parse(handle, &uiInfo.uiDC.Assets.fadeCycle))
                return qfalse;

            continue;
        }

        if (Q_stricmp(token.string, "fadeAmount") == 0)
        {
            if (!PC_Float_Parse(handle, &uiInfo.uiDC.Assets.fadeAmount))
                return qfalse;

            continue;
        }

        if (Q_stricmp(token.string, "shadowX") == 0)
        {
            if (!PC_Float_Parse(handle, &uiInfo.uiDC.Assets.shadowX))
                return qfalse;

            continue;
        }

        if (Q_stricmp(token.string, "shadowY") == 0)
        {
            if (!PC_Float_Parse(handle, &uiInfo.uiDC.Assets.shadowY))
                return qfalse;

            continue;
        }

        if (Q_stricmp(token.string, "shadowColor") == 0)
        {
            if (!PC_Color_Parse(handle, &uiInfo.uiDC.Assets.shadowColor))
                return qfalse;

            uiInfo.uiDC.Assets.shadowFadeClamp = uiInfo.uiDC.Assets.shadowColor[3];
            continue;
        }
    }

    return qfalse;
}

void UI_Report(void) { String_Report(); }

void UI_ParseMenu(const char *menuFile)
{
    int handle;
    pc_token_t token;

    handle = trap_Parse_LoadSource(menuFile);

    if (!handle)
    {
        Com_Printf(S_COLOR_YELLOW "WARNING: Menu file %s not found\n", menuFile);
        return;
    }

    while (1)
    {
        memset(&token, 0, sizeof(pc_token_t));

        if (!trap_Parse_ReadToken(handle, &token))
            break;

        // if( Q_stricmp( token, "{" ) ) {
        //  Com_Printf( "Missing { in menu file\n" );
        //  break;
        //}

        // if( menuCount == MAX_MENUS ) {
        //  Com_Printf( "Too many menus!\n" );
        //  break;
        //}

        if (token.string[0] == '}')
            break;

        if (Q_stricmp(token.string, "assetGlobalDef") == 0)
        {
            if (Asset_Parse(handle))
                continue;
            else
                break;
        }

        if (Q_stricmp(token.string, "menudef") == 0)
        {
            // start a new menu
            Menu_New(handle);
        }
    }

    trap_Parse_FreeSource(handle);
}

qboolean Load_Menu(int handle)
{
    pc_token_t token;

    if (!trap_Parse_ReadToken(handle, &token))
        return qfalse;

    if (token.string[0] != '{')
        return qfalse;

    while (1)
    {
        if (!trap_Parse_ReadToken(handle, &token))
            return qfalse;

        if (token.string[0] == 0)
            return qfalse;

        if (token.string[0] == '}')
            return qtrue;

        UI_ParseMenu(token.string);
    }

    return qfalse;
}

void UI_LoadMenus(const char *menuFile, qboolean reset)
{
    pc_token_t token;
    int handle;
    int start;

    start = trap_Milliseconds();

    handle = trap_Parse_LoadSource(menuFile);

    if (!handle)
        trap_Error(va(S_COLOR_RED "menu list '%s' not found, unable to continue!", menuFile));

    if (reset)
        Menu_Reset();

    while (1)
    {
        if (!trap_Parse_ReadToken(handle, &token))
            break;

        if (token.string[0] == 0 || token.string[0] == '}')
            break;

        if (token.string[0] == '}')
            break;

        if (Q_stricmp(token.string, "loadmenu") == 0)
        {
            if (Load_Menu(handle))
                continue;
            else
                break;
        }
    }

    Com_Printf("UI menu file '%s' loaded in %d msec\n", menuFile, trap_Milliseconds() - start);

    trap_Parse_FreeSource(handle);
}

void UI_LoadHelp(const char *helpFile)
{
    pc_token_t token;
    int handle, start;
    char title[32], buffer[1024];

    start = trap_Milliseconds();

    handle = trap_Parse_LoadSource(helpFile);
    if (!handle)
    {
        Com_Printf(S_COLOR_YELLOW "WARNING: help file '%s' not found!\n", helpFile);
        return;
    }

    if (!trap_Parse_ReadToken(handle, &token) || token.string[0] == 0 || token.string[0] != '{')
    {
        Com_Printf(S_COLOR_YELLOW
            "WARNING: help file '%s' does not start with "
            "'{'\n",
            helpFile);
        return;
    }

    uiInfo.helpCount = 0;
    title[0] = 0;
    while (1)
    {
        if (!trap_Parse_ReadToken(handle, &token) || token.string[0] == 0 || token.string[0] == '}')
            break;

        if (token.string[0] == '{')
        {
            buffer[0] = 0;
            Q_strcat(buffer, sizeof(buffer), title);
            Q_strcat(buffer, sizeof(buffer), "\n\n");
            while (trap_Parse_ReadToken(handle, &token) && token.string[0] != 0 && token.string[0] != '}')
            {
                Q_strcat(buffer, sizeof(buffer), token.string);
            }

            uiInfo.helpList[uiInfo.helpCount].text = String_Alloc(title);
            uiInfo.helpList[uiInfo.helpCount].v.text = String_Alloc(buffer);
            uiInfo.helpList[uiInfo.helpCount].type = INFOTYPE_TEXT;
            uiInfo.helpCount++;
            title[0] = 0;
        }
        else
            Q_strcat(title, sizeof(title), token.string);
    }

    trap_Parse_FreeSource(handle);

    Com_Printf("UI help file '%s' loaded in %d msec (%d infopanes)\n", helpFile, trap_Milliseconds() - start,
        uiInfo.helpCount);
}

void UI_Load(void)
{
    char lastName[1024];
    menuDef_t *menu = Menu_GetFocused();

    if (menu && menu->window.name)
        strcpy(lastName, menu->window.name);

    String_Init();

    UI_LoadMenus("ui/menus.txt", qtrue);
    UI_LoadMenus("ui/ingame.txt", qfalse);
    UI_LoadMenus("ui/tremulous.txt", qfalse);
    UI_LoadHelp("ui/help.txt");
    Menus_CloseAll();
    Menus_ActivateByName(lastName);
}

/*
===============
UI_GetCurrentAlienStage
===============
*/
static stage_t UI_GetCurrentAlienStage(void)
{
    char buffer[MAX_TOKEN_CHARS];
    stage_t stage, dummy;

    trap_Cvar_VariableStringBuffer("ui_stages", buffer, sizeof(buffer));
    sscanf(buffer, "%d %d", (int *)&stage, (int *)&dummy);

    return stage;
}

/*
===============
UI_GetCurrentHumanStage
===============
*/
static stage_t UI_GetCurrentHumanStage(void)
{
    char buffer[MAX_TOKEN_CHARS];
    stage_t stage, dummy;

    trap_Cvar_VariableStringBuffer("ui_stages", buffer, sizeof(buffer));
    sscanf(buffer, "%d %d", (int *)&dummy, (int *)&stage);

    return stage;
}

/*
===============
UI_alienStates
===============
*/
static void UI_alienStates(alienStates_t *state)
{
  char alienStates[MAX_TOKEN_CHARS];

  trap_Cvar_VariableStringBuffer("ui_alienStates", alienStates, sizeof(alienStates));

  sscanf( alienStates, "%d %d %d %d %d", &state->omBuilding, &state->omHealth,
      &state->spawns, &state->builders, &state->boosters );
}

/*
===============
UI_humanStates
===============
*/
static void UI_humanStates(humanStates_t *state)
{
  char humanStates[MAX_TOKEN_CHARS];

  trap_Cvar_VariableStringBuffer("ui_humanStates", humanStates, sizeof(humanStates));

  sscanf( humanStates, "%d %d %d %d %d %d %d", &state->rcBuilding, &state->rcHealth,
      &state->spawns, &state->builders, &state->armourys, &state->medicals, &state->computers );
}

/*
===============
UI_GetStageText
===============
*/
static char *UI_GetStageText(int stages)
{
    if (stages == (( 1 << S1 )|( 1 << S2 )|( 1 << S3 )))
      return "from stage 1";
    else if (stages == (( 1 << S2 )|( 1 << S3 )))
      return "from stage 2";
    else if (stages == (( 1 << S1 )|( 1 << S3 )))
      return "at stages 1 and 3";
    else if (stages == (( 1 << S1 )|( 1 << S2 )))
      return "at stages 1 and 2";
    else if (stages == (( 1 << S3 )))
      return "at stage 3";
    else if (stages == ( 1 << S1 ))
      return "at stage 1";
    else if (stages == ( 1 << S2 ))
      return "at stage 2";
    else
      return "nevertime";
}

/*
===============
UI_DrawNewProgressBar
===============
*/
static void UI_DrawNewProgressBar( rectDef_t *rect, vec4_t color,
                                vec4_t backColor, float scale, int align,
                                int textalign, int textStyle, float borderSize,
                                float progress )
{
  float   rimWidth;
  float   doneWidth, leftWidth;
  float   tx, ty;
  char    textBuffer[ 8 ];
  float   borderStyle[ 4 ];
  int     w, h;

  borderStyle[0] = BORDER_FOLD;
  borderStyle[1] = BORDER_FOLD;
  borderStyle[2] = BORDER_FOLD;
  borderStyle[3] = BORDER_FOLD;

  if( borderSize >= 0.0f )
    rimWidth = borderSize;
  else
  {
    rimWidth = rect->h / 20.0f;
    if( rimWidth < 0.6f )
      rimWidth = 0.6f;
  }

  if( progress < 0.0f )
    progress = 0.0f;
  else if( progress > 1.0f )
    progress = 1.0f;

  doneWidth = ( rect->w - (8 + 6) * rimWidth ) * progress + 6 * rimWidth;

  //draw rim and bar
  UI_DrawRoundedRect(rect->x, rect->y, rect->w, rect->h, rimWidth, borderStyle, color);
  UI_FillRoundedRect(
    rect->x + rimWidth * 4,
    rect->y + rimWidth * 4,
    doneWidth,
    rect->h - rimWidth * 8,
    rimWidth, borderStyle, backColor);


  //draw text
  if( scale > 0.0 )
  {
    Com_sprintf( textBuffer, sizeof( textBuffer ), "%d%%", (int)( progress * 100 ) );
    w = UI_Text_Width(textBuffer, scale);
    h = UI_Text_Height(textBuffer, scale);
    UI_Text_Paint( rect->x + (rect->w - w ) / 2.0, rect->y + h + ( rect->h - h ) / 2.0f, scale, color, textBuffer, 0, 0, textStyle );
  }
}

/*
===============
UI_DrawDownloadOverall
===============
*/
static void UI_DrawDownloadOverall( rectDef_t *rect, vec4_t color, vec4_t backColor, float scale,
                                    int align, int textalign, int textStyle,
                                    float borderSize )
{
  char downloadName[MAX_INFO_VALUE];
  int downloadSize, downloadCount, downloadTotal, downloadDone;

  trap_Cvar_VariableStringBuffer("cl_downloadName", downloadName, sizeof(downloadName));
  if (!*downloadName)
    return;

  downloadSize = trap_Cvar_VariableValue("cl_downloadSize");
  downloadCount = trap_Cvar_VariableValue("cl_downloadCount");
  downloadTotal = trap_Cvar_VariableValue("cl_downloadTotal");
  downloadDone = trap_Cvar_VariableValue("cl_downloadDone");

  UI_DrawNewProgressBar( rect, color, backColor, scale, align, textalign, textStyle, borderSize,
    (downloadSize ? ((float)downloadCount / downloadSize / downloadTotal) : 0) + (float)downloadDone / downloadTotal );
}

/*
===============
UI_DrawInfoPane
===============
*/
static void UI_DrawInfoPane(menuItem_t *item, rectDef_t *rect, float text_x, float text_y, float scale, int textalign,
    int textvalign, vec4_t color, int textStyle)
{
    int value = 0;
    const char *s = "";
    char *string = "";

    int class, credits;
    char ui_currentClass[MAX_STRING_CHARS];

    trap_Cvar_VariableStringBuffer("ui_currentClass", ui_currentClass, MAX_STRING_CHARS);

    sscanf(ui_currentClass, "%d %d", &class, &credits);

    switch (item->type)
    {
        case INFOTYPE_TEXT:
        case INFOTYPE_VOICECMD:
            s = item->v.text;
            break;

        case INFOTYPE_CLASS:
            value = (BG_ClassCanEvolveFromTo(class, item->v.pclass, credits, UI_GetCurrentAlienStage(), 0) +
                        ALIEN_CREDITS_PER_KILL - 1) /
                    ALIEN_CREDITS_PER_KILL;

            s = va("%s\n\n%s\nAvailable %s.%s",
                  BG_ClassConfig(item->v.pclass)->humanName,
                  BG_Class(item->v.pclass)->info,
                  UI_GetStageText(BG_Class(item->v.pclass)->stages),
                  (
                    (value > 0) ?
                      va("\n\nFrags: %d", value) :
                      ""
                  )
                );

            break;

        case INFOTYPE_WEAPON:
            value = BG_Weapon(item->v.weapon)->price;

            s = va("%s\n\n%s\nAvailable %s.\n\nCredits: %s",
                  BG_Weapon(item->v.weapon)->humanName,
                  BG_Weapon(item->v.weapon)->info,
                  UI_GetStageText(BG_Weapon(item->v.weapon)->stages),
                  (
                    (value > 0) ?
                      va("%d", value) :
                      "Free"
                  )
                );

            break;

        case INFOTYPE_UPGRADE:
            value = BG_Upgrade(item->v.upgrade)->price;

            s = va("%s\n\n%s\nAvailable %s.\n\nCredits: %s",
                  BG_Upgrade(item->v.upgrade)->humanName,
                  BG_Upgrade(item->v.upgrade)->info,
                  UI_GetStageText(BG_Upgrade(item->v.upgrade)->stages),
                  (
                    (value > 0) ?
                      va("%d", value) :
                      "Free"
                  )
                );

            break;

        case INFOTYPE_BUILDABLE:
            value = BG_Buildable(item->v.buildable)->buildPoints;

            switch (BG_Buildable(item->v.buildable)->team)
            {
                case TEAM_ALIENS:
                    string = "Sentience";
                    break;

                case TEAM_HUMANS:
                    string = "Power";
                    break;

                default:
                    break;
            }

            s = va("%s\n\n%s\nAvailable %s.%s",
                BG_Buildable(item->v.buildable)->humanName,
                BG_Buildable(item->v.buildable)->info,
                UI_GetStageText(BG_Buildable(item->v.buildable)->stages),
                (
                  (value > 0) ?
                    va("\n\n%s: %d", string, value) :
                    ""
                )
              );

            break;
    }

    UI_DrawTextBlock(rect, text_x, text_y, color, scale, textalign, textvalign, textStyle, s);
}

static void UI_PositionRotatedEntityOnTag( refEntity_t *entity, const refEntity_t *parent,
                                    qhandle_t parentModel, char *tagName )  // Imported from cg_ents.c
{
  int           i;
  orientation_t lerped;
  vec3_t        tempAxis[ 3 ];

//AxisClear( entity->axis );
  // lerp the tag
  uiInfo.uiDC.lerpTag( &lerped, parentModel, parent->oldframe, parent->frame,
                  1.0 - parent->backlerp, tagName );

  // FIXME: allow origin offsets along tag?
  VectorCopy( parent->origin, entity->origin );
  for( i = 0; i < 3; i++ )
    VectorMA( entity->origin, lerped.origin[ i ], parent->axis[ i ], entity->origin );

  // had to cast away the const to avoid compiler problems...
  MatrixMultiply( entity->axis, lerped.axis, tempAxis );
  MatrixMultiply( tempAxis, ( (refEntity_t *)parent )->axis, entity->axis );
}

static void UI_DrawInfoPaneModel(menuItemModel_t *model, rectDef_t *rect)
{
  float x, y, w, h;
  refdef_t refdef;
  refEntity_t ent[4];
  vec3_t tmpMins, tmpMaxs;
  vec3_t mins, maxs;
  vec3_t origin;
  vec3_t angles;
  float suggestedDist;
  int i;
  int millisPerDeg = 100;  // 36s = 1 turn
  float tmpFrame;

  if (model->assetCount == 0)
    return;

  // setup the refdef
  memset(&refdef, 0, sizeof(refdef));

  refdef.rdflags = RDF_NOWORLDMODEL;

  AxisClear(refdef.viewaxis);

  x = rect->x;
  y = rect->y;
  w = rect->w;
  h = rect->h;

  UI_AdjustFrom640(&x, &y, &w, &h);

  refdef.x = x;
  refdef.y = y;
  refdef.width = w;
  refdef.height = h;

  if (model->autoAdjust)
  {
    uiInfo.uiDC.modelBounds(model->asset[0], mins, maxs);
    for (i = 1; i < model->assetCount; i++)  // Maybe there are already a function for this ?
    {
      uiInfo.uiDC.modelBounds(model->asset[i], tmpMins, tmpMaxs);
      if (tmpMins[0] < mins[0])
        mins[0] = tmpMins[0];
      if (tmpMins[1] < mins[1])
        mins[1] = tmpMins[1];
      if (tmpMins[2] < mins[2])
        mins[2] = tmpMins[2];
      if (tmpMaxs[0] > maxs[0])
        maxs[0] = tmpMaxs[0];
      if (tmpMaxs[1] > maxs[1])
        maxs[1] = tmpMaxs[1];
      if (tmpMaxs[2] > maxs[2])
        maxs[2] = tmpMaxs[2];
    }

    origin[1] = 0.5 * (mins[1] + maxs[1]);
    suggestedDist = ((0.5 * (maxs[2]*model->scale - mins[2]*model->scale)) / 0.268); // len / tan( fov/2 )

    if (model->forceCentering)
    {
      origin[0] = suggestedDist * 2.0;
      origin[2] = -0.5 * (mins[2] + maxs[2]);
    }
    else
    {
      origin[0] = suggestedDist * 0.5 + model->cameraDist * 0.5;
      origin[2] = model->zOffset * 0.2 + (-0.5 * (mins[2] + maxs[2])) * 0.8;
    }
  }
  else
  {
    origin[0] = model->cameraDist;
    origin[1] = 0;
    origin[2] = model->zOffset;
  }


  refdef.fov_x = (int)((float)refdef.width / 640.0f * 90.0f);
  refdef.fov_y = atan2(
    refdef.height,
    refdef.width / tan( refdef.fov_x / 360 * M_PI )
  );
  refdef.fov_y *= ( 360 / M_PI );

  refdef.fov_x *= 2;
  refdef.fov_y *= 2;

  uiInfo.uiDC.clearScene();

  refdef.time = uiInfo.uiDC.realTime;

  // add the model

  VectorSet(angles, 0,
    (
      (float)(uiInfo.uiDC.realTime % (360 * millisPerDeg)) / (float)(millisPerDeg)
    ), 0);

  for (i = 0; i < model->assetCount; i++)
  {
    memset(&(ent[i]), 0, sizeof(ent[i]));

    if( model->scale != 1.0f )
    {
      VectorScale( ent[i].axis[ 0 ], model->scale, ent[i].axis[ 0 ] );
      VectorScale( ent[i].axis[ 1 ], model->scale, ent[i].axis[ 1 ] );
      VectorScale( ent[i].axis[ 2 ], model->scale, ent[i].axis[ 2 ] );

      ent[i].nonNormalizedAxes = qtrue;
    }
    else
      ent[i].nonNormalizedAxes = qfalse;

    ent[i].hModel = model->asset[i];
    if (model->skin[i])
      ent[i].customSkin = model->skin[i];
    if (model->frame[i] == -1)
    {
      // Animate
      tmpFrame = (uiInfo.uiDC.realTime * model->animationFPS[i]) / 1000;
      ent[i].backlerp = 1.0f - (tmpFrame - floor(tmpFrame));
      tmpFrame = (int)tmpFrame % (model->animation[i][1] - model->animation[i][0])
                  + model->animation[i][0];
      ent[i].frame = tmpFrame + 1;
      ent[i].oldframe = tmpFrame;
    }
    else
    {
      // Static
      ent[i].frame = model->frame[i];
      ent[i].oldframe = model->frame[i];
    }
    if (i && strlen(model->parent[i - 1].parentTagName))
    {
      AxisClear(ent[i].axis);
      UI_PositionRotatedEntityOnTag(&(ent[i]), &(ent[model->parent[i - 1].parentIndex]), model->asset[model->parent[i - 1].parentIndex], model->parent[i - 1].parentTagName);
    }
    else
    {
      VectorCopy(origin, ent[i].origin);
      VectorCopy(origin, ent[i].lightingOrigin);
      VectorCopy(ent[i].origin, ent[i].oldorigin);
      AnglesToAxis(angles, ent[i].axis);
    }

    ent[i].renderfx = RF_LIGHTING_ORIGIN | RF_NOSHADOW;
    uiInfo.uiDC.addRefEntityToScene(&(ent[i]));
  }
  uiInfo.uiDC.renderScene(&refdef);
}

static void UI_DrawServerMapPreview(rectDef_t *rect, float scale, vec4_t color)
{
    if (uiInfo.serverStatus.currentServerCinematic >= 0)
    {
        trap_CIN_RunCinematic(uiInfo.serverStatus.currentServerCinematic);
        trap_CIN_SetExtents(uiInfo.serverStatus.currentServerCinematic, rect->x, rect->y, rect->w, rect->h);
        trap_CIN_DrawCinematic(uiInfo.serverStatus.currentServerCinematic);
    }
    else if (uiInfo.serverStatus.currentServerPreview > 0)
        UI_DrawHandlePic(rect->x, rect->y, rect->w, rect->h, uiInfo.serverStatus.currentServerPreview);
    else
        UI_DrawHandlePic(rect->x, rect->y, rect->w, rect->h, trap_R_RegisterShaderNoMip("gfx/2d/load_screen"));
}

static void UI_DrawSelectedMapPreview(rectDef_t *rect, float scale, vec4_t color)
{
    int map = ui_selectedMap.integer;

    if (map < 0 || map > uiInfo.mapCount)
    {
        ui_selectedMap.integer = 0;
        trap_Cvar_Set("ui_selectedMap", "0");
        map = 0;
    }

    if (uiInfo.mapList[map].cinematic >= -1)
    {
        if (uiInfo.mapList[map].cinematic == -1)
            uiInfo.mapList[map].cinematic = trap_CIN_PlayCinematic(
                va("%s.roq", uiInfo.mapList[map].mapLoadName), 0, 0, 0, 0, (CIN_loop | CIN_silent));

        if (uiInfo.mapList[map].cinematic >= 0)
        {
            trap_CIN_RunCinematic(uiInfo.mapList[map].cinematic);
            trap_CIN_SetExtents(uiInfo.mapList[map].cinematic, rect->x, rect->y, rect->w, rect->h);
            trap_CIN_DrawCinematic(uiInfo.mapList[map].cinematic);
        }
        else
            uiInfo.mapList[map].cinematic = -2;
    }
    else
    {
        if (uiInfo.mapList[map].levelShot == -1)
            uiInfo.mapList[map].levelShot = trap_R_RegisterShaderNoMip(uiInfo.mapList[map].imageName);

        if (uiInfo.mapList[map].levelShot > 0)
            UI_DrawHandlePic(rect->x, rect->y, rect->w, rect->h, uiInfo.mapList[map].levelShot);
        else
            UI_DrawHandlePic(rect->x, rect->y, rect->w, rect->h, trap_R_RegisterShaderNoMip("gfx/2d/load_screen"));
    }
}

static void UI_DrawSelectedMapName(rectDef_t *rect, float scale, vec4_t color, int textStyle)
{
    int map = ui_selectedMap.integer;

    if (map >= 0 && map < uiInfo.mapCount)
        UI_Text_Paint(rect->x, rect->y, scale, color, uiInfo.mapList[map].mapName, 0, 0, textStyle);
}

static const char *UI_OwnerDrawText(int ownerDraw)
{
    const char *s = NULL;

    switch (ownerDraw)
    {
        case UI_NETSOURCE:
            if (ui_netSource.integer < 0 || ui_netSource.integer >= numNetSources)
                ui_netSource.integer = 0;

            s = netSources[ui_netSource.integer];
            break;

        case UI_KEYBINDSTATUS:
            if (Display_KeyBindPending())
                s = "Waiting for new key... Press ESCAPE to cancel";
            else
                s = "Press ENTER or CLICK to change, Press BACKSPACE to clear";

            break;

        case UI_SERVERREFRESHDATE:
            if (uiInfo.serverStatus.refreshActive)
            {
#define MAX_DOTS 5
                int numServers = trap_LAN_GetServerCount(ui_netSource.integer);
                int numDots = (uiInfo.uiDC.realTime / 500) % (MAX_DOTS + 1);
                char dots[MAX_DOTS + 1];
                int i;

                for (i = 0; i < numDots; i++)
                    dots[i] = '.';

                dots[i] = '\0';

                s = numServers < 0 ? va("Waiting for response%s", dots)
                                   : va("Getting info for %d servers (ESC to cancel)%s", numServers, dots);
            }
            else
                s = va("Refresh Time: %s", UI_Cvar_VariableString(va("ui_lastServerRefresh_%i", ui_netSource.integer)));

            break;

        case UI_SERVERMOTD:
            s = uiInfo.serverStatus.motd;
            break;

        default:
            break;
    }

    return s;
}

static int UI_OwnerDrawWidth(int ownerDraw, float scale)
{
    const char *s = NULL;

    switch (ownerDraw)
    {
        case UI_NETSOURCE:
        case UI_KEYBINDSTATUS:
        case UI_SERVERREFRESHDATE:
        case UI_SERVERMOTD:
            s = UI_OwnerDrawText(ownerDraw);
            break;

        default:
            break;
    }

    if (s)
        return UI_Text_Width(s, scale);

    return 0;
}

/*
===============
UI_BuildPlayerList
===============
*/
static void UI_BuildPlayerList(void)
{
    uiClientState_t cs;
    int n, count, team, team2;
    char info[MAX_INFO_STRING];

    trap_GetClientState(&cs);
    trap_GetConfigString(CS_PLAYERS + cs.clientNum, info, MAX_INFO_STRING);
    uiInfo.playerNumber = cs.clientNum;
    team = atoi(Info_ValueForKey(info, "t"));
    trap_GetConfigString(CS_SERVERINFO, info, sizeof(info));
    count = atoi(Info_ValueForKey(info, "sv_maxclients"));
    uiInfo.playerCount = 0;
    uiInfo.myTeamCount = 0;
    uiInfo.myPlayerIndex = 0;

    for (n = 0; n < count; n++)
    {
        trap_GetConfigString(CS_PLAYERS + n, info, MAX_INFO_STRING);

        if (info[0])
        {
            Com_ClientListParse(&uiInfo.ignoreList[uiInfo.playerCount], Info_ValueForKey(info, "ig"));
            Q_strncpyz(uiInfo.rawPlayerNames[uiInfo.playerCount], Info_ValueForKey(info, "n"), MAX_COLORFUL_NAME_LENGTH);
            Q_strncpyz(uiInfo.playerNames[uiInfo.playerCount], Info_ValueForKey(info, "n"), MAX_COLORFUL_NAME_LENGTH);
            Q_CleanStr(uiInfo.playerNames[uiInfo.playerCount]);
            uiInfo.clientNums[uiInfo.playerCount] = n;

            if (n == uiInfo.playerNumber)
                uiInfo.myPlayerIndex = uiInfo.playerCount;

            uiInfo.playerCount++;

            team2 = atoi(Info_ValueForKey(info, "t"));

            if (team2 == team)
            {
                Q_strncpyz(uiInfo.rawTeamNames[uiInfo.myTeamCount], Info_ValueForKey(info, "n"), MAX_COLORFUL_NAME_LENGTH);
                Q_strncpyz(uiInfo.teamNames[uiInfo.myTeamCount], Info_ValueForKey(info, "n"), MAX_COLORFUL_NAME_LENGTH);
                Q_CleanStr(uiInfo.teamNames[uiInfo.myTeamCount]);
                uiInfo.teamClientNums[uiInfo.myTeamCount] = n;

                uiInfo.myTeamCount++;
            }
        }
    }
}

static void UI_DrawGLInfo(rectDef_t *rect, float scale, int textalign, int textvalign, vec4_t color, int textStyle,
    float text_x, float text_y)
{
    char buffer[8192];

    Com_sprintf(buffer, sizeof(buffer),
        "VENDOR: %s\nVERSION: %s\n"
        "PIXELFORMAT: color(%d-bits) Z(%d-bits) stencil(%d-bits)\n%s",
        uiInfo.uiDC.glconfig.vendor_string, uiInfo.uiDC.glconfig.renderer_string, uiInfo.uiDC.glconfig.colorBits,
        uiInfo.uiDC.glconfig.depthBits, uiInfo.uiDC.glconfig.stencilBits, uiInfo.uiDC.glconfig.extensions_string);

    UI_DrawTextBlock(rect, text_x, text_y, color, scale, textalign, textvalign, textStyle, buffer);
}

// FIXME: table drive
//
static void UI_OwnerDraw(float x, float y, float w, float h, float text_x, float text_y, int ownerDraw,
    int ownerDrawFlags, int align, int textalign, int textvalign, float borderSize, float scale, vec4_t foreColor,
    vec4_t backColor, qhandle_t shader, int textStyle)
{
    rectDef_t rect;

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    switch (ownerDraw)
    {
        case UI_DOWNLOAD_OVERALL:
            UI_DrawDownloadOverall(&rect, foreColor, backColor, scale, align, textalign,
                textStyle, borderSize );
            break;

        case UI_TEAMINFOPANE:
            UI_DrawInfoPane(&uiInfo.teamList[uiInfo.teamIndex], &rect, text_x, text_y, scale, textalign, textvalign,
                foreColor, textStyle);
            break;

        case UI_TEAMINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.teamListModel[uiInfo.teamIndex], &rect);
            break;

        case UI_VOICECMDINFOPANE:
            UI_DrawInfoPane(&uiInfo.voiceCmdList[uiInfo.voiceCmdIndex], &rect, text_x, text_y, scale, textalign,
                textvalign, foreColor, textStyle);
            break;

        case UI_ACLASSINFOPANE:
            UI_DrawInfoPane(&uiInfo.alienClassList[uiInfo.alienClassIndex], &rect, text_x, text_y, scale, textalign,
                textvalign, foreColor, textStyle);
            break;

        case UI_ACLASSINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.alienClassListModel[uiInfo.alienClassIndex], &rect);
            break;

        case UI_AUPGRADEINFOPANE:
            UI_DrawInfoPane(&uiInfo.alienUpgradeList[uiInfo.alienUpgradeIndex], &rect, text_x, text_y, scale, textalign,
                textvalign, foreColor, textStyle);
            break;

        case UI_AUPGRADEINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.alienUpgradeListModel[
                uiInfo.alienUpgradeList[uiInfo.alienUpgradeIndex].v.pclass
              ], &rect);
            break;

        case UI_HITEMINFOPANE:
            UI_DrawInfoPane(&uiInfo.humanItemList[uiInfo.humanItemIndex], &rect, text_x, text_y, scale, textalign,
                textvalign, foreColor, textStyle);
            break;

        case UI_HITEMINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.humanItemListModel[uiInfo.humanItemIndex], &rect);
            break;

        case UI_HBUYINFOPANE:
            UI_DrawInfoPane(&uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex], &rect, text_x, text_y, scale,
                textalign, textvalign, foreColor, textStyle);
            break;

        case UI_HBUYINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.humanArmouryBuyListModel[
                uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].type == INFOTYPE_WEAPON ?
                  uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].v.weapon :
                  (uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].v.upgrade + WP_NUM_WEAPONS)
              ], &rect);
            break;

        case UI_HSELLINFOPANE:
            UI_DrawInfoPane(&uiInfo.humanArmourySellList[uiInfo.humanArmourySellIndex], &rect, text_x, text_y, scale,
                textalign, textvalign, foreColor, textStyle);
            break;

        case UI_ABUILDINFOPANE:
            UI_DrawInfoPane(&uiInfo.alienBuildList[uiInfo.alienBuildIndex], &rect, text_x, text_y, scale, textalign,
                textvalign, foreColor, textStyle);
            break;

        case UI_ABUILDINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.alienBuildListModel[
                uiInfo.alienBuildList[uiInfo.alienBuildIndex].v.buildable
              ], &rect);
            break;

        case UI_HBUILDINFOPANE:
            UI_DrawInfoPane(&uiInfo.humanBuildList[uiInfo.humanBuildIndex], &rect, text_x, text_y, scale, textalign,
                textvalign, foreColor, textStyle);
            break;
            break;

        case UI_HBUILDINFOPANEMODEL:
            UI_DrawInfoPaneModel(&uiInfo.humanBuildListModel[
                uiInfo.humanBuildList[uiInfo.humanBuildIndex].v.buildable
              ], &rect);
            break;

        case UI_HELPINFOPANE:
            UI_DrawInfoPane(&uiInfo.helpList[uiInfo.helpIndex], &rect, text_x, text_y, scale, textalign, textvalign,
                foreColor, textStyle);
            break;

        case UI_NETMAPPREVIEW:
            UI_DrawServerMapPreview(&rect, scale, foreColor);
            break;

        case UI_SELECTEDMAPPREVIEW:
            UI_DrawSelectedMapPreview(&rect, scale, foreColor);
            break;

        case UI_SELECTEDMAPNAME:
            UI_DrawSelectedMapName(&rect, scale, foreColor, textStyle);
            break;

        case UI_GLINFO:
            UI_DrawGLInfo(&rect, scale, textalign, textvalign, foreColor, textStyle, text_x, text_y);
            break;

        default:
            break;
    }
}

static qboolean UI_OwnerDrawVisible(int flags)
{
    qboolean vis = qtrue;
    uiClientState_t cs;
    team_t team;
    char info[MAX_INFO_STRING];
    trap_GetClientState(&cs);
    trap_GetConfigString(CS_PLAYERS + cs.clientNum, info, MAX_INFO_STRING);
    team = atoi(Info_ValueForKey(info, "t"));

    while (flags)
    {
        if (flags & UI_SHOW_NOTSPECTATING)
        {
            if (team == TEAM_NONE)
                vis = qfalse;

            flags &= ~UI_SHOW_NOTSPECTATING;
        }

        if (flags & UI_SHOW_VOTEACTIVE)
        {
            if (!trap_Cvar_VariableValue("ui_voteActive"))
                vis = qfalse;

            flags &= ~UI_SHOW_VOTEACTIVE;
        }

        if (flags & UI_SHOW_CANVOTE)
        {
            if (trap_Cvar_VariableValue("ui_voteActive"))
                vis = qfalse;

            flags &= ~UI_SHOW_CANVOTE;
        }

        if (flags & UI_SHOW_TEAMVOTEACTIVE)
        {
            if (team == TEAM_ALIENS)
            {
                if (!trap_Cvar_VariableValue("ui_alienTeamVoteActive"))
                    vis = qfalse;
            }
            else if (team == TEAM_HUMANS)
            {
                if (!trap_Cvar_VariableValue("ui_humanTeamVoteActive"))
                    vis = qfalse;
            }

            flags &= ~UI_SHOW_TEAMVOTEACTIVE;
        }

        if (flags & UI_SHOW_CANTEAMVOTE)
        {
            if (team == TEAM_ALIENS)
            {
                if (trap_Cvar_VariableValue("ui_alienTeamVoteActive"))
                    vis = qfalse;
            }
            else if (team == TEAM_HUMANS)
            {
                if (trap_Cvar_VariableValue("ui_humanTeamVoteActive"))
                    vis = qfalse;
            }

            flags &= ~UI_SHOW_CANTEAMVOTE;
        }

        if (flags & UI_SHOW_FAVORITESERVERS)
        {
            // this assumes you only put this type of display flag on something showing in the proper context

            if (ui_netSource.integer != AS_FAVORITES)
                vis = qfalse;

            flags &= ~UI_SHOW_FAVORITESERVERS;
        }

        if (flags & UI_SHOW_NOTFAVORITESERVERS)
        {
            // this assumes you only put this type of display flag on something showing in the proper context

            if (ui_netSource.integer == AS_FAVORITES)
                vis = qfalse;

            flags &= ~UI_SHOW_NOTFAVORITESERVERS;
        }
        else
            flags = 0;
    }

    return vis;
}

static qboolean UI_NetSource_HandleKey(int key)
{
    Com_Printf("NetSource HandleKey\n");
    if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_ENTER || key == K_KP_ENTER)
    {
        if (key == K_MOUSE2)
        {
            ui_netSource.integer--;

            if (ui_netSource.integer == AS_MPLAYER)
                ui_netSource.integer--;
        }
        else
        {
            ui_netSource.integer++;

            if (ui_netSource.integer == AS_MPLAYER)
                ui_netSource.integer++;
        }

        if (ui_netSource.integer < 0)
            ui_netSource.integer = numNetSources - 1;
        else if (ui_netSource.integer >= numNetSources)
            ui_netSource.integer = 0;

        UI_BuildServerDisplayList(qtrue);

        if (ui_netSource.integer != AS_GLOBAL)
            UI_StartServerRefresh(qtrue);

        trap_Cvar_Set("ui_netSource", va("%d", ui_netSource.integer));
        return qtrue;
    }

    return qfalse;
}

static qboolean UI_OwnerDrawHandleKey(int ownerDraw, int key)
{
    Com_Printf("UI_OwnerDrawHandleKey %d %d %d\n", ownerDraw, key, UI_NETSOURCE);
    switch (ownerDraw)
    {
        case UI_NETSOURCE:
            UI_NetSource_HandleKey(key);
            break;

        default:
            break;
    }

    return qfalse;
}

/*
=================
UI_ServersQsortCompare
=================
*/
static int QDECL UI_ServersQsortCompare(const void *arg1, const void *arg2)
{
    return trap_LAN_CompareServers(
        ui_netSource.integer, uiInfo.serverStatus.sortKey, uiInfo.serverStatus.sortDir, *(int *)arg1, *(int *)arg2);
}

/*
=================
UI_ServersSort
=================
*/
void UI_ServersSort(int column, qboolean force)
{
    if (!force)
    {
        if (uiInfo.serverStatus.sortKey == column)
            return;
    }

    uiInfo.serverStatus.sortKey = column;
    qsort(&uiInfo.serverStatus.displayServers[0], uiInfo.serverStatus.numDisplayServers, sizeof(int),
        UI_ServersQsortCompare);
}

/*
===============
UI_LoadTeams
===============
*/
static void UI_LoadTeams(void)
{
    class_t alienPreviewClass = PCL_ALIEN_LEVEL0;

    memset(&(uiInfo.teamListModel), 0, sizeof(uiInfo.teamListModel));
    uiInfo.teamCount = 4;

    uiInfo.teamList[0].text = "Aliens";
    uiInfo.teamList[0].cmd = "cmd team aliens\n";
    uiInfo.teamList[0].type = INFOTYPE_TEXT;
    uiInfo.teamList[0].v.text =
        "The Alien Team\n\n"
        "The Aliens' strengths are in movement and the ability to "
        "quickly construct new bases quickly. They possess a range "
        "of abilities including basic melee attacks, movement-"
        "crippling poisons and more.";

    uiInfo.teamListModel[0].asset[0] = uiInfo.uiDC.registerModel(va("models/players/%s/nonseg.md3", BG_ClassConfig(alienPreviewClass)->modelName));
    uiInfo.teamListModel[0].assetCount = 1;
    uiInfo.teamListModel[0].skin[0] = uiInfo.uiDC.registerSkin(va("models/players/%s/nonseg_%s.skin", BG_ClassConfig(alienPreviewClass)->modelName, BG_ClassConfig(alienPreviewClass)->skinName));
    uiInfo.teamListModel[0].scale = BG_ClassConfig(alienPreviewClass)->modelScale;
    uiInfo.teamListModel[0].zOffset = BG_ClassConfig(alienPreviewClass)->zOffset;
    uiInfo.teamListModel[0].cameraDist = 100;
    uiInfo.teamListModel[0].autoAdjust = qtrue;

    uiInfo.teamList[1].text = "Humans";
    uiInfo.teamList[1].cmd = "cmd team humans\n";
    uiInfo.teamList[1].type = INFOTYPE_TEXT;
    uiInfo.teamList[1].v.text =
        "The Human Team\n\n"
        "The humans are the masters of technology. Although their "
        "bases take long to construct, their automated defense "
        "ensures they stay built. A wide range of upgrades and "
        "weapons are available to the humans, each contributing "
        "to eradicate the alien threat.";
    uiInfo.teamListModel[1].assetCount = 4;
    uiInfo.teamListModel[1].asset[0] = uiInfo.uiDC.registerModel("models/players/human_base/lower.md3");
    uiInfo.teamListModel[1].asset[1] = uiInfo.uiDC.registerModel("models/players/human_base/upper.md3");
    uiInfo.teamListModel[1].asset[2] = uiInfo.uiDC.registerModel("models/players/human_base/head.md3");
    uiInfo.teamListModel[1].asset[3] = uiInfo.uiDC.registerModel("models/weapons/rifle/rifle.md3");
    uiInfo.teamListModel[1].skin[0] = uiInfo.uiDC.registerSkin("models/players/human_base/lower_default.skin");
    uiInfo.teamListModel[1].skin[1] = uiInfo.uiDC.registerSkin("models/players/human_base/upper_default.skin");
    uiInfo.teamListModel[1].skin[2] = uiInfo.uiDC.registerSkin("models/players/human_base/head_default.skin");
    uiInfo.teamListModel[1].frame[0] = 157;
    uiInfo.teamListModel[1].frame[1] = 151;
    uiInfo.teamListModel[1].parent[0].parentTagName = "tag_torso";
    // uiInfo.teamListModel[1].parent[0].parentIndex = 0;
    uiInfo.teamListModel[1].parent[1].parentTagName = "tag_head";
    uiInfo.teamListModel[1].parent[1].parentIndex = 1;
    uiInfo.teamListModel[1].parent[2].parentTagName = "tag_weapon";
    uiInfo.teamListModel[1].parent[2].parentIndex = 1;
    uiInfo.teamListModel[1].cameraDist = 75;
    uiInfo.teamListModel[1].zOffset = -16;
    uiInfo.teamListModel[1].scale = 1.0;

    uiInfo.teamList[2].text = "Spectate";
    uiInfo.teamList[2].cmd = "cmd team spectate\n";
    uiInfo.teamList[2].type = INFOTYPE_TEXT;
    uiInfo.teamList[2].v.text = "Watch the game without playing.";

    uiInfo.teamList[3].text = "Auto select";
    uiInfo.teamList[3].cmd = "cmd team auto\n";
    uiInfo.teamList[3].type = INFOTYPE_TEXT;
    uiInfo.teamList[3].v.text = "Join the team with the least players.";
}

/*
===============
UI_AddClass
===============
*/

static void UI_AddClass(class_t class, char *prefix)
{
    uiInfo.alienClassList[uiInfo.alienClassCount].text = String_Alloc(va("%s%s", prefix, BG_ClassConfig(class)->humanName));
    uiInfo.alienClassList[uiInfo.alienClassCount].cmd = String_Alloc(va("cmd class %s\n", BG_Class(class)->name));
    uiInfo.alienClassList[uiInfo.alienClassCount].type = INFOTYPE_CLASS;

    uiInfo.alienClassList[uiInfo.alienClassCount].v.pclass = class;

    uiInfo.alienClassListModel[uiInfo.alienClassCount].asset[0] = uiInfo.uiDC.registerModel(va("models/players/%s/nonseg.md3", BG_ClassConfig(class)->modelName));
    uiInfo.alienClassListModel[uiInfo.alienClassCount].assetCount = 1;
    uiInfo.alienClassListModel[uiInfo.alienClassCount].skin[0] = uiInfo.uiDC.registerSkin(va("models/players/%s/nonseg_%s.skin", BG_ClassConfig(class)->modelName, BG_ClassConfig(class)->skinName));
    uiInfo.alienClassListModel[uiInfo.alienClassCount].scale = BG_ClassConfig(class)->modelScale;
    uiInfo.alienClassListModel[uiInfo.alienClassCount].zOffset = BG_ClassConfig(class)->zOffset;
    uiInfo.alienClassListModel[uiInfo.alienClassCount].cameraDist = 100;
    uiInfo.alienClassListModel[uiInfo.alienClassCount].autoAdjust = qtrue;

    uiInfo.alienClassCount++;
}

/*
===============
UI_LoadAlienClasses
===============
*/
static void UI_LoadAlienClasses(void)
{
    alienStates_t state;
    stage_t       stage;
    char          *prefix;

    memset(&(uiInfo.alienClassListModel), 0, sizeof(uiInfo.alienClassListModel));
    uiInfo.alienClassCount = 0;
    UI_alienStates(&state);
    stage = UI_GetCurrentAlienStage();

    if ( ( !state.omHealth || !state.spawns || BG_BuildableAllowedInStage(BA_A_BOOSTER, stage) && !state.boosters )
          && !state.builders )
        prefix = "[!] ";
    else
        prefix = "";

    if (BG_ClassIsAllowed(PCL_ALIEN_LEVEL0))
        UI_AddClass(PCL_ALIEN_LEVEL0, "");

    if (BG_ClassIsAllowed(PCL_ALIEN_BUILDER0_UPG) &&
        BG_ClassAllowedInStage(PCL_ALIEN_BUILDER0_UPG, UI_GetCurrentAlienStage()))
        UI_AddClass(PCL_ALIEN_BUILDER0_UPG, prefix);
    else if (BG_ClassIsAllowed(PCL_ALIEN_BUILDER0))
        UI_AddClass(PCL_ALIEN_BUILDER0, prefix);
}

/*
===============
UI_AddItem
===============
*/
static void UI_AddItem(weapon_t weapon, char *prefix)
{
    uiInfo.humanItemList[uiInfo.humanItemCount].text = String_Alloc(va("%s%s", prefix, BG_Weapon(weapon)->humanName));
    uiInfo.humanItemList[uiInfo.humanItemCount].cmd = String_Alloc(va("cmd class %s\n", BG_Weapon(weapon)->name));
    uiInfo.humanItemList[uiInfo.humanItemCount].type = INFOTYPE_WEAPON;
    uiInfo.humanItemList[uiInfo.humanItemCount].v.weapon = weapon;

    uiInfo.humanItemListModel[uiInfo.humanItemCount].asset[0] = uiInfo.uiDC.registerModel(va("models/weapons/%s/%s.md3", BG_Weapon(weapon)->name, BG_Weapon(weapon)->name));
    uiInfo.humanItemListModel[uiInfo.humanItemCount].assetCount = 1;
    uiInfo.humanItemListModel[uiInfo.humanItemCount].scale = 1.0;
    uiInfo.humanItemListModel[uiInfo.humanItemCount].cameraDist = 0.0;
    uiInfo.humanItemListModel[uiInfo.humanItemCount].autoAdjust = qtrue;
    uiInfo.humanItemListModel[uiInfo.humanItemCount].forceCentering = qtrue;

    uiInfo.humanItemCount++;
}

/*
===============
UI_LoadHumanItems
===============
*/
static void UI_LoadHumanItems(void)
{
    humanStates_t state;
    stage_t       stage;
    char          *prefix;

    memset(&(uiInfo.humanItemListModel), 0, sizeof(uiInfo.humanItemListModel));
    uiInfo.humanItemCount = 0;
    UI_humanStates(&state);
    stage = UI_GetCurrentHumanStage();

    if ( ( !state.rcHealth || !state.spawns || BG_BuildableAllowedInStage(BA_H_ARMOURY, stage) && !state.armourys
            || BG_BuildableAllowedInStage(BA_H_MEDISTAT, stage) && !state.medicals
            || BG_BuildableAllowedInStage(BA_H_DCC, stage) && !state.computers )
          && !state.builders )
        prefix = "[!] ";
    else
        prefix = "";

    if (BG_WeaponIsAllowed(WP_MACHINEGUN))
        UI_AddItem(WP_MACHINEGUN, "");

    if (BG_WeaponIsAllowed(WP_HBUILD))
        UI_AddItem(WP_HBUILD, prefix);
}

/*
===============
UI_ParseCarriageList
===============
*/
static void UI_ParseCarriageList(void)
{
    int i;
    char carriageCvar[MAX_TOKEN_CHARS];
    char *iterator;
    char buffer[MAX_TOKEN_CHARS];
    char *bufPointer;

    trap_Cvar_VariableStringBuffer("ui_carriage", carriageCvar, sizeof(carriageCvar));
    iterator = carriageCvar;

    uiInfo.weapons = 0;
    uiInfo.upgrades = 0;

    // simple parser to give rise to weapon/upgrade list

    while (iterator && iterator[0] != '$')
    {
        bufPointer = buffer;

        if (iterator[0] == 'W')
        {
            iterator++;

            while (iterator[0] != ' ')
                *bufPointer++ = *iterator++;

            *bufPointer++ = '\n';

            i = atoi(buffer);

            uiInfo.weapons |= (1 << i);
        }
        else if (iterator[0] == 'U')
        {
            iterator++;

            while (iterator[0] != ' ')
                *bufPointer++ = *iterator++;

            *bufPointer++ = '\n';

            i = atoi(buffer);

            uiInfo.upgrades |= (1 << i);
        }

        iterator++;
    }
}

/*
===============
UI_GetCurrentCredits
===============
*/
static int UI_GetCurrentCredits(void)
{
  char creditCvar[MAX_TOKEN_CHARS];

  trap_Cvar_VariableStringBuffer("ui_credit", creditCvar, sizeof(creditCvar));
  return (atoi(creditCvar));
}

/*
===============
UI_GetConflictingUpgradeBudget

Calculate a reduction of what will be sold when buying the upgrade
===============
*/
static int UI_GetConflictingUpgradeBudget( upgrade_t upgrade )
{
  int budget = 0;
  int i;
  int refSlots = BG_Upgrade(upgrade)->slots;

  for (i = UP_NONE + 1; i < UP_NUM_UPGRADES; i++)
      if (uiInfo.upgrades & (1 << i) && BG_Upgrade(i)->slots & refSlots)
          budget += BG_Upgrade(i)->price;

  return (budget);
}

/*
===============
UI_IsAmmoFull
===============
*/
static qboolean UI_IsAmmoFull(void)
{
  char ammoFullCvar[MAX_TOKEN_CHARS];

  trap_Cvar_VariableStringBuffer("ui_ammoFull", ammoFullCvar, sizeof(ammoFullCvar));
  return (atoi(ammoFullCvar));
}

/*
===============
UI_CanUpgradeToWeapon
===============
*/
static qboolean UI_CanUpgradeToWeapon(weapon_t weapon, int sellingBudget, int credits)
{
  return (BG_Weapon(weapon)->price <= sellingBudget + credits);
}

/*
===============
UI_IsBetterWeapon
===============
*/
static qboolean UI_IsBetterWeapon(weapon_t weapon, int sellingBudget)
{
  return (BG_Weapon(weapon)->price > sellingBudget);
}

/*
===============
UI_CanGotUpgrade
===============
*/
static qboolean UI_CanGotUpgrade(upgrade_t upgrade, int credits)
{
  credits += UI_GetConflictingUpgradeBudget( upgrade );
  return (BG_Upgrade(upgrade)->price <= credits);
}

/*
===============
UI_IsUpgradeBetter
===============
*/
static qboolean UI_IsUpgradeBetter(upgrade_t upgrade, int slots)
{
  return (!(BG_Upgrade(upgrade)->slots & slots) || upgrade == UP_BATTLESUIT);
}

/*
===============
UI_LoadHumanArmouryModels
===============
*/
static void UI_LoadHumanArmouryModels(void)
{
    int i, j = 0;

    memset(&(uiInfo.humanArmouryBuyListModel), 0, sizeof(uiInfo.humanArmouryBuyListModel));
    for (i = WP_NONE + 1; i < WP_NUM_WEAPONS; i++)
    {
        uiInfo.humanArmouryBuyListModel[i].assetCount = 0;
        if (BG_Weapon(i)->team == TEAM_HUMANS && BG_Weapon(i)->purchasable)
        {
            uiInfo.humanArmouryBuyListModel[i].asset[0] = uiInfo.uiDC.registerModel(va("models/weapons/%s/%s.md3", BG_Weapon(i)->name, BG_Weapon(i)->name));
            uiInfo.humanArmouryBuyListModel[i].assetCount = 1;
            uiInfo.humanArmouryBuyListModel[i].scale = 1.0;
            uiInfo.humanArmouryBuyListModel[i].autoAdjust = qtrue;
            uiInfo.humanArmouryBuyListModel[i].forceCentering = qtrue;
        }
    }
    for (i = UP_NONE + 1; i < UP_NUM_UPGRADES; i++)
    {
        j = WP_NUM_WEAPONS + i;
        uiInfo.humanArmouryBuyListModel[j].assetCount = 0;

        if (BG_Upgrade(i)->team == TEAM_HUMANS && BG_Upgrade(i)->purchasable)
        {
            uiInfo.humanArmouryBuyListModel[j].assetCount = 1;
            uiInfo.humanArmouryBuyListModel[j].scale = 1.0;
            switch (i) {
              case UP_LIGHTARMOUR:
                uiInfo.humanArmouryBuyListModel[j].assetCount = 3;
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/players/human_base/lower.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[1] = uiInfo.uiDC.registerModel("models/players/human_base/upper.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[2] = uiInfo.uiDC.registerModel("models/players/human_base/head.md3");
                uiInfo.humanArmouryBuyListModel[j].skin[0] = uiInfo.uiDC.registerSkin("models/players/human_base/lower_light.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[1] = uiInfo.uiDC.registerSkin("models/players/human_base/upper_light.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[2] = uiInfo.uiDC.registerSkin("models/players/human_base/head_default.skin");
                uiInfo.humanArmouryBuyListModel[j].frame[0] = 157;
                uiInfo.humanArmouryBuyListModel[j].frame[1] = 151;
                uiInfo.humanArmouryBuyListModel[j].parent[0].parentTagName = "tag_torso";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 40;
                uiInfo.humanArmouryBuyListModel[j].zOffset = -15;
                break;
              case UP_HELMET:
                uiInfo.humanArmouryBuyListModel[j].assetCount = 3;
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/players/human_base/lower.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[1] = uiInfo.uiDC.registerModel("models/players/human_base/upper.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[2] = uiInfo.uiDC.registerModel("models/players/human_base/head.md3");
                uiInfo.humanArmouryBuyListModel[j].skin[0] = uiInfo.uiDC.registerSkin("models/players/human_base/lower_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[1] = uiInfo.uiDC.registerSkin("models/players/human_base/upper_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[2] = uiInfo.uiDC.registerSkin("models/players/human_base/head_light.skin");
                uiInfo.humanArmouryBuyListModel[j].frame[0] = 157;
                uiInfo.humanArmouryBuyListModel[j].frame[1] = 151;
                uiInfo.humanArmouryBuyListModel[j].parent[0].parentTagName = "tag_torso";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 32;
                uiInfo.humanArmouryBuyListModel[j].zOffset = -28;
                break;
              case UP_MEDKIT:
                // Should get the red cross of medical station
                break;
              case UP_BATTPACK:
                uiInfo.humanArmouryBuyListModel[j].assetCount = 4;
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/players/human_base/lower.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[1] = uiInfo.uiDC.registerModel("models/players/human_base/upper.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[2] = uiInfo.uiDC.registerModel("models/players/human_base/head.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[3] = uiInfo.uiDC.registerModel("models/players/human_base/battpack.md3");
                uiInfo.humanArmouryBuyListModel[j].skin[0] = uiInfo.uiDC.registerSkin("models/players/human_base/lower_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[1] = uiInfo.uiDC.registerSkin("models/players/human_base/upper_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[2] = uiInfo.uiDC.registerSkin("models/players/human_base/head_default.skin");
                uiInfo.humanArmouryBuyListModel[j].frame[0] = 157;
                uiInfo.humanArmouryBuyListModel[j].frame[1] = 151;
                uiInfo.humanArmouryBuyListModel[j].parent[0].parentTagName = "tag_torso";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].parent[2].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[2].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 40;
                uiInfo.humanArmouryBuyListModel[j].zOffset = -22;
                break;
              case UP_JETPACK:
                uiInfo.humanArmouryBuyListModel[j].assetCount = 4;
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/players/human_base/lower.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[1] = uiInfo.uiDC.registerModel("models/players/human_base/upper.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[2] = uiInfo.uiDC.registerModel("models/players/human_base/head.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[3] = uiInfo.uiDC.registerModel("models/players/human_base/jetpack.md3");
                uiInfo.humanArmouryBuyListModel[j].skin[0] = uiInfo.uiDC.registerSkin("models/players/human_base/lower_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[1] = uiInfo.uiDC.registerSkin("models/players/human_base/upper_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[2] = uiInfo.uiDC.registerSkin("models/players/human_base/head_default.skin");
                uiInfo.humanArmouryBuyListModel[j].frame[0] = 157;
                uiInfo.humanArmouryBuyListModel[j].frame[1] = 151;
                uiInfo.humanArmouryBuyListModel[j].parent[0].parentTagName = "tag_torso";
                uiInfo.humanArmouryBuyListModel[j].parent[0].parentIndex = 0;
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].parent[2].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[2].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 40;
                uiInfo.humanArmouryBuyListModel[j].zOffset = -22;
                break;
              case UP_BATTLESUIT:
              uiInfo.humanArmouryBuyListModel[j].assetCount = 3;
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/players/human_bsuit/lower.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[1] = uiInfo.uiDC.registerModel("models/players/human_bsuit/upper.md3");
                uiInfo.humanArmouryBuyListModel[j].asset[2] = uiInfo.uiDC.registerModel("models/players/human_bsuit/head.md3");
                uiInfo.humanArmouryBuyListModel[j].skin[0] = uiInfo.uiDC.registerSkin("models/players/human_bsuit/lower_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[1] = uiInfo.uiDC.registerSkin("models/players/human_bsuit/upper_default.skin");
                uiInfo.humanArmouryBuyListModel[j].skin[2] = uiInfo.uiDC.registerSkin("models/players/human_bsuit/head_default.skin");
                uiInfo.humanArmouryBuyListModel[j].frame[0] = 166;
                uiInfo.humanArmouryBuyListModel[j].frame[1] = 151;
                uiInfo.humanArmouryBuyListModel[j].parent[0].parentTagName = "tag_torso";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentTagName = "tag_head";
                uiInfo.humanArmouryBuyListModel[j].parent[1].parentIndex = 1;
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 100;
                uiInfo.humanArmouryBuyListModel[j].zOffset = -25;
                break;
              case UP_GRENADE:
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/weapons/grenade/grenade.md3");
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 15;
                break;
              case UP_AMMO:
                uiInfo.humanArmouryBuyListModel[j].asset[0] = uiInfo.uiDC.registerModel("models/weapons/shells/rifle-shell.md3");
                uiInfo.humanArmouryBuyListModel[j].cameraDist = 15;
                break;
            }
        }
    }
}

/*
===============
UI_LoadHumanArmouryBuysWeapon
===============
*/
static void UI_LoadHumanArmouryBuysWeapon(int priority, int *j, int stage, int sellingBudget,
                                          int credits, qboolean criticalBuilds)
{
  int       i = 0;
  qboolean  addWeapon;
  char      *prefix;

  for (i = WP_NONE + 1; i < WP_NUM_WEAPONS; i++)
  {
      if (BG_Weapon(i)->team == TEAM_HUMANS && BG_Weapon(i)->purchasable &&
          BG_WeaponIsAllowed(i) && !(uiInfo.weapons & (1 << i)))
      {
          addWeapon = qfalse;
          switch (priority) {
              case 0:
                if (criticalBuilds == qtrue && i == WP_HBUILD)  // If there are critical builds to build
                {
                    addWeapon = qtrue;
                    prefix = "[!] ";
                }
                break;
              case 1:
                  if (BG_WeaponAllowedInStage(i, stage) && UI_CanUpgradeToWeapon(i, sellingBudget, credits)
                      && !(criticalBuilds == qtrue && i == WP_HBUILD))
                  {
                      addWeapon = qtrue;
                      prefix = UI_IsBetterWeapon(i, sellingBudget) ? "[upgrade] " : "";
                  }
                  break;
              case 2:
                  if (BG_WeaponAllowedInStage(i, stage) && !UI_CanUpgradeToWeapon(i, sellingBudget, credits))
                  {
                      addWeapon = qtrue;
                      prefix = "^0";
                  }
                  break;
              case 3:
                  if (!BG_WeaponAllowedInStage(i, stage))
                  {
                      addWeapon = qtrue;
                      prefix = "[locked] ^0";
                  }
                  break;
          }

          if (addWeapon == qtrue)
          {
              uiInfo.humanArmouryBuyList[*j].text = String_Alloc(va("%s%s", prefix, BG_Weapon(i)->humanName));
              uiInfo.humanArmouryBuyList[*j].cmd = String_Alloc(va("cmd buy %s\n", BG_Weapon(i)->name));
              uiInfo.humanArmouryBuyList[*j].type = INFOTYPE_WEAPON;
              uiInfo.humanArmouryBuyList[*j].v.weapon = i;

              (*j)++;
              uiInfo.humanArmouryBuyCount++;
          }
      }
  }
}


/*
===============
UI_LoadHumanArmouryBuysUpgrade
===============
*/
static void UI_LoadHumanArmouryBuysUpgrade(int priority, int *j, int stage, int upgrSlots, int credits)
{
    int       i = 0;
    qboolean  addUpgrade;
    char      *prefix;

    for (i = UP_NONE + 1; i < UP_NUM_UPGRADES; i++)
    {
        if (BG_Upgrade(i)->team == TEAM_HUMANS && BG_Upgrade(i)->purchasable &&
            BG_UpgradeIsAllowed(i) && !(uiInfo.upgrades & (1 << i)))
        {
            addUpgrade = qfalse;
            switch (priority) {
                case 0:
                    if (i == UP_AMMO && !UI_IsAmmoFull())
                    {
                        addUpgrade = qtrue;
                        prefix = "[!] ";
                    }
                    break;
                case 1:
                    if (i != UP_AMMO && BG_UpgradeAllowedInStage(i, stage) && UI_CanGotUpgrade(i, credits))
                    {
                        addUpgrade = qtrue;
                        prefix = UI_IsUpgradeBetter(i, upgrSlots) ? "[upgrade] " : "";
                    }
                    break;
                case 2:
                    if ((BG_UpgradeAllowedInStage(i, stage) && !UI_CanGotUpgrade(i, credits))
                        || (i == UP_AMMO && UI_IsAmmoFull()))
                    {
                        addUpgrade = qtrue;
                        prefix = "^0";
                    }
                    break;
                case 3:
                    if (!BG_UpgradeAllowedInStage(i, stage))
                    {
                        addUpgrade = qtrue;
                        prefix = "[locked] ^0";
                    }
                    break;
            }

            if (addUpgrade == qtrue)
            {
                uiInfo.humanArmouryBuyList[*j].text = String_Alloc(va("%s%s", prefix, BG_Upgrade(i)->humanName));
                uiInfo.humanArmouryBuyList[*j].cmd = String_Alloc(va("cmd buy %s\n", BG_Upgrade(i)->name));
                uiInfo.humanArmouryBuyList[*j].type = INFOTYPE_UPGRADE;
                uiInfo.humanArmouryBuyList[*j].v.upgrade = i;

                (*j)++;
                uiInfo.humanArmouryBuyCount++;
            }
        }
    }
}

/*
===============
UI_LoadHumanArmouryBuys
===============
*/
static void UI_LoadHumanArmouryBuys(void)
{
    int i, j = 0;
    humanStates_t state;
    qboolean criticalBuilds;
    stage_t stage = UI_GetCurrentHumanStage();
    int sellingBudget = 0;
    int upgrSlots = 0;
    int credits = UI_GetCurrentCredits();

    UI_humanStates(&state);
    UI_ParseCarriageList();

    for (i = WP_NONE + 1; i < WP_NUM_WEAPONS; i++)
        if (uiInfo.weapons & (1 << i))
          sellingBudget += BG_Weapon(i)->price;

    for (i = UP_NONE + 1; i < UP_NUM_UPGRADES; i++)
        if (uiInfo.upgrades & (1 << i))
            upgrSlots |= BG_Upgrade(i)->slots;

    criticalBuilds = ( ( !state.rcHealth || !state.spawns || BG_BuildableAllowedInStage(BA_H_ARMOURY, stage) && !state.armourys
            || BG_BuildableAllowedInStage(BA_H_MEDISTAT, stage) && !state.medicals
            || BG_BuildableAllowedInStage(BA_H_DCC, stage) && !state.computers )
          && !state.builders );

    uiInfo.humanArmouryBuyCount = 0;

    // Critical (ammo mainly)
    UI_LoadHumanArmouryBuysWeapon(0, &j, stage, sellingBudget, credits, criticalBuilds);
    UI_LoadHumanArmouryBuysUpgrade(0, &j, stage, upgrSlots, credits);

    // Available to buy
    UI_LoadHumanArmouryBuysWeapon(1, &j, stage, sellingBudget, credits, criticalBuilds);
    UI_LoadHumanArmouryBuysUpgrade(1, &j, stage, upgrSlots, credits);

    // Unlocked but no enough funds
    UI_LoadHumanArmouryBuysWeapon(2, &j, stage, sellingBudget, credits, criticalBuilds);
    UI_LoadHumanArmouryBuysUpgrade(2, &j, stage, upgrSlots, credits);

    // Locked
    UI_LoadHumanArmouryBuysWeapon(3, &j, stage, sellingBudget, credits, criticalBuilds);
    UI_LoadHumanArmouryBuysUpgrade(3, &j, stage, upgrSlots, credits);
}

/*
===============
UI_LoadHumanArmourySells
===============
*/
static void UI_LoadHumanArmourySells(void)
{
    int i, j = 0;

    uiInfo.humanArmourySellCount = 0;
    UI_ParseCarriageList();

    for (i = WP_NONE + 1; i < WP_NUM_WEAPONS; i++)
    {
        if (uiInfo.weapons & (1 << i))
        {
            uiInfo.humanArmourySellList[j].text = BG_Weapon(i)->humanName;
            uiInfo.humanArmourySellList[j].cmd = String_Alloc(va("cmd sell %s\n", BG_Weapon(i)->name));
            uiInfo.humanArmourySellList[j].type = INFOTYPE_WEAPON;
            uiInfo.humanArmourySellList[j].v.weapon = i;

            j++;

            uiInfo.humanArmourySellCount++;
        }
    }

    for (i = UP_NONE + 1; i < UP_NUM_UPGRADES; i++)
    {
        if (uiInfo.upgrades & (1 << i))
        {
            uiInfo.humanArmourySellList[j].text = BG_Upgrade(i)->humanName;
            uiInfo.humanArmourySellList[j].cmd = String_Alloc(va("cmd sell %s\n", BG_Upgrade(i)->name));
            uiInfo.humanArmourySellList[j].type = INFOTYPE_UPGRADE;
            uiInfo.humanArmourySellList[j].v.upgrade = i;

            j++;

            uiInfo.humanArmourySellCount++;
        }
    }
}

/*
===============
UI_ArmouryRefreshCb
===============
*/
static void UI_ArmouryRefreshCb(void *data)
{
    static int oldFullAmmo = -1;
    int oldWeapons = uiInfo.weapons;
    int oldUpgrades = uiInfo.upgrades;

    UI_ParseCarriageList();

    if (uiInfo.weapons != oldWeapons || uiInfo.upgrades != oldUpgrades)
    {
        UI_LoadHumanArmouryBuys();
        UI_LoadHumanArmourySells();
        UI_RemoveCaptureFunc();
    }
    else if (UI_IsAmmoFull() != oldFullAmmo)
    {
        UI_LoadHumanArmouryBuys();
    }
    oldFullAmmo = UI_IsAmmoFull();
}

/*
===============
UI_LoadAlienUpgradesModels
===============
*/
static void UI_LoadAlienUpgradesModels(void)
{
  int i;

  memset(&(uiInfo.alienUpgradeListModel), 0, sizeof(uiInfo.alienUpgradeListModel));
  // No clean way found to determin is class is aliens one
  for (i = PCL_NONE + 1; i < PCL_HUMAN; i++)
  {
    uiInfo.alienUpgradeListModel[i].asset[0] = uiInfo.uiDC.registerModel(va("models/players/%s/nonseg.md3", BG_ClassConfig(i)->modelName));
    uiInfo.alienUpgradeListModel[i].assetCount = 1;
    uiInfo.alienUpgradeListModel[i].skin[0] = uiInfo.uiDC.registerSkin(va("models/players/%s/nonseg_%s.skin", BG_ClassConfig(i)->modelName, BG_ClassConfig(i)->skinName));
    uiInfo.alienUpgradeListModel[i].scale = BG_ClassConfig(i)->modelScale;
    uiInfo.alienUpgradeListModel[i].zOffset = BG_ClassConfig(i)->zOffset;
    uiInfo.alienUpgradeListModel[i].cameraDist = 100;
    uiInfo.alienUpgradeListModel[i].autoAdjust = qtrue;
  }
}

/*
===============
UI_LoadAlienUpgradesClass
===============
*/
static void UI_LoadAlienUpgradesClass(int priority, int *j, int class, int credits, int stage)
{
    int       i = 0;
    qboolean  addClass;
    char      *prefix;

    // No clean way found to determin is class is aliens one
    for (i = PCL_NONE + 1; i < PCL_HUMAN; i++)
    {
        addClass = qfalse;
        if (BG_ClassIsAllowed(i))
        {
            switch (priority) {
                case 1:
                    if (BG_ClassCanEvolveFromTo(class, i, credits, stage, 0) >= 0)
                    {
                        addClass = qtrue;
                        // Is it stage 1 or is it newer in current stage, or is level0 (cause no adv dretch)
                        prefix = (!stage || !BG_ClassAllowedInStage( i, 0 )
                            || i == PCL_ALIEN_LEVEL0) ? "[upgrade] " : "";
                    }
                    break;
                case 2:
                    if (BG_ClassCanEvolveFromTo(class, i, credits, stage, 0) < 0 && BG_ClassAllowedInStage( i, stage ))
                    {
                        addClass = qtrue;
                        prefix = "^0";
                    }
                    break;
                case 3:
                    if (BG_ClassCanEvolveFromTo(class, i, credits, stage, 0) < 0 && !BG_ClassAllowedInStage( i, stage ))
                    {
                        addClass = qtrue;
                        prefix = "[locked] ^0";
                    }
                    break;
            }
        }

        if (addClass == qtrue)
        {
            uiInfo.alienUpgradeList[*j].text = String_Alloc(va("%s%s", prefix, BG_ClassConfig(i)->humanName));
            uiInfo.alienUpgradeList[*j].cmd = String_Alloc(va("cmd class %s\n", BG_Class(i)->name));
            uiInfo.alienUpgradeList[*j].type = INFOTYPE_CLASS;
            uiInfo.alienUpgradeList[*j].v.pclass = i;

            (*j)++;
            uiInfo.alienUpgradeCount++;
        }
    }
}

/*
===============
UI_LoadAlienUpgrades
===============
*/
static void UI_LoadAlienUpgrades(void)
{
    int i, j = 0;
    int class, credits;
    char ui_currentClass[MAX_STRING_CHARS];
    stage_t stage = UI_GetCurrentAlienStage();

    trap_Cvar_VariableStringBuffer("ui_currentClass", ui_currentClass, MAX_STRING_CHARS);

    sscanf(ui_currentClass, "%d %d", &class, &credits);

    uiInfo.alienUpgradeCount = 0;

    // UI_LoadAlienUpgradesClass(0, &j, class, credits, stage);
    UI_LoadAlienUpgradesClass(1, &j, class, credits, stage);
    UI_LoadAlienUpgradesClass(2, &j, class, credits, stage);
    UI_LoadAlienUpgradesClass(3, &j, class, credits, stage);
}

/*
===============
UI_LoadAlienBuildsModels
===============
*/
static void UI_LoadAlienBuildsModels(void)
{
  int i;
  fileHandle_t  h;
  char *modelFile;

  memset(&(uiInfo.alienBuildListModel), 0, sizeof(uiInfo.alienBuildListModel));
  for (i = BA_NONE + 1; i < BA_NUM_BUILDABLES; i++)
  {
      if (BG_Buildable(i)->team == TEAM_ALIENS && BG_BuildableIsAllowed(i))
      {
          uiInfo.alienBuildListModel[i].assetCount = 0;
          for( h = 0; h < MAX_BUILDABLE_MODELS; h++ )
          {
            modelFile = BG_BuildableConfig( i )->models[ h ];
            if( strlen( modelFile ) > 0 )
              uiInfo.alienBuildListModel[i].asset[ uiInfo.alienBuildListModel[i].assetCount++ ]
                = uiInfo.uiDC.registerModel( modelFile );
          }
          uiInfo.alienBuildListModel[i].scale = BG_BuildableConfig( i )->modelScale;
          uiInfo.alienBuildListModel[i].zOffset = BG_BuildableConfig( i )->zOffset;
          uiInfo.alienBuildListModel[i].cameraDist = 100;
          uiInfo.alienBuildListModel[i].autoAdjust = qtrue;
      }
  }
}

/*
===============
UI_LoadAlienBuildsItems
===============
*/
static void UI_LoadAlienBuildsItems(int priority, int *j, int stage)
{
    int           i = 0;
    qboolean      addItem;
    char          *prefix;
    qboolean      critical;
    alienStates_t state;

    UI_alienStates(&state);

    for (i = BA_NONE + 1; i < BA_NUM_BUILDABLES; i++)
    {
        addItem = qfalse;
        critical = qfalse;
        if (BG_Buildable(i)->team == TEAM_ALIENS && BG_BuildableIsAllowed(i))
        {
            switch (priority) {
                case 0:
                    break;
                case 1:
                    if (BG_BuildableAllowedInStage(i, stage))
                    {
                        addItem = qtrue;
                        if (i == BA_A_SPAWN && !state.spawns
                            || i == BA_A_BOOSTER && !state.boosters
                            || i == BA_A_OVERMIND && !state.omHealth )
                            critical = qtrue;
                        prefix = (char*)(va(
                          "%s%s%s%s",
                          ( (!state.omHealth || state.omBuilding) && i != BA_A_OVERMIND ) ? "[overmind] " : "", // Show an overmind if it need it
                          ( !state.omHealth && i != BA_A_OVERMIND ) ? "^0" : "", // Grey out the item if om is not building too
                          ( !(BG_Buildable(i)->buildWeapon & uiInfo.weapons) ) ? "[upgrade][advgranger]^0 " : "", // Grey out and ask to upgrade if adv granger is needed
                          ( critical ) ? "[!] " : "" // Inform if this item is important
                        ));
                    }
                    break;
                case 2:
                    if (!BG_BuildableAllowedInStage(i, stage))
                    {
                        addItem = qtrue;
                        prefix = "[locked] ^0";
                    }
                    break;
            }

            if (addItem == qtrue)
            {
                uiInfo.alienBuildList[*j].text = String_Alloc(va("%s%s", prefix, BG_Buildable(i)->humanName));
                uiInfo.alienBuildList[*j].cmd = String_Alloc(va("cmd build %s\n", BG_Buildable(i)->name));
                uiInfo.alienBuildList[*j].type = INFOTYPE_BUILDABLE;
                uiInfo.alienBuildList[*j].v.buildable = i;

                (*j)++;
                uiInfo.alienBuildCount++;
            }
        }
    }
}

/*
===============
UI_LoadAlienBuilds
===============
*/
static void UI_LoadAlienBuilds(void)
{
    int i, j = 0;
    stage_t stage;

    UI_ParseCarriageList();
    stage = UI_GetCurrentAlienStage();

    uiInfo.alienBuildCount = 0;

    // UI_LoadAlienBuildsItems(0, &j, stage);
    UI_LoadAlienBuildsItems(1, &j, stage);
    UI_LoadAlienBuildsItems(2, &j, stage);
}

/*
===============
UI_LoadHumansBuildsModels
===============
*/
static void UI_LoadHumansBuildsModels(void)
{
  int i;
  fileHandle_t  h;
  char *modelFile;

  memset(&(uiInfo.humanBuildListModel), 0, sizeof(uiInfo.humanBuildListModel));
  for (i = BA_NONE + 1; i < BA_NUM_BUILDABLES; i++)
  {
      if (BG_Buildable(i)->team == TEAM_HUMANS && BG_BuildableIsAllowed(i))
      {
          for( h = 0; h < MAX_BUILDABLE_MODELS; h++ )
          {
            modelFile = BG_BuildableConfig( i )->models[ h ];
            if( strlen( modelFile ) > 0 )
              uiInfo.humanBuildListModel[i].asset[ uiInfo.humanBuildListModel[i].assetCount++ ]
                = uiInfo.uiDC.registerModel( modelFile );
          }
          uiInfo.humanBuildListModel[i].scale = BG_BuildableConfig( i )->modelScale;
          uiInfo.humanBuildListModel[i].zOffset = BG_BuildableConfig( i )->zOffset;
          uiInfo.humanBuildListModel[i].cameraDist = 160;
          uiInfo.humanBuildListModel[i].autoAdjust = qtrue;
      }
  }
}

/*
===============
UI_LoadHumanBuildsItems
===============
*/
static void UI_LoadHumanBuildsItems(int priority, int *j, int stage)
{
    int           i = 0;
    qboolean      addItem;
    char          *prefix;
    qboolean      critical;
    humanStates_t state;

    UI_humanStates(&state);

    for (i = BA_NONE + 1; i < BA_NUM_BUILDABLES; i++)
    {
        addItem = qfalse;
        critical = qfalse;
        if (BG_Buildable(i)->team == TEAM_HUMANS && BG_BuildableIsAllowed(i))
        {
            switch (priority) {
                case 0:
                    break;
                case 1:
                    if (BG_BuildableAllowedInStage(i, stage))
                    {
                        addItem = qtrue;
                        if (i == BA_H_SPAWN && !state.spawns
                            || i == BA_H_ARMOURY && !state.armourys
                            || i == BA_H_MEDISTAT && !state.medicals
                            || i == BA_H_DCC && !state.computers
                            || i == BA_H_REACTOR && !state.rcHealth )
                            critical = qtrue;
                        prefix = (char*)(va(
                          "%s%s%s%s",
                          ( (!state.rcHealth || state.rcBuilding) && i != BA_H_REACTOR ) ? "[reactor] " : "", // Show a reactor if it need it
                          ( !state.rcHealth && i != BA_H_REACTOR ) ? "^0" : "", // Grey out the item if om is not building too
                          ( !(BG_Buildable(i)->buildWeapon & uiInfo.weapons) ) ? "[upgrade][ckit]^0 " : "", // Grey out and ask to upgrade if adv ckit is needed. Now Ckit = AdvCkit (keep for compatibility)
                          ( critical ) ? "[!] " : "" // Inform if this item is important
                        ));
                    }
                    break;
                case 2:
                    if (!BG_BuildableAllowedInStage(i, stage))
                    {
                        addItem = qtrue;
                        prefix = "[locked] ^0";
                    }
                    break;
            }

            if (addItem == qtrue)
            {
                uiInfo.humanBuildList[*j].text = String_Alloc(va("%s%s", prefix, BG_Buildable(i)->humanName));
                uiInfo.humanBuildList[*j].cmd = String_Alloc(va("cmd build %s\n", BG_Buildable(i)->name));
                uiInfo.humanBuildList[*j].type = INFOTYPE_BUILDABLE;
                uiInfo.humanBuildList[*j].v.buildable = i;

                (*j)++;
                uiInfo.humanBuildCount++;
            }
        }
    }
}

/*
===============
UI_LoadHumanBuilds
===============
*/
static void UI_LoadHumanBuilds(void)
{
    int i, j = 0;
    fileHandle_t h;
    char *modelFile;
    stage_t stage;

    UI_ParseCarriageList();
    stage = UI_GetCurrentHumanStage();

    uiInfo.humanBuildCount = 0;


    // UI_LoadHumanBuildsItems(0, &j, stage);
    UI_LoadHumanBuildsItems(1, &j, stage);
    UI_LoadHumanBuildsItems(2, &j, stage);
}

/*
===============
UI_LoadVoiceCmds
===============
*/
static void UI_LoadVoiceCmds(void)
{
    voice_t *v;
    voiceCmd_t *c;

    const char *cmd;
    char mode[2];
    char ui_voice[MAX_VOICE_CMD_LEN];

    trap_Cvar_VariableStringBuffer("ui_voicemenu", mode, sizeof(mode));
    trap_Cvar_VariableStringBuffer("voice", ui_voice, sizeof(ui_voice));

    uiInfo.voiceCmdCount = 0;

    switch (mode[0])
    {
        default:
        case '1':
            cmd = "vsay";
            break;
        case '2':
            cmd = "vsay_team";
            break;
        case '3':
            cmd = "vsay_local";
            break;
    };

    v = BG_VoiceByName(uiInfo.voices, ui_voice);
    if (!v)
        return;

    for (c = v->cmds; c; c = c->next)
    {
        uiInfo.voiceCmdList[uiInfo.voiceCmdCount].text = c->cmd;
        uiInfo.voiceCmdList[uiInfo.voiceCmdCount].cmd = String_Alloc(va("cmd %s %s\n", cmd, c->cmd));
        uiInfo.voiceCmdList[uiInfo.voiceCmdCount].type = INFOTYPE_VOICECMD;
        uiInfo.voiceCmdList[uiInfo.voiceCmdCount].v.text = c->tracks[0].text;

        uiInfo.voiceCmdCount++;
    }
}

/*
===============
UI_LoadMods
===============
*/
static void UI_LoadMods(void)
{
    int numdirs;
    char dirlist[2048];
    char *dirptr;
    char *descptr;
    int i;
    int dirlen;

    uiInfo.modCount = 0;
    numdirs = trap_FS_GetFileList("$modlist", "", dirlist, sizeof(dirlist));
    dirptr = dirlist;

    for (i = 0; i < numdirs; i++)
    {
        dirlen = strlen(dirptr) + 1;
        uiInfo.modList[uiInfo.modCount].modName = String_Alloc(dirptr);
        dirptr += dirlen;
        uiInfo.modCount++;

        if (uiInfo.modCount >= MAX_MODS)
            break;
    }
}

/*
===============
UI_LoadMovies
===============
*/
static void UI_LoadMovies(void)
{
    char movielist[4096];
    char *moviename;
    int i, len;

    uiInfo.movieCount = trap_FS_GetFileList("video", "roq", movielist, 4096);

    if (uiInfo.movieCount)
    {
        if (uiInfo.movieCount > MAX_MOVIES)
            uiInfo.movieCount = MAX_MOVIES;

        moviename = movielist;

        for (i = 0; i < uiInfo.movieCount; i++)
        {
            len = strlen(moviename);

            if (!Q_stricmp(moviename + len - 4, ".roq"))
                moviename[len - 4] = '\0';

            Q_strupr(moviename);
            uiInfo.movieList[i] = String_Alloc(moviename);
            moviename += len + 1;
        }
    }
}

/*
===============
UI_LoadDemos
===============
*/
static void UI_LoadDemos(void)
{
    char demolist[4096];
    char demoExt[32];
    char *demoname;
    int  i = 0;
    int  len, protocol;

    uiInfo.demoCount = 0;

    for(protocol = 0; protocol < 3; protocol++) {
      Com_sprintf(
        demoExt, sizeof(demoExt), "%s%d", DEMOEXT,
        protocol == 2 ? 69 : protocol == 1 ? 70 : 71);

      uiInfo.demoCount += trap_FS_GetFileList("demos", demoExt, demolist, 4096);

      Com_sprintf(
          demoExt, sizeof(demoExt), ".%s%d", DEMOEXT,
          protocol == 2 ? 69 : protocol == 1 ? 70 : 71);

      if (uiInfo.demoCount)
      {
          if (uiInfo.demoCount > MAX_DEMOS)
              uiInfo.demoCount = MAX_DEMOS;

          demoname = demolist;

          for (; i < uiInfo.demoCount; i++)
          {
              len = strlen(demoname);

              if (!Q_stricmp(demoname + len - strlen(demoExt), demoExt))
                  demoname[len - strlen(demoExt)] = '\0';

              uiInfo.demoList[i] = String_Alloc(demoname);
              demoname += len + 1;
          }
      }
    }
}

static void UI_Update(const char *name)
{
    int val = trap_Cvar_VariableValue(name);

    if (Q_stricmp(name, "ui_SetName") == 0)
        trap_Cvar_Set("name", UI_Cvar_VariableString("ui_Name"));
    else if (Q_stricmp(name, "ui_setRate") == 0)
    {
        float rate = trap_Cvar_VariableValue("rate");

        if (rate >= 5000)
        {
            trap_Cvar_Set("cl_maxpackets", "30");
            trap_Cvar_Set("cl_packetdup", "1");
        }
        else if (rate >= 4000)
        {
            trap_Cvar_Set("cl_maxpackets", "15");
            trap_Cvar_Set("cl_packetdup", "2");  // favor less prediction errors when there's packet loss
        }
        else
        {
            trap_Cvar_Set("cl_maxpackets", "15");
            trap_Cvar_Set("cl_packetdup", "1");  // favor lower bandwidth
        }
    }
    else if (Q_stricmp(name, "ui_GetName") == 0)
        trap_Cvar_Set("ui_Name", UI_Cvar_VariableString("name"));
    else if (Q_stricmp(name, "r_colorbits") == 0)
    {
        switch (val)
        {
            case 0:
                trap_Cvar_SetValue("r_depthbits", 0);
                trap_Cvar_SetValue("r_stencilbits", 0);
                break;

            case 16:
                trap_Cvar_SetValue("r_depthbits", 16);
                trap_Cvar_SetValue("r_stencilbits", 0);
                break;

            case 32:
                trap_Cvar_SetValue("r_depthbits", 24);
                break;
        }
    }
    else if (Q_stricmp(name, "r_lodbias") == 0)
    {
        switch (val)
        {
            case 0:
                trap_Cvar_SetValue("r_subdivisions", 4);
                break;

            case 1:
                trap_Cvar_SetValue("r_subdivisions", 12);
                break;

            case 2:
                trap_Cvar_SetValue("r_subdivisions", 20);
                break;
        }
    }
    else if (Q_stricmp(name, "ui_glCustom") == 0)
    {
        switch (val)
        {
            case 0:  // high quality
                trap_Cvar_SetValue("r_subdivisions", 4);
                trap_Cvar_SetValue("r_vertexlight", 0);
                trap_Cvar_SetValue("r_lodbias", 0);
                trap_Cvar_SetValue("r_colorbits", 32);
                trap_Cvar_SetValue("r_depthbits", 24);
                trap_Cvar_SetValue("r_picmip", 0);
                trap_Cvar_SetValue("r_texturebits", 32);
                trap_Cvar_SetValue("r_fastSky", 0);
                trap_Cvar_SetValue("r_inGameVideo", 1);
                trap_Cvar_SetValue("cg_shadows", 1);
                trap_Cvar_SetValue("cg_bounceParticles", 1);
                trap_Cvar_Set("r_texturemode", "GL_LINEAR_MIPMAP_LINEAR");
                break;

            case 1:  // normal
                trap_Cvar_SetValue("r_subdivisions", 12);
                trap_Cvar_SetValue("r_vertexlight", 0);
                trap_Cvar_SetValue("r_lodbias", 0);
                trap_Cvar_SetValue("r_colorbits", 0);
                trap_Cvar_SetValue("r_depthbits", 24);
                trap_Cvar_SetValue("r_picmip", 1);
                trap_Cvar_SetValue("r_texturebits", 0);
                trap_Cvar_SetValue("r_fastSky", 0);
                trap_Cvar_SetValue("r_inGameVideo", 1);
                trap_Cvar_Set("r_texturemode", "GL_LINEAR_MIPMAP_LINEAR");
                trap_Cvar_SetValue("cg_shadows", 0);
                trap_Cvar_SetValue("cg_bounceParticles", 0);
                break;

            case 2:  // fast
                trap_Cvar_SetValue("r_subdivisions", 8);
                trap_Cvar_SetValue("r_vertexlight", 0);
                trap_Cvar_SetValue("r_lodbias", 1);
                trap_Cvar_SetValue("r_colorbits", 0);
                trap_Cvar_SetValue("r_depthbits", 0);
                trap_Cvar_SetValue("r_picmip", 1);
                trap_Cvar_SetValue("r_texturebits", 0);
                trap_Cvar_SetValue("cg_shadows", 0);
                trap_Cvar_SetValue("r_fastSky", 1);
                trap_Cvar_SetValue("r_inGameVideo", 0);
                trap_Cvar_SetValue("cg_bounceParticles", 0);
                trap_Cvar_Set("r_texturemode", "GL_LINEAR_MIPMAP_NEAREST");
                break;

            case 3:  // fastest
                trap_Cvar_SetValue("r_subdivisions", 20);
                trap_Cvar_SetValue("r_vertexlight", 1);
                trap_Cvar_SetValue("r_lodbias", 2);
                trap_Cvar_SetValue("r_colorbits", 16);
                trap_Cvar_SetValue("r_depthbits", 16);
                trap_Cvar_SetValue("r_picmip", 2);
                trap_Cvar_SetValue("r_texturebits", 16);
                trap_Cvar_SetValue("cg_shadows", 0);
                trap_Cvar_SetValue("r_fastSky", 1);
                trap_Cvar_SetValue("r_inGameVideo", 0);
                trap_Cvar_SetValue("cg_bounceParticles", 0);
                trap_Cvar_Set("r_texturemode", "GL_LINEAR_MIPMAP_NEAREST");
                break;
        }
    }
    else if (Q_stricmp(name, "ui_mousePitch") == 0)
    {
        if (val == 0)
            trap_Cvar_SetValue("m_pitch", 0.022f);
        else
            trap_Cvar_SetValue("m_pitch", -0.022f);
    }
}

/*
===============
UI_PartialGetEnteredPartialNameFromBuffer

Copies the entered partial name to entered_partial_name as output.
Returns qtrue if the entered partial name is incomplete.
===============
*/
static qboolean UI_PartialGetEnteredPartialNameFromBuffer(
    int at_pos, char *buffer, char *clean_name, char *entered_partial_name,
    int size_of_entered_partial_name,
    int *uncleaned_entered_partial_name_length) {
    char      lower_case_entered_partial_name[MAX_CVAR_VALUE_STRING] = "";
    const int clean_name_length = strlen(clean_name);
    int       i, j;

    memset(entered_partial_name, 0, size_of_entered_partial_name);

    if(uncleaned_entered_partial_name_length) {
        *uncleaned_entered_partial_name_length = 0;
    }

    for(
        i = at_pos + 1, j = 0;
        i <= chatInfo.say_length && j < clean_name_length; i++, j++) {
        char *s = &buffer[i];

        //skip over color codes for comparing
        if ( Q_IsColorString( s ) ) {
            int color_string_length = Q_ColorStringLength(s);

            i += color_string_length - 1;
            s += color_string_length - 1;
            if(uncleaned_entered_partial_name_length) {
                *uncleaned_entered_partial_name_length += color_string_length;
            }
        } else if ( s[0] >= 0x20 && s[0] <= 0x7E ) {
            if(Q_IsColorEscapeEscape(s)) {
                i++;
                s++;
                if(uncleaned_entered_partial_name_length) {
                    (*uncleaned_entered_partial_name_length)++;
                }
            }
        }

        entered_partial_name[j] = s[0];
        lower_case_entered_partial_name[j] = tolower(s[0]);

    

        if(
            entered_partial_name[0] &&
            entered_partial_name[0] != ' ' &&
            strstr(clean_name, lower_case_entered_partial_name)) {
            if(uncleaned_entered_partial_name_length) {
                (*uncleaned_entered_partial_name_length)++;
            }
        } else {
            entered_partial_name[j] = '\0';
            return qtrue;
        }
    }

    if(strlen(entered_partial_name) != clean_name_length) {
        return qtrue;
    }

    return qfalse;
}

static void UI_CleanStr(
    const char *string_in, char *string_out, size_t size_of_string_out) {
    char string_temp[MAX_COLORFUL_NAME_LENGTH] = {""};

    Q_strncpyz(string_temp, string_in, sizeof(string_temp));
    Q_CleanStr(string_temp);
    Q_StringToLower(string_temp, string_out, size_of_string_out);
}

/*
===============
UI_TabCompleteName
===============
*/
static void UI_TabCompleteName(
    char *string, char *buffer, char *playerName, int at_pos,
    qboolean add_space) {
    int  i, j;
    char n2[MAX_NAME_LENGTH] = {""};
    char s2[MAX_NAME_LENGTH] = {""};

    UI_CleanStr(string, s2, sizeof(s2));
    UI_CleanStr(playerName, n2, sizeof(n2));

    if(!s2[0] || Q_strncmp(s2, n2, sizeof(n2))) {
        char entered_partial_name[MAX_CVAR_VALUE_STRING];
        int uncleaned_entered_partial_name_length;

        // check that the name isn't already complete in the buffer doesn't already
        // complete 

        if(UI_PartialGetEnteredPartialNameFromBuffer(
            at_pos, buffer, n2, entered_partial_name, sizeof(entered_partial_name),
            &uncleaned_entered_partial_name_length)) {
            char  clean_name[MAX_NAME_LENGTH] = {""};
            int clean_name_length;

            Q_strncpyz(
                clean_name, playerName, sizeof(clean_name));
            Q_CleanStr(clean_name);

            clean_name_length = strlen(clean_name);

            if(uncleaned_entered_partial_name_length > 0) {
                qboolean end_of_buffer =
                    !buffer[at_pos + uncleaned_entered_partial_name_length];
                //delete the unclean partial name for replacement by the completed clean name
                if(end_of_buffer) {
                    buffer[at_pos +1] = '\0';
                } else {
                    memmove(
                        &buffer[at_pos + 1],
                        &buffer[at_pos + uncleaned_entered_partial_name_length + 1],
                        chatInfo.say_length -
                        (at_pos + uncleaned_entered_partial_name_length + 1));
                    buffer[
                        chatInfo.say_length -
                        uncleaned_entered_partial_name_length] = '\0';
                }
            }

            //tab complete the name
            if(!trap_Key_GetOverstrikeMode( )) {
                if(
                    buffer[at_pos +1] &&
                    (*chatInfo.say_cursor_pos < chatInfo.say_length) &&
                    (chatInfo.say_length <  MAX_EDITFIELD - clean_name_length) &&
                    (
                        !chatInfo.say_max_chars ||
                        chatInfo.say_length < chatInfo.say_max_chars ) ) {

                //move the subsequent text to the right for insertion
                memmove(
                    &buffer[at_pos + clean_name_length + 1 + (add_space ? 1 : 0)],
                    &buffer[at_pos + 1],
                    chatInfo.say_length - (at_pos + 1));
                }
            }

            for(
                i = at_pos + 1, j = 0;
                i < MAX_EDITFIELD && j < clean_name_length;
                i++, j++) {
                buffer[i] = clean_name[j];
            }

            if(add_space && (i < MAX_EDITFIELD) && (buffer[i] != ' ')) {
                buffer[i] = ' ';
            }

            *chatInfo.say_cursor_pos =
            at_pos + clean_name_length + 1 + (add_space ? 1 : 0);
            trap_Cvar_Set("ui_sayBuffer", buffer);
        }
    }
}

// FIXME: lookup table
static void UI_RunMenuScript(char **args)
{
    const char *name, *name2;
    char buff[1024];
    const char *cmd;

    if (String_Parse(args, &name))
    {
        if (Q_stricmp(name, "StartServer") == 0)
        {
            trap_Cvar_SetValue("dedicated", Com_Clamp(0, 2, ui_dedicated.integer));
            trap_Cmd_ExecuteText(
                EXEC_APPEND, va("wait ; wait ; map \"%s\"\n", uiInfo.mapList[ui_selectedMap.integer].mapLoadName));
        }
        else if (Q_stricmp(name, "resetDefaults") == 0)
        {
            trap_Cmd_ExecuteText(EXEC_APPEND, "exec default.cfg\n");
            trap_Cmd_ExecuteText(EXEC_APPEND, "cvar_restart\n");
            Controls_SetDefaults();
            trap_Cvar_Set("com_introPlayed", "1");
            trap_Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");
        }
        else if (Q_stricmp(name, "loadArenas") == 0)
        {
            UI_LoadArenas();
            Menu_SetFeederSelection(NULL, FEEDER_MAPS, 0, "createserver");
        }
        else if (Q_stricmp(name, "loadServerInfo") == 0)
            UI_ServerInfo();
        else if (Q_stricmp(name, "getNews") == 0)
            UI_UpdateNews(qtrue);
        else if (Q_stricmp(name, "checkForUpdate") == 0)
            UI_UpdateGithubRelease();
        else if (Q_stricmp(name, "downloadUpdate") == 0)
            trap_Cmd_ExecuteText(EXEC_APPEND, "downloadUpdate");
        else if (Q_stricmp(name, "installUpdate") == 0)
            trap_Cmd_ExecuteText(EXEC_APPEND, "installUpdate");
        else if (Q_stricmp(name, "saveControls") == 0)
            Controls_SetConfig(qtrue);
        else if (Q_stricmp(name, "loadControls") == 0)
            Controls_GetConfig();
        else if (Q_stricmp(name, "clearError") == 0)
        {
            trap_Cvar_Set("com_errorMessage", "");
        }
        else if (Q_stricmp(name, "clearDemoError") == 0)
        {
            trap_Cvar_Set("com_demoErrorMessage", "");
        }
        else if (Q_stricmp(name, "downloadIgnore") == 0)
        {
            trap_Cvar_Set("com_downloadPrompt", va("%d", DLP_IGNORE));
        }
        else if (Q_stricmp(name, "downloadCURL") == 0)
        {
            trap_Cvar_Set("com_downloadPrompt", va("%d", DLP_CURL));
        }
        else if (Q_stricmp(name, "downloadUDP") == 0)
        {
            trap_Cvar_Set("com_downloadPrompt", va("%d", DLP_UDP));
        }
        else if (Q_stricmp(name, "RefreshServers") == 0)
        {
            UI_StartServerRefresh(qtrue);
            UI_BuildServerDisplayList(qtrue);
        }
        else if (Q_stricmp(name, "InitServerList") == 0)
        {
            int time = trap_RealTime(NULL);
            int last;
            int sortColumn;

            // set up default sorting

            if (!uiInfo.serverStatus.sorted && Int_Parse(args, &sortColumn))
            {
                uiInfo.serverStatus.sortKey = sortColumn;
                uiInfo.serverStatus.sortDir = 0;
            }

            // refresh if older than 3 days or if list is empty
            last = atoi(UI_Cvar_VariableString(va("ui_lastServerRefresh_%i_time", ui_netSource.integer)));

            if (trap_LAN_GetServerCount(ui_netSource.integer) < 1 || (time - last) > 3600)
            {
                UI_StartServerRefresh(qtrue);
                UI_BuildServerDisplayList(qtrue);
            }
        }
        else if (Q_stricmp(name, "RefreshFilter") == 0)
        {
            UI_StartServerRefresh(qfalse);
            UI_BuildServerDisplayList(qtrue);
        }
        else if (Q_stricmp(name, "LoadDemos") == 0)
            UI_LoadDemos();
        else if (Q_stricmp(name, "LoadMovies") == 0)
            UI_LoadMovies();
        else if (Q_stricmp(name, "LoadMods") == 0)
            UI_LoadMods();
        else if (Q_stricmp(name, "LoadTeams") == 0)
            UI_LoadTeams();
        else if (Q_stricmp(name, "JoinTeam") == 0)
        {
            if ((cmd = uiInfo.teamList[uiInfo.teamIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "LoadHumanItems") == 0)
            UI_LoadHumanItems();
        else if (Q_stricmp(name, "SpawnWithHumanItem") == 0)
        {
            if ((cmd = uiInfo.humanItemList[uiInfo.humanItemIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "LoadAlienClasses") == 0)
            UI_LoadAlienClasses();
        else if (Q_stricmp(name, "SpawnAsAlienClass") == 0)
        {
            if ((cmd = uiInfo.alienClassList[uiInfo.alienClassIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "LoadHumanArmouryBuys") == 0)
        {
            UI_LoadHumanArmouryModels();
            UI_LoadHumanArmouryBuys();
        }
        else if (Q_stricmp(name, "BuyFromArmoury") == 0)
        {
            if ((cmd = uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);

            UI_InstallCaptureFunc(UI_ArmouryRefreshCb, NULL, 1000);
        }
        else if (Q_stricmp(name, "LoadHumanArmourySells") == 0)
            UI_LoadHumanArmourySells();
        else if (Q_stricmp(name, "SellToArmoury") == 0)
        {
            if ((cmd = uiInfo.humanArmourySellList[uiInfo.humanArmourySellIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);

            UI_InstallCaptureFunc(UI_ArmouryRefreshCb, NULL, 1000);
        }
        else if (Q_stricmp(name, "LoadAlienUpgrades") == 0)
        {
            UI_LoadAlienUpgradesModels();
            UI_LoadAlienUpgrades();
        }
        else if (Q_stricmp(name, "UpgradeToNewClass") == 0)
        {
            if ((cmd = uiInfo.alienUpgradeList[uiInfo.alienUpgradeIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "LoadAlienBuilds") == 0)
        {
          UI_LoadAlienBuildsModels();
          UI_LoadAlienBuilds();
        }
        else if (Q_stricmp(name, "BuildAlienBuildable") == 0)
        {
            if ((cmd = uiInfo.alienBuildList[uiInfo.alienBuildIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "LoadHumanBuilds") == 0)
        {
          UI_LoadHumansBuildsModels();
          UI_LoadHumanBuilds();
        }
        else if (Q_stricmp(name, "BuildHumanBuildable") == 0)
        {
            if ((cmd = uiInfo.humanBuildList[uiInfo.humanBuildIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "LoadVoiceCmds") == 0)
        {
            UI_LoadVoiceCmds();
        }
        else if (Q_stricmp(name, "ExecuteVoiceCmd") == 0)
        {
            if ((cmd = uiInfo.voiceCmdList[uiInfo.voiceCmdIndex].cmd))
                trap_Cmd_ExecuteText(EXEC_APPEND, cmd);
        }
        else if (Q_stricmp(name, "Say") == 0)
        {
            char buffer[ MAX_CVAR_VALUE_STRING ];
            char clantagDecolored[ 32 ];

            trap_Cvar_VariableStringBuffer( "ui_sayBuffer", buffer, sizeof( buffer ) );

            if( !buffer[ 0 ] )
            ;
            else {

                // copy line to history buffer
                if(
                    !chatInfo.say_history_current) {
                    Q_strncpyz(
                        chatInfo.say_history_lines[chatInfo.nextHistoryLine % MAX_SAY_HISTORY_LINES],
                        chatInfo.say_unsubmitted_line, MAX_CVAR_VALUE_STRING);
                    chatInfo.nextHistoryLine++;
                }
                Q_strncpyz(
                    chatInfo.say_history_lines[chatInfo.nextHistoryLine % MAX_SAY_HISTORY_LINES],
                    buffer, MAX_CVAR_VALUE_STRING);
                chatInfo.nextHistoryLine++;
                chatInfo.historyLine = chatInfo.nextHistoryLine;
                chatInfo.say_history_current = qtrue;

                if( ui_chatCommands.integer && ( buffer[ 0 ] == '/' ||
                    buffer[ 0 ] == '\\' ) )
                {
                    trap_Cmd_ExecuteText( EXEC_APPEND, va( "%s\n", buffer + 1 ) );
                } else {
                    switch (chatInfo.chat_mode) {
                        case CHAT_GLOBAL:
                            trap_Cmd_ExecuteText( EXEC_APPEND, va( "say \"%s\"\n", buffer ) );
                            break;

                        case CHAT_TEAM:
                            trap_Cmd_ExecuteText( EXEC_APPEND, va( "say_team \"%s\"\n", buffer ) );
                            break;

                        case CHAT_ADMINS:
                            trap_Cmd_ExecuteText( EXEC_APPEND, va( "a \"%s\"\n", buffer ) );
                            break;

                        case CHAT_CLAN:
                            Q_strncpyz(
                                clantagDecolored, ui_clantag.string,
                                sizeof(clantagDecolored) );
                            Q_CleanStr( clantagDecolored );

                            if(
                                strlen(clantagDecolored) > 2 &&
                                strlen(clantagDecolored) < 11) {
                            trap_Cmd_ExecuteText(
                                EXEC_APPEND,
                                va( "m \"%s\" \"%s\"\n", clantagDecolored, buffer));
                            } else {
                                //string isnt long enough
                                Com_Printf ( 
                                    "^3Error:your ui_clantag has to be between 3 and 10 characters long. current value is:^7 %s^7\n",
                                    clantagDecolored );
                                key_pressed_onCharEntry = K_NONE;
                                return;
                            }
                            break;

                        case NUM_CHAT_MODES:
                            chatInfo.chat_mode = CHAT_GLOBAL;
                            trap_Cmd_ExecuteText( EXEC_APPEND, va( "say \"%s\"\n", buffer ) );
                            break;
                    }
                }

                chatInfo.say_unsubmitted_line[0] = '\0';
                trap_Cvar_Set( "ui_sayBuffer", "" );
            }
        }
        else if (Q_stricmp(name, "SayKeydown") == 0)
        {
            char buffer[ MAX_CVAR_VALUE_STRING ];

            trap_Cvar_VariableStringBuffer("ui_sayBuffer", buffer, sizeof(buffer));
            if(chatInfo.say_history_current && !chatInfo.say_make_current_line_blank) {
                Q_strncpyz(chatInfo.say_unsubmitted_line, buffer, sizeof(chatInfo.say_unsubmitted_line));
            }

            if( ui_chatCommands.integer )
            {
                if( buffer[ 0 ] == '/' || buffer[ 0 ] == '\\' ) {
                    Menus_ReplaceActiveByName( "say_command" );
                } else {
                    switch (chatInfo.chat_mode) {
                        case CHAT_GLOBAL:
                            Menus_ReplaceActiveByName( "say" );
                            break;

                        case CHAT_TEAM:
                            Menus_ReplaceActiveByName( "say_team" );
                            break;

                        case CHAT_ADMINS:
                            Menus_ReplaceActiveByName( "say_admins" );
                            break;

                        case CHAT_CLAN:
                            Menus_ReplaceActiveByName( "say_clan" );
                            break;

                        case NUM_CHAT_MODES:
                            chatInfo.chat_mode = CHAT_GLOBAL;
                            trap_Cmd_ExecuteText( EXEC_APPEND, va( "say \"%s\"\n", buffer ) );
                            break;
                    }
                }

                //handle player name tab completion
                if(key_pressed_onCharEntry == K_TAB) {
                    int at_pos = -1;
                    int i;

                    //find the position of the nearest @ from the left of the
                    //cursor
                    i = *chatInfo.say_cursor_pos;
                    while(i >= 0) {
                        if(buffer[i] == '@') {
                            at_pos = i;
                            break;
                        }

                        i--;
                    }

                    if(at_pos >= 0) {
                        char string[MAX_CVAR_VALUE_STRING];
                        char string_clean[MAX_CVAR_VALUE_STRING];
                        int  pids[MAX_CLIENTS];
                        int  matches = 0;
                        int  j;

                        memset(string, 0, sizeof(string));
                        for(
                            i = at_pos + 1, j = 0;
                            i <= *chatInfo.say_cursor_pos &&
                            j < MAX_CVAR_VALUE_STRING;
                            i++, j++) {
                            if(
                                (i == *chatInfo.say_cursor_pos) &&
                                (buffer[1] == ' ')) {
                                break;
                            }

                            string[j] = buffer[i];
                        }

                        matches =
                            UI_ClientNumbersFromString(
                                string, pids, MAX_CLIENTS);

                        if(matches == 0) {
                            //try one more time ignoring the cursor location
                            string[j-1] = '\0';
                            matches =
                                UI_ClientNumbersFromString(
                                    string, pids, MAX_CLIENTS);
                        } else {
                            //make sure that the cursor is at the end of the
                            //entered partial name
                            while(
                                buffer[i] &&
                                (UI_ClientNumbersFromString(
                                    string, pids, MAX_CLIENTS) > 0)) {
                                (*chatInfo.say_cursor_pos)++;
                                string[j] = buffer[i];
                                i++;
                                j++;
                            }
                        }

                        Q_strncpyz(
                            string_clean, string, sizeof(string_clean));
                        Q_CleanStr(string_clean);

                        if(matches > 1) {
                            char tab_completed_name[MAX_CVAR_VALUE_STRING] = "";
                            int  max_entered_partial_name_length =
                                strlen(string_clean);

                            for(i = 0; i < matches; i++) {
                                char  entered_partial_name[MAX_CVAR_VALUE_STRING];
                                char  clean_name[MAX_NAME_LENGTH] = {""};
                                int   entered_partial_name_length;
                                int   clean_name_length;
                                int   new_matches;
                                int   temp_pids[MAX_CLIENTS];

                                UI_CleanStr(
                                    uiInfo.playerNames[pids[i]], clean_name,
                                    sizeof(clean_name));
                                clean_name_length = strlen(clean_name);

                                //check if any of the matches can be completed
                                //without reducing the matches
                                if(
                                    (
                                        clean_name_length >
                                        max_entered_partial_name_length) &&
                                    (
                                        matches ==
                                            UI_ClientNumbersFromString(
                                                uiInfo.playerNames[pids[i]],
                                                temp_pids, MAX_CLIENTS))) {
                                    Q_strncpyz(
                                        tab_completed_name,
                                        uiInfo.playerNames[pids[i]],
                                        sizeof(tab_completed_name));
                                        max_entered_partial_name_length =
                                            clean_name_length;
                                }

                                //check if partial name entered into the buffer better matches
                                //this name
                                UI_PartialGetEnteredPartialNameFromBuffer(
                                    at_pos, buffer, clean_name,
                                    entered_partial_name,
                                    sizeof(entered_partial_name), NULL);
                                entered_partial_name_length =
                                    strlen(entered_partial_name);
                                new_matches =
                                    UI_ClientNumbersFromString(
                                        entered_partial_name, temp_pids,
                                        MAX_CLIENTS);

                                if(
                                    (
                                        entered_partial_name_length >
                                        max_entered_partial_name_length) &&
                                    new_matches &&
                                    (new_matches < matches)) {
                                    Q_strncpyz(
                                        tab_completed_name, entered_partial_name,
                                        sizeof(tab_completed_name));
                                    max_entered_partial_name_length = entered_partial_name_length;
                                    //reset the matches
                                    matches = new_matches;
                                    for(j = 0; j < matches; j++) {
                                        pids[j] = temp_pids[j];
                                    }
                                    i = 0;
                                } else if(
                                    clean_name_length >
                                    max_entered_partial_name_length) {
                                    char *s_ptr =
                                        strstr(clean_name, entered_partial_name);
                                    char *start = s_ptr;
                                    char partial_completion[MAX_CVAR_VALUE_STRING];

                                    //check if we can have a partial tab name
                                    //completion
                                    Q_strncpyz(
                                        partial_completion, entered_partial_name,
                                        sizeof(partial_completion));
                                    for(
                                        j = strlen(entered_partial_name);
                                        s_ptr[j]; j++) {
                                        partial_completion[j] = s_ptr[j];
                                        //don't change the matches
                                        if(
                                            matches !=
                                            UI_ClientNumbersFromString(
                                                partial_completion, temp_pids,
                                                MAX_CLIENTS)) {
                                            break;
                                        }
                                    }

                                    partial_completion[j] = '\0';

                                    //check if we can have a partial tab name 
                                    //completion to the left of the string
                                    for(
                                        s_ptr = start - 1;
                                        s_ptr - clean_name >= 0; s_ptr--) {
                                        int  partial_completion_length =
                                            strlen(partial_completion);
                                        char backup[MAX_CVAR_VALUE_STRING];

                                        memcpy(
                                            backup, partial_completion,
                                            sizeof(backup));

                                        memmove(
                                            &partial_completion[1],
                                            &partial_completion[0],
                                            partial_completion_length);

                                            partial_completion[0] = s_ptr[0];

                                            if(
                                                matches !=
                                                UI_ClientNumbersFromString(
                                                    partial_completion,
                                                    temp_pids, MAX_CLIENTS)) {
                                                //restore to the previous value
                                                //that still worked
                                                memcpy(
                                                    partial_completion, backup,
                                                    sizeof(partial_completion));
                                                break;
                                            }
                                    }

                                    if(
                                        strlen(partial_completion) >
                                            max_entered_partial_name_length) {
                                        max_entered_partial_name_length = j;
                                        Q_strncpyz(
                                            tab_completed_name,
                                            partial_completion,
                                            sizeof(tab_completed_name));
                                    }
                                }
                            }

                            if(tab_completed_name[0]) {
                                UI_TabCompleteName(
                                    "\0", buffer, tab_completed_name, at_pos, (matches <= 1));
                            }

                            //print the multiple possibile matches
                            for(i = 0; i < matches; i++) {
                                Com_Printf(
                                    "    %s\n", uiInfo.playerNames[pids[i]]);
                            }
                        } else if(matches == 1) {
                            UI_TabCompleteName(
                                string, buffer,
                                uiInfo.playerNames[pids[0]], at_pos, qtrue);
                        }
                    }
                }
            } else {
                switch (chatInfo.chat_mode) {
                    case CHAT_GLOBAL:
                        Menus_ReplaceActiveByName( "say" );
                        break;

                    case CHAT_TEAM:
                        Menus_ReplaceActiveByName( "say_team" );
                        break;

                    case CHAT_ADMINS:
                        Menus_ReplaceActiveByName( "say_admins" );
                        break;

                    case CHAT_CLAN:
                        Menus_ReplaceActiveByName( "say_clan" );
                        break;

                    case NUM_CHAT_MODES:
                        chatInfo.chat_mode = CHAT_GLOBAL;
                        trap_Cmd_ExecuteText( EXEC_APPEND, va( "say \"%s\"\n", buffer ) );
                        break;
                }
            }
        }
        else if (Q_stricmp(name, "playMovie") == 0)
        {
            if (uiInfo.previewMovie >= 0)
                trap_CIN_StopCinematic(uiInfo.previewMovie);

            trap_Cmd_ExecuteText(EXEC_APPEND, va("cinematic \"%s.roq\" 2\n", uiInfo.movieList[uiInfo.movieIndex]));
        }
        else if (Q_stricmp(name, "RunMod") == 0)
        {
            trap_Cvar_Set("fs_game", uiInfo.modList[uiInfo.modIndex].modName);
            trap_Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");
        }
        else if (Q_stricmp(name, "RunDemo") == 0)
        {
            if(uiInfo.demoList[uiInfo.demoIndex])
            {
                trap_Cmd_ExecuteText(EXEC_APPEND, va("demo \"%s\"\n", uiInfo.demoList[uiInfo.demoIndex]));
            } else {
                trap_Cvar_Set("com_demoErrorMessage", "No demo selected.");
                Menus_ActivateByName("demo_error_popmenu");
            }
        }
        else if (Q_stricmp(name, "Tremulous") == 0)
        {
            trap_Cvar_Set("fs_game", "");
            trap_Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");
        }
        else if (Q_stricmp(name, "closeJoin") == 0)
        {
            if (uiInfo.serverStatus.refreshActive)
            {
                UI_StopServerRefresh();
                uiInfo.serverStatus.nextDisplayRefresh = 0;
                uiInfo.nextServerStatusRefresh = 0;
                uiInfo.nextFindPlayerRefresh = 0;
                UI_BuildServerDisplayList(qtrue);
            }
            else
            {
                Menus_CloseByName("joinserver");
                Menus_ActivateByName("main");
            }
        }
        else if (Q_stricmp(name, "StopRefresh") == 0)
        {
            UI_StopServerRefresh();
            uiInfo.serverStatus.nextDisplayRefresh = 0;
            uiInfo.nextServerStatusRefresh = 0;
            uiInfo.nextFindPlayerRefresh = 0;
        }
        else if (Q_stricmp(name, "UpdateFilter") == 0)
        {
            if (ui_netSource.integer == AS_LOCAL)
                UI_StartServerRefresh(qtrue);

            UI_BuildServerDisplayList(qtrue);
            UI_FeederSelection(FEEDER_SERVERS, 0);
        }
        else if (Q_stricmp(name, "ServerStatus") == 0)
        {
            trap_LAN_GetServerAddressString(ui_netSource.integer,
                uiInfo.serverStatus.displayServers[uiInfo.serverStatus.currentServer], uiInfo.serverStatusAddress,
                sizeof(uiInfo.serverStatusAddress));
            UI_BuildServerStatus(qtrue);
        }
        else if (Q_stricmp(name, "FoundPlayerServerStatus") == 0)
        {
            Q_strncpyz(uiInfo.serverStatusAddress, uiInfo.foundPlayerServerAddresses[uiInfo.currentFoundPlayerServer],
                sizeof(uiInfo.serverStatusAddress));
            UI_BuildServerStatus(qtrue);
            Menu_SetFeederSelection(NULL, FEEDER_FINDPLAYER, 0, NULL);
        }
        else if (Q_stricmp(name, "FindPlayer") == 0)
        {
            UI_BuildFindPlayerList(qtrue);
            // clear the displayed server status info
            uiInfo.serverStatusInfo.numLines = 0;
            Menu_SetFeederSelection(NULL, FEEDER_FINDPLAYER, 0, NULL);
        }
        else if (Q_stricmp(name, "JoinServer") == 0)
        {
            if (uiInfo.serverStatus.currentServer >= 0 &&
                uiInfo.serverStatus.currentServer < uiInfo.serverStatus.numDisplayServers)
            {
                trap_LAN_GetServerAddressString(ui_netSource.integer,
                    uiInfo.serverStatus.displayServers[uiInfo.serverStatus.currentServer], buff, 1024);
                trap_Cmd_ExecuteText(EXEC_APPEND, va("connect %s\n", buff));
            }
        }
        else if (Q_stricmp(name, "FoundPlayerJoinServer") == 0)
        {
            if (uiInfo.currentFoundPlayerServer >= 0 && uiInfo.currentFoundPlayerServer < uiInfo.numFoundPlayerServers)
            {
                trap_Cmd_ExecuteText(EXEC_APPEND,
                    va("connect %s\n", uiInfo.foundPlayerServerAddresses[uiInfo.currentFoundPlayerServer]));
            }
        }
        else if (Q_stricmp(name, "Quit") == 0)
            trap_Cmd_ExecuteText(EXEC_APPEND, "quit\n");
        else if (Q_stricmp(name, "Leave") == 0)
        {
            trap_Cmd_ExecuteText(EXEC_APPEND, "disconnect\n");
            trap_Key_SetCatcher(KEYCATCH_UI);
            Menus_CloseAll();
            Menus_ActivateByName("main");
        }
        else if (Q_stricmp(name, "ServerSort") == 0)
        {
            int sortColumn;

            if (Int_Parse(args, &sortColumn))
            {
                // if same column we're already sorting on then flip the direction

                if (sortColumn == uiInfo.serverStatus.sortKey)
                    uiInfo.serverStatus.sortDir = !uiInfo.serverStatus.sortDir;

                // make sure we sort again
                UI_ServersSort(sortColumn, qtrue);

                uiInfo.serverStatus.sorted = qtrue;
            }
        }
        else if (Q_stricmp(name, "closeingame") == 0)
        {
            trap_Key_SetCatcher(trap_Key_GetCatcher() & ~KEYCATCH_UI);
            trap_Key_ClearStates();
            trap_Cvar_Set("cl_paused", "0");
            Menus_CloseAll();
        }
        else if (Q_stricmp(name, "voteMap") == 0)
        {
            if (ui_selectedMap.integer >= 0 && ui_selectedMap.integer < uiInfo.mapCount)
            {
                trap_Cmd_ExecuteText(
                    EXEC_APPEND, va("callvote map \"%s\"\n", uiInfo.mapList[ui_selectedMap.integer].mapLoadName));
            }
        }
        else if (Q_stricmp(name, "voteNextMap") == 0)
        {
            if (ui_selectedMap.integer >= 0 && ui_selectedMap.integer < uiInfo.mapCount)
            {
                trap_Cmd_ExecuteText(
                    EXEC_APPEND, va("callvote nextmap \"%s\"\n", uiInfo.mapList[ui_selectedMap.integer].mapLoadName));
            }
        }
        else if (Q_stricmp(name, "voteKick") == 0)
        {
            if (uiInfo.playerIndex >= 0 && uiInfo.playerIndex < uiInfo.playerCount)
            {
                char buffer[MAX_CVAR_VALUE_STRING];
                trap_Cvar_VariableStringBuffer("ui_reason", buffer, sizeof(buffer));

                trap_Cmd_ExecuteText(EXEC_APPEND, va("callvote kick %d%s\n", uiInfo.clientNums[uiInfo.playerIndex],
                                                      (buffer[0] ? va(" \"%s\"", buffer) : "")));
                trap_Cvar_Set("ui_reason", "");
            }
        }
        else if (Q_stricmp(name, "voteMute") == 0)
        {
            if (uiInfo.playerIndex >= 0 && uiInfo.playerIndex < uiInfo.playerCount)
            {
                char buffer[MAX_CVAR_VALUE_STRING];
                trap_Cvar_VariableStringBuffer("ui_reason", buffer, sizeof(buffer));

                trap_Cmd_ExecuteText(EXEC_APPEND, va("callvote mute %d%s\n", uiInfo.clientNums[uiInfo.playerIndex],
                                                      (buffer[0] ? va(" \"%s\"", buffer) : "")));
                trap_Cvar_Set("ui_reason", "");
            }
        }
        else if (Q_stricmp(name, "voteUnMute") == 0)
        {
            if (uiInfo.playerIndex >= 0 && uiInfo.playerIndex < uiInfo.playerCount)
            {
                trap_Cmd_ExecuteText(EXEC_APPEND, va("callvote unmute %d\n", uiInfo.clientNums[uiInfo.playerIndex]));
            }
        }
        else if (Q_stricmp(name, "voteTeamKick") == 0)
        {
            if (uiInfo.teamPlayerIndex >= 0 && uiInfo.teamPlayerIndex < uiInfo.myTeamCount)
            {
                char buffer[MAX_CVAR_VALUE_STRING];
                trap_Cvar_VariableStringBuffer("ui_reason", buffer, sizeof(buffer));

                trap_Cmd_ExecuteText(
                    EXEC_APPEND, va("callteamvote kick %d%s\n", uiInfo.teamClientNums[uiInfo.teamPlayerIndex],
                                     (buffer[0] ? va(" \"%s\"", buffer) : "")));
                trap_Cvar_Set("ui_reason", "");
            }
        }
        else if (Q_stricmp(name, "voteTeamDenyBuild") == 0)
        {
            if (uiInfo.teamPlayerIndex >= 0 && uiInfo.teamPlayerIndex < uiInfo.myTeamCount)
            {
                char buffer[MAX_CVAR_VALUE_STRING];
                trap_Cvar_VariableStringBuffer("ui_reason", buffer, sizeof(buffer));

                trap_Cmd_ExecuteText(
                    EXEC_APPEND, va("callteamvote denybuild %d%s\n", uiInfo.teamClientNums[uiInfo.teamPlayerIndex],
                                     (buffer[0] ? va(" \"%s\"", buffer) : "")));
                trap_Cvar_Set("ui_reason", "");
            }
        }
        else if (Q_stricmp(name, "voteTeamAllowBuild") == 0)
        {
            if (uiInfo.teamPlayerIndex >= 0 && uiInfo.teamPlayerIndex < uiInfo.myTeamCount)
            {
                trap_Cmd_ExecuteText(
                    EXEC_APPEND, va("callteamvote allowbuild %d\n", uiInfo.teamClientNums[uiInfo.teamPlayerIndex]));
            }
        }
        else if (Q_stricmp(name, "addFavorite") == 0)
        {
            if (ui_netSource.integer != AS_FAVORITES)
            {
                char name[MAX_NAME_LENGTH];
                char addr[MAX_NAME_LENGTH];
                int res;

                trap_LAN_GetServerInfo(ui_netSource.integer,
                    uiInfo.serverStatus.displayServers[uiInfo.serverStatus.currentServer], buff, MAX_STRING_CHARS);
                name[0] = addr[0] = '\0';
                Q_strncpyz(name, Info_ValueForKey(buff, "hostname"), MAX_NAME_LENGTH);
                Q_strncpyz(addr, Info_ValueForKey(buff, "addr"), MAX_NAME_LENGTH);

                if (strlen(name) > 0 && strlen(addr) > 0)
                {
                    res = trap_LAN_AddServer(AS_FAVORITES, name, addr);

                    if (res == 0)
                    {
                        // server already in the list
                        Com_Printf("Favorite already in list\n");
                    }
                    else if (res == -1)
                    {
                        // list full
                        Com_Printf("Favorite list full\n");
                    }
                    else
                    {
                        // successfully added
                        Com_Printf("Added favorite server %s\n", addr);
                    }
                }
            }
        }
        else if (Q_stricmp(name, "deleteFavorite") == 0)
        {
            if (ui_netSource.integer == AS_FAVORITES)
            {
                char addr[MAX_NAME_LENGTH];
                trap_LAN_GetServerInfo(ui_netSource.integer,
                    uiInfo.serverStatus.displayServers[uiInfo.serverStatus.currentServer], buff, MAX_STRING_CHARS);
                addr[0] = '\0';
                Q_strncpyz(addr, Info_ValueForKey(buff, "addr"), MAX_NAME_LENGTH);

                if (strlen(addr) > 0)
                    trap_LAN_RemoveServer(AS_FAVORITES, addr);
            }
        }
        else if (Q_stricmp(name, "createFavorite") == 0)
        {
            if (ui_netSource.integer == AS_FAVORITES)
            {
                char name[MAX_NAME_LENGTH];
                char addr[MAX_NAME_LENGTH];
                int res;

                name[0] = addr[0] = '\0';
                Q_strncpyz(name, UI_Cvar_VariableString("ui_favoriteName"), MAX_NAME_LENGTH);
                Q_strncpyz(addr, UI_Cvar_VariableString("ui_favoriteAddress"), MAX_NAME_LENGTH);

                if (strlen(name) > 0 && strlen(addr) > 0)
                {
                    res = trap_LAN_AddServer(AS_FAVORITES, name, addr);

                    if (res == 0)
                    {
                        // server already in the list
                        Com_Printf("Favorite already in list\n");
                    }
                    else if (res == -1)
                    {
                        // list full
                        Com_Printf("Favorite list full\n");
                    }
                    else
                    {
                        // successfully added
                        Com_Printf("Added favorite server %s\n", addr);
                    }
                }
            }
        }
        else if (Q_stricmp(name, "glCustom") == 0)
            trap_Cvar_Set("ui_glCustom", "4");
        else if (Q_stricmp(name, "update") == 0)
        {
            if (String_Parse(args, &name2))
                UI_Update(name2);
        }
        else if (Q_stricmp(name, "InitIgnoreList") == 0)
            UI_BuildPlayerList();
        else if (Q_stricmp(name, "ToggleIgnore") == 0)
        {
            if (uiInfo.ignoreIndex >= 0 && uiInfo.ignoreIndex < uiInfo.playerCount)
            {
                if (Com_ClientListContains(
                        &uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]))
                {
                    Com_ClientListRemove(
                        &uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]);
                    trap_Cmd_ExecuteText(EXEC_APPEND, va("unignore %i\n", uiInfo.clientNums[uiInfo.ignoreIndex]));
                }
                else
                {
                    Com_ClientListAdd(&uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]);
                    trap_Cmd_ExecuteText(EXEC_APPEND, va("ignore %i\n", uiInfo.clientNums[uiInfo.ignoreIndex]));
                }
            }
        }
        else if (Q_stricmp(name, "IgnorePlayer") == 0)
        {
            if (uiInfo.ignoreIndex >= 0 && uiInfo.ignoreIndex < uiInfo.playerCount)
            {
                if (!Com_ClientListContains(
                        &uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]))
                {
                    Com_ClientListAdd(&uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]);
                    trap_Cmd_ExecuteText(EXEC_APPEND, va("ignore %i\n", uiInfo.clientNums[uiInfo.ignoreIndex]));
                }
            }
        }
        else if (Q_stricmp(name, "UnIgnorePlayer") == 0)
        {
            if (uiInfo.ignoreIndex >= 0 && uiInfo.ignoreIndex < uiInfo.playerCount)
            {
                if (Com_ClientListContains(
                        &uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]))
                {
                    Com_ClientListRemove(
                        &uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[uiInfo.ignoreIndex]);
                    trap_Cmd_ExecuteText(EXEC_APPEND, va("unignore %i\n", uiInfo.clientNums[uiInfo.ignoreIndex]));
                }
            }
        }
        else
            Com_Printf("unknown UI script %s\n", name);
    }

    key_pressed_onCharEntry = K_NONE;
}

static int UI_FeederInitialise(int feederID);

/*
==================
UI_FeederCount
==================
*/
static int UI_FeederCount(int feederID)
{
    if (feederID == FEEDER_CINEMATICS)
        return uiInfo.movieCount;
    else if (feederID == FEEDER_MAPS)
        return uiInfo.mapCount;
    else if (feederID == FEEDER_SERVERS)
        return uiInfo.serverStatus.numDisplayServers;
    else if (feederID == FEEDER_SERVERSTATUS)
        return uiInfo.serverStatusInfo.numLines;
    else if (feederID == FEEDER_NEWS)
        return uiInfo.newsInfo.numLines;
    else if (feederID == FEEDER_GITHUB_RELEASE)
        return uiInfo.githubRelease.numLines;
    else if (feederID == FEEDER_FINDPLAYER)
        return uiInfo.numFoundPlayerServers;
    else if (feederID == FEEDER_PLAYER_LIST)
    {
        if (uiInfo.uiDC.realTime > uiInfo.playerRefresh)
        {
            uiInfo.playerRefresh = uiInfo.uiDC.realTime + 3000;
            UI_BuildPlayerList();
        }

        return uiInfo.playerCount;
    }
    else if (feederID == FEEDER_TEAM_LIST)
    {
        if (uiInfo.uiDC.realTime > uiInfo.playerRefresh)
        {
            uiInfo.playerRefresh = uiInfo.uiDC.realTime + 3000;
            UI_BuildPlayerList();
        }

        return uiInfo.myTeamCount;
    }
    else if (feederID == FEEDER_IGNORE_LIST)
        return uiInfo.playerCount;
    else if (feederID == FEEDER_HELP_LIST)
        return uiInfo.helpCount;
    else if (feederID == FEEDER_MODS)
        return uiInfo.modCount;
    else if (feederID == FEEDER_DEMOS)
        return uiInfo.demoCount;
    else if (feederID == FEEDER_TREMTEAMS)
        return uiInfo.teamCount;
    else if (feederID == FEEDER_TREMHUMANITEMS)
        return uiInfo.humanItemCount;
    else if (feederID == FEEDER_TREMALIENCLASSES)
        return uiInfo.alienClassCount;
    else if (feederID == FEEDER_TREMHUMANARMOURYBUY)
        return uiInfo.humanArmouryBuyCount;
    else if (feederID == FEEDER_TREMHUMANARMOURYSELL)
        return uiInfo.humanArmourySellCount;
    else if (feederID == FEEDER_TREMALIENUPGRADE)
        return uiInfo.alienUpgradeCount;
    else if (feederID == FEEDER_TREMALIENBUILD)
        return uiInfo.alienBuildCount;
    else if (feederID == FEEDER_TREMHUMANBUILD)
        return uiInfo.humanBuildCount;
    else if (feederID == FEEDER_RESOLUTIONS)
    {
        if (UI_FeederInitialise(feederID) == uiInfo.numResolutions)
            return uiInfo.numResolutions + 1;
        else
            return uiInfo.numResolutions;
    }
    else if (feederID == FEEDER_TREMVOICECMD)
        return uiInfo.voiceCmdCount;

    return 0;
}

static const char *UI_SelectedMap(int index, int *actual)
{
    int i, c;
    c = 0;
    *actual = 0;

    for (i = 0; i < uiInfo.mapCount; i++)
    {
        if (c == index)
        {
            *actual = i;
            return uiInfo.mapList[i].mapName;
        }
        else
            c++;
    }

    return "";
}

static int GCD(int a, int b)
{
    int c;

    while (b != 0)
    {
        c = a % b;
        a = b;
        b = c;
    }

    return a;
}

static const char *UI_DisplayAspectString(int w, int h)
{
    int gcd = GCD(w, h);

    w /= gcd;
    h /= gcd;

    // For some reason 8:5 is usually referred to as 16:10
    if (w == 8 && h == 5)
    {
        w = 16;
        h = 10;
    }

    return va("%d:%d", w, h);
}

static const char *UI_FeederItemText(int feederID, int index, int column, qhandle_t *handle)
{
    if (handle)
        *handle = -1;

    if (feederID == FEEDER_MAPS)
    {
        int actual;
        return UI_SelectedMap(index, &actual);
    }
    else if (feederID == FEEDER_SERVERS)
    {
        if (index >= 0 && index < UI_FeederCount(feederID))
        {
            static char info[MAX_STRING_CHARS];
            static char clientBuff[32];
            static char cleaned[MAX_STRING_CHARS];
            static int lastColumn = -1;
            static int lastTime = 0;
            int ping;
            char *cleanedcpy;

            if (lastColumn != column || lastTime > uiInfo.uiDC.realTime + 5000)
            {
                trap_LAN_GetServerInfo(
                    ui_netSource.integer, uiInfo.serverStatus.displayServers[index], info, MAX_STRING_CHARS);
                lastColumn = column;
                lastTime = uiInfo.uiDC.realTime;
            }

            ping = atoi(Info_ValueForKey(info, "ping"));

            UI_EscapeEmoticons(cleaned, Info_ValueForKey(info, "hostname"), sizeof(cleaned));

            switch (column)
            {
                case SORT_HOST:
                    if (ping <= 0)
                        return Info_ValueForKey(info, "addr");
                    else
                    {
                        static char hostname[1024];

                        // Strip leading whitespace
                        cleanedcpy = cleaned;
                        while (*cleanedcpy != '\0' && isspace(*cleanedcpy))
                            cleanedcpy++;

                        if (ui_netSource.integer == AS_LOCAL)
                        {
                            Com_sprintf(hostname, sizeof(hostname), "^7%s [%s]", cleaned,
                                netnames[atoi(Info_ValueForKey(info, "nettype"))]);
                            return hostname;
                        }
                        else
                        {
                            char *label;

                            label = Info_ValueForKey(info, "label");
                            if (label[0])
                            {
                                // First char of the label response is a sorting tag. Skip it.
                                label += 1;

                                Com_sprintf(hostname, sizeof(hostname), "^7%s %s", label, cleaned);
                            }
                            else
                            {
                                Com_sprintf(hostname, sizeof(hostname), "^7%s", cleaned);
                            }

                            return hostname;
                        }
                    }

                case SORT_GAME:
                    return Info_ValueForKey(info, "game");

                case SORT_MAP:
                    return Info_ValueForKey(info, "mapname");

                case SORT_CLIENTS:
                    Com_sprintf(clientBuff, sizeof(clientBuff), "%s (%s)", Info_ValueForKey(info, "clients"),
                        Info_ValueForKey(info, "sv_maxclients"));
                    return clientBuff;

                case SORT_PING:
                    if (ping <= 0)
                        return "...";
                    else
                        return Info_ValueForKey(info, "ping");
            }
        }
    }
    else if (feederID == FEEDER_SERVERSTATUS)
    {
        if (index >= 0 && index < uiInfo.serverStatusInfo.numLines)
        {
            if (column >= 0 && column < 4)
                return uiInfo.serverStatusInfo.lines[index][column];
        }
    }
    else if (feederID == FEEDER_NEWS)
    {
        if (index >= 0 && index < uiInfo.newsInfo.numLines)
            return uiInfo.newsInfo.text[index];
    }
    else if (feederID == FEEDER_GITHUB_RELEASE)
    {
        if (index >= 0 && index < uiInfo.githubRelease.numLines)
            return uiInfo.githubRelease.text[index];
    }
    else if (feederID == FEEDER_FINDPLAYER)
    {
        if (index >= 0 && index < uiInfo.numFoundPlayerServers)
            return uiInfo.foundPlayerServerNames[index];
    }
    else if (feederID == FEEDER_PLAYER_LIST)
    {
        if (index >= 0 && index < uiInfo.playerCount)
            return uiInfo.playerNames[index];
    }
    else if (feederID == FEEDER_TEAM_LIST)
    {
        if (index >= 0 && index < uiInfo.myTeamCount)
            return uiInfo.teamNames[index];
    }
    else if (feederID == FEEDER_IGNORE_LIST)
    {
        if (index >= 0 && index < uiInfo.playerCount)
        {
            switch (column)
            {
                case 1:
                    // am I ignoring him
                    return Com_ClientListContains(&uiInfo.ignoreList[uiInfo.myPlayerIndex], uiInfo.clientNums[index])
                               ? "X"
                               : "";

                case 2:
                    // is he ignoring me
                    return Com_ClientListContains(&uiInfo.ignoreList[index], uiInfo.playerNumber) ? "X" : "";

                default:
                    return uiInfo.playerNames[index];
            }
        }
    }
    else if (feederID == FEEDER_HELP_LIST)
    {
        if (index >= 0 && index < uiInfo.helpCount)
            return uiInfo.helpList[index].text;
    }
    else if (feederID == FEEDER_MODS)
    {
        if (index >= 0 && index < uiInfo.modCount)
        {
            if (uiInfo.modList[index].modDescr && *uiInfo.modList[index].modDescr)
                return uiInfo.modList[index].modDescr;
            else
                return uiInfo.modList[index].modName;
        }
    }
    else if (feederID == FEEDER_CINEMATICS)
    {
        if (index >= 0 && index < uiInfo.movieCount)
            return uiInfo.movieList[index];
    }
    else if (feederID == FEEDER_DEMOS)
    {
        if (index >= 0 && index < uiInfo.demoCount)
            return uiInfo.demoList[index];
    }
    else if (feederID == FEEDER_TREMTEAMS)
    {
        if (index >= 0 && index < uiInfo.teamCount)
            return uiInfo.teamList[index].text;
    }
    else if (feederID == FEEDER_TREMHUMANITEMS)
    {
        if (index >= 0 && index < uiInfo.humanItemCount)
            return uiInfo.humanItemList[index].text;
    }
    else if (feederID == FEEDER_TREMALIENCLASSES)
    {
        if (index >= 0 && index < uiInfo.alienClassCount)
            return uiInfo.alienClassList[index].text;
    }
    else if (feederID == FEEDER_TREMHUMANARMOURYBUY)
    {
        if (index >= 0 && index < uiInfo.humanArmouryBuyCount)
            return uiInfo.humanArmouryBuyList[index].text;
    }
    else if (feederID == FEEDER_TREMHUMANARMOURYSELL)
    {
        if (index >= 0 && index < uiInfo.humanArmourySellCount)
        {
            // If conflicting with selected
            if (
                (
                    uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].type == INFOTYPE_WEAPON ?
                    BG_Weapon(uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].v.weapon)->slots :
                    BG_Upgrade(uiInfo.humanArmouryBuyList[uiInfo.humanArmouryBuyIndex].v.upgrade)->slots
                ) & (
                    uiInfo.humanArmourySellList[index].type == INFOTYPE_WEAPON ?
                    BG_Weapon(uiInfo.humanArmourySellList[index].v.weapon)->slots :
                    BG_Upgrade(uiInfo.humanArmourySellList[index].v.upgrade)->slots
                )
            )
                return (va("[!] %s", uiInfo.humanArmourySellList[index].text));
            else
                return uiInfo.humanArmourySellList[index].text;
        }
    }
    else if (feederID == FEEDER_TREMALIENUPGRADE)
    {
        if (index >= 0 && index < uiInfo.alienUpgradeCount)
            return uiInfo.alienUpgradeList[index].text;
    }
    else if (feederID == FEEDER_TREMALIENBUILD)
    {
        if (index >= 0 && index < uiInfo.alienBuildCount)
            return uiInfo.alienBuildList[index].text;
    }
    else if (feederID == FEEDER_TREMHUMANBUILD)
    {
        if (index >= 0 && index < uiInfo.humanBuildCount)
            return uiInfo.humanBuildList[index].text;
    }
    else if (feederID == FEEDER_RESOLUTIONS)
    {
        static char resolution[MAX_STRING_CHARS];
        int w, h;

        if (index >= 0 && index < uiInfo.numResolutions)
        {
            w = uiInfo.resolutions[index].w;
            h = uiInfo.resolutions[index].h;

            if (w == 0 && h == 0)
              Com_sprintf(resolution, sizeof(resolution), "Automatic");
            else
              Com_sprintf(resolution, sizeof(resolution), "%dx%d (%s)", w, h, UI_DisplayAspectString(w, h));

            return resolution;
        }

        w = (int)trap_Cvar_VariableValue("r_width");
        h = (int)trap_Cvar_VariableValue("r_height");
        Com_sprintf(resolution, sizeof(resolution), "Custom (%dx%d)", w, h);

        return resolution;
    }
    else if (feederID == FEEDER_TREMVOICECMD)
    {
        if (index >= 0 && index < uiInfo.voiceCmdCount)
            return uiInfo.voiceCmdList[index].text;
    }

    return "";
}

static qhandle_t UI_FeederItemImage(int feederID, int index)
{
    if (feederID == FEEDER_MAPS)
    {
        int actual;
        UI_SelectedMap(index, &actual);
        index = actual;

        if (index >= 0 && index < uiInfo.mapCount)
        {
            if (uiInfo.mapList[index].levelShot == -1)
                uiInfo.mapList[index].levelShot = trap_R_RegisterShaderNoMip(uiInfo.mapList[index].imageName);

            return uiInfo.mapList[index].levelShot;
        }
    }

    return 0;
}

static void UI_FeederSelection(int feederID, int index)
{
    static char info[MAX_STRING_CHARS];

    if (feederID == FEEDER_MAPS)
    {
        int actual, map;
        map = ui_selectedMap.integer;

        if (uiInfo.mapList[map].cinematic >= 0)
        {
            trap_CIN_StopCinematic(uiInfo.mapList[map].cinematic);
            uiInfo.mapList[map].cinematic = -1;
        }

        UI_SelectedMap(index, &actual);

        ui_selectedMap.integer = actual;
        trap_Cvar_Set("ui_selectedMap", va("%d", actual));
        uiInfo.mapList[ui_selectedMap.integer].cinematic = trap_CIN_PlayCinematic(
            va("%s.roq", uiInfo.mapList[ui_selectedMap.integer].mapLoadName), 0, 0, 0, 0, (CIN_loop | CIN_silent));
    }
    else if (feederID == FEEDER_SERVERS)
    {
        const char *mapName = NULL;

        uiInfo.serverStatus.currentServer = index;
        trap_LAN_GetServerInfo(ui_netSource.integer, uiInfo.serverStatus.displayServers[index], info, MAX_STRING_CHARS);
        uiInfo.serverStatus.currentServerPreview =
            trap_R_RegisterShaderNoMip(va("levelshots/%s", Info_ValueForKey(info, "mapname")));

        if (uiInfo.serverStatus.currentServerCinematic >= 0)
        {
            trap_CIN_StopCinematic(uiInfo.serverStatus.currentServerCinematic);
            uiInfo.serverStatus.currentServerCinematic = -1;
        }

        mapName = Info_ValueForKey(info, "mapname");

        if (mapName && *mapName)
        {
            uiInfo.serverStatus.currentServerCinematic =
                trap_CIN_PlayCinematic(va("%s.roq", mapName), 0, 0, 0, 0, (CIN_loop | CIN_silent));
        }
    }
    else if (feederID == FEEDER_SERVERSTATUS)
    {
    }
    else if (feederID == FEEDER_FINDPLAYER)
    {
        uiInfo.currentFoundPlayerServer = index;
        //

        if (index < uiInfo.numFoundPlayerServers - 1)
        {
            // build a new server status for this server
            Q_strncpyz(uiInfo.serverStatusAddress, uiInfo.foundPlayerServerAddresses[uiInfo.currentFoundPlayerServer],
                sizeof(uiInfo.serverStatusAddress));
            Menu_SetFeederSelection(NULL, FEEDER_SERVERSTATUS, 0, NULL);
            UI_BuildServerStatus(qtrue);
        }
    }
    else if (feederID == FEEDER_PLAYER_LIST)
        uiInfo.playerIndex = index;
    else if (feederID == FEEDER_TEAM_LIST)
        uiInfo.teamPlayerIndex = index;
    else if (feederID == FEEDER_IGNORE_LIST)
        uiInfo.ignoreIndex = index;
    else if (feederID == FEEDER_HELP_LIST)
        uiInfo.helpIndex = index;
    else if (feederID == FEEDER_MODS)
        uiInfo.modIndex = index;
    else if (feederID == FEEDER_CINEMATICS)
    {
        uiInfo.movieIndex = index;

        if (uiInfo.previewMovie >= 0)
            trap_CIN_StopCinematic(uiInfo.previewMovie);

        uiInfo.previewMovie = -1;
    }
    else if (feederID == FEEDER_DEMOS)
        uiInfo.demoIndex = index;
    else if (feederID == FEEDER_TREMTEAMS)
        uiInfo.teamIndex = index;
    else if (feederID == FEEDER_TREMHUMANITEMS)
        uiInfo.humanItemIndex = index;
    else if (feederID == FEEDER_TREMALIENCLASSES)
        uiInfo.alienClassIndex = index;
    else if (feederID == FEEDER_TREMHUMANARMOURYBUY)
        uiInfo.humanArmouryBuyIndex = index;
    else if (feederID == FEEDER_TREMHUMANARMOURYSELL)
        uiInfo.humanArmourySellIndex = index;
    else if (feederID == FEEDER_TREMALIENUPGRADE)
        uiInfo.alienUpgradeIndex = index;
    else if (feederID == FEEDER_TREMALIENBUILD)
        uiInfo.alienBuildIndex = index;
    else if (feederID == FEEDER_TREMHUMANBUILD)
        uiInfo.humanBuildIndex = index;
    else if (feederID == FEEDER_RESOLUTIONS)
    {
        if (index >= 0 && index < uiInfo.numResolutions)
        {
            trap_Cvar_Set("r_width", va("%d", uiInfo.resolutions[index].w));
            trap_Cvar_Set("r_height", va("%d", uiInfo.resolutions[index].h));
        }

        uiInfo.resolutionIndex = index;
    }
    else if (feederID == FEEDER_TREMVOICECMD)
        uiInfo.voiceCmdIndex = index;
}

static int UI_FeederInitialise(int feederID)
{
    if (feederID == FEEDER_RESOLUTIONS)
    {
        int i;
        int w = trap_Cvar_VariableValue("r_width");
        int h = trap_Cvar_VariableValue("r_height");

        for (i = 0; i < uiInfo.numResolutions; i++)
        {
            if (w == uiInfo.resolutions[i].w && h == uiInfo.resolutions[i].h)
                return i;
        }

        return uiInfo.numResolutions;
    }

    return 0;
}

static void UI_Pause(qboolean b)
{
    if (b)
    {
        // pause the game and set the ui keycatcher
        trap_Cvar_Set("cl_paused", "1");
        trap_Key_SetCatcher(KEYCATCH_UI);
    }
    else
    {
        // unpause the game and clear the ui keycatcher
        trap_Key_SetCatcher(trap_Key_GetCatcher() & ~KEYCATCH_UI);
        trap_Key_ClearStates();
        trap_Cvar_Set("cl_paused", "0");
    }
}

static int UI_PlayCinematic(const char *name, float x, float y, float w, float h)
{
    return trap_CIN_PlayCinematic(name, x, y, w, h, (CIN_loop | CIN_silent));
}

static void UI_StopCinematic(int handle)
{
    if (handle >= 0)
        trap_CIN_StopCinematic(handle);
    else
    {
        handle = abs(handle);

        if (handle == UI_NETMAPCINEMATIC)
        {
            if (uiInfo.serverStatus.currentServerCinematic >= 0)
            {
                trap_CIN_StopCinematic(uiInfo.serverStatus.currentServerCinematic);
                uiInfo.serverStatus.currentServerCinematic = -1;
            }
        }
    }
}

static void UI_DrawCinematic(int handle, float x, float y, float w, float h)
{
    trap_CIN_SetExtents(handle, x, y, w, h);
    trap_CIN_DrawCinematic(handle);
}

static void UI_RunCinematicFrame(int handle) { trap_CIN_RunCinematic(handle); }

static float UI_GetValue(int ownerDraw) { return 0.0f; }

/*
=================
UI_ParseResolutions
=================
*/
void UI_ParseResolutions(void)
{
    char buf[MAX_STRING_CHARS];
    char w[16], h[16];
    char *p;
    const char *out;
    char *s = NULL;

    trap_Cvar_VariableStringBuffer("r_availableModes", buf, sizeof(buf));
    p = buf;
    uiInfo.numResolutions = 0;

    while (uiInfo.numResolutions < MAX_RESOLUTIONS && String_Parse(&p, &out))
    {
        Q_strncpyz(w, out, sizeof(w));
        s = strchr(w, 'x');
        if (!s)
            return;

        *s++ = '\0';
        Q_strncpyz(h, s, sizeof(h));

        uiInfo.resolutions[uiInfo.numResolutions].w = atoi(w);
        uiInfo.resolutions[uiInfo.numResolutions].h = atoi(h);
        uiInfo.numResolutions++;
    }
}

/*
=================
UI_Init
=================
*/
void UI_Init(qboolean inGameLoad)
{
    BG_InitClassConfigs();
    BG_InitBuildableConfigs();
    BG_InitAllowedGameElements();

    uiInfo.inGameLoad = inGameLoad;

    UI_RegisterCvars();
    UI_InitMemory();
    BG_InitMemory();  // FIXIT-M: Merge with UI_InitMemory or something

    ctrl_held = qfalse;

    chatInfo.chat_mode = CHAT_GLOBAL;
    chatInfo.chat_mode_blink_time = 0;

    memset(chatInfo.say_unsubmitted_line, 0, sizeof(chatInfo.say_unsubmitted_line));
    memset(
        chatInfo.say_history_lines, 0,
        sizeof(chatInfo.say_history_lines[0][0]) *
        MAX_SAY_HISTORY_LINES * MAX_CVAR_VALUE_STRING);
    chatInfo.say_make_current_line_blank = qfalse;
    chatInfo.say_history_current = qtrue;
    chatInfo.nextHistoryLine = 0;
    chatInfo.historyLine = 0;

    // cache redundant calulations
    trap_GetGlconfig(&uiInfo.uiDC.glconfig);

    // for 640x480 virtualized screen
    uiInfo.uiDC.yscale = uiInfo.uiDC.glconfig.vidHeight * (1.0f / 480.0f);
    uiInfo.uiDC.xscale = uiInfo.uiDC.glconfig.vidWidth * (1.0f / 640.0f);

    // wide screen
    uiInfo.uiDC.aspectScale = ((640.0f * uiInfo.uiDC.glconfig.vidHeight) / (480.0f * uiInfo.uiDC.glconfig.vidWidth));

    uiInfo.uiDC.smallFontScale = trap_Cvar_VariableValue("ui_smallFont");
    uiInfo.uiDC.bigFontScale = trap_Cvar_VariableValue("ui_bigFont");

    uiInfo.uiDC.registerShaderNoMip = &trap_R_RegisterShaderNoMip;
    uiInfo.uiDC.setColor = &UI_SetColor;
    uiInfo.uiDC.drawHandlePic = &UI_DrawHandlePic;
    uiInfo.uiDC.drawStretchPic = &trap_R_DrawStretchPic;
    uiInfo.uiDC.registerModel = &trap_R_RegisterModel;
    uiInfo.uiDC.registerSkin = &trap_R_RegisterSkin;
    uiInfo.uiDC.modelBounds = &trap_R_ModelBounds;
    uiInfo.uiDC.lerpTag = &trap_CM_LerpTag;
    uiInfo.uiDC.fillRect = &UI_FillRect;
    uiInfo.uiDC.fillRoundedRect = &UI_FillRoundedRect;
    uiInfo.uiDC.drawRect = &UI_DrawRect;
    uiInfo.uiDC.drawRoundedRect = &UI_DrawRoundedRect;
    uiInfo.uiDC.drawSides = &UI_DrawSides;
    uiInfo.uiDC.drawTopBottom = &UI_DrawTopBottom;
    uiInfo.uiDC.clearScene = &trap_R_ClearScene;
    uiInfo.uiDC.addRefEntityToScene = &trap_R_AddRefEntityToScene;
    uiInfo.uiDC.renderScene = &trap_R_RenderScene;
    uiInfo.uiDC.registerFont = &trap_R_RegisterFont;
    uiInfo.uiDC.ownerDrawItem = &UI_OwnerDraw;
    uiInfo.uiDC.getValue = &UI_GetValue;
    uiInfo.uiDC.ownerDrawVisible = &UI_OwnerDrawVisible;
    uiInfo.uiDC.runScript = &UI_RunMenuScript;
    uiInfo.uiDC.setCVar = trap_Cvar_Set;
    uiInfo.uiDC.resetCVar = trap_Cvar_Reset;
    uiInfo.uiDC.getCVarString = trap_Cvar_VariableStringBuffer;
    uiInfo.uiDC.getCVarValue = trap_Cvar_VariableValue;
    uiInfo.uiDC.setOverstrikeMode = &trap_Key_SetOverstrikeMode;
    uiInfo.uiDC.getOverstrikeMode = &trap_Key_GetOverstrikeMode;
    uiInfo.uiDC.startLocalSound = &trap_S_StartLocalSound;
    uiInfo.uiDC.ownerDrawHandleKey = &UI_OwnerDrawHandleKey;
    uiInfo.uiDC.feederCount = &UI_FeederCount;
    uiInfo.uiDC.feederItemImage = &UI_FeederItemImage;
    uiInfo.uiDC.feederItemText = &UI_FeederItemText;
    uiInfo.uiDC.feederSelection = &UI_FeederSelection;
    uiInfo.uiDC.feederInitialise = &UI_FeederInitialise;
    uiInfo.uiDC.setBinding = &trap_Key_SetBinding;
    uiInfo.uiDC.getBindingBuf = &trap_Key_GetBindingBuf;
    uiInfo.uiDC.keynumToStringBuf = &trap_Key_KeynumToStringBuf;
    uiInfo.uiDC.executeText = &trap_Cmd_ExecuteText;
    uiInfo.uiDC.Error = &Com_Error;
    uiInfo.uiDC.Print = &Com_Printf;
    uiInfo.uiDC.Pause = &UI_Pause;
    uiInfo.uiDC.ownerDrawWidth = &UI_OwnerDrawWidth;
    uiInfo.uiDC.ownerDrawText = &UI_OwnerDrawText;
    uiInfo.uiDC.registerSound = &trap_S_RegisterSound;
    uiInfo.uiDC.startBackgroundTrack = &trap_S_StartBackgroundTrack;
    uiInfo.uiDC.stopBackgroundTrack = &trap_S_StopBackgroundTrack;
    uiInfo.uiDC.playCinematic = &UI_PlayCinematic;
    uiInfo.uiDC.stopCinematic = &UI_StopCinematic;
    uiInfo.uiDC.drawCinematic = &UI_DrawCinematic;
    uiInfo.uiDC.runCinematicFrame = &UI_RunCinematicFrame;
    uiInfo.uiDC.Bucket_Create_Bucket = BG_Bucket_Create_Bucket;
    uiInfo.uiDC.Bucket_Delete_Bucket = BG_Bucket_Delete_Bucket;
    uiInfo.uiDC.Bucket_Destroy_All_Buckets = BG_Bucket_Destroy_All_Buckets;
    uiInfo.uiDC.Bucket_Add_Item_To_Bucket = BG_Bucket_Add_Item_To_Bucket;
    uiInfo.uiDC.Bucket_Remove_Item_From_Bucket = BG_Bucket_Remove_Item_From_Bucket;
    uiInfo.uiDC.Bucket_Select_A_Random_Item = BG_Bucket_Select_A_Random_Item;
    uiInfo.uiDC.Bucket_Select_A_Specific_Item = BG_Bucket_Select_A_Specific_Item;
    uiInfo.uiDC.FS_GetFileList = trap_FS_GetFileList;

    Init_Display(&uiInfo.uiDC);

    String_Init();

    uiInfo.uiDC.whiteShader = trap_R_RegisterShaderNoMip("white");

    AssetCache();

    UI_LoadMenus("ui/menus.txt", qtrue);
    UI_LoadMenus("ui/ingame.txt", qfalse);
    UI_LoadMenus("ui/tremulous.txt", qfalse);
    UI_LoadHelp("ui/help.txt");

    Menus_CloseAll();

    trap_LAN_LoadCachedServers();

    // sets defaults for ui temp cvars
    trap_Cvar_Set("ui_mousePitch", (trap_Cvar_VariableValue("m_pitch") >= 0) ? "0" : "1");

    uiInfo.serverStatus.currentServerCinematic = -1;
    uiInfo.previewMovie = -1;

    UI_ParseResolutions();
    uiInfo.voices = BG_VoiceInit();
}

/*
=================
UI_KeyEvent
=================
*/
void UI_KeyEvent(int key, qboolean down)
{
    if (Menu_Count() > 0)
    {
        menuDef_t *menu = Menu_GetFocused();

        if(key == K_CTRL) {
            if(down) {
                ctrl_held = qtrue;
            } else {
                ctrl_held = qfalse;
            }
        }

        if (menu)
        {
            if (key == K_ESCAPE && down && !Menus_AnyFullScreenVisible())
                Menus_CloseAll();
            else
                Menu_HandleKey(menu, key, down);
        }
        else
        {
            trap_Key_SetCatcher(trap_Key_GetCatcher() & ~KEYCATCH_UI);
            trap_Key_ClearStates();
            trap_Cvar_Set("cl_paused", "0");
        }
    }
}

/*
=================
UI_MouseEvent
=================
*/
void UI_MouseEvent(int dx, int dy)
{
    // update mouse screen position
    uiInfo.uiDC.cursorx += (dx * uiInfo.uiDC.aspectScale);

    if (uiInfo.uiDC.cursorx < 0)
        uiInfo.uiDC.cursorx = 0;
    else if (uiInfo.uiDC.cursorx > SCREEN_WIDTH)
        uiInfo.uiDC.cursorx = SCREEN_WIDTH;

    uiInfo.uiDC.cursory += dy;

    if (uiInfo.uiDC.cursory < 0)
        uiInfo.uiDC.cursory = 0;
    else if (uiInfo.uiDC.cursory > SCREEN_HEIGHT)
        uiInfo.uiDC.cursory = SCREEN_HEIGHT;

    if (Menu_Count() > 0)
        Display_MouseMove(NULL, uiInfo.uiDC.cursorx, uiInfo.uiDC.cursory);
}

/*
=================
UI_MousePosition
=================
*/
int UI_MousePosition(void) { return (int)rint(uiInfo.uiDC.cursorx) | (int)rint(uiInfo.uiDC.cursory) << 16; }

/*
=================
UI_SetMousePosition
=================
*/
void UI_SetMousePosition(int x, int y)
{
    uiInfo.uiDC.cursorx = x;
    uiInfo.uiDC.cursory = y;

    if (Menu_Count() > 0)
        Display_MouseMove(NULL, uiInfo.uiDC.cursorx, uiInfo.uiDC.cursory);
}

void UI_SetActiveMenu(uiMenuCommand_t menu)
{
    char buf[256];

    // this should be the ONLY way the menu system is brought up
    // enusure minumum menu data is cached

    if (Menu_Count() > 0)
    {
        vec3_t v;
        v[0] = v[1] = v[2] = 0;

        switch (menu)
        {
            case UIMENU_NONE:
                trap_Key_SetCatcher(trap_Key_GetCatcher() & ~KEYCATCH_UI);
                trap_Key_ClearStates();
                trap_Cvar_Set("cl_paused", "0");
                Menus_CloseAll();
                return;

            case UIMENU_MAIN:
                trap_Cvar_Set("sv_killserver", "1");
                trap_Key_SetCatcher(KEYCATCH_UI);
                Menus_CloseAll();
                Menus_ActivateByName("main");
                trap_Cvar_VariableStringBuffer("com_errorMessage", buf, sizeof(buf));

                if (strlen(buf))
                {
                    if (trap_Cvar_VariableValue("com_errorCode") == ERR_SERVERDISCONNECT)
                        Menus_ActivateByName("drop_popmenu");
                    else
                        Menus_ActivateByName("error_popmenu");
                }
                return;

            case UIMENU_INGAME:
                trap_Cvar_Set("cl_paused", "1");
                trap_Key_SetCatcher(KEYCATCH_UI);
                UI_BuildPlayerList();
                Menus_CloseAll();
                Menus_ActivateByName("ingame");
                return;
        }
    }
}

qboolean UI_IsFullscreen(void) { return Menus_AnyFullScreenVisible(); }

static connstate_t lastConnState;
static char lastLoadingText[MAX_INFO_VALUE];

static void UI_ReadableSize(char *buf, int bufsize, int value)
{
    if (value > 1024 * 1024 * 1024)
    {  // gigs
        Com_sprintf(buf, bufsize, "%d", value / (1024 * 1024 * 1024));
        Com_sprintf(buf + strlen(buf), bufsize - strlen(buf), ".%02d GB",
            (value % (1024 * 1024 * 1024)) * 100 / (1024 * 1024 * 1024));
    }
    else if (value > 1024 * 1024)
    {  // megs
        Com_sprintf(buf, bufsize, "%d", value / (1024 * 1024));
        Com_sprintf(
            buf + strlen(buf), bufsize - strlen(buf), ".%02d MB", (value % (1024 * 1024)) * 100 / (1024 * 1024));
    }
    else if (value > 1024)
    {  // kilos
        Com_sprintf(buf, bufsize, "%d KB", value / 1024);
    }
    else
    {  // bytes
        Com_sprintf(buf, bufsize, "%d bytes", value);
    }
}

// Assumes time is in msec
static void UI_PrintTime(char *buf, int bufsize, int time)
{
    time /= 1000;  // change to seconds

    if (time > 3600)
    {  // in the hours range
        Com_sprintf(buf, bufsize, "%d hr %d min", time / 3600, (time % 3600) / 60);
    }
    else if (time > 60)
    {  // mins
        Com_sprintf(buf, bufsize, "%d min %d sec", time / 60, time % 60);
    }
    else
    {  // secs
        Com_sprintf(buf, bufsize, "%d sec", time);
    }
}

// FIXME: move to ui_shared.c?
void Text_PaintCenter(float x, float y, float scale, vec4_t color, const char *text, float adjust)
{
    int len = UI_Text_Width(text, scale);
    UI_Text_Paint(x - len / 2, y, scale, color, text, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE);
}

void Text_PaintCenter_AutoWrapped(
    float x, float y, float xmax, float ystep, float scale, vec4_t color, const char *str, float adjust)
{
    int width;
    char *s1, *s2, *s3;
    char c_bcp;
    char buf[1024];

    if (!str || str[0] == '\0')
        return;

    Q_strncpyz(buf, str, sizeof(buf));

    s1 = s2 = s3 = buf;

    while (1)
    {
        do
            s3++;
        while (*s3 != ' ' && *s3 != '\0');

        c_bcp = *s3;

        *s3 = '\0';

        width = UI_Text_Width(s1, scale);

        *s3 = c_bcp;

        if (width > xmax)
        {
            if (s1 == s2)
            {
                // fuck, don't have a clean cut, we'll overflow
                s2 = s3;
            }

            *s2 = '\0';
            Text_PaintCenter(x, y, scale, color, s1, adjust);
            y += ystep;

            if (c_bcp == '\0')
            {
                // that was the last word
                // we could start a new loop, but that wouldn't be much use
                // even if the word is too long, we would overflow it (see above)
                // so just print it now if needed
                s2++;

                if (*s2 != '\0')  // if we are printing an overflowing line we have s2 == s3
                    Text_PaintCenter(x, y, scale, color, s2, adjust);

                break;
            }

            s2++;
            s1 = s2;
            s3 = s2;
        }
        else
        {
            s2 = s3;

            if (c_bcp == '\0')  // we reached the end
            {
                Text_PaintCenter(x, y, scale, color, s1, adjust);
                break;
            }
        }
    }
}

static void UI_DisplayDownloadInfo(const char *downloadName, float centerPoint, float yStart, float scale)
{
    static char dlText[] = "Downloading:";
    static char etaText[] = "Estimated time left:";
    static char xferText[] = "Transfer rate:";

    int downloadSize, downloadCount, downloadTime, downloadTotal, downloadDone;
    char dlSizeBuf[64], totalSizeBuf[64], xferRateBuf[64], dlTimeBuf[64];
    int xferRate;
    int leftWidth;
    const char *s;

    downloadSize = trap_Cvar_VariableValue("cl_downloadSize");
    downloadCount = trap_Cvar_VariableValue("cl_downloadCount");
    downloadTime = trap_Cvar_VariableValue("cl_downloadTime");
    downloadTotal = trap_Cvar_VariableValue("cl_downloadTotal");
    downloadDone = trap_Cvar_VariableValue("cl_downloadDone");

    leftWidth = 320;

    UI_SetColor(colorWhite);
    Text_PaintCenter(centerPoint, yStart + 112, scale, colorWhite, dlText, 0);
    Text_PaintCenter(centerPoint, yStart + 192, scale, colorWhite, etaText, 0);
    Text_PaintCenter(centerPoint, yStart + 248, scale, colorWhite, xferText, 0);

    if (downloadSize > 0)
        s = va("%s (%d%%, %d/%d)", downloadName, (int)((float)downloadCount * 100.0f / downloadSize), downloadDone + 1, downloadTotal);
    else
        s = downloadName;

    Text_PaintCenter(centerPoint, yStart + 136, scale, colorWhite, s, 0);

    UI_ReadableSize(dlSizeBuf, sizeof dlSizeBuf, downloadCount);
    UI_ReadableSize(totalSizeBuf, sizeof totalSizeBuf, downloadSize);

    if (downloadCount < 4096 || !downloadTime)
    {
        Text_PaintCenter(leftWidth, yStart + 216, scale, colorWhite, "estimating", 0);
        Text_PaintCenter(
            leftWidth, yStart + 160, scale, colorWhite, va("(%s of %s copied)", dlSizeBuf, totalSizeBuf), 0);
    }
    else
    {
        if ((uiInfo.uiDC.realTime - downloadTime) / 1000)
            xferRate = downloadCount / ((uiInfo.uiDC.realTime - downloadTime) / 1000);
        else
            xferRate = 0;

        UI_ReadableSize(xferRateBuf, sizeof xferRateBuf, xferRate);

        // Extrapolate estimated completion time

        if (downloadSize && xferRate)
        {
            int n = downloadSize / xferRate;  // estimated time for entire d/l in secs

            // We do it in K (/1024) because we'd overflow around 4MB
            UI_PrintTime(
                dlTimeBuf, sizeof dlTimeBuf, (n - (((downloadCount / 1024) * n) / (downloadSize / 1024))) * 1000);

            Text_PaintCenter(leftWidth, yStart + 216, scale, colorWhite, dlTimeBuf, 0);
            Text_PaintCenter(
                leftWidth, yStart + 160, scale, colorWhite, va("(%s of %s copied)", dlSizeBuf, totalSizeBuf), 0);
        }
        else
        {
            Text_PaintCenter(leftWidth, yStart + 216, scale, colorWhite, "estimating", 0);

            if (downloadSize)
                Text_PaintCenter(
                    leftWidth, yStart + 160, scale, colorWhite, va("(%s of %s copied)", dlSizeBuf, totalSizeBuf), 0);
            else
                Text_PaintCenter(leftWidth, yStart + 160, scale, colorWhite, va("(%s copied)", dlSizeBuf), 0);
        }

        if (xferRate)
            Text_PaintCenter(leftWidth, yStart + 272, scale, colorWhite, va("%s/Sec", xferRateBuf), 0);
    }
}

/*
========================
UI_DrawConnectScreen
========================
*/
void UI_DrawConnectScreen()
{
    const char *s;
    uiClientState_t cstate;
    char info[MAX_INFO_VALUE];
    char text[256];
    float centerPoint = 320, yStart = 130, scale = 0.5f;

    menuDef_t *menu = Menus_FindByName("Connect");

    if ( menu )
        Menu_Paint(menu, qtrue);

    // see what information we should display
    trap_GetClientState(&cstate);

    info[0] = '\0';

    if (trap_GetConfigString(CS_SERVERINFO, info, sizeof(info)))
        Text_PaintCenter(
            centerPoint, yStart, scale, colorWhite, va("Loading %s", Info_ValueForKey(info, "mapname")), 0);

    if (!Q_stricmp(cstate.servername, "localhost"))
        Text_PaintCenter(centerPoint, yStart + 48, scale, colorWhite, "Starting up...", ITEM_TEXTSTYLE_SHADOWEDMORE);
    else
    {
        Com_sprintf(text, sizeof(text), "Connecting to %s", cstate.servername);
        Text_PaintCenter(centerPoint, yStart + 48, scale, colorWhite, text, ITEM_TEXTSTYLE_SHADOWEDMORE);
    }

    // display global MOTD at bottom
    Text_PaintCenter(centerPoint, 600, scale, colorWhite, Info_ValueForKey(cstate.updateInfoString, "motd"), 0);

    // print any server info (server full, bad version, etc)
    if (cstate.connState < CA_CONNECTED)
        Text_PaintCenter(centerPoint, yStart + 176, scale, colorWhite, cstate.messageString, 0);

    if (lastConnState > cstate.connState)
        lastLoadingText[0] = '\0';

    lastConnState = cstate.connState;

    switch (cstate.connState)
    {
        case CA_CONNECTING:
            s = va("Awaiting connection...%i", cstate.connectPacketCount);
            break;

        case CA_CHALLENGING:
            s = va("Awaiting challenge...%i", cstate.connectPacketCount);
            break;

        case CA_CONNECTED:
        {
            char downloadName[MAX_INFO_VALUE];
            int prompt = trap_Cvar_VariableValue("com_downloadPrompt");

            if (prompt & DLP_SHOW)
            {
                trap_Key_SetCatcher(KEYCATCH_UI);
                Menus_ActivateByName("download_popmenu");
                trap_Cvar_Set("com_downloadPrompt", "0");
            }

            trap_Cvar_VariableStringBuffer("cl_downloadName", downloadName, sizeof(downloadName));

            if (*downloadName)
            {
                UI_DisplayDownloadInfo(downloadName, centerPoint, yStart, scale);
                return;
            }
        }

            s = "Awaiting gamestate...";
            break;

        case CA_LOADING:
            return;

        case CA_PRIMED:
            return;

        default:
            return;
    }

    if (Q_stricmp(cstate.servername, "localhost"))
        Text_PaintCenter(centerPoint, yStart + 80, scale, colorWhite, s, 0);

    // password required / connection rejected information goes here
}

/*
=================
UI_RegisterCvars
=================
*/
void UI_RegisterCvars(void)
{
    size_t i;
    cvarTable_t *cv;

    for (i = 0, cv = cvarTable; i < cvarTableSize; i++, cv++)
        trap_Cvar_Register(cv->vmCvar, cv->cvarName, cv->defaultString, cv->cvarFlags);
}

/*
=================
UI_UpdateCvars
=================
*/
void UI_UpdateCvars(void)
{
    size_t i;
    cvarTable_t *cv;

    for (i = 0, cv = cvarTable; i < cvarTableSize; i++, cv++)
        trap_Cvar_Update(cv->vmCvar);
}

/*
=================
UI_UpdateNews
=================
*/
void UI_UpdateNews(qboolean begin)
{
    char newsString[MAX_NEWS_STRING];
    const char *c;
    const char *wrapped;
    int line = 0;
    int linePos = 0;
    qboolean finished;

    if (begin && !uiInfo.newsInfo.refreshActive)
    {
        uiInfo.newsInfo.refreshtime = uiInfo.uiDC.realTime + 10000;
        uiInfo.newsInfo.refreshActive = qtrue;
    }
    else if (!uiInfo.newsInfo.refreshActive)  // do nothing
    {
        return;
    }
    else if (uiInfo.uiDC.realTime > uiInfo.newsInfo.refreshtime)
    {
        strcpy(uiInfo.newsInfo.text[0], "^1Error: Timed out while contacting the server.");
        uiInfo.newsInfo.numLines = 1;
        return;
    }

    // start the news fetching
    finished = trap_GetNews(begin);

    // parse what comes back. Parse newlines and otherwise chop when necessary
    trap_Cvar_VariableStringBuffer("cl_newsString", newsString, sizeof(newsString));

    // FIXME remove magic width constant
    wrapped = Item_Text_Wrap(newsString, 0.25f, 325 * uiInfo.uiDC.aspectScale);

    for (c = wrapped; *c != '\0'; ++c)
    {
        if (linePos == (MAX_NEWS_LINEWIDTH - 1) || *c == '\n')
        {
            uiInfo.newsInfo.text[line][linePos] = '\0';

            if (line == (MAX_NEWS_LINES - 1))
                break;

            linePos = 0;
            line++;

            if (*c != '\n')
            {
                uiInfo.newsInfo.text[line][linePos] = *c;
                linePos++;
            }
        }
        else if (isprint(*c))
        {
            uiInfo.newsInfo.text[line][linePos] = *c;
            linePos++;
        }
    }

    uiInfo.newsInfo.text[line][linePos] = '\0';
    uiInfo.newsInfo.numLines = line + 1;

    if (finished)
        uiInfo.newsInfo.refreshActive = qfalse;
}

void UI_UpdateGithubRelease()
{
    char newsString[MAX_NEWS_STRING];
    const char *c;
    const char *wrapped;
    int line = 0, linePos = 0;
    int nexttime = uiInfo.githubRelease.nextTime;

    if (nexttime && !(nexttime > uiInfo.uiDC.realTime))
        return;

    // Limit checks to 1x every 10seconds
    uiInfo.githubRelease.nextTime = uiInfo.uiDC.realTime + 10000;
    trap_Cmd_ExecuteText(EXEC_INSERT, "checkForUpdate");

    // parse what comes back. Parse newlines and otherwise chop when necessary
    trap_Cvar_VariableStringBuffer("cl_latestRelease", newsString, sizeof(newsString));

    // FIXME remove magic width constant
    wrapped = Item_Text_Wrap(newsString, 0.33f, 450 * uiInfo.uiDC.aspectScale);

    for (c = wrapped; *c != '\0'; ++c)
    {
        if (linePos == (MAX_NEWS_LINEWIDTH - 1) || *c == '\n')
        {
            uiInfo.githubRelease.text[line][linePos] = '\0';

            if (line == (MAX_NEWS_LINES - 1))
                break;

            linePos = 0;
            line++;

            if (*c != '\n')
            {
                uiInfo.githubRelease.text[line][linePos] = *c;
                linePos++;
            }
        }
        else if (isprint(*c))
        {
            uiInfo.githubRelease.text[line][linePos] = *c;
            linePos++;
        }
    }

    uiInfo.githubRelease.text[line][linePos] = '\0';
    uiInfo.githubRelease.numLines = line + 1;
}

#ifdef MODULE_INTERFACE_11
void trap_R_SetClipRegion(const float *region) {}

qboolean trap_GetNews(qboolean force) { return qtrue; }
#endif
