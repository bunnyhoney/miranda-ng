/*

Miranda NG: the free IM client for Microsoft* Windows*

Copyright (c) 2012-18 Miranda NG team (https://miranda-ng.org),
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

MDatabaseCommon *currDb = nullptr;

/////////////////////////////////////////////////////////////////////////////////////////
// database functions

MIR_CORE_DLL(void) db_set_safety_mode(BOOL bNewMode)
{
	if (currDb)
		currDb->SetCacheSafetyMode(bNewMode != 0);
}

MIR_CORE_DLL(int) db_get_contact_count(void)
{
	return (currDb) ? currDb->GetContactCount() : 0;
}

MIR_CORE_DLL(MDatabaseCommon*) db_get_current()
{
	return currDb;
}

MIR_CORE_DLL(int) db_delete_module(MCONTACT hContact, const char *szModuleName)
{
	return (currDb) ? currDb->DeleteModule(hContact, szModuleName) : 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// contact functions

MIR_CORE_DLL(MCONTACT) db_add_contact(void)
{
	MCONTACT hNew = (currDb) ? currDb->AddContact() : 0;
	Netlib_Logf(nullptr, "New contact created: %d", hNew);
	return hNew;
}

MIR_CORE_DLL(int) db_delete_contact(MCONTACT hContact)
{
	ptrW wszPhoto(db_get_wsa(hContact, "ContactPhoto", "File"));
	if (wszPhoto != NULL)
		DeleteFile(wszPhoto);

	Netlib_Logf(nullptr, "Contact deleted: %d", hContact);
	return (currDb) ? currDb->DeleteContact(hContact) : 0;
}

MIR_CORE_DLL(int) db_is_contact(MCONTACT hContact)
{
	return (currDb) ? currDb->IsDbContact(hContact) : 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// enumerators

MIR_CORE_DLL(int) db_enum_modules(DBMODULEENUMPROC pFunc, void *param)
{
	return (currDb) ? currDb->EnumModuleNames(pFunc, param) : 0;
}

MIR_CORE_DLL(int) db_enum_residents(DBMODULEENUMPROC pFunc, void *param)
{
	return (currDb) ? currDb->EnumResidentSettings(pFunc, param) : 0;
}

EXTERN_C MIR_CORE_DLL(int) db_enum_settings(MCONTACT hContact, DBSETTINGENUMPROC pFunc, const char *szModule, void *param)
{
	return (currDb) ? currDb->EnumContactSettings(hContact, pFunc, szModule, param) : 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// getting data

MIR_CORE_DLL(int) db_get_b(MCONTACT hContact, const char *szModule, const char *szSetting, int errorValue)
{
	if (currDb != nullptr) {
		DBVARIANT dbv;
		if (!currDb->GetContactSetting(hContact, szModule, szSetting, &dbv))
		{
			switch(dbv.type) {
				case DBVT_BYTE:	return dbv.bVal;
				case DBVT_WORD:   return BYTE(dbv.wVal);
				case DBVT_DWORD:	return BYTE(dbv.dVal);
			}
			currDb->FreeVariant(&dbv);
		}
	}
	return errorValue;
}

MIR_CORE_DLL(int) db_get_w(MCONTACT hContact, const char *szModule, const char *szSetting, int errorValue)
{
	if (currDb != nullptr) {
		DBVARIANT dbv;
		if (!currDb->GetContactSetting(hContact, szModule, szSetting, &dbv)) {
			switch(dbv.type) {
				case DBVT_BYTE:	return dbv.bVal;
				case DBVT_WORD:   return dbv.wVal;
				case DBVT_DWORD:	return WORD(dbv.dVal);
			}
			currDb->FreeVariant(&dbv);
		}
	}
	return errorValue;
}

MIR_CORE_DLL(DWORD) db_get_dw(MCONTACT hContact, const char *szModule, const char *szSetting, DWORD errorValue)
{
	if (currDb != nullptr) {
		DBVARIANT dbv;
		if (!currDb->GetContactSetting(hContact, szModule, szSetting, &dbv)) {
			switch(dbv.type) {
				case DBVT_BYTE:	return dbv.bVal;
				case DBVT_WORD:   return dbv.wVal;
				case DBVT_DWORD:	return dbv.dVal;
			}
			currDb->FreeVariant(&dbv);
		}
	}

	return errorValue;
}

MIR_CORE_DLL(INT_PTR) db_get(MCONTACT hContact, const char *szModule, const char *szSetting, DBVARIANT *dbv)
{
	if (currDb == nullptr)
		return 1;

	return currDb->GetContactSetting(hContact, szModule, szSetting, dbv);
}

MIR_CORE_DLL(INT_PTR) db_get_s(MCONTACT hContact, const char *szModule, const char *szSetting, DBVARIANT *dbv, const int nType)
{
	if (currDb == nullptr)
		return 1;

	dbv->type = (BYTE)nType;
	return currDb->GetContactSettingStr(hContact, szModule, szSetting, dbv);
}

MIR_CORE_DLL(char*) db_get_sa(MCONTACT hContact, const char *szModule, const char *szSetting)
{
	if (currDb == nullptr)
		return nullptr;

	DBVARIANT dbv = { DBVT_ASCIIZ };
	return currDb->GetContactSettingStr(hContact, szModule, szSetting, &dbv) ? nullptr : dbv.pszVal;
}

MIR_CORE_DLL(wchar_t*) db_get_wsa(MCONTACT hContact, const char *szModule, const char *szSetting)
{
	if (currDb == nullptr)
		return nullptr;

	DBVARIANT dbv = { DBVT_WCHAR };
	return currDb->GetContactSettingStr(hContact, szModule, szSetting, &dbv) ? nullptr : dbv.pwszVal;
}

MIR_CORE_DLL(CMStringA) db_get_sm(MCONTACT hContact, LPCSTR szModule, LPCSTR szSetting)
{
	if (currDb == nullptr)
		return CMStringA();

	DBVARIANT dbv = { DBVT_ASCIIZ };
	if (currDb->GetContactSettingStr(hContact, szModule, szSetting, &dbv))
		return CMStringA();

	return CMStringA(ptrA(dbv.pszVal));
}

MIR_CORE_DLL(CMStringW) db_get_wsm(MCONTACT hContact, LPCSTR szModule, LPCSTR szSetting)
{
	if (currDb == nullptr)
		return CMStringW();

	DBVARIANT dbv = { DBVT_WCHAR };
	if (currDb->GetContactSettingStr(hContact, szModule, szSetting, &dbv))
		return CMStringW();

	return CMStringW(ptrW(dbv.pwszVal));
}

/////////////////////////////////////////////////////////////////////////////////////////
// getting static data

MIR_CORE_DLL(int) db_get_static(MCONTACT hContact, const char *szModule, const char *szSetting, char *pDest, int cbDest)
{
	if (currDb == nullptr)
		return 1;

	DBVARIANT dbv;
	dbv.type = DBVT_ASCIIZ;
	dbv.pszVal = pDest;
	dbv.cchVal = cbDest;
	return currDb->GetContactSettingStatic(hContact, szModule, szSetting, &dbv);
}

MIR_CORE_DLL(int) db_get_static_utf(MCONTACT hContact, const char *szModule, const char *szSetting, char *pDest, int cbDest)
{
	if (currDb == nullptr)
		return 1;

	DBVARIANT dbv;
	dbv.type = DBVT_UTF8;
	dbv.pszVal = pDest;
	dbv.cchVal = cbDest;
	return currDb->GetContactSettingStatic(hContact, szModule, szSetting, &dbv);
}

MIR_CORE_DLL(int) db_get_wstatic(MCONTACT hContact, const char *szModule, const char *szSetting, wchar_t *pDest, int cbDest)
{
	if (currDb == nullptr)
		return 1;

	DBVARIANT dbv;
	dbv.type = DBVT_WCHAR;
	dbv.pwszVal = pDest;
	dbv.cchVal = cbDest;
	return currDb->GetContactSettingStatic(hContact, szModule, szSetting, &dbv);
}

/////////////////////////////////////////////////////////////////////////////////////////
// setting data

MIR_CORE_DLL(INT_PTR) db_set(MCONTACT hContact, const char *szModule, const char *szSetting, DBVARIANT *dbv)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value = *dbv;
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_b(MCONTACT hContact, const char *szModule, const char *szSetting, BYTE val)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_BYTE;
	cws.value.bVal = val;
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_w(MCONTACT hContact, const char *szModule, const char *szSetting, WORD val)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_WORD;
	cws.value.wVal = val;
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_dw(MCONTACT hContact, const char *szModule, const char *szSetting, DWORD val)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_DWORD;
	cws.value.dVal = val;
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_s(MCONTACT hContact, const char *szModule, const char *szSetting, const char *val)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_ASCIIZ;
	cws.value.pszVal = (char*)(val == nullptr ? "" : val);
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_ws(MCONTACT hContact, const char *szModule, const char *szSetting, const wchar_t *val)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_WCHAR;
	cws.value.pwszVal = (wchar_t*)(val == nullptr ? L"" : val);
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_utf(MCONTACT hContact, const char *szModule, const char *szSetting, const char *val)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_UTF8;
	cws.value.pszVal = (char*)(val == nullptr ? "" : val);
	return currDb->WriteContactSetting(hContact, &cws);
}

MIR_CORE_DLL(INT_PTR) db_set_blob(MCONTACT hContact, const char *szModule, const char *szSetting, void *val, unsigned len)
{
	if (currDb == nullptr) return 1;

	DBCONTACTWRITESETTING cws;
	cws.szModule = szModule;
	cws.szSetting = szSetting;
	cws.value.type = DBVT_BLOB;
	cws.value.cpbVal = (WORD)len;
	cws.value.pbVal = (unsigned char*)val;
	return currDb->WriteContactSetting(hContact, &cws);
}

/////////////////////////////////////////////////////////////////////////////////////////
// events

MIR_CORE_DLL(MEVENT) db_event_add(MCONTACT hContact, DBEVENTINFO *dbei)
{
	return (currDb == nullptr) ? 0 : currDb->AddEvent(hContact, dbei);
}

MIR_CORE_DLL(int) db_event_count(MCONTACT hContact)
{
	return (currDb == nullptr) ? 0 : currDb->GetEventCount(hContact);
}

MIR_CORE_DLL(int) db_event_delete(MCONTACT hContact, MEVENT hDbEvent)
{
	return (currDb == nullptr) ? 0 : currDb->DeleteEvent(hContact, hDbEvent);
}

MIR_CORE_DLL(int) db_event_edit(MCONTACT hContact, MEVENT hDbEvent, DBEVENTINFO *dbei)
{
	return (currDb == nullptr) ? 0 : currDb->EditEvent(hContact, hDbEvent, dbei);
}

MIR_CORE_DLL(MEVENT) db_event_first(MCONTACT hContact)
{
	return (currDb == nullptr) ? 0 : currDb->FindFirstEvent(hContact);
}

MIR_CORE_DLL(MEVENT) db_event_firstUnread(MCONTACT hContact)
{
	return (currDb == nullptr) ? 0 : currDb->FindFirstUnreadEvent(hContact);
}

MIR_CORE_DLL(int) db_event_get(MEVENT hDbEvent, DBEVENTINFO *dbei)
{
	return (currDb == nullptr) ? 1 : currDb->GetEvent(hDbEvent, dbei);
}

MIR_CORE_DLL(int) db_event_getBlobSize(MEVENT hDbEvent)
{
	return (currDb == nullptr) ? 0 : currDb->GetBlobSize(hDbEvent);
}

MIR_CORE_DLL(MCONTACT) db_event_getContact(MEVENT hDbEvent)
{
	return (currDb == nullptr) ? 0 : currDb->GetEventContact(hDbEvent);
}

MIR_CORE_DLL(MEVENT) db_event_last(MCONTACT hContact)
{
	return (currDb == nullptr) ? 0 : currDb->FindLastEvent(hContact);
}

MIR_CORE_DLL(int) db_event_markRead(MCONTACT hContact, MEVENT hDbEvent)
{
	return (currDb == nullptr) ? 0 : currDb->MarkEventRead(hContact, hDbEvent);
}

MIR_CORE_DLL(MEVENT) db_event_next(MCONTACT hContact, MEVENT hDbEvent)
{
	return (currDb == nullptr) ? 0 : currDb->FindNextEvent(hContact, hDbEvent);
}

MIR_CORE_DLL(MEVENT) db_event_prev(MCONTACT hContact, MEVENT hDbEvent)
{
	return (currDb == nullptr) ? 0 : currDb->FindPrevEvent(hContact, hDbEvent);
}

MIR_CORE_DLL(MEVENT) db_event_getById(const char *szModule, const char *szId)
{
	return (currDb == nullptr) ? 0 : currDb->GetEventById(szModule, szId);
}

MIR_CORE_DLL(MEVENT) db_event_setId(const char *szModule, MEVENT hDbEvent, const char *szId)
{
	return (currDb == nullptr) ? 0 : currDb->SetEventId(szModule, hDbEvent, szId);
}

/////////////////////////////////////////////////////////////////////////////////////////
// misc functions

MIR_CORE_DLL(INT_PTR) db_free(DBVARIANT *dbv)
{
	return (currDb == nullptr) ? 1 : currDb->FreeVariant(dbv);
}

MIR_CORE_DLL(INT_PTR) db_unset(MCONTACT hContact, const char *szModule, const char *szSetting)
{
	if (currDb == nullptr)
		return 1;

	return currDb->DeleteContactSetting(hContact, szModule, szSetting);
}

MIR_CORE_DLL(DBCachedContact*) db_get_contact(MCONTACT hContact)
{
	return (currDb == nullptr) ? nullptr : currDb->getCache()->GetCachedContact(hContact);
}

MIR_CORE_DLL(MCONTACT) db_find_first(const char *szProto)
{
	return (currDb == nullptr) ? NULL : currDb->FindFirstContact(szProto);
}

MIR_CORE_DLL(MCONTACT) db_find_next(MCONTACT hContact, const char *szProto)
{
	return (currDb == nullptr) ? NULL : currDb->FindNextContact(hContact, szProto);
}

MIR_CORE_DLL(void) db_setCurrent(MDatabaseCommon *_db)
{
	currDb = _db;
	if (currDb == nullptr)
		return;

	// try to get the langpack's name from a profile
	ptrW langpack(db_get_wsa(NULL, "Langpack", "Current"));
	if (langpack && langpack[0] != '\0')
		LoadLangPack(langpack);
	else
		GetDefaultLang();
}

MIR_CORE_DLL(BOOL) db_set_resident(const char *szModule, const char *szService, BOOL bEnable)
{
	if (currDb == nullptr || szModule == nullptr || szService == nullptr)
		return FALSE;

	char str[MAXMODULELABELLENGTH * 2];
	mir_snprintf(str, "%s/%s", szModule, szService);
	return currDb->SetSettingResident(bEnable, str);
}
