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
	if(err != 0) {
		Com_Error(ERR_DROP, "CL_XHR_Download failed with err code %d", err);
		clc.XHRUsed = false;
	}
	else {
		clc.downloadRestart = true;
		CL_NextDownload();
	}

	return;
}

/*
=================
CL_XHR_StartDownload
=================
*/
void CL_XHR_StartDownload(const char *localName, const char *remoteURL)
{
	clc.XHRUsed = true;

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

	if(!(clc.sv_allowDownload & DLF_NO_DISCONNECT) && !clc.XHRDisconnected)
    {
		CL_AddReliableCommand("disconnect", true);
		CL_WritePacket();
		CL_WritePacket();
		CL_WritePacket();
		clc.XHRDisconnected = true;
	}

	return;
}
#endif
