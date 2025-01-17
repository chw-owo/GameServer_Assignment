#pragma once

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <windows.h>

#define PROFILE

#ifdef PROFILE
#define PRO_BEGIN(TagName)		ProfileBegin(TagName)
#define PRO_END(TagName)		ProfileEnd(TagName)
#define PRO_RESET()				ProfileReset()
#define PRO_FILE_OUT(FileName)	ProfileDataOutText(FileName)
#define PRO_PRINT_CONSOLE()		PrintDataOnConsole()
#else
#define PRO_BEGIN(TagName)
#define PRO_END(TagName)
#define PRO_RESET()	
#define PRO_FILE_OUT(TagName)	
#define PRO_PRO_PRINT_CONSOLE()			
#endif

struct _PROFILE_RESULT;
void ProfileBegin(const WCHAR* szName);
void ProfileEnd(const WCHAR* szName);
void ProfileDataOutText(const WCHAR* szFileName);
void ProfileReset(void);
void PrintDataOnConsole(void);
