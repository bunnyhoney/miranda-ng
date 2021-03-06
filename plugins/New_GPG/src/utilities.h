// Copyright © 2010-18 sss
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#ifndef UTILITIES_H
#define UTILITIES_H

wchar_t* __stdcall UniGetContactSettingUtf(MCONTACT hContact, const char *szModule,const char* szSetting, wchar_t* szDef);
char* __stdcall UniGetContactSettingUtf(MCONTACT hContact, const char *szModule,const char* szSetting, char* szDef);
void GetFilePath(wchar_t *WindowTittle, char *szSetting, wchar_t *szExt, wchar_t *szExtDesc);
wchar_t *GetFilePath(wchar_t *WindowTittle, wchar_t *szExt, wchar_t *szExtDesc, bool save_file = false);
void GetFolderPath(wchar_t *WindowTittle);

void setSrmmIcon(MCONTACT);
void setClistIcon(MCONTACT);

void send_encrypted_msgs_thread(void*);

int ComboBoxAddStringUtf(HWND hCombo, const wchar_t *szString, DWORD data);
bool isContactSecured(MCONTACT hContact);
bool isContactHaveKey(MCONTACT hContact);
bool isTabsrmmUsed();
bool isGPGKeyExist();
bool isGPGValid();
void ExportGpGKeysFunc(int type);
const bool StriStr(const char *str, const char *substr);
string toUTF8(wstring str);
wstring toUTF16(string str);
string get_random(int length);
string time_str();

struct db_event : public DBEVENTINFO
{
public:
	db_event(char* msg)
	{
		eventType = EVENTTYPE_MESSAGE;
		flags = 0;
		timestamp = time(0);
		szModule = 0;
		cbBlob = DWORD(mir_strlen(msg)+1);
		pBlob = (PBYTE)msg;
	}
	db_event(char* msg, DWORD time)
	{
		cbBlob = DWORD(mir_strlen(msg)+1);
		pBlob = (PBYTE)msg;
		eventType = EVENTTYPE_MESSAGE;
		flags = 0;
		timestamp = time;
		szModule = 0;
	}
	db_event(char* msg, DWORD time, int type)
	{
		cbBlob = DWORD(mir_strlen(msg)+1);
		pBlob = (PBYTE)msg;
		if(type)
			eventType = type;
		else
			eventType = EVENTTYPE_MESSAGE;
		flags = 0;
		timestamp = time;
		szModule = 0;
	}
	db_event(char* msg, int type)
	{
		cbBlob = DWORD(mir_strlen(msg)+1);
		pBlob = (PBYTE)msg;
		flags = 0;
		if(type)
			eventType = type;
		else
			eventType = EVENTTYPE_MESSAGE;
		timestamp = time(0);
		szModule = 0;
	}
	db_event(char* msg, DWORD time, int type, DWORD _flags)
	{
		cbBlob = DWORD(mir_strlen(msg)+1);
		pBlob = (PBYTE)msg;
		if(type)
			eventType = type;
		else
			eventType = EVENTTYPE_MESSAGE;
		flags = _flags;
		timestamp = time;
		szModule = 0;
	}
};

void HistoryLog(MCONTACT, db_event);
void fix_line_term(std::string &s);
void fix_line_term(std::wstring &s);
void strip_line_term(std::wstring &s);
void strip_line_term(std::string &s);
void strip_tags(std::wstring &s);
void clean_temp_dir();
bool gpg_validate_paths(wchar_t *gpg_bin_path, wchar_t *gpg_home_path);
void gpg_save_paths(wchar_t *gpg_bin_path, wchar_t *gpg_home_path);
bool gpg_use_new_random_key(char *account_name = Translate("Default"), wchar_t *gpg_bin_path = nullptr, wchar_t *gpg_home_dir = nullptr);

#endif
