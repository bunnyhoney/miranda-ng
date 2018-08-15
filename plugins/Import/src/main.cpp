/*

Import plugin for Miranda NG

Copyright (c) 2012-18 Miranda NG team (https://miranda-ng.org)

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

int g_iImportOptions;
MCONTACT g_hImportContact;

bool g_bServiceMode = false, g_bSendQuit = false;
HWND g_hwndWizard, g_hwndAccMerge;

CMPlugin g_plugin;

/////////////////////////////////////////////////////////////////////////////////////////

static PLUGININFOEX pluginInfoEx = {
	sizeof(PLUGININFOEX),
	__PLUGIN_NAME,
	PLUGIN_MAKE_VERSION(__MAJOR_VERSION, __MINOR_VERSION, __RELEASE_NUM, __BUILD_NUM),
	__DESCRIPTION,
	__AUTHOR,
	__COPYRIGHT,
	__AUTHORWEB,
	UNICODE_AWARE,
	// {2D77A746-00A6-4343-BFC5-F808CDD772EA}
	{0x2d77a746, 0xa6, 0x4343, {0xbf, 0xc5, 0xf8, 0x8, 0xcd, 0xd7, 0x72, 0xea}}
};

CMPlugin::CMPlugin() :
	PLUGIN<CMPlugin>(IMPORT_MODULE, pluginInfoEx)
{}

/////////////////////////////////////////////////////////////////////////////////////////
// MirandaInterfaces - returns the protocol interface to the core

extern "C" __declspec(dllexport) const MUUID MirandaInterfaces[] = { MIID_IMPORT, MIID_SERVICEMODE, MIID_LAST };

/////////////////////////////////////////////////////////////////////////////////////////
// Performs a primary set of actions upon plugin loading

static int ModulesLoaded(WPARAM, LPARAM)
{
	// menu item
	CMenuItem mi(&g_plugin);
	SET_UID(mi, 0x20ffaf55, 0xafa0, 0x4da3, 0xa9, 0x46, 0x20, 0x51, 0xa0, 0x24, 0xb, 0x41);
	mi.hIcolibItem = GetIconHandle(IDI_IMPORT);
	mi.name.a = LPGEN("&Import...");
	mi.position = 500050000;
	mi.pszService = MS_IMPORT_SERVICE;
	Menu_AddMainMenuItem(&mi);

	SET_UID(mi, 0x4D237903, 0x24F1, 0x41AD, 0x82, 0xeb, 0x8f, 0xff, 0xb7, 0x3c, 0x28, 0xcc);
	mi.pszService = MS_IMPORT_CONTACT;
	Menu_AddContactMenuItem(&mi);

	if (!db_get_b(NULL, IMPORT_MODULE, IMP_KEY_FR, 0)) {
		// Only autorun import wizard if at least one protocol is installed
		if (Accounts().getCount() > 0) {
			CallService(MS_IMPORT_SERVICE, 0, 0);
			db_set_b(NULL, IMPORT_MODULE, IMP_KEY_FR, 1);
		}
	}
	return 0;
}

static int OnExit(WPARAM, LPARAM)
{
	if (g_hwndWizard)
		SendMessage(g_hwndWizard, WM_CLOSE, 0, 0);
	if (g_hwndAccMerge)
		SendMessage(g_hwndAccMerge, WM_CLOSE, 0, 0);
	return 0;
}

static INT_PTR ServiceMode(WPARAM, LPARAM)
{
	g_bServiceMode = true;

	ptrW wszFullName(Utils_ReplaceVarsW(L"%miranda_userdata%\\%miranda_profilename%.dat.bak"));
	if (!_waccess(wszFullName, 0)) {
		g_iImportOptions = IOPT_ADDUNKNOWN + IOPT_COMPLETE + IOPT_CHECKDUPS;
		wcsncpy_s(importFile, MAX_PATH, wszFullName, _TRUNCATE);

		WizardDlgParam param = { IDD_PROGRESS, (LPARAM)ProgressPageProc };
		DialogBoxParam(g_plugin.getInst(), MAKEINTRESOURCE(IDD_WIZARD), nullptr, WizardDlgProc, LPARAM(&param));
		return SERVICE_CONTINUE;
	}

	g_bSendQuit = true;

	WizardDlgParam param = { IDD_WIZARDINTRO, (LPARAM)WizardIntroPageProc };
	CreateDialogParam(g_plugin.getInst(), MAKEINTRESOURCE(IDD_WIZARD), nullptr, WizardDlgProc, (LPARAM)&param);
	return SERVICE_ONLYDB;
}

static INT_PTR CustomImport(WPARAM wParam, LPARAM)
{
	MImportOptions *opts = (MImportOptions*)wParam;
	wcsncpy_s(importFile, MAX_PATH, opts->pwszFileName, _TRUNCATE);
	g_iImportOptions = opts->dwFlags;
	g_hImportContact = 0;

	WizardDlgParam param = { IDD_PROGRESS, (LPARAM)ProgressPageProc };
	return DialogBoxParam(g_plugin.getInst(), MAKEINTRESOURCE(IDD_WIZARD), nullptr, WizardDlgProc, LPARAM(&param));
}

static INT_PTR ImportContact(WPARAM hContact, LPARAM)
{
	CContactImportDlg dlg(hContact);
	if (!dlg.DoModal())
		return 0;

	g_hImportContact = hContact;
	g_iImportOptions = IOPT_HISTORY + dlg.getFlags();

	WizardDlgParam param = { IDD_PROGRESS, (LPARAM)ProgressPageProc };
	return DialogBoxParam(g_plugin.getInst(), MAKEINTRESOURCE(IDD_WIZARD), nullptr, WizardDlgProc, LPARAM(&param));
}

static INT_PTR ImportCommand(WPARAM, LPARAM)
{
	if (IsWindow(g_hwndWizard)) {
		SetForegroundWindow(g_hwndWizard);
		SetFocus(g_hwndWizard);
	}
	else {
		WizardDlgParam param = { IDD_WIZARDINTRO, (LPARAM)WizardIntroPageProc };
		CreateDialogParam(g_plugin.getInst(), MAKEINTRESOURCE(IDD_WIZARD), nullptr, WizardDlgProc, (LPARAM)&param);
	}

	return 0;
}

int CMPlugin::Load()
{
	CreateServiceFunction(MS_IMPORT_CONTACT, ImportContact);
	CreateServiceFunction(MS_IMPORT_SERVICE, ImportCommand);
	CreateServiceFunction(MS_SERVICEMODE_LAUNCH, ServiceMode);
	CreateServiceFunction(MS_IMPORT_RUN, CustomImport);
	RegisterIcons();

	HookEvent(ME_SYSTEM_MODULESLOADED, ModulesLoaded);
	HookEvent(ME_SYSTEM_OKTOEXIT, OnExit);

	INITCOMMONCONTROLSEX icex;
	icex.dwSize = sizeof(icex);
	icex.dwICC = ICC_DATE_CLASSES;
	InitCommonControlsEx(&icex);

	RegisterDbrw();
	RegisterMContacts();
	RegisterJson();
	return 0;
}
