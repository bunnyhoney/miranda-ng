/*

Jabber Protocol Plugin for Miranda NG

Copyright (c) 2002-04  Santithorn Bunchua
Copyright (c) 2005-12  George Hazan
Copyright (c) 2007     Maxim Mluhov
Copyright (�) 2012-16 Miranda NG project

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

#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "m_addcontact.h"
#include "jabber_disco.h"

/////////////////////////////////////////////////////////////////////////////////////////
// GetMyAwayMsg - obtain the current away message

INT_PTR __cdecl CJabberProto::GetMyAwayMsg(WPARAM wParam, LPARAM lParam)
{
	TCHAR *szStatus = NULL;

	mir_cslock lck(m_csModeMsgMutex);
	switch (wParam ? (int)wParam : m_iStatus) {
	case ID_STATUS_ONLINE:
		szStatus = m_modeMsgs.szOnline;
		break;
	case ID_STATUS_AWAY:
	case ID_STATUS_ONTHEPHONE:
	case ID_STATUS_OUTTOLUNCH:
		szStatus = m_modeMsgs.szAway;
		break;
	case ID_STATUS_NA:
		szStatus = m_modeMsgs.szNa;
		break;
	case ID_STATUS_DND:
	case ID_STATUS_OCCUPIED:
		szStatus = m_modeMsgs.szDnd;
		break;
	case ID_STATUS_FREECHAT:
		szStatus = m_modeMsgs.szFreechat;
		break;
	default: // Should not reach here
		break;
	}

	if (szStatus)
		return (lParam & SGMA_UNICODE) ? (INT_PTR)mir_t2u(szStatus) : (INT_PTR)mir_t2a(szStatus);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// JabberGetAvatar - retrieves the file name of my own avatar

INT_PTR __cdecl CJabberProto::JabberGetAvatar(WPARAM wParam, LPARAM lParam)
{
	TCHAR *buf = (TCHAR*)wParam;
	int size = (int)lParam;

	if (buf == NULL || size <= 0)
		return -1;

	if (!m_options.EnableAvatars)
		return -2;

	GetAvatarFileName(NULL, buf, size);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// JabberGetAvatarCaps - returns directives how to process avatars

INT_PTR __cdecl CJabberProto::JabberGetAvatarCaps(WPARAM wParam, LPARAM lParam)
{
	switch(wParam) {
	case AF_MAXSIZE:
		{
			POINT* size = (POINT*)lParam;
			if (size)
				size->x = size->y = 96;
		}
      return 0;

	case AF_PROPORTION:
		return PIP_NONE;

	case AF_FORMATSUPPORTED: // Jabber supports avatars of virtually all formats
		return 1;

	case AF_ENABLED:
		return m_options.EnableAvatars;
	}
	return -1;
}

/////////////////////////////////////////////////////////////////////////////////////////
// JabberGetAvatarInfo - retrieves the avatar info

INT_PTR __cdecl CJabberProto::JabberGetAvatarInfo(WPARAM wParam, LPARAM lParam)
{
	if (!m_options.EnableAvatars)
		return GAIR_NOAVATAR;

	PROTO_AVATAR_INFORMATION* pai = (PROTO_AVATAR_INFORMATION*)lParam;

	ptrA szHashValue( getStringA(pai->hContact, "AvatarHash"));
	if (szHashValue == NULL) {
		debugLogA("No avatar");
		return GAIR_NOAVATAR;
	}

	TCHAR tszFileName[MAX_PATH];
	GetAvatarFileName(pai->hContact, tszFileName, _countof(tszFileName));
	_tcsncpy_s(pai->filename, tszFileName, _TRUNCATE);

	pai->format = (pai->hContact == NULL) ? PA_FORMAT_PNG : getByte(pai->hContact, "AvatarType", 0);

	if (::_taccess(pai->filename, 0) == 0) {
		ptrA szSavedHash( getStringA(pai->hContact, "AvatarSaved"));
		if (szSavedHash != NULL && !mir_strcmp(szSavedHash, szHashValue)) {
			debugLogA("Avatar is Ok: %s == %s", szSavedHash, szHashValue);
			return GAIR_SUCCESS;
		}
	}

	if ((wParam & GAIF_FORCE) != 0 && pai->hContact != NULL && m_bJabberOnline) {
		ptrT tszJid( getTStringA(pai->hContact, "jid"));
		if (tszJid != NULL) {
			JABBER_LIST_ITEM *item = ListGetItemPtr(LIST_ROSTER, tszJid);
			if (item != NULL) {
				BOOL isXVcard = getByte(pai->hContact, "AvatarXVcard", 0);

				TCHAR szJid[JABBER_MAX_JID_LEN]; szJid[0] = 0;
				if (item->arResources.getCount() != NULL && !isXVcard)
					if (TCHAR *bestResName = ListGetBestClientResourceNamePtr(tszJid))
						mir_sntprintf(szJid, L"%s/%s", tszJid, bestResName);

				if (szJid[0] == 0)
					_tcsncpy_s(szJid, tszJid, _TRUNCATE);

				debugLog(L"Rereading %s for %s", isXVcard ? JABBER_FEAT_VCARD_TEMP : JABBER_FEAT_AVATAR, szJid);

				m_ThreadInfo->send((isXVcard) ?
					XmlNodeIq( AddIQ(&CJabberProto::OnIqResultGetVCardAvatar, JABBER_IQ_TYPE_GET, szJid)) << XCHILDNS(L"vCard", JABBER_FEAT_VCARD_TEMP) :
					XmlNodeIq( AddIQ(&CJabberProto::OnIqResultGetClientAvatar, JABBER_IQ_TYPE_GET, szJid)) << XQUERY(JABBER_FEAT_AVATAR));
				return GAIR_WAITFOR;
			}
		}
	}

	debugLogA("No avatar");
	return GAIR_NOAVATAR;
}

////////////////////////////////////////////////////////////////////////////////////////
// JabberGetEventTextChatStates - retrieves a chat state description from an event

INT_PTR __cdecl CJabberProto::OnGetEventTextChatStates(WPARAM, LPARAM lParam)
{
	DBEVENTGETTEXT *pdbEvent = (DBEVENTGETTEXT *)lParam;
	if (pdbEvent->dbei->cbBlob > 0) {
		if (pdbEvent->dbei->pBlob[0] == JABBER_DB_EVENT_CHATSTATES_GONE) {
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("closed chat session"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("closed chat session"));
		}
	}

	return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////
// OnGetEventTextPresence - retrieves presence state description from an event

INT_PTR __cdecl CJabberProto::OnGetEventTextPresence(WPARAM, LPARAM lParam)
{
	DBEVENTGETTEXT *pdbEvent = (DBEVENTGETTEXT *)lParam;
	if (pdbEvent->dbei->cbBlob > 0) {
		switch (pdbEvent->dbei->pBlob[0]) {
		case JABBER_DB_EVENT_PRESENCE_SUBSCRIBE:
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("sent subscription request"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("sent subscription request"));
			break;

		case JABBER_DB_EVENT_PRESENCE_SUBSCRIBED:
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("approved subscription request"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("approved subscription request"));
			break;

		case JABBER_DB_EVENT_PRESENCE_UNSUBSCRIBE:
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("declined subscription"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("declined subscription"));
			break;

		case JABBER_DB_EVENT_PRESENCE_UNSUBSCRIBED:
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("declined subscription"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("declined subscription"));
			break;

		case JABBER_DB_EVENT_PRESENCE_ERROR:
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("sent error presence"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("sent error presence"));
			break;

		default:
			if (pdbEvent->datatype == DBVT_WCHAR)
				return (INT_PTR)mir_tstrdup(TranslateT("sent unknown presence type"));
			else if (pdbEvent->datatype == DBVT_ASCIIZ)
				return (INT_PTR)mir_strdup(Translate("sent unknown presence type"));
			break;
		}
	}

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// JabberSetAvatar - sets an avatar without UI

INT_PTR __cdecl CJabberProto::JabberSetAvatar(WPARAM, LPARAM lParam)
{
	TCHAR *tszFileName = (TCHAR*)lParam;

	if (m_bJabberOnline) {
		SetServerVcard(TRUE, tszFileName);
		SendPresence(m_iDesiredStatus, false);
	}
	else if (tszFileName == NULL || tszFileName[0] == 0) {
		// Remove avatar
		TCHAR tFileName[ MAX_PATH ];
		GetAvatarFileName(NULL, tFileName, MAX_PATH);
		DeleteFile(tFileName);

		delSetting("AvatarSaved");
		delSetting("AvatarHash");
	}
	else {
		int fileIn = _topen(tszFileName, O_RDWR | O_BINARY, S_IREAD | S_IWRITE);
		if (fileIn == -1) {
			mir_free(tszFileName);
			return 1;
		}

		long dwPngSize = _filelength(fileIn);
		char *pResult = new char[ dwPngSize ];
		if (pResult == NULL) {
			_close(fileIn);
			mir_free(tszFileName);
			return 2;
		}

		_read(fileIn, pResult, dwPngSize);
		_close(fileIn);

		BYTE digest[MIR_SHA1_HASH_SIZE];
		mir_sha1_ctx sha1ctx;
		mir_sha1_init(&sha1ctx);
		mir_sha1_append(&sha1ctx, (BYTE*)pResult, dwPngSize);
		mir_sha1_finish(&sha1ctx, digest);

		TCHAR tFileName[MAX_PATH];
		GetAvatarFileName(NULL, tFileName, MAX_PATH);
		DeleteFile(tFileName);

		char buf[MIR_SHA1_HASH_SIZE*2+1];
		bin2hex(digest, sizeof(digest), buf);

		m_options.AvatarType = ProtoGetBufferFormat(pResult);

		GetAvatarFileName(NULL, tFileName, MAX_PATH);
		FILE *out = _tfopen(tFileName, L"wb");
		if (out != NULL) {
			fwrite(pResult, dwPngSize, 1, out);
			fclose(out);
		}
		delete[] pResult;

		setString("AvatarSaved", buf);
	}

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// JabberSetNickname - sets the user nickname without UI

INT_PTR __cdecl CJabberProto::JabberSetNickname(WPARAM wParam, LPARAM lParam)
{
	TCHAR *nickname = (wParam & SMNN_UNICODE) ? mir_u2t((WCHAR*)lParam) : mir_a2t((char*)lParam);

	setTString("Nick", nickname);
	SetServerVcard(FALSE, L"");
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// "/SendXML" - Allows external plugins to send XML to the server

INT_PTR __cdecl CJabberProto::ServiceSendXML(WPARAM, LPARAM lParam)
{
	return m_ThreadInfo->send((char*)lParam);
}

/////////////////////////////////////////////////////////////////////////////////////////
// "/GCGetToolTipText" - gets tooltip text

static const TCHAR *JabberEnum2AffilationStr[] = { LPGENT("None"), LPGENT("Outcast"), LPGENT("Member"), LPGENT("Admin"), LPGENT("Owner") },
	*JabberEnum2RoleStr[] = { LPGENT("None"), LPGENT("Visitor"), LPGENT("Participant"), LPGENT("Moderator") };

static void appendString(bool bIsTipper, const TCHAR *tszTitle, const TCHAR *tszValue, CMString &out)
{
	if (!out.IsEmpty())
		out.Append(bIsTipper ? L"\n" : L"\r\n");

	if (bIsTipper)
		out.AppendFormat(L"<b>%s</b>\t%s", TranslateTS(tszTitle), tszValue);
	else {
		TCHAR *p = TranslateTS(tszTitle);
		out.AppendFormat(L"%s%s\t%s", p, mir_tstrlen(p) <= 7 ? L"\t" : L"", tszValue);
	}
}

INT_PTR __cdecl CJabberProto::JabberGCGetToolTipText(WPARAM wParam, LPARAM lParam)
{
	if (!wParam || !lParam)
		return 0; //room global tooltip not supported yet

	JABBER_LIST_ITEM *item = ListGetItemPtr(LIST_CHATROOM, (TCHAR*)wParam);
	if (item == NULL)
		return 0;  //no room found

	pResourceStatus info( item->findResource((TCHAR*)lParam));
	if (info == NULL)
		return 0; //no info found

	// ok process info output will be:
	// JID:			real@jid/resource or
	// Nick:		Nickname
	// Status:		StatusText
	// Role:		Moderator
	// Affiliation:  Affiliation

	bool bIsTipper = db_get_b(NULL, "Tab_SRMsg", "adv_TipperTooltip", 0) && ServiceExists("mToolTip/HideTip");

	//JID:
	CMString outBuf;
	if (_tcschr(info->m_tszResourceName, _T('@')) != NULL)
		appendString(bIsTipper, LPGENT("JID:"), info->m_tszResourceName, outBuf);
	else if (lParam) //or simple nick
		appendString(bIsTipper, LPGENT("Nick:"), (TCHAR*)lParam, outBuf);

	// status
	if (info->m_iStatus >= ID_STATUS_OFFLINE && info->m_iStatus <= ID_STATUS_IDLE )
		appendString(bIsTipper, LPGENT("Status:"), pcli->pfnGetStatusModeDescription(info->m_iStatus, 0), outBuf);

	// status text
	if (info->m_tszStatusMessage)
		appendString(bIsTipper, LPGENT("Status message:"), info->m_tszStatusMessage, outBuf);

	// Role
	appendString(bIsTipper, LPGENT("Role:"), TranslateTS(JabberEnum2RoleStr[info->m_role]), outBuf);

	// Affiliation
	appendString(bIsTipper, LPGENT("Affiliation:"), TranslateTS(JabberEnum2AffilationStr[info->m_affiliation]), outBuf);

	// real jid
	if (info->m_tszRealJid)
		appendString(bIsTipper, LPGENT("Real JID:"), info->m_tszRealJid, outBuf);

	return (outBuf.IsEmpty() ? NULL : (INT_PTR)mir_tstrdup(outBuf));
}

// File Association Manager plugin support
INT_PTR __cdecl CJabberProto::JabberServiceParseXmppURI(WPARAM, LPARAM lParam)
{
	TCHAR *arg = (TCHAR *)lParam;
	if (arg == NULL)
		return 1;

	// skip leading prefix
	TCHAR szUri[ 1024 ];
	_tcsncpy_s(szUri, arg, _TRUNCATE);
	TCHAR *szJid = _tcschr(szUri, _T(':'));
	if (szJid == NULL)
		return 1;

	// skip //
	for (++szJid; *szJid == _T('/'); ++szJid);

	// empty jid?
	if (!*szJid)
		return 1;

	// command code
	TCHAR *szCommand = szJid;
	szCommand = _tcschr(szCommand, _T('?'));
	if (szCommand)
		*(szCommand++) = 0;

	// parameters
	TCHAR *szSecondParam = szCommand ? _tcschr(szCommand, _T(';')) : NULL;
	if (szSecondParam)
		*(szSecondParam++) = 0;

	// no command or message command
	if (!szCommand || (szCommand && !mir_tstrcmpi(szCommand, L"message"))) {
		// message
		if (!ServiceExists(MS_MSG_SENDMESSAGEW))
			return 1;

		TCHAR *szMsgBody = NULL;
		MCONTACT hContact = HContactFromJID(szJid, false);
		if (hContact == NULL)
			hContact = DBCreateContact(szJid, szJid, true, true);
		if (hContact == NULL)
			return 1;

		if (szSecondParam) { //there are parameters to message
			szMsgBody = _tcsstr(szSecondParam, L"body=");
			if (szMsgBody) {
				szMsgBody += 5;
				TCHAR *szDelim = _tcschr(szMsgBody, _T(';'));
				if (szDelim)
					szDelim = 0;
				JabberHttpUrlDecode(szMsgBody);
			}
		}

		CallService(MS_MSG_SENDMESSAGEW, hContact, (LPARAM)szMsgBody);
		return 0;
	}
	
	if (!mir_tstrcmpi(szCommand, L"roster")) {
		if (!HContactFromJID(szJid)) {
			PROTOSEARCHRESULT psr = { 0 };
			psr.cbSize = sizeof(psr);
			psr.flags = PSR_TCHAR;
			psr.nick.t = szJid;
			psr.id.t = szJid;

			ADDCONTACTSTRUCT acs;
			acs.handleType = HANDLE_SEARCHRESULT;
			acs.szProto = m_szModuleName;
			acs.psr = &psr;
			CallService(MS_ADDCONTACT_SHOW, 0, (LPARAM)&acs);
		}
		return 0;
	}
	
	// chat join invitation
	if (!mir_tstrcmpi(szCommand, L"join")) {
		GroupchatJoinRoomByJid(NULL, szJid);
		return 0;
	}
	
	// service discovery request
	if (!mir_tstrcmpi(szCommand, L"disco")) {
		OnMenuHandleServiceDiscovery(0, (LPARAM)szJid);
		return 0;
	}
	
	// ad-hoc commands
	if (!mir_tstrcmpi(szCommand, L"command")) {
		if (szSecondParam) {
			if (!_tcsnicmp(szSecondParam, L"node=", 5)) {
				szSecondParam += 5;
				if (!*szSecondParam)
					szSecondParam = NULL;
			}
			else szSecondParam = NULL;
		}
		CJabberAdhocStartupParams* pStartupParams = new CJabberAdhocStartupParams(this, szJid, szSecondParam);
		ContactMenuRunCommands(0, (LPARAM)pStartupParams);
		return 0;
	}
	
	// send file
	if (!mir_tstrcmpi(szCommand, L"sendfile")) {
		MCONTACT hContact = HContactFromJID(szJid, false);
		if (hContact == NULL)
			hContact = DBCreateContact(szJid, szJid, true, true);
		if (hContact == NULL)
			return 1;
		CallService(MS_FILE_SENDFILE, hContact, NULL);
		return 0;
	}

	return 1; /* parse failed */
}

// XEP-0224 support (Attention/Nudge)
INT_PTR __cdecl CJabberProto::JabberSendNudge(WPARAM hContact, LPARAM)
{
	if (!m_bJabberOnline)
		return 0;

	ptrT jid( getTStringA(hContact, "jid"));
	if (jid == NULL)
		return 0;

	TCHAR tszJid[JABBER_MAX_JID_LEN];
	TCHAR *szResource = ListGetBestClientResourceNamePtr(jid);
	if (szResource)
		mir_sntprintf(tszJid, L"%s/%s", jid, szResource);
	else
		_tcsncpy_s(tszJid, jid, _TRUNCATE);

	m_ThreadInfo->send(
		XmlNode(L"message") << XATTR(L"type", L"headline") << XATTR(L"to", tszJid)
			<< XCHILDNS(L"attention", JABBER_FEAT_ATTENTION));
	return 0;
}

BOOL CJabberProto::SendHttpAuthReply(CJabberHttpAuthParams *pParams, BOOL bAuthorized)
{
	if (!m_bJabberOnline || !pParams || !m_ThreadInfo)
		return FALSE;

	if (pParams->m_nType == CJabberHttpAuthParams::IQ) {
		XmlNodeIq iq(bAuthorized ? L"result" : L"error", pParams->m_szIqId, pParams->m_szFrom);
		if (!bAuthorized) {
			iq << XCHILDNS(L"confirm", JABBER_FEAT_HTTP_AUTH) << XATTR(L"id", pParams->m_szId)
					<< XATTR(L"method", pParams->m_szMethod) << XATTR(L"url", pParams->m_szUrl);
			iq << XCHILD(L"error") << XATTRI(L"code", 401) << XATTR(L"type", L"auth")
					<< XCHILDNS(L"not-authorized", L"urn:ietf:params:xml:xmpp-stanzas");
		}
		m_ThreadInfo->send(iq);
	}
	else if (pParams->m_nType == CJabberHttpAuthParams::MSG) {
		XmlNode msg(L"message");
		msg << XATTR(L"to", pParams->m_szFrom);
		if (!bAuthorized)
			msg << XATTR(L"type", L"error");
		if (pParams->m_szThreadId)
			msg << XCHILD(L"thread", pParams->m_szThreadId);

		msg << XCHILDNS(L"confirm", JABBER_FEAT_HTTP_AUTH) << XATTR(L"id", pParams->m_szId)
					<< XATTR(L"method", pParams->m_szMethod) << XATTR(L"url", pParams->m_szUrl);

		if (!bAuthorized)
			msg << XCHILD(L"error") << XATTRI(L"code", 401) << XATTR(L"type", L"auth")
					<< XCHILDNS(L"not-authorized", L"urn:ietf:params:xml:xmpp-stanzas");

		m_ThreadInfo->send(msg);
	}
	else return FALSE;

	return TRUE;
}

class CJabberDlgHttpAuth: public CJabberDlgBase
{
	typedef CJabberDlgBase CSuper;

public:
	CJabberDlgHttpAuth(CJabberProto *proto, HWND hwndParent, CJabberHttpAuthParams *pParams):
		CSuper(proto, IDD_HTTP_AUTH, true),
		m_txtInfo(this, IDC_EDIT_HTTP_AUTH_INFO),
		m_btnAuth(this, IDOK),
		m_btnDeny(this, IDCANCEL),
		m_pParams(pParams)
	{
		SetParent(hwndParent);

		m_btnAuth.OnClick = Callback(this, &CJabberDlgHttpAuth::btnAuth_OnClick);
		m_btnDeny.OnClick = Callback(this, &CJabberDlgHttpAuth::btnDeny_OnClick);
	}

	void OnInitDialog()
	{
		CSuper::OnInitDialog();

		Window_SetIcon_IcoLib(m_hwnd, g_GetIconHandle(IDI_OPEN));

		SetDlgItemText(m_hwnd, IDC_TXT_URL, m_pParams->m_szUrl);
		SetDlgItemText(m_hwnd, IDC_TXT_FROM, m_pParams->m_szFrom);
		SetDlgItemText(m_hwnd, IDC_TXT_ID, m_pParams->m_szId);
		SetDlgItemText(m_hwnd, IDC_TXT_METHOD, m_pParams->m_szMethod);
	}

	BOOL SendReply(BOOL bAuthorized)
	{
		BOOL bRetVal = m_proto->SendHttpAuthReply(m_pParams, bAuthorized);
		m_pParams->Free();
		mir_free(m_pParams);
		m_pParams = NULL;
		return bRetVal;
	}

	void btnAuth_OnClick(CCtrlButton*)
	{
		SendReply(TRUE);
		Close();
	}
	void btnDeny_OnClick(CCtrlButton*)
	{
		SendReply(FALSE);
		Close();
	}

	UI_MESSAGE_MAP(CJabberDlgHttpAuth, CSuper);
		UI_MESSAGE(WM_CTLCOLORSTATIC, OnCtlColorStatic);
	UI_MESSAGE_MAP_END();

	INT_PTR OnCtlColorStatic(UINT, WPARAM, LPARAM)
	{
		return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
	}

private:
	CCtrlEdit	m_txtInfo;
	CCtrlButton	m_btnAuth;
	CCtrlButton	m_btnDeny;

	CJabberHttpAuthParams *m_pParams;
};

// XEP-0070 support (http auth)
INT_PTR __cdecl CJabberProto::OnHttpAuthRequest(WPARAM wParam, LPARAM lParam)
{
	CLISTEVENT *pCle = (CLISTEVENT *)lParam;
	CJabberHttpAuthParams *pParams = (CJabberHttpAuthParams *)pCle->lParam;
	if (!pParams)
		return 0;

	CJabberDlgHttpAuth *pDlg = new CJabberDlgHttpAuth(this, (HWND)wParam, pParams);
	if (!pDlg) {
		pParams->Free();
		mir_free(pParams);
		return 0;
	}

	pDlg->Show();

	return 0;
}
