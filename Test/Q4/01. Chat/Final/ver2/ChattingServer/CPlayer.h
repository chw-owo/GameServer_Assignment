#pragma once
#include "CLockFreeQueue.h"
#include "CObjectPool.h"
#include "CommonProtocol.h"

struct ID
{
	wchar_t _id[dfID_LEN];
};

struct Nickname
{
	wchar_t _nickname[dfNICKNAME_LEN];
};

struct SessionKey
{
	char _key[dfSESSIONKEY_LEN];
};

extern CObjectPool<ID> g_IDPool;
extern CObjectPool<Nickname> g_NicknamePool;
extern CObjectPool<SessionKey> g_SessionKey;

class CPlayer
{
public:
	inline void Initailize(unsigned __int64 sessionID, __int64 playerID)
	{
		_id = g_IDPool.Alloc();
		_nickname = g_NicknamePool.Alloc();
		_key = g_SessionKey.Alloc();

		_sessionID = sessionID;
		_playerID = playerID;

		_accountNo = -1;
		_sectorX = -1;
		_sectorY = -1;
		_lastRecvTime = timeGetTime();
	}

	inline void Terminate()
	{
		g_IDPool.Free(_id);
		g_NicknamePool.Free(_nickname);
		g_SessionKey.Free(_key);

		_sessionID = MAXULONGLONG;
		_playerID = -1;

		_accountNo = -1;		
		_sectorX = -1;
		_sectorY = -1;
		_lastRecvTime = 0;
	}

public:
	unsigned __int64 _sessionID = MAXULONGLONG;
	__int64 _playerID = -1;
	__int64 _accountNo = -1;

	ID* _id;
	Nickname* _nickname;
	SessionKey* _key;

	WORD _sectorX = -1;
	WORD _sectorY = -1;
	DWORD _lastRecvTime = 0;
};