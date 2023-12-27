#pragma once
#include "Utils.h"
// TO-DO: 상속 받아서 덧붙일 수 있는 형태로 수정

class CRecvLanPacket;
class CLanJob
{
public:
	CLanJob() : _type(JOB_TYPE::NONE), _sysType(SYS_TYPE::NONE), _sessionID(-1), _packet(nullptr) {}

	void Setting(JOB_TYPE type, SYS_TYPE sysType, unsigned __int64 sessionID, CRecvLanPacket* packet)
	{
		_type = type;
		_sysType = sysType;
		_sessionID = sessionID;
		_packet = packet;
	}

public:
	JOB_TYPE _type;
	SYS_TYPE _sysType;
	unsigned __int64 _sessionID;
	CRecvLanPacket* _packet;
};