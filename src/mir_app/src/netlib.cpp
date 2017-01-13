/*

Miranda NG: the free IM client for Microsoft* Windows*

Copyright (�) 2012-17 Miranda NG project (http://miranda-ng.org),
Copyright (c) 2000-12 Miranda IM project,
all portions of this codebase are copyrighted to the people
listed in contributors.txt.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "stdafx.h"
#include "netlib.h"

static BOOL bModuleInitialized = FALSE;

HANDLE hConnectionHeaderMutex, hConnectionOpenMutex;
DWORD g_LastConnectionTick;
int connectionTimeout;
HANDLE hSendEvent = NULL, hRecvEvent = NULL;

typedef BOOL (WINAPI *tGetProductInfo)(DWORD, DWORD, DWORD, DWORD, PDWORD);

static int CompareNetlibUser(const NetlibUser* p1, const NetlibUser* p2)
{
	return mir_strcmp(p1->user.szSettingsModule, p2->user.szSettingsModule);
}

LIST<NetlibUser> netlibUser(5, CompareNetlibUser);
mir_cs csNetlibUser;

SSL_API sslApi;

void NetlibFreeUserSettingsStruct(NETLIBUSERSETTINGS *settings)
{
	mir_free(settings->szIncomingPorts);
	mir_free(settings->szOutgoingPorts);
	mir_free(settings->szProxyAuthPassword);
	mir_free(settings->szProxyAuthUser);
	mir_free(settings->szProxyServer);
}

int GetNetlibHandleType(void *p)
{
	__try {
		return *(int*)p;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{}

	return NLH_INVALID;
}

void NetlibInitializeNestedCS(NetlibNestedCriticalSection *nlncs)
{
	nlncs->dwOwningThreadId = 0;
	nlncs->lockCount = 0;
	nlncs->hMutex = CreateMutex(NULL, FALSE, NULL);
}

void NetlibDeleteNestedCS(NetlibNestedCriticalSection *nlncs)
{
	CloseHandle(nlncs->hMutex);
}

int NetlibEnterNestedCS(NetlibConnection *nlc, int which)
{
	NetlibNestedCriticalSection *nlncs;
	DWORD dwCurrentThreadId = GetCurrentThreadId();

	WaitForSingleObject(hConnectionHeaderMutex, INFINITE);
	if (nlc == NULL || nlc->handleType != NLH_CONNECTION) {
		ReleaseMutex(hConnectionHeaderMutex);
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	nlncs = (which == NLNCS_SEND) ? &nlc->ncsSend : &nlc->ncsRecv;
	if (nlncs->lockCount && nlncs->dwOwningThreadId == dwCurrentThreadId) {
		nlncs->lockCount++;
		ReleaseMutex(hConnectionHeaderMutex);
		return 1;
	}
	InterlockedIncrement(&nlc->dontCloseNow);
	ResetEvent(nlc->hOkToCloseEvent);
	ReleaseMutex(hConnectionHeaderMutex);
	WaitForSingleObject(nlncs->hMutex, INFINITE);
	nlncs->dwOwningThreadId = dwCurrentThreadId;
	nlncs->lockCount = 1;
	if (InterlockedDecrement(&nlc->dontCloseNow) == 0)
		SetEvent(nlc->hOkToCloseEvent);
	return 1;
}

void NetlibLeaveNestedCS(NetlibNestedCriticalSection *nlncs)
{
	if (--nlncs->lockCount == 0) {
		nlncs->dwOwningThreadId = 0;
		ReleaseMutex(nlncs->hMutex);
	}
}

static INT_PTR GetNetlibUserSettingInt(const char *szUserModule, const char *szSetting, int defValue)
{
	DBVARIANT dbv;
	if (db_get(NULL, szUserModule, szSetting, &dbv) && db_get(NULL, "Netlib", szSetting, &dbv))
		return defValue;

	if (dbv.type == DBVT_BYTE) return dbv.bVal;
	if (dbv.type == DBVT_WORD) return dbv.wVal;
	return dbv.dVal;
}

static char* GetNetlibUserSettingString(const char *szUserModule, const char *szSetting)
{
	char *szRet = db_get_sa(NULL, szUserModule, szSetting);
	if (szRet == NULL)
		if ((szRet = db_get_sa(NULL, "Netlib", szSetting)) == NULL)
			return NULL;

	return szRet;
}

/////////////////////////////////////////////////////////////////////////////////////////

MIR_APP_DLL(HNETLIBUSER) Netlib_RegisterUser(const NETLIBUSER *nlu)
{
	if (nlu == NULL || nlu->szSettingsModule == NULL || (!(nlu->flags & NUF_NOOPTIONS) && nlu->szDescriptiveName == NULL) || (nlu->flags & NUF_HTTPGATEWAY && (nlu->pfnHttpGatewayInit == NULL))) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	HNETLIBUSER thisUser = (HNETLIBUSER)mir_calloc(sizeof(NetlibUser));
	thisUser->handleType = NLH_USER;
	thisUser->user = *nlu;

	int idx;
	{
		mir_cslock lck(csNetlibUser);
		idx = netlibUser.getIndex(thisUser);
	}
	if (idx != -1) {
		mir_free(thisUser);
		SetLastError(ERROR_DUP_NAME);
		return 0;
	}

	if (nlu->szDescriptiveName)
		thisUser->user.ptszDescriptiveName = (thisUser->user.flags&NUF_UNICODE ? mir_wstrdup((WCHAR*)nlu->ptszDescriptiveName) : mir_a2u(nlu->szDescriptiveName));

	if ((thisUser->user.szSettingsModule = mir_strdup(nlu->szSettingsModule)) == NULL
	   || (nlu->szDescriptiveName && thisUser->user.ptszDescriptiveName == NULL)
	   || (nlu->szHttpGatewayUserAgent && (thisUser->user.szHttpGatewayUserAgent = mir_strdup(nlu->szHttpGatewayUserAgent)) == NULL))
	{
		mir_free(thisUser);
		SetLastError(ERROR_OUTOFMEMORY);
		return 0;
	}
	if (nlu->szHttpGatewayHello)
		thisUser->user.szHttpGatewayHello = mir_strdup(nlu->szHttpGatewayHello);
	else
		thisUser->user.szHttpGatewayHello = NULL;

	thisUser->settings.cbSize = sizeof(NETLIBUSERSETTINGS);
	thisUser->settings.useProxy = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLUseProxy", 0);
	thisUser->settings.proxyType = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLProxyType", PROXYTYPE_SOCKS5);
	if (thisUser->user.flags&NUF_NOHTTPSOPTION && thisUser->settings.proxyType == PROXYTYPE_HTTPS)
		thisUser->settings.proxyType = PROXYTYPE_HTTP;
	if (!(thisUser->user.flags&(NUF_HTTPCONNS|NUF_HTTPGATEWAY)) && thisUser->settings.proxyType == PROXYTYPE_HTTP) {
		thisUser->settings.useProxy = 0;
		thisUser->settings.proxyType = PROXYTYPE_SOCKS5;
	}
	thisUser->settings.szProxyServer = GetNetlibUserSettingString(thisUser->user.szSettingsModule, "NLProxyServer");
	thisUser->settings.wProxyPort = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLProxyPort", 1080);
	thisUser->settings.useProxyAuth = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLUseProxyAuth", 0);
	thisUser->settings.szProxyAuthUser = GetNetlibUserSettingString(thisUser->user.szSettingsModule, "NLProxyAuthUser");
	thisUser->settings.szProxyAuthPassword = GetNetlibUserSettingString(thisUser->user.szSettingsModule, "NLProxyAuthPassword");
	thisUser->settings.dnsThroughProxy = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLDnsThroughProxy", 1);
	thisUser->settings.specifyIncomingPorts = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLSpecifyIncomingPorts", 0);
	thisUser->settings.szIncomingPorts = GetNetlibUserSettingString(thisUser->user.szSettingsModule, "NLIncomingPorts");
	thisUser->settings.specifyOutgoingPorts = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLSpecifyOutgoingPorts", 0);
	thisUser->settings.szOutgoingPorts = GetNetlibUserSettingString(thisUser->user.szSettingsModule, "NLOutgoingPorts");
	thisUser->settings.enableUPnP = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLEnableUPnP", 1); //default to on
	thisUser->settings.validateSSL = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLValidateSSL", 0);

	thisUser->toLog = GetNetlibUserSettingInt(thisUser->user.szSettingsModule, "NLlog", 1);

	mir_cslock lck(csNetlibUser);
	netlibUser.insert(thisUser);
	return thisUser;
}

/////////////////////////////////////////////////////////////////////////////////////////

MIR_APP_DLL(int) Netlib_GetUserSettings(HNETLIBUSER nlu, NETLIBUSERSETTINGS *nlus)
{
	if (GetNetlibHandleType(nlu) != NLH_USER || nlus == NULL || nlus->cbSize != sizeof(NETLIBUSERSETTINGS)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	*nlus = nlu->settings;
	return 1;
}

MIR_APP_DLL(int) Netlib_SetUserSettings(HNETLIBUSER nlu, const NETLIBUSERSETTINGS *nlus)
{
	if (GetNetlibHandleType(nlu) != NLH_USER || nlus == NULL || nlus->cbSize != sizeof(NETLIBUSERSETTINGS)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	NetlibSaveUserSettingsStruct(nlu->user.szSettingsModule, nlus);
	return 1;
}

/////////////////////////////////////////////////////////////////////////////////////////

void NetlibDoCloseSocket(NetlibConnection *nlc, bool noShutdown)
{
	if (nlc->s == INVALID_SOCKET) return;

	Netlib_Logf(nlc->nlu, "(%p:%u) Connection closed internal", nlc, nlc->s);
	if (nlc->hSsl) {
		if (!noShutdown) sslApi.shutdown(nlc->hSsl);
		sslApi.sfree(nlc->hSsl);
		nlc->hSsl = NULL;
	}
	closesocket(nlc->s);
	nlc->s = INVALID_SOCKET;
}

MIR_APP_DLL(int) Netlib_CloseHandle(HANDLE hNetlib)
{
	if (hNetlib == NULL)
		return 0;

	switch (GetNetlibHandleType(hNetlib)) {
	case NLH_USER:
		{
			NetlibUser *nlu = (NetlibUser*)hNetlib;
			{
				mir_cslock lck(csNetlibUser);
				int i = netlibUser.getIndex(nlu);
				if (i >= 0)
					netlibUser.remove(i);
			}

			NetlibFreeUserSettingsStruct(&nlu->settings);
			mir_free(nlu->user.szSettingsModule);
			mir_free(nlu->user.szDescriptiveName);
			mir_free(nlu->user.szHttpGatewayHello);
			mir_free(nlu->user.szHttpGatewayUserAgent);
			mir_free(nlu->szStickyHeaders);
		}
		break;

	case NLH_CONNECTION:
		WaitForSingleObject(hConnectionHeaderMutex, INFINITE);
		{
			NetlibConnection *nlc = (NetlibConnection*)hNetlib;
			if (GetNetlibHandleType(nlc) == NLH_CONNECTION) {
				if (nlc->usingHttpGateway)
					HttpGatewayRemovePacket(nlc, -1);
				else {
					if (nlc->s != INVALID_SOCKET)
						NetlibDoCloseSocket(nlc, nlc->termRequested);
					if (nlc->s2 != INVALID_SOCKET)
						closesocket(nlc->s2);
					nlc->s2 = INVALID_SOCKET;
				}
				ReleaseMutex(hConnectionHeaderMutex);

				HANDLE waitHandles[4] = { hConnectionHeaderMutex, nlc->hOkToCloseEvent, nlc->ncsRecv.hMutex, nlc->ncsSend.hMutex };
				DWORD waitResult = WaitForMultipleObjects(_countof(waitHandles), waitHandles, TRUE, INFINITE);
				if (waitResult >= WAIT_OBJECT_0 + _countof(waitHandles)) {
					ReleaseMutex(hConnectionHeaderMutex);
					SetLastError(ERROR_INVALID_PARAMETER);  //already been closed
					return 0;
				}

				Netlib_Logf(nlc->nlu, "(%p:%u) Connection closed", nlc, nlc->s);
				delete nlc;
			}
		}
		ReleaseMutex(hConnectionHeaderMutex);
		return 1;

	case NLH_BOUNDPORT:
		return NetlibFreeBoundPort((NetlibBoundPort*)hNetlib);

	case NLH_PACKETRECVER:
		{
			struct NetlibPacketRecver *nlpr = (NetlibPacketRecver*)hNetlib;
			mir_free(nlpr->packetRecver.buffer);
		}
		break;

	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	mir_free(hNetlib);
	return 1;
}

/////////////////////////////////////////////////////////////////////////////////////////

MIR_APP_DLL(UINT_PTR) Netlib_GetSocket(HANDLE hConnection)
{
	SOCKET s;
	if (hConnection == 0) {
		s = INVALID_SOCKET;
		SetLastError(ERROR_INVALID_PARAMETER);
	}
	else {
		WaitForSingleObject(hConnectionHeaderMutex, INFINITE);
		switch (GetNetlibHandleType(hConnection)) {
		case NLH_CONNECTION:
			s = ((NetlibConnection*)hConnection)->s;
			break;
		case NLH_BOUNDPORT:
			s = ((NetlibBoundPort*)hConnection)->s;
			break;
		default:
			s = INVALID_SOCKET;
			SetLastError(ERROR_INVALID_PARAMETER);
			break;
		}
		ReleaseMutex(hConnectionHeaderMutex);
	}
	return s;
}

/////////////////////////////////////////////////////////////////////////////////////////

MIR_APP_DLL(HNETLIBUSER) Netlib_GetConnNlu(HANDLE hConn)
{
	if (GetNetlibHandleType(hConn) != NLH_CONNECTION)
		return NULL;

	return ((NetlibConnection*)hConn)->nlu;
}

/////////////////////////////////////////////////////////////////////////////////////////

MIR_APP_DLL(void) Netlib_Shutdown(HANDLE h)
{
	if (h) {
		WaitForSingleObject(hConnectionHeaderMutex, INFINITE);
		switch (GetNetlibHandleType(h)) {
		case NLH_CONNECTION:
			{
				NetlibConnection *nlc = (NetlibConnection*)h;
				if (!nlc->termRequested) {
					if (nlc->hSsl) sslApi.shutdown(nlc->hSsl);
					if (nlc->s != INVALID_SOCKET) shutdown(nlc->s, 2);
					if (nlc->s2 != INVALID_SOCKET) shutdown(nlc->s2, 2);
					nlc->termRequested = true;
				}
			}
			break;

		case NLH_BOUNDPORT:
			struct NetlibBoundPort *nlb = (NetlibBoundPort*)h;
			if (nlb->s != INVALID_SOCKET)
				shutdown(nlb->s, 2);
			break;
		}
		ReleaseMutex(hConnectionHeaderMutex);
	}
}

void UnloadNetlibModule(void)
{
	if (!bModuleInitialized || hConnectionHeaderMutex == NULL) return;

	NetlibUnloadIeProxy();
	NetlibUPnPDestroy();
	NetlibLogShutdown();

	DestroyHookableEvent(hRecvEvent); hRecvEvent = NULL;
	DestroyHookableEvent(hSendEvent); hSendEvent = NULL;

	for (int i = netlibUser.getCount(); i > 0; i--)
		Netlib_CloseHandle(netlibUser[i-1]);

	CloseHandle(hConnectionHeaderMutex);
	if (hConnectionOpenMutex)
		CloseHandle(hConnectionOpenMutex);
	WSACleanup();
}

int LoadNetlibModule(void)
{
	WSADATA wsadata;

	bModuleInitialized = TRUE;

	WSAStartup(MAKEWORD(2, 2), &wsadata);

	HookEvent(ME_OPT_INITIALISE, NetlibOptInitialise);

	hConnectionHeaderMutex = CreateMutex(NULL, FALSE, NULL);
	NetlibLogInit();

	connectionTimeout = 0;

	OSVERSIONINFOEX osvi = {0};
	osvi.dwOSVersionInfoSize = sizeof(osvi);
	if (GetVersionEx((LPOSVERSIONINFO)&osvi)) {
		// Connection limiting was introduced in Windows XP SP2 and later and set to 10 / sec
		if (osvi.dwMajorVersion == 5 && ((osvi.dwMinorVersion == 1 && osvi.wServicePackMajor >= 2) || osvi.dwMinorVersion > 1))
			connectionTimeout = 150;
		// Connection limiting has limits based on addition Windows Vista pre SP2
		else if (osvi.dwMajorVersion == 6 && osvi.wServicePackMajor < 2) {
			DWORD dwType = 0;
			tGetProductInfo pGetProductInfo = (tGetProductInfo) GetProcAddress(GetModuleHandleA("kernel32"), "GetProductInfo");
			if (pGetProductInfo != NULL) pGetProductInfo(6, 0, 0, 0, &dwType);
			switch(dwType) {
			case 0x01:  // Vista Ultimate edition have connection limit of 25 / sec - plenty for Miranda
			case 0x1c:
				break;

			case 0x02:  // Vista Home Basic edition have connection limit of 2 / sec
			case 0x05:
				connectionTimeout = 1000;
				break;

			default:    // all other editions have connection limit of 10 / sec
				connectionTimeout = 150;
				break;
			}
		}
		// Connection limiting is disabled by default and is controlled by registry setting in Windows Vista SP2 and later
		else if (osvi.dwMajorVersion >= 6) {
			static const char keyn[] = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
			static const char valn[] = "EnableConnectionRateLimiting";

			HKEY hSettings;
			if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keyn, 0, KEY_QUERY_VALUE, &hSettings) == ERROR_SUCCESS) {
				DWORD tValueLen, enabled;
				tValueLen = sizeof(enabled);
				if (RegQueryValueExA(hSettings, valn, NULL, NULL, (BYTE*)&enabled, &tValueLen) == ERROR_SUCCESS && enabled)
					connectionTimeout = 150;  // if enabled limit is set to 10 / sec
				RegCloseKey(hSettings);
			}
		}
	}

	hConnectionOpenMutex = connectionTimeout ? CreateMutex(NULL, FALSE, NULL) : NULL;
	g_LastConnectionTick = GetTickCount();

	hRecvEvent = CreateHookableEvent(ME_NETLIB_FASTRECV);
	hSendEvent = CreateHookableEvent(ME_NETLIB_FASTSEND);

	NetlibUPnPInit();
	NetlibLoadIeProxy();
	return 0;
}
