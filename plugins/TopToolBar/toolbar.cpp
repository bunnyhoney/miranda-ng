
#include "common.h"

#define OLD_TBBUTTON_SIZE  (offsetof(TTBButton, pszTooltipUp))

pfnCustomProc g_CustomProc = NULL;
LPARAM g_CustomProcParam = 0;
TTBCtrl *g_ctrl = NULL;

INT_PTR OnEventFire(WPARAM wParam, LPARAM lParam);

HWND hwndContactList = 0;

int nextButtonId = 200;

HANDLE hTTBModuleLoaded, hTTBInitButtons;
static WNDPROC buttonWndProc;

CRITICAL_SECTION csButtonsHook;

int sortfunc(const TopButtonInt* a, const TopButtonInt* b)
{
	return a->arrangedpos - b->arrangedpos;
}

LIST<TopButtonInt> Buttons(8, sortfunc);

static void SetAllBitmaps()
{
	mir_cslock lck(csButtonsHook);
	for (int i = 0; i < Buttons.getCount(); i++)
		Buttons[i]->SetBitmap();
}

TopButtonInt* idtopos(int id, int* pPos)
{
	for ( int i = 0; i < Buttons.getCount(); i++)
		if (Buttons[i]->id == id) {
			if (pPos) *pPos = i;
			return Buttons[i];
		}

	if (pPos) *pPos = -1;
	return NULL;
}

//----- Service buttons -----
void InsertSBut(int i)
{
	TTBButton ttb = { 0 };
	ttb.cbSize = sizeof(ttb);
	ttb.hIconDn = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_RUN), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
	ttb.hIconUp = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_RUN), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
	ttb.dwFlags = TTBBF_VISIBLE|TTBBF_ISSBUTTON|TTBBF_INTERNAL;
	ttb.wParamDown = i;
	TTBAddButton(( WPARAM )&ttb, 0);
}

void LoadAllSButs()
{
	//must be locked
	int cnt = DBGetContactSettingByte(0, TTB_OPTDIR, "ServiceCnt", 0);
	if (cnt > 0) {
		for (int i = 1; i<=cnt; i++)
		InsertSBut(i);
	}
}

//----- Launch buttons -----
INT_PTR LaunchService(WPARAM wParam, LPARAM lParam)
{
	PROCESS_INFORMATION pi;
	STARTUPINFO si = {0};
	si.cb = sizeof(si);

	if ( CreateProcess(NULL, Buttons[lParam]->ptszProgram, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}

	return 0;
}

void InsertLBut(int i)
{
	TTBButton ttb = { 0 };
	ttb.cbSize = sizeof(ttb);
	ttb.hIconDn = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_RUN), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
	ttb.dwFlags = TTBBF_VISIBLE | TTBBF_ISLBUTTON | TTBBF_INTERNAL;
	ttb.name = LPGEN("Default");
	ttb.program = _T("Execute Path");
	ttb.wParamDown = i;
	TTBAddButton(( WPARAM )&ttb, 0);
}

void LoadAllLButs()
{
	//must be locked
	int cnt = DBGetContactSettingByte(0, TTB_OPTDIR, "LaunchCnt", 0);
	for (int i = 0; i < cnt; i++)
		InsertLBut(i);
}

//----- Separators -----

void InsertSeparator(int i)
{
	TTBButton ttb = { 0 };
	ttb.cbSize = sizeof(ttb);
	ttb.dwFlags = TTBBF_VISIBLE | TTBBF_ISSEPARATOR | TTBBF_INTERNAL;
	ttb.wParamDown = i;
	TTBAddButton((WPARAM)&ttb, 0);
}

void LoadAllSeparators()
{
	//must be locked
	int cnt = DBGetContactSettingByte(0, TTB_OPTDIR, "SepCnt", 0);
	for (int i = 0; i < cnt; i++)
		InsertSeparator(i);
}

int SaveAllButtonsOptions()
{
	int SeparatorCnt = 0;
	int LaunchCnt = 0;
	{
		mir_cslock lck(csButtonsHook);
		for (int i = 0; i < Buttons.getCount(); i++)
			Buttons[i]->SaveSettings(&SeparatorCnt, &LaunchCnt);
	}
	DBWriteContactSettingByte(0, TTB_OPTDIR, "SepCnt", SeparatorCnt);
	DBWriteContactSettingByte(0, TTB_OPTDIR, "LaunchCnt", LaunchCnt);
	return 0;
}

INT_PTR TTBRemoveButton(WPARAM wParam, LPARAM lParam)
{
	mir_cslock lck(csButtonsHook);

	int idx;
	TopButtonInt* b = idtopos(wParam, &idx);
	if (b == NULL)
		return -1;
	
	RemoveFromOptions(b->id);

	Buttons.remove(idx);
	delete b;

	ArrangeButtons();
	return 0;
}

static bool nameexists(const char *name)
{
	if (name == NULL)
		return false;

	for (int i = 0; i < Buttons.getCount(); i++)
		if ( !lstrcmpA(Buttons[i]->pszName, name))
			return true;

	return false;
}

HICON LoadIconFromLibrary(char *Name, HICON hIcon, HANDLE& phIcolib)
{		
	char iconame[256];
	_snprintf(iconame, SIZEOF(iconame), "toptoolbar_%s", Name);
	if (phIcolib == NULL) {
		SKINICONDESC sid = {0};
		sid.cbSize = sizeof(sid);
		sid.pszSection = "Toolbar";				
		sid.pszName = iconame;
		sid.pszDefaultFile = NULL;
		sid.pszDescription = Name;
		sid.hDefaultIcon = hIcon;
		phIcolib = Skin_AddIcon(&sid);
	}
	return Skin_GetIconByHandle(phIcolib);
}

static void ReloadIcons()
{
	mir_cslock lck(csButtonsHook);
	for (int i = 0; i < Buttons.getCount(); i++) {
		TopButtonInt* b = Buttons[i];

		char buf[256];
		if (b->hIconHandleUp) {
			sprintf(buf, "%s_up", b->pszName);
			b->hIconUp = LoadIconFromLibrary(buf, b->hIconUp, b->hIconHandleUp);
		}
		if (b->hIconHandleDn) {
			sprintf(buf, "%s_dn", b->pszName);
			b->hIconDn = LoadIconFromLibrary(buf, b->hIconDn, b->hIconHandleDn);
		}
	}
}

TopButtonInt* CreateButton(TTBButton* but)
{
	TopButtonInt* b = new TopButtonInt;
	b->id = nextButtonId++;

	b->dwFlags = but->dwFlags;

	b->wParamUp = but->wParamUp;
	b->lParamUp = but->lParamUp;
	b->wParamDown = but->wParamDown;
	b->lParamDown = but->lParamDown;

	if ( !(b->dwFlags & TTBBF_ISSEPARATOR)) {
		b->bPushed = (but->dwFlags & TTBBF_PUSHED) ? TRUE : FALSE;

		if (but->dwFlags & TTBBF_ISLBUTTON) {
			b->ptszProgram = mir_tstrdup(but->program);
			b->pszService = mir_strdup(TTB_LAUNCHSERVICE);
		}
		else {
			b->ptszProgram = NULL;
			b->pszService = mir_strdup(but->pszService);
		}

		b->pszName = mir_strdup(but->name);

		if (b->dwFlags & TTBBF_ICONBYHANDLE) {
			b->hIconUp = Skin_GetIconByHandle(b->hIconHandleUp = but->hIconHandleUp);
			if (but->hIconHandleDn)
				b->hIconDn = Skin_GetIconByHandle(b->hIconHandleDn = but->hIconHandleDn);
			else
				b->hIconDn = 0, b->hIconHandleDn = 0;
		}
		else {
			char buf[256];
			mir_snprintf(buf, SIZEOF(buf), (b->hIconDn) ? "%s_up" : "%s", b->pszName);
			b->hIconUp = LoadIconFromLibrary(buf, but->hIconUp, b->hIconHandleUp);
			if (b->hIconDn) {
				mir_snprintf(buf, SIZEOF(buf), "%s_dn", b->pszName);
				b->hIconDn = LoadIconFromLibrary(buf, but->hIconDn, b->hIconHandleDn);
			}
		}

		if (but->cbSize > OLD_TBBUTTON_SIZE) {
			b->ptszTooltipUp = mir_a2t(but->pszTooltipUp);
			b->ptszTooltipDn = mir_a2t(but->pszTooltipDn);
		}
	}
	return b;
}

INT_PTR TTBAddButton(WPARAM wParam, LPARAM lParam)
{
	if (wParam == 0)
		return -1;

	TopButtonInt* b;
	{	
		mir_cslock lck(csButtonsHook);

		TTBButton *but = (TTBButton*)wParam;
		if (but->cbSize != sizeof(TTBButton) && but->cbSize != OLD_TBBUTTON_SIZE)
			return -1;

		if ( !(but->dwFlags && TTBBF_ISLBUTTON) && nameexists(but->name))
			return -1;

		b = CreateButton(but);
		b->LoadSettings();
		Buttons.insert(b);
		b->CreateWnd();
	}

	ArrangeButtons();
	AddToOptions(b);
	return b->id;
}

int ArrangeButtons()
{
	mir_cslock lck(csButtonsHook);

	RECT rcClient;
	GetClientRect(g_ctrl->hWnd, &rcClient);
	int nBarSize = rcClient.right - rcClient.left;
	if (nBarSize == 0)
		return g_ctrl->nButtonHeight;

	g_ctrl->nLineCount = 0;

	int i, ypos = 1, xpos = g_ctrl->nButtonSpace, nextX = 0, y = 0;
	int newheight = g_ctrl->nButtonHeight+1;

	LIST<TopButtonInt> tmpList = Buttons;
	for (i=tmpList.getCount()-1; i >= 0; i--) {
		TopButtonInt *b = tmpList[i];
		if (b->hwnd == NULL || !(b->dwFlags & TTBBF_VISIBLE)) {
			ShowWindow(b->hwnd, SW_HIDE);
			tmpList.remove(i);
		}
	}

	if (tmpList.getCount() == 0)
		return g_ctrl->nButtonHeight;

	HDWP hdwp = BeginDeferWindowPos(tmpList.getCount());

	bool bWasButttonBefore;
	int  nUsedWidth, iFirstButtonId = 0, iLastButtonId = 0;

	do
	{
		g_ctrl->nLineCount++;
		bWasButttonBefore = false;
		nUsedWidth = 0;

		for (i=iFirstButtonId; i < tmpList.getCount(); i++) {
			TopButtonInt *b = tmpList[i];

			int width = (b->isSep()) ? SEPWIDTH+2 : g_ctrl->nButtonWidth + ((bWasButttonBefore) ? g_ctrl->nButtonSpace : 0);
			if (nUsedWidth + width > nBarSize)
				break;

			nUsedWidth += width;
			iLastButtonId = i+1;
			bWasButttonBefore = !b->isSep();
		}

		int nFreeSpace = nBarSize - nUsedWidth;

		for (i=iFirstButtonId; i < iLastButtonId; i++) {
			TopButtonInt *b = tmpList[i];

			hdwp = DeferWindowPos(hdwp, b->hwnd, NULL, nextX, y, 0,	0,	SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
			if ( b->isSep())
				nextX += SEPWIDTH+2;
			else
				nextX += g_ctrl->nButtonWidth + g_ctrl->nButtonSpace;
		}

		if (iFirstButtonId == iLastButtonId)
			break;

		iFirstButtonId = iLastButtonId;
		y += g_ctrl->nButtonHeight + g_ctrl->nButtonSpace;
		nextX = 0;
		if (g_ctrl->bSingleLine)
			break;
	}
		while (iFirstButtonId < tmpList.getCount() && y >= 0 && (g_ctrl->bAutoSize || (y + g_ctrl->nButtonHeight <= rcClient.bottom - rcClient.top)));		

	for (i=iFirstButtonId; i < tmpList.getCount(); i++)
		hdwp = DeferWindowPos(hdwp, tmpList[i]->hwnd, NULL, nextX, y, 0,	0,	SWP_NOSIZE | SWP_NOZORDER | SWP_HIDEWINDOW);

	if (hdwp)
		EndDeferWindowPos(hdwp);

	return (g_ctrl->nButtonHeight + g_ctrl->nButtonSpace)*g_ctrl->nLineCount - g_ctrl->nButtonSpace;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Toolbar services

//wparam = hTTBButton
//lparam = state 
INT_PTR TTBSetState(WPARAM wParam, LPARAM lParam)
{
	mir_cslock lck(csButtonsHook);

	TopButtonInt* b = idtopos(wParam);
	if (b == NULL)
		return -1;

	b->bPushed = (lParam & TTBST_PUSHED)?TRUE:FALSE;
	b->bPushed = (lParam & TTBST_RELEASED)?FALSE:TRUE;
	b->SetBitmap();
	return 0;
}

//wparam = hTTBButton
//lparam = 0
//return = state
INT_PTR TTBGetState(WPARAM wParam, LPARAM lParam)
{
	mir_cslock lck(csButtonsHook);
	TopButtonInt* b = idtopos(wParam);
	if (b == NULL)
		return -1;

	int retval = (b->bPushed == TRUE) ? TTBST_PUSHED : TTBST_RELEASED;
	return retval;
}

INT_PTR TTBGetOptions(WPARAM wParam, LPARAM lParam)
{
	INT_PTR retval;

	mir_cslock lck(csButtonsHook);
	TopButtonInt* b = idtopos(wParam);
	if (b == NULL)
		return -1;

	switch(LOWORD(wParam)) {
	case TTBO_FLAGS:
		retval = b->dwFlags & (~TTBBF_PUSHED);
		if (b->bPushed)
			retval |= TTBBF_PUSHED;
		break;

	case TTBO_TIPNAME:
		retval = (INT_PTR)b->ptszTooltip;
		break;

	case TTBO_ALLDATA:
		if (lParam) {
			lpTTBButton lpTTB = (lpTTBButton)lParam;
			if (lpTTB->cbSize != sizeof(TTBButton))
				break;
				
			lpTTB->dwFlags = b->dwFlags & (~TTBBF_PUSHED);
			if (b->bPushed)
				lpTTB->dwFlags |= TTBBF_PUSHED;

			lpTTB->hIconDn = b->hIconDn;
			lpTTB->hIconUp = b->hIconUp;

			lpTTB->lParamUp = b->lParamUp;
			lpTTB->wParamUp = b->wParamUp;
			lpTTB->lParamDown = b->lParamDown;
			lpTTB->wParamDown = b->wParamDown;

			if (b->dwFlags & TTBBF_ISLBUTTON)
				replaceStrT(lpTTB->program, b->ptszProgram);
			else
				replaceStr(lpTTB->pszService, b->pszService);

			retval = ( INT_PTR )lpTTB;
		}
		break;

	default:
		retval = -1;
		break;
	}
	
	return retval;
}

INT_PTR TTBSetOptions(WPARAM wParam, LPARAM lParam)
{
	int retval;

	mir_cslock lck(csButtonsHook);
	TopButtonInt* b = idtopos(HIWORD(wParam));
	if (b == NULL)
		return -1;

	switch(LOWORD(wParam)) {
	case TTBO_FLAGS:
		if (b->dwFlags == lParam)
			break;

		retval = b->CheckFlags(lParam);
		
		if (retval & TTBBF_PUSHED)
			b->SetBitmap();
		if (retval & TTBBF_VISIBLE) {
			ArrangeButtons();
			b->SaveSettings(0,0);
		}
				
		retval = 1;
		break;

	case TTBO_TIPNAME:
		if (lParam == 0)
			break;

		replaceStrT(b->ptszTooltip, TranslateTS( _A2T((LPCSTR)lParam)));
		SendMessage(b->hwnd,BUTTONADDTOOLTIP,(WPARAM)b->ptszTooltip,BATF_UNICODE);
		retval = 1;
		break;

	case TTBO_ALLDATA:
		if (lParam) {
			lpTTBButton lpTTB = (lpTTBButton)lParam;
			if (lpTTB->cbSize != sizeof(TTBButton))
				break;

			retval = b->CheckFlags(lpTTB->dwFlags);

			int changed = 0;
			if (b->hIconUp != lpTTB->hIconUp) {
				b->hIconUp = lpTTB->hIconUp;
				changed = 1;
			}
			if (b->hIconDn != lpTTB->hIconDn) {
				b->hIconDn = lpTTB->hIconDn;
				changed = 1;
			}
			if (changed)
				b->SetBitmap();

			if (retval & TTBBF_VISIBLE) {
				ArrangeButtons();
				b->SaveSettings(0,0);
			}

			if (b->dwFlags & TTBBF_ISLBUTTON)
				replaceStrT(b->ptszProgram, lpTTB->program);
			else 
				replaceStr(b->pszService, lpTTB->pszService);

			b->lParamUp = lpTTB->lParamUp;
			b->wParamUp = lpTTB->wParamUp;
			b->lParamDown = lpTTB->lParamDown;
			b->wParamDown = lpTTB->wParamDown;

			retval = 1;
		}
		break;

	default:
		retval = -1;
		break;
	}
	
	return retval;
}

int OnIconChange(WPARAM wParam, LPARAM lParam)
{
	ReloadIcons();
	SetAllBitmaps();
	return 0;
}

static int OnBGChange(WPARAM wParam, LPARAM lParam)
{
	LoadBackgroundOptions();
	return 0;
}

static INT_PTR TTBSetCustomProc(WPARAM wParam, LPARAM lParam)
{
	g_CustomProc = (pfnCustomProc)wParam;
	g_CustomProcParam = lParam;
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

int OnModulesLoad(WPARAM wParam, LPARAM lParam)
{
	if (!ServiceExists(MS_CLIST_FRAMES_ADDFRAME)) {
		MessageBox(0, TranslateT("Frames Services not found - plugin disabled.You need MultiWindow plugin."), _T("TopToolBar"), 0);
		return 0;
	}

	LoadAllSeparators();
	LoadAllLButs();

	ArrangeButtons();

	HANDLE hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);//anonymous event
	if (hEvent != 0)
		CallService(MS_SYSTEM_WAITONHANDLE, (WPARAM)hEvent, (LPARAM)"TTB_ONSTARTUPFIRE");
	
	if ( HookEvent(ME_BACKGROUNDCONFIG_CHANGED, OnBGChange)) {
		char buf[256];
		sprintf(buf, "TopToolBar Background/%s", TTB_OPTDIR);
		CallService(MS_BACKGROUNDCONFIG_REGISTER, (WPARAM)buf, 0);
	}	
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

static LRESULT CALLBACK TTBButtonWndProc(HWND hwnd, UINT msg,  WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = buttonWndProc(hwnd, msg, wParam, lParam);

	if (msg == WM_NCCREATE) {
		TopButtonInt* p = (TopButtonInt*)((CREATESTRUCT*)lParam)->lpCreateParams;
		if (g_CustomProc)
			g_CustomProc((HANDLE)p->id, hwnd, g_CustomProcParam);
	}

	return lResult;
}

int LoadToolbarModule()
{
	g_ctrl = (TTBCtrl*)mir_calloc( sizeof(TTBCtrl));
	g_ctrl->nButtonHeight = db_get_dw(0, TTB_OPTDIR, "BUTTHEIGHT", DEFBUTTHEIGHT);
	g_ctrl->nButtonWidth = db_get_dw(0, TTB_OPTDIR, "BUTTWIDTH", DEFBUTTWIDTH);
	g_ctrl->nButtonSpace = db_get_dw(0, TTB_OPTDIR, "BUTTGAP", DEFBUTTGAP);
	g_ctrl->nLastHeight = db_get_dw(0, TTB_OPTDIR, "LastHeight", DEFBUTTHEIGHT);

	g_ctrl->bFlatButtons = db_get_b(0, TTB_OPTDIR, "UseFlatButton", true);
	g_ctrl->bSingleLine = db_get_b(0, TTB_OPTDIR, "SingleLine", true);
	g_ctrl->bAutoSize = db_get_b(0, TTB_OPTDIR, "AutoSize", true);

	InitializeCriticalSection(&csButtonsHook);
	hBmpSeparator = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_SEP));

	HookEvent(ME_SYSTEM_MODULESLOADED, OnModulesLoad);
	HookEvent(ME_SKIN2_ICONSCHANGED, OnIconChange);
	HookEvent(ME_OPT_INITIALISE, TTBOptInit);

	hTTBModuleLoaded = CreateHookableEvent(ME_TTB_MODULELOADED);
	hTTBInitButtons = CreateHookableEvent(ME_TTB_INITBUTTONS);
	SetHookDefaultForHookableEvent(hTTBInitButtons, InitInternalButtons);

	CreateServiceFunction("TopToolBar/AddButton", TTBAddButton);
	CreateServiceFunction(MS_TTB_REMOVEBUTTON, TTBRemoveButton);

	CreateServiceFunction(MS_TTB_SETBUTTONSTATE, TTBSetState);
	CreateServiceFunction(MS_TTB_GETBUTTONSTATE, TTBGetState);
	
	CreateServiceFunction(MS_TTB_GETBUTTONOPTIONS, TTBGetOptions);
	CreateServiceFunction(MS_TTB_SETBUTTONOPTIONS, TTBSetOptions);

	CreateServiceFunction(TTB_LAUNCHSERVICE, LaunchService);
	
	CreateServiceFunction("TopToolBar/SetCustomProc", TTBSetCustomProc);
	CreateServiceFunction("TTB_ONSTARTUPFIRE", OnEventFire);

	buttonWndProc = (WNDPROC)CallService("Button/GetWindowProc",0,0);
	WNDCLASSEX wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpszClassName = TTB_BUTTON_CLASS;
	wc.lpfnWndProc = TTBButtonWndProc;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.cbWndExtra = sizeof(void*);
	wc.hbrBackground = 0;
	wc.style = CS_GLOBALCLASS;
	RegisterClassEx(&wc);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

int UnloadToolbarModule()
{
	DestroyHookableEvent(hTTBModuleLoaded);
	DestroyHookableEvent(hTTBInitButtons);

	DeleteObject(hBmpSeparator);
	DeleteCriticalSection(&csButtonsHook);

	for (int i=0; i < Buttons.getCount(); i++)
		delete Buttons[i];
	Buttons.destroy();

	mir_free(g_ctrl);
	return 0;
}
