#include "globals.h"

#define MSG_WND_CLASS "MIM_SNMsgWindow"

HWND HKHwnd;

enum KB_ACTIONS {KB_NEW_NOTE = 1, KB_TOGGLE_NOTES, KB_NEW_REMINDER};

void RegisterKeyBindings()
{
	HOTKEYDESC desc = {};
	desc.szSection.w = _A2W(SECTIONNAME);
	desc.dwFlags = HKD_UNICODE;

	desc.pszName = MODULENAME"/NewNote";
	desc.szDescription.w = LPGENW("New Note");
	desc.lParam = KB_NEW_NOTE;
	desc.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL|HOTKEYF_SHIFT, VK_INSERT);
	desc.pszService = MODULENAME"/MenuCommandAddNew";
	g_plugin.addHotkey(&desc);

	desc.pszName = MODULENAME"/ToggleNotesVis";
	desc.szDescription.w = LPGENW("Toggle Notes Visibility");
	desc.lParam = KB_TOGGLE_NOTES;
	desc.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL|HOTKEYF_SHIFT, VK_ADD);
	desc.pszService = MODULENAME"/MenuCommandShowHide";
	g_plugin.addHotkey(&desc);

	desc.pszName = MODULENAME"/BringNotesFront";
	desc.szDescription.w = LPGENW("Bring All Notes to Front");
	desc.lParam = KB_TOGGLE_NOTES;
	desc.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL|HOTKEYF_SHIFT, VK_HOME);
	desc.pszService = MODULENAME"/MenuCommandBringAllFront";
	g_plugin.addHotkey(&desc);

	desc.pszName = MODULENAME"/NewReminder";
	desc.szDescription.w = LPGENW("New Reminder");
	desc.lParam = KB_NEW_REMINDER;
	desc.DefHotKey = HOTKEYCODE(HOTKEYF_CONTROL|HOTKEYF_SHIFT, VK_SUBTRACT);
	desc.pszService = MODULENAME"/MenuCommandNewReminder";
	g_plugin.addHotkey(&desc);
}

LRESULT CALLBACK NotifyHotKeyWndProc(HWND AHwnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
	switch (Message) {
	case WM_TIMER:
		KillTimer(HKHwnd, 1026);
		BOOL b = CheckRemindersAndStart();
		SetTimer(HKHwnd, 1026, b ? REMINDER_UPDATE_INTERVAL_SHORT : REMINDER_UPDATE_INTERVAL, nullptr);

		return FALSE;
	}

	return DefWindowProc(AHwnd, Message, wParam, lParam);
}

void CreateMsgWindow(void)
{
	HWND hParent = nullptr;
	WNDCLASSEX TWC = { 0 };

	if (!GetClassInfoEx(hmiranda, MSG_WND_CLASS, &TWC)) {
		TWC.style = 0;
		TWC.cbClsExtra = 0;
		TWC.cbWndExtra = 0;
		TWC.hInstance = hmiranda;
		TWC.hIcon = nullptr;
		TWC.hCursor = nullptr;
		TWC.hbrBackground = nullptr;
		TWC.lpszMenuName = nullptr;
		TWC.lpszClassName = MSG_WND_CLASS;
		TWC.cbSize = sizeof(TWC);
		TWC.lpfnWndProc = NotifyHotKeyWndProc;
		RegisterClassEx(&TWC);
	}

	hParent = HWND_MESSAGE;

	HKHwnd = CreateWindowEx(WS_EX_TOOLWINDOW, MSG_WND_CLASS, "StickyNotes", 0, 0, 0, 0, 0, hParent, nullptr, hmiranda, nullptr);
	SetTimer(HKHwnd, 1026, REMINDER_UPDATE_INTERVAL, nullptr);
}

void DestroyMsgWindow(void)
{
	KillTimer(HKHwnd, 1026);
	DestroyWindow(HKHwnd);
}
