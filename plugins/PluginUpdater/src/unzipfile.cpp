/*
Copyright (C) 2012 George Hazan

This is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this file; see the file license.txt. If
not, write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.
*/

#include "stdafx.h"

#define DATA_BUF_SIZE (4 * 1024 * 1024)

static void PrepareFileName(wchar_t *dest, size_t destSize, const wchar_t *ptszPath, const wchar_t *ptszFileName)
{
	mir_snwprintf(dest, destSize, L"%s\\%s", ptszPath, ptszFileName);

	for (wchar_t *p = dest; *p; ++p)
		if (*p == '/')
			*p = '\\'; 
}

int extractCurrentFile(unzFile uf, wchar_t *ptszDestPath, wchar_t *ptszBackPath, bool ch)
{
	unz_file_info64 file_info;
	char filename[MAX_PATH];
	mir_ptr<char> buf((char *)mir_alloc(DATA_BUF_SIZE+1));

	int err = unzGetCurrentFileInfo64(uf, &file_info, filename, sizeof(filename), buf, DATA_BUF_SIZE, nullptr, 0);
	if (err != UNZ_OK)
		return err;

	for (char *p = strchr(filename, '/'); p; p = strchr(p+1, '/'))
		*p = '\\';
		
	// This is because there may be more then one file in a single zip
	// So we need to check each file
	if (ch && 1 != db_get_b(0, DB_MODULE_FILES, StrToLower(ptrA(mir_strdup(filename))), 1))
		return UNZ_OK;

	wchar_t tszDestFile[MAX_PATH], tszBackFile[MAX_PATH];
	ptrW ptszNewName(mir_utf8decodeW(filename));
	if (ptszNewName == nullptr)
		ptszNewName = mir_a2u(filename);

	if (!(file_info.external_fa & FILE_ATTRIBUTE_DIRECTORY)) {
		err = unzOpenCurrentFile(uf);
		if (err != UNZ_OK)
			return err;

		if (ptszBackPath != nullptr) {
			PrepareFileName(tszDestFile, _countof(tszDestFile), ptszDestPath, ptszNewName);
			PrepareFileName(tszBackFile, _countof(tszBackFile), ptszBackPath, ptszNewName);
			if (err = BackupFile(tszDestFile, tszBackFile))
				return err;
		}

		PrepareFileName(tszDestFile, _countof(tszDestFile), ptszDestPath, ptszNewName);
		SafeCreateFilePath(tszDestFile);

		wchar_t *ptszFile2unzip;
		if (hPipe == nullptr) // direct mode
			ptszFile2unzip = tszDestFile;
		else {
			wchar_t tszTempPath[MAX_PATH];
			GetTempPathW(_countof(tszTempPath), tszTempPath);
			GetTempFileNameW(tszTempPath, L"PUtemp", GetCurrentProcessId(), tszBackFile);
			ptszFile2unzip = tszBackFile;
		}

		HANDLE hFile = CreateFile(ptszFile2unzip, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, file_info.external_fa, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
			return GetLastError();

		while (true) {
			err = unzReadCurrentFile(uf, buf, DATA_BUF_SIZE);
			if (err <= 0)
				break;

			DWORD bytes;
			if (!WriteFile(hFile, buf, err, &bytes, FALSE)) {
				err = GetLastError();
				break;
			}
		}

		FILETIME ftLocal, ftCreate, ftLastAcc, ftLastWrite;
		GetFileTime(hFile, &ftCreate, &ftLastAcc, &ftLastWrite);
		DosDateTimeToFileTime(HIWORD(file_info.dosDate), LOWORD(file_info.dosDate), &ftLocal);
		LocalFileTimeToFileTime(&ftLocal, &ftLastWrite);
		SetFileTime(hFile, &ftCreate, &ftLastAcc, &ftLastWrite);

		CloseHandle(hFile);
		unzCloseCurrentFile(uf); /* don't lose the error */

		if (hPipe)
			SafeMoveFile(ptszFile2unzip, tszDestFile);
	}
	return err;
}

int unzip(const wchar_t *ptszZipFile, wchar_t *ptszDestPath, wchar_t *ptszBackPath,bool ch)
{
	int iErrorCode = 0;

	zlib_filefunc64_def ffunc;
	fill_fopen64_filefunc(&ffunc);
	
	unzFile uf = unzOpen2_64(ptszZipFile, &ffunc);
	if (uf) {
		do {
			if (int err = extractCurrentFile(uf, ptszDestPath, ptszBackPath,ch))
				iErrorCode = err;
		}
			while (unzGoToNextFile(uf) == UNZ_OK);
		unzClose(uf);
	}

	return iErrorCode;
}
