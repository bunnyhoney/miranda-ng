#include "stdafx.h"

//struct 

int Popup2Show(WPARAM, LPARAM lParam)
{

	HANDLE hNotify = (HANDLE)lParam;

	PopupData *pd_out = (PopupData *)mir_alloc(sizeof(PopupData));
	memset(pd_out, 0, sizeof(PopupData));

	PostMPMessage(MUM_CREATEPOPUP, (WPARAM)hNotify, (LPARAM)pd_out);
	PostMPMessage(MUM_NMUPDATE, (WPARAM)hNotify, 0);
	return 0;
}

INT_PTR svcPopup2Show(WPARAM wParam, LPARAM lParam)
{
	return Popup2Show(wParam, lParam);
}

int Popup2Update(WPARAM, LPARAM lParam)
{
	HANDLE hNotify = (HANDLE)lParam;
	PostMPMessage(MUM_NMUPDATE, (WPARAM)hNotify, 0);
	return 0;
}

int AvatarChanged(WPARAM, LPARAM)
{
	PostMPMessage(MUM_NMAVATAR, 0, 0);
	return 0;
}

INT_PTR svcPopup2Update(WPARAM wParam, LPARAM lParam)
{
	return Popup2Update(wParam, lParam);
}

int Popup2Remove(WPARAM, LPARAM lParam)
{
	HANDLE hNotify = (HANDLE)lParam;
	PostMPMessage(MUM_NMREMOVE, (WPARAM)hNotify, 0);
	return 0;
}

INT_PTR svcPopup2Remove(WPARAM wParam, LPARAM lParam)
{
	return Popup2Remove(wParam, lParam);
}

INT_PTR svcPopup2DefaultActions(WPARAM wParam, LPARAM)
{
	//PopupWindow *wnd = (PopupWindow *)MNotifyGetDWord((HANDLE)lParam, "Popup2/data", (DWORD)NULL);
	//if (!wnd) return 0;
	switch (wParam) {
	case 0:
		{ // send message
			//if (wnd->lchContact) CallServiceSync(MS_MSG_SENDMESSAGE, (WPARAM)wnd->lchContact, 0);
			//SendMessage(wnd->lchMain, UM_DESTROYPOPUP, 0, 0);
			break;
		}
	case 1:
		{ // dismiss popup
			//SendMessage(wnd->lchMain, UM_DESTROYPOPUP, 0, 0);
			break;
		}
	}
	return 0;
}

INT_PTR CALLBACK DlgProcPopups(HWND hwnd, UINT msg, WPARAM, LPARAM lParam)
{
	/* To change options use MNotifySet*(hNotify, ....) Apply/Cancel is implemented in notify.dll */
	HANDLE hNotify = (HANDLE)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_USER + 100:
		{
			// This will be extendet to handle array of handles for multiselect lParam
			// will be HANDLE * and wParam wil; store amount of handles passed
			hNotify = (HANDLE)lParam;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);
			return TRUE;
		}

	case WM_COMMAND:
		// This in different from Miranda options!
		SendMessage(GetParent(GetParent(hwnd)), PSM_CHANGED, 0, 0);
		break;
	}
	return FALSE;
}

int NotifyOptionsInitialize(WPARAM wParam, LPARAM)
{
	OPTIONSDIALOGPAGE odp = {};
	odp.pszTemplate = MAKEINTRESOURCEA(IDD_OPT_NOTIFY);
	odp.szTitle.a = LPGEN("YAPP Popups");
	odp.flags = ODPF_BOLDGROUPS;
	odp.pfnDlgProc = DlgProcPopups;
	CallService(MS_NOTIFY_OPT_ADDPAGE, wParam, (LPARAM)&odp);
	return 0;
}

int NotifyModulesLoaded(WPARAM, LPARAM)
{
	HookEvent(ME_NOTIFY_OPT_INITIALISE, NotifyOptionsInitialize);
	HookEvent(ME_AV_AVATARCHANGED, AvatarChanged);
	return 0;
}

void InitNotify()
{
	HookEvent(ME_NOTIFY_SHOW, Popup2Show);
	HookEvent(ME_NOTIFY_UPDATE, Popup2Update);
	HookEvent(ME_NOTIFY_REMOVE, Popup2Remove);

	CreateServiceFunction("Popup2/DefaultActions", svcPopup2DefaultActions);

	CreateServiceFunction(MS_POPUP2_SHOW, svcPopup2Show);
	CreateServiceFunction(MS_POPUP2_UPDATE, svcPopup2Update);
	CreateServiceFunction(MS_POPUP2_REMOVE, svcPopup2Remove);

	HookEvent(ME_SYSTEM_MODULESLOADED, NotifyModulesLoaded);
}
