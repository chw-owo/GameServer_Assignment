#pragma once
#include "CLanPacket.h"
#include "CLockFreeQueue.h"
#include "Utils.h"

class CClientSession
{
public:
	inline CClientSession(SOCKET socket)
	{
		_disconnect = false;
		_sock = socket;

		_sendFlag = 0;
		_sendCount = 0;
		_useCount = 0;

		ZeroMemory(&_recvComplOvl._ovl, sizeof(_recvComplOvl._ovl));
		ZeroMemory(&_sendComplOvl._ovl, sizeof(_sendComplOvl._ovl));
		ZeroMemory(&_releaseOvl._ovl, sizeof(_releaseOvl._ovl));

		_recvComplOvl._type = NET_TYPE::RECV_COMPLETE;
		_sendComplOvl._type = NET_TYPE::SEND_COMPLETE;
		_releaseOvl._type = NET_TYPE::RELEASE;

		_recvBuf = CLanPacket::Alloc();
		_recvBuf->AddUsageCount(1);
	}

	inline ~CClientSession()
	{
		_ID = MAXULONGLONG;
		_disconnect = true;
		_sock = INVALID_SOCKET;

		while (_sendBuf.GetUseSize() > 0)
		{
			CLanPacket* packet = _sendBuf.Dequeue();
			CLanPacket::Free(packet);
		}
		while (_tempBuf.GetUseSize() > 0)
		{
			CLanPacket* packet = _tempBuf.Dequeue();
			CLanPacket::Free(packet);
		}

		CLanPacket::Free(_recvBuf);
	}

private:
	unsigned __int64 _ID = MAXULONGLONG;

public:
	bool _disconnect = true;
	volatile long _sendFlag;
	volatile long _sendCount;
	volatile short _useCount;

	SOCKET _sock;
	CLanPacket* _recvBuf;
	CLockFreeQueue<CLanPacket*> _sendBuf;
	CLockFreeQueue<CLanPacket*> _tempBuf;
	WSABUF _wsaRecvbuf[dfWSARECVBUF_CNT];
	WSABUF _wsaSendbuf[dfWSASENDBUF_CNT];

	NetworkOverlapped _recvComplOvl;
	NetworkOverlapped _sendComplOvl;
	NetworkOverlapped _releaseOvl;
};