// Copyright © 2008 sss, chaos.persei
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


#include "stdafx.h"

wchar_t* __stdcall UniGetContactSettingUtf(MCONTACT hContact, const char *szModule,const char* szSetting, wchar_t* szDef)
{
	wchar_t *szRes = db_get_wsa(hContact, szModule, szSetting);
	return szRes ? szRes  : mir_wstrdup(szDef);
}
