#pragma once
#include <wchar.h>
#include <vector>
#include "Protocol.h"
#include "Session.h"
#define dfUSER_MAX_CNT 32
using namespace std;

class User
{
public:
	User(int ID, Session* pSession, wchar_t* name) 
		: _ID(ID), _pSession(pSession) 
	{
		wcscpy_s(_name, dfNICK_MAX_LEN, name);
	}
	~User() {}

public:
	void SetUserAlive() { _alive = true; }
	void SetUserDead() { _alive = false; }
	bool GetUserAlive() { return _alive; }

public:
	bool _alive = true;
	int _ID;

public:
	int _roomID = -1;
	Session* _pSession;
	wchar_t _name[dfNICK_MAX_LEN] = { '/0', };
};

extern vector<User*> _allUsers;
