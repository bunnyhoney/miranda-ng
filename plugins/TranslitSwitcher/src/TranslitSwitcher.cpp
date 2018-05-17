/*
Copyright (C) 2007 Dmitry Titkov (C) 2010 tico-tico, Mataes

This is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this file; see the file license.txt.  If
not, write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.
*/

#include "stdafx.h"

int hLangpack;

struct CMPlugin : public PLUGIN<CMPlugin>
{
	CMPlugin() :
		PLUGIN<CMPlugin>(nullptr)
	{}
}
g_plugin;

/////////////////////////////////////////////////////////////////////////////////////////

PLUGININFOEX pluginInfoEx = {
	sizeof(PLUGININFOEX),
	__PLUGIN_NAME,
	PLUGIN_MAKE_VERSION(__MAJOR_VERSION, __MINOR_VERSION, __RELEASE_NUM, __BUILD_NUM),
	__DESCRIPTION,
	__AUTHOR,
	__COPYRIGHT,
	__AUTHORWEB,
	UNICODE_AWARE,
	// {0286947D-3140-4222-B5AD-2C92315E1C1E}
	{ 0x286947d, 0x3140, 0x4222, { 0xb5, 0xad, 0x2c, 0x92, 0x31, 0x5e, 0x1c, 0x1e } }
};

extern "C" __declspec(dllexport) PLUGININFOEX* MirandaPluginInfoEx(DWORD)
{
	return &pluginInfoEx;
}

/////////////////////////////////////////////////////////////////////////////////////////

static IconItem iconList[] =
{
	{ LPGEN("Switch Layout and Send"), "Switch Layout and Send", IDI_SWITCHSEND },
	{ LPGEN("Translit and Send"), "Translit and Send", IDI_TRANSLITSEND },
	{ LPGEN("Invert Case and Send"), "Invert Case and Send", IDI_INVERTSEND },
};

//-------------------------------------------------------------------------------------------------------
#define MS_TS_SWITCHLAYOUT "TranslitSwitcher/SwitchLayout"
INT_PTR ServiceSwitch(WPARAM, LPARAM lParam)
{
	SwitchLayout(lParam != 0);
	return 0;
}

#define MS_TS_TRANSLITLAYOUT "TranslitSwitcher/TranslitLayout"
INT_PTR ServiceTranslit(WPARAM, LPARAM lParam)
{
	TranslitLayout(lParam != 0);
	return 0;
}

#define MS_TS_INVERTCASE "TranslitSwitcher/InvertCase"
INT_PTR ServiceInvert(WPARAM, LPARAM lParam)
{
	InvertCase(lParam != 0);
	return 0;
}

int OnModulesLoaded(WPARAM, LPARAM)
{
	HookEvent(ME_MSG_BUTTONPRESSED, OnButtonPressed);

	Icon_Register(g_plugin.getInst(), "TabSRMM/TranslitSwitcher", iconList, _countof(iconList));

	BBButton bbd = {};
	bbd.bbbFlags = BBBF_ISIMBUTTON | BBBF_ISCHATBUTTON | BBBF_ISRSIDEBUTTON;
	bbd.pszModuleName = "Switch Layout and Send";
	bbd.pwszTooltip = TranslateT("Switch Layout and Send");
	bbd.hIcon = iconList[0].hIcolib;
	bbd.dwButtonID = 1;
	bbd.dwDefPos = 30;
	Srmm_AddButton(&bbd);

	bbd.pszModuleName = "Translit and Send";
	bbd.pwszTooltip = TranslateT("Translit and Send");
	bbd.hIcon = iconList[1].hIcolib;
	bbd.dwButtonID = 1;
	bbd.dwDefPos = 40;
	Srmm_AddButton(&bbd);

	bbd.pszModuleName = "Invert Case and Send";
	bbd.pwszTooltip = TranslateT("Invert Case and Send");
	bbd.hIcon = iconList[2].hIcolib;
	bbd.dwButtonID = 1;
	bbd.dwDefPos = 50;
	Srmm_AddButton(&bbd);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) int Load(void)
{
	mir_getLP(&pluginInfoEx);

	CreateServiceFunction(MS_TS_SWITCHLAYOUT, ServiceSwitch);
	CreateServiceFunction(MS_TS_TRANSLITLAYOUT, ServiceTranslit);
	CreateServiceFunction(MS_TS_INVERTCASE, ServiceInvert);

	HookEvent(ME_SYSTEM_MODULESLOADED, OnModulesLoaded);

	HOTKEYDESC hkd = {};
	hkd.dwFlags = HKD_UNICODE;
	hkd.pszName = "TranslitSwitcher/ConvertAllOrSelected";
	hkd.szDescription.w = LPGENW("Convert All / Selected");
	hkd.szSection.w = L"TranslitSwitcher";
	hkd.pszService = MS_TS_SWITCHLAYOUT;
	hkd.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL + HKCOMB_A, 'R') | HKF_MIRANDA_LOCAL;
	Hotkey_Register(&hkd);

	hkd.pszName = "TranslitSwitcher/ConvertLastOrSelected";
	hkd.szDescription.w = LPGENW("Convert Last / Selected");
	hkd.DefHotKey = HOTKEYCODE(HOTKEYF_ALT + HKCOMB_A, 'R') | HKF_MIRANDA_LOCAL;
	hkd.lParam = true;
	Hotkey_Register(&hkd);

	hkd.pszName = "TranslitSwitcher/TranslitAllOrSelected";
	hkd.szDescription.w = LPGENW("Translit All / Selected");
	hkd.pszService = MS_TS_TRANSLITLAYOUT;
	hkd.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL + HKCOMB_A, 'T') | HKF_MIRANDA_LOCAL;
	hkd.lParam = false;
	Hotkey_Register(&hkd);

	hkd.pszName = "TranslitSwitcher/TranslitLastOrSelected";
	hkd.szDescription.w = LPGENW("Translit Last / Selected");
	hkd.DefHotKey = HOTKEYCODE(HOTKEYF_ALT + HKCOMB_A, 'T') | HKF_MIRANDA_LOCAL;
	hkd.lParam = true;
	Hotkey_Register(&hkd);

	hkd.pszName = "TranslitSwitcher/InvertCaseAllOrSelected";
	hkd.szDescription.w = LPGENW("Invert Case All / Selected");
	hkd.pszService = MS_TS_INVERTCASE;
	hkd.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL + HKCOMB_A, 'Y') | HKF_MIRANDA_LOCAL;
	hkd.lParam = false;
	Hotkey_Register(&hkd);

	hkd.pszName = "TranslitSwitcher/InvertCaseLastOrSelected";
	hkd.szDescription.w = LPGENW("Invert Case Last / Selected");
	hkd.DefHotKey = HOTKEYCODE(HOTKEYF_ALT + HKCOMB_A, 'Y') | HKF_MIRANDA_LOCAL;
	hkd.lParam = true;
	Hotkey_Register(&hkd);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) int Unload(void)
{
	return 0;
}