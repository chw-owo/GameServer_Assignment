#pragma once
#include "Config.h"
#ifdef NETSERVER

#include "CLockFreeStack.h"
#include "CNetSession.h"
#include "CNetMsg.h"
#include "CMonitorManager.h"

#include <ws2tcpip.h>
#include <synchapi.h>
#include <process.h>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Synchronization.lib")
using namespace std;

class CNetGroup;
class CNetServer
{
	friend CNetGroup;

protected:
	CNetServer();
	~CNetServer() {};

protected:
	bool NetworkInitialize(const wchar_t* IP, short port, long sendTime, int numOfThreads, int numOfRunnings, bool nagle, bool monitorServer);
	bool NetworkTerminate();

protected:
	bool Disconnect(unsigned __int64 sessionID);
	bool SendPacket(unsigned __int64 sessionID, CNetSendPacket* packet, bool disconnect = false);
	
protected:
	bool MoveGroup(unsigned __int64 sessionID, CNetGroup* pGroup);
	bool RegisterGroup(CNetGroup* pGroup);
	bool RemoveGroup(CNetGroup* pGroup);

protected:
	virtual void OnInitialize() = 0;
	virtual void OnTerminate() = 0;
	virtual void OnThreadTerminate(wchar_t* threadName) = 0;

protected:
	virtual bool OnConnectRequest(WCHAR* addr) = 0;
	virtual void OnAcceptClient(unsigned __int64 sessionID, WCHAR* addr) = 0;

protected:
	virtual void OnReleaseClient(unsigned __int64 sessionID) = 0;
	virtual void OnRecv(unsigned __int64 sessionID, CNetMsg* packet) = 0;
	virtual void OnSend(unsigned __int64 sessionID) = 0;
	virtual void OnError(int errorCode, wchar_t* errorMsg) = 0;
	virtual void OnDebug(int debugCode, wchar_t* debugMsg) = 0;

	// Called in Network Library
private:
	static unsigned int WINAPI AcceptThread(void* arg);
	static unsigned int WINAPI NetworkThread(void* arg);
	static unsigned int WINAPI SendThread(void* arg);

private:
	inline void HandleRecvCP(CNetSession* pSession, int recvBytes);
	inline void HandleSendCP(CNetSession* pSession, int sendBytes);
	inline void HandleRelease(unsigned __int64 sessionID);
	inline bool RecvPost(CNetSession* pSession);
	inline bool SendPost(CNetSession* pSession);
	inline bool SendCheck(CNetSession* pSession);

private:
	inline CNetSession* AcquireSessionUsage(unsigned __int64 sessionID);
	inline void ReleaseSessionUsage(CNetSession* pSession);
	inline void IncrementUseCount(CNetSession* pSession);
	inline void DecrementUseCount(CNetSession* pSession);

private:
	inline void SleepForFixedSend();

private:
	wchar_t _IP[10];
	short _port;
	bool _nagle;
	int _numOfThreads;
	int _numOfRunnings;
	long _sendTime;

private:
	DWORD _oldTick;

private:
	SOCKET _listenSock;
	volatile long _networkAlive = 0;

private:
	HANDLE _acceptThread;
	HANDLE _releaseThread;
	HANDLE _sendThread;
	HANDLE* _networkThreads;
	HANDLE _hNetworkCP;

public:
#define __ID_BIT__ 47
	CNetSession* _sessions[dfSESSION_MAX] = { nullptr, };
	CLockFreeStack<long> _emptyIdx;
	volatile long _sessionCnt = 0;
	volatile __int64 _sessionID = 0;
	unsigned __int64 _indexMask = 0;
	unsigned __int64 _idMask = 0;
	ValidFlag _releaseFlag;

private:
	unordered_map<CNetGroup*, HANDLE> _groupThreads;

public:
	CMonitorManager* _mm;

public: // Monitor

	inline void UpdateMonitorData()
	{
		_acceptTPS = InterlockedExchange(&_acceptCnt, 0);
		_disconnectTPS = InterlockedExchange(&_disconnectCnt, 0);
		_recvTPS = InterlockedExchange(&_recvCnt, 0);
		_sendTPS = InterlockedExchange(&_sendCnt, 0);

		_acceptTotal += _acceptTPS;
		_disconnectTotal += _disconnectTPS;
		_recvTotal += _recvTPS;
		_sendTotal += _sendTPS;
	}

	inline long GetSessionCount() { return _sessionCnt; }

	inline unsigned long long GetAcceptTotal() { return _acceptTotal; }
	inline unsigned long long GetDisconnectTotal() { return _disconnectTotal; }
	inline unsigned long long GetRecvTotal() { return _recvTotal; }
	inline unsigned long long GetSendTotal() { return _sendTotal; }

	inline long GetAcceptTPS() { return _acceptTPS; }
	inline long GetDisconnectTPS() { return _disconnectTPS; }
	inline long GetRecvTPS() { return _recvTPS; }
	inline long GetSendTPS() { return _sendTPS; }

private:
	unsigned long long _acceptTotal = 0;
	unsigned long long _disconnectTotal = 0;
	unsigned long long _recvTotal = 0;
	unsigned long long _sendTotal = 0;

	long _acceptTPS = 0;
	long _disconnectTPS = 0;
	long _recvTPS = 0;
	long _sendTPS = 0;

	volatile long _acceptCnt = 0;
	volatile long _disconnectCnt = 0;
	volatile long _recvCnt = 0;
	volatile long _sendCnt = 0;
};
#endif