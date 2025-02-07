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
// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"

bool scr_initialized;  // ready to draw

cvar_t *cl_timegraph;
cvar_t *cl_debuggraph;
cvar_t *cl_graphheight;
cvar_t *cl_graphscale;
cvar_t *cl_graphshift;

/*
================
SCR_DrawNamedPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawNamedPic(float x, float y, float width, float height, const char *picname)
{
    qhandle_t hShader;

    assert(width != 0);

    hShader = re.RegisterShader(picname);
    SCR_AdjustFrom640(&x, &y, &width, &height);
    re.DrawStretchPic(x, y, width, height, 0, 0, 1, 1, hShader);
}

/*
================
SCR_AdjustFrom640

Adjusted for resolution and screen aspect ratio
================
*/
void SCR_AdjustFrom640(float *x, float *y, float *w, float *h)
{
    float xscale;
    float yscale;

#if 0
		// adjust for wide screens
		if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
			*x += 0.5 * ( cls.glconfig.vidWidth - ( cls.glconfig.vidHeight * 640 / 480 ) );
		}
#endif

    // scale for screen sizes
    xscale = cls.glconfig.vidWidth / 640.0;
    yscale = cls.glconfig.vidHeight / 480.0;
    if (x)
    {
        *x *= xscale;
    }
    if (y)
    {
        *y *= yscale;
    }
    if (w)
    {
        *w *= xscale;
    }
    if (h)
    {
        *h *= yscale;
    }
}

/*
================
SCR_FillRect

Coordinates are 640*480 virtual values
=================
*/
void SCR_FillRect(float x, float y, float width, float height, const float *color)
{
    re.SetColor(color);

    SCR_AdjustFrom640(&x, &y, &width, &height);
    re.DrawStretchPic(x, y, width, height, 0, 0, 0, 0, cls.whiteShader);

    re.SetColor(NULL);
}

/*
================
SCR_DrawPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawPic(float x, float y, float width, float height, qhandle_t hShader)
{
    SCR_AdjustFrom640(&x, &y, &width, &height);
    re.DrawStretchPic(x, y, width, height, 0, 0, 1, 1, hShader);
}

/*
** SCR_DrawChar
** chars are drawn at 640*480 virtual screen size
*/
static void SCR_DrawChar(int x, int y, float size, int ch)
{
    int row, col;
    float frow, fcol;
    float ax, ay, aw, ah;

    ch &= 255;

    if (ch == ' ')
    {
        return;
    }

    if (y < -size)
    {
        return;
    }

    ax = x;
    ay = y;
    aw = size;
    ah = size;
    SCR_AdjustFrom640(&ax, &ay, &aw, &ah);

    row = ch >> 4;
    col = ch & 15;

    frow = row * 0.0625;
    fcol = col * 0.0625;
    size = 0.0625;

    re.DrawStretchPic(ax, ay, aw, ah, fcol, frow, fcol + size, frow + size, cls.charSetShader);
}

/*
** SCR_DrawSmallChar
** small chars are drawn at native screen resolution
*/
void SCR_DrawSmallChar(int x, int y, int ch)
{
    int row, col;
    float frow, fcol;
    float size;

    ch &= 255;

    if (ch == ' ')
    {
        return;
    }

    if (y < -SMALLCHAR_HEIGHT)
    {
        return;
    }

    row = ch >> 4;
    col = ch & 15;

    frow = row * 0.0625;
    fcol = col * 0.0625;
    size = 0.0625;

    re.DrawStretchPic(x, y, SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, fcol, frow, fcol + size, frow + size, cls.charSetShader);
}

/*
==================
SCR_DrawBigString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.

Coordinates are at 640 by 480 virtual resolution
==================
*/
void SCR_DrawStringExt(
    int x, int y, float size, const char *string, float *setColor,
    bool forceColor, bool noColorEscape) {
    vec4_t		color;
    const char	*s;
    int			xx;
    bool skip_color_string_check = false;

    // draw the drop shadow
    color[0] = color[1] = color[2] = 0;
    color[3] = setColor[3];
    re.SetColor( color );
    s = string;
    xx = x;
    while ( *s ) {
        if ( !noColorEscape && Q_IsColorString( s ) ) {
            s += Q_ColorStringLength(s);
            continue;
        } else if (!noColorEscape && Q_IsColorEscapeEscape(s)) {
            s++;
        }
        SCR_DrawChar( xx+2, y+2, size, *s );
        xx += size;
        s++;
    }


    // draw the colored text
    s = string;
    xx = x;
    re.SetColor( setColor );
    while ( *s ) {
        if(skip_color_string_check) {
            skip_color_string_check = false;
        } else if ( Q_IsColorString( s ) ) {
            if ( !forceColor ) {
                if ( Q_IsHardcodedColor(s) ) {
                    ::memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
                } else {
                    Q_GetVectFromHexColor(s, color);
                }
                color[3] = setColor[3];
                re.SetColor( color );
            }
            if ( !noColorEscape ) {
                s += Q_ColorStringLength(s);
                continue;
            }
        } else if(Q_IsColorEscapeEscape(s)) {
            if(!noColorEscape) {
                s++;
            } else if(!forceColor) {
                skip_color_string_check = true;
            }
        }
        SCR_DrawChar( xx, y, size, *s );
        xx += size;
        s++;
    }
    re.SetColor( NULL );
}

void SCR_DrawBigString(int x, int y, const char *s, float alpha, bool noColorEscape)
{
    float color[4];

    color[0] = color[1] = color[2] = 1.0;
    color[3] = alpha;
    SCR_DrawStringExt(x, y, BIGCHAR_WIDTH, s, color, false, noColorEscape);
}

void SCR_DrawBigStringColor(int x, int y, const char *s, vec4_t color, bool noColorEscape)
{
    SCR_DrawStringExt(x, y, BIGCHAR_WIDTH, s, color, true, noColorEscape);
}

/*
==================
SCR_DrawSmallString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.
==================
*/
void SCR_DrawSmallStringExt(
    int x, int y, const char *string, float *setColor, bool forceColor,
    bool noColorEscape) {
    vec4_t      color;
    const char  *s;
    int         xx;
    bool        skip_color_string_check = false;

    // draw the colored text
    s = string;
    xx = x;
    re.SetColor(setColor);
    while (*s) {
        if(skip_color_string_check) {
            skip_color_string_check = false;
        } else if(Q_IsColorString(s)) {
            if ( !forceColor ) {
                if(Q_IsHardcodedColor(s)) {
                    ::memcpy(
                        color, g_color_table[ColorIndex(*(s+1))], sizeof(color));
                } else {
                    Q_GetVectFromHexColor(s, color);
                }
                color[3] = setColor[3];
                re.SetColor( color );
            }
            if (!noColorEscape) {
                s += Q_ColorStringLength(s);
                continue;
            }
        } else if(Q_IsColorEscapeEscape(s)) {
            if(!noColorEscape) {
                s++;
            } else if(!forceColor) {
                skip_color_string_check = true;
            }
        }
        SCR_DrawSmallChar(xx, y, *s);
        xx += SMALLCHAR_WIDTH;
        s++;
    }
    re.SetColor(NULL);
}

/*
** SCR_Strlen -- skips color escape codes
*/
static int SCR_Strlen(const char *str) {
    const char *s = str;
    int   count = 0;

    while (*s) {
        if(Q_IsColorString(s)) {
            s += Q_ColorStringLength(s);
        } else {
            if(Q_IsColorEscapeEscape(s)) {
                s++;
            }
            count++;
            s++;
        }
    }

    return count;
}

/*
** SCR_GetBigStringWidth
*/
int SCR_GetBigStringWidth(const char *str) { return SCR_Strlen(str) * BIGCHAR_WIDTH; }
//===============================================================================

#ifdef USE_VOIP
/*
=================
SCR_DrawVoipMeter

FIXME: inherited from ioq3, move to cgame/ui
=================
*/
void SCR_DrawVoipMeter(void)
{
    char buffer[16];
    char string[256];
    int limit, i;

    if (!cl_voipShowMeter->integer)
        return;  // player doesn't want to show meter at all.
    else if (!cl_voipSend->integer)
        return;  // not recording at the moment.
    else if (clc.state != CA_ACTIVE)
        return;  // not connected to a server.
    else if (!clc.voipEnabled)
        return;  // server doesn't support VoIP.
    else if (clc.demoplaying)
        return;  // playing back a demo.
    else if (!cl_voip->integer)
        return;  // client has VoIP support disabled.

    limit = (int)(clc.voipPower * 10.0f);
    if (limit > 10) limit = 10;

    for (i = 0; i < limit; i++) buffer[i] = '*';
    while (i < 10) buffer[i++] = ' ';
    buffer[i] = '\0';

    SCR_DrawStringExt(
        320 - strlen(string) * 4, 10, 8, string,
        g_color_table[ColorIndex(COLOR_WHITE)], true, false);
}
#endif

/*
===============================================================================

DEBUG GRAPH

===============================================================================
*/

static int current;
static float values[1024];

/*
==============
SCR_DebugGraph
==============
*/
void SCR_DebugGraph(float value)
{
    values[current] = value;
    current = (current + 1) % ARRAY_LEN(values);
}

/*
==============
SCR_DrawDebugGraph
==============
*/
void SCR_DrawDebugGraph(void)
{
    int a, x, y, w, i, h;
    float v;

    //
    // draw the graph
    //
    w = cls.glconfig.vidWidth;
    x = 0;
    y = cls.glconfig.vidHeight;
    re.SetColor(g_color_table[ColorIndex(COLOR_BLACK)]);
    re.DrawStretchPic(x, y - cl_graphheight->integer, w, cl_graphheight->integer, 0, 0, 0, 0, cls.whiteShader);
    re.SetColor(NULL);

    for (a = 0; a < w; a++)
    {
        i = (ARRAY_LEN(values) + current - 1 - (a % ARRAY_LEN(values))) % ARRAY_LEN(values);
        v = values[i];
        v = v * cl_graphscale->integer + cl_graphshift->integer;

        if (v < 0) v += cl_graphheight->integer * (1 + (int)(-v / cl_graphheight->integer));
        h = (int)v % cl_graphheight->integer;
        re.DrawStretchPic(x + w - 1 - a, y - h, 1, h, 0, 0, 0, 0, cls.whiteShader);
    }
}

//=============================================================================

/*
==================
SCR_Init
==================
*/
void SCR_Init(void)
{
    cl_timegraph = Cvar_Get("timegraph", "0", CVAR_CHEAT);
    cl_debuggraph = Cvar_Get("debuggraph", "0", CVAR_CHEAT);
    cl_graphheight = Cvar_Get("graphheight", "32", CVAR_CHEAT);
    cl_graphscale = Cvar_Get("graphscale", "1", CVAR_CHEAT);
    cl_graphshift = Cvar_Get("graphshift", "0", CVAR_CHEAT);

    scr_initialized = true;
}

//=======================================================

/*
==================
SCR_DrawScreenField

This will be called twice if rendering in stereo mode
==================
*/
void SCR_DrawScreenField(stereoFrame_t stereoFrame)
{
    bool uiFullscreen;

    re.BeginFrame(stereoFrame);

    uiFullscreen = (cls.ui && VM_Call(cls.ui, UI_IS_FULLSCREEN - (cls.uiInterface == 2 ? 2 : 0)));

    // wide aspect ratio screens need to have the sides cleared
    // unless they are displaying game renderings
    if (uiFullscreen || clc.state < CA_LOADING)
    {
        if (cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640)
        {
            re.SetColor(g_color_table[ColorIndex(COLOR_BLACK)]);
            re.DrawStretchPic(0, 0, cls.glconfig.vidWidth, cls.glconfig.vidHeight, 0, 0, 0, 0, cls.whiteShader);
            re.SetColor(NULL);
        }
    }

    // if the menu is going to cover the entire screen, we
    // don't need to render anything under it
    if (cls.ui && !uiFullscreen)
    {
        switch (clc.state)
        {
            default:
                Com_Error(ERR_FATAL, "SCR_DrawScreenField: bad clc.state");
                break;
            case CA_CINEMATIC:
                SCR_DrawCinematic();
                break;
            case CA_DISCONNECTED:
                // force menu up
                S_StopAllSounds();
                VM_Call(cls.ui, UI_SET_ACTIVE_MENU - (cls.uiInterface == 2 ? 2 : 0), UIMENU_MAIN);
                break;
            case CA_CONNECTING:
            case CA_CHALLENGING:
            case CA_CONNECTED:
                // connecting clients will only show the connection dialog
                // refresh to update the time
                VM_Call(cls.ui, UI_REFRESH - (cls.uiInterface == 2 ? 2 : 0), cls.realtime);
                VM_Call(cls.ui, UI_DRAW_CONNECT_SCREEN - (cls.uiInterface == 2 ? 2 : 0), false);
                break;
            case CA_LOADING:
            case CA_PRIMED:
                // draw the game information screen and loading progress
                CL_CGameRendering(stereoFrame);
                break;
            case CA_ACTIVE:
                // always supply STEREO_CENTER as vieworg offset is now done by the engine.
                CL_CGameRendering(stereoFrame);
#ifdef USE_VOIP
                SCR_DrawVoipMeter();
#endif
                break;
        }
    }

    // the menu draws next
    if (Key_GetCatcher() & KEYCATCH_UI && cls.ui)
    {
        VM_Call(cls.ui, UI_REFRESH - (cls.uiInterface == 2 ? 2 : 0), cls.realtime);
    }

    // console draws next
    Con_DrawConsole();

    // debug graph can be drawn on top of anything
    if (cl_debuggraph->integer || cl_timegraph->integer || cl_debugMove->integer)
    {
        SCR_DrawDebugGraph();
    }
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen(void)
{
    static int recursive;

    if (!scr_initialized)
    {
        return;  // not initialized yet
    }

    if (++recursive > 2)
    {
        Com_Error(ERR_FATAL, "SCR_UpdateScreen: recursively called");
    }
    recursive = 1;

    // If there is no VM, there are also no rendering commands issued. Stop the renderer in
    // that case.
    if (cls.ui || com_dedicated->integer)
    {
        // XXX
        int in_anaglyphMode = Cvar_VariableIntegerValue("r_anaglyphMode");
        // if running in stereo, we need to draw the frame twice
        if (cls.glconfig.stereoEnabled || in_anaglyphMode)
        {
            SCR_DrawScreenField(STEREO_LEFT);
            SCR_DrawScreenField(STEREO_RIGHT);
        }
        else
        {
            SCR_DrawScreenField(STEREO_CENTER);
        }

        if (com_speeds->integer)
        {
            re.EndFrame(&time_frontend, &time_backend);
        }
        else
        {
            re.EndFrame(NULL, NULL);
        }
    }

    recursive = 0;
}
