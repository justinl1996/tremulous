/*
===========================================================================
Copyright (C) 2006 Tony J. White (tjw@tjw.org)
Copyright (C) 2000-2013 Darklegion Development
Copyright (C) 2015-2019 GrangerHub
And the dudes that worked on the web port

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

#ifdef EMSCRIPTEN

#include "client.h"

extern "C"
{
	extern void Sys_DownloadAndSaveAsset(const char *localName, const char *remoteName, 
		void (*onprogress)(int loaded, int total), void (*onassetend)(int err));
}

// the download finishes in a browser callback, outside of any frame, where a
// Com_Error has nothing to unwind to, so it's only noted here and handled in
// CL_XHR_PerformDownload
static bool xhrFinished;
static int xhrError;

/*
=================
CL_XHR_ProgressCallback
=================
*/
void CL_XHR_ProgressCallback(int loaded, int total)
{
	clc.downloadSize = (int)total;
	Cvar_SetValue( "cl_downloadSize", clc.downloadSize );
	clc.downloadCount = (int)loaded;
	Cvar_SetValue( "cl_downloadCount", clc.downloadCount );
	return;
}

/*
=================
CL_XHR_FinishedCallback
=================
*/
void CL_XHR_FinishedCallback(int err)
{
	xhrError = err;
	xhrFinished = true;
}

/*
=================
CL_XHR_ServerURL

The game server's own address as an https:// URL, for servers that serve
their downloads from their websocket port (sv_wsDownload)
=================
*/
const char *CL_XHR_ServerURL(void)
{
	static char url[MAX_OSPATH];
	const char *host = clc.servername;
	const char *colon = strchr(host, ':');

	if (*host == '[')
	{
		// [ipv6] or [ipv6]:port
		if (strstr(host, "]:"))
			Com_sprintf(url, sizeof(url), "https://%s", host);
		else
			Com_sprintf(url, sizeof(url), "https://%s:%d", host, PORT_SERVER);
	}
	else if (colon && strchr(colon + 1, ':'))
	{
		// bare ipv6 address, without a port
		Com_sprintf(url, sizeof(url), "https://[%s]:%d", host, PORT_SERVER);
	}
	else if (colon)
		Com_sprintf(url, sizeof(url), "https://%s", host);
	else
		Com_sprintf(url, sizeof(url), "https://%s:%d", host, PORT_SERVER);

	return url;
}

/*
=================
CL_XHR_StartDownload
=================
*/
void CL_XHR_StartDownload(const char *localName, const char *remoteURL)
{
	clc.XHRUsed = true;
	clc.downloadXHR = true;
	xhrFinished = false;
	xhrError = 0;

    Com_Printf("URL: %s\n", remoteURL);
	Com_DPrintf("***** CL_XHR_StartDownload *****\n"
		"Localname: %s\n"
		"RemoteURL: %s\n"
		"****************************\n", localName, remoteURL);

    Q_strncpyz(clc.downloadURL, remoteURL, sizeof(clc.downloadURL));
	Q_strncpyz(clc.downloadName, localName, sizeof(clc.downloadName));

    // Set so UI gets access to it
	Cvar_Set("cl_downloadName", localName);
	Cvar_Set("cl_downloadSize", "0");
	Cvar_Set("cl_downloadCount", "0");
	Cvar_SetValue("cl_downloadTime", cls.realtime);

    clc.downloadBlock = 0; // Starting new file
	clc.downloadCount = 0;

    Sys_DownloadAndSaveAsset(clc.downloadName, clc.downloadURL, CL_XHR_ProgressCallback, CL_XHR_FinishedCallback);

	// only game downloads leave the server (the news isn't one)
	if(!clc.activeXHRNotGameRelated && clc.state >= CA_CONNECTED &&
		!(clc.sv_allowDownload & DLF_NO_DISCONNECT) && !clc.XHRDisconnected)
    {
		CL_AddReliableCommand("disconnect", true);
		CL_WritePacket();
		CL_WritePacket();
		CL_WritePacket();
		clc.XHRDisconnected = true;
	}

	return;
}

/*
=================
CL_XHR_PerformDownload

Called every frame while an XHR download is running
=================
*/
void CL_XHR_PerformDownload(void)
{
	if (!xhrFinished)
		return;

	xhrFinished = false;
	clc.downloadXHR = false;

	if (xhrError)
	{
		clc.XHRUsed = false;
		clc.activeXHRNotGameRelated = false;
		Com_Error(ERR_DROP, "Download Error: code %d URL: %s", xhrError, clc.downloadURL);
		return;
	}

	clc.downloadRestart = true;
	CL_NextDownload();
}
#endif
