#pragma once
#include "Config.h"
#ifdef LANSERVER

#include "CLockFreeStack.h"
#include "CLanSession.h"
#include "CLanMsg.h"
#include "CMonitorManager.h"
#include "CLanJob.h"
#include <ws2tcpip.h>
#include <process.h>
#pragma comment(lib, "ws2_32.lib")

class CLanServer
{
protected:
	CLanServer();
	~CLanServer() {};

protected:
	bool NetworkInitialize(const wchar_t* IP, short port, long sendTime,
		int numOfThreads, int numOfRunnings, bool nagle, bool monitorServer = false);
	bool NetworkTerminate();

protected:
	bool Disconnect(unsigned __int64 sessionID);
	bool SendPacket(unsigned __int64 sessionID, CLanSendPacket* packet);
	void EnqueueJob(unsigned __int64 sessionID, CLanJob* job);
	CLanJob* DequeueJob(unsigned __int64 sessionID);

protected:
	virtual void OnInitialize() = 0;
	virtual void OnTerminate() = 0;
	virtual void OnThreadTerminate(wchar_t* threadName) = 0;

protected:
	virtual bool OnConnectRequest(WCHAR* addr) = 0;
	virtual void OnAcceptClient(unsigned __int64 sessionID, WCHAR* addr) = 0;
	virtual void OnReleaseClient(unsigned __int64 sessionID) = 0;
	virtual void OnRecv(unsigned __int64 sessionID, CLanMsg* packet) = 0;
	virtual void OnSend(unsigned __int64 sessionID, int sendSize) = 0;
	virtual void OnError(int errorCode, wchar_t* errorMsg) = 0;
	virtual void OnDebug(int debugCode, wchar_t* debugMsg) = 0;

	// Called in Network Library
private:
	static unsigned int WINAPI AcceptThread(void* arg);
	static unsigned int WINAPI NetworkThread(void* arg);
	static unsigned int WINAPI SendThread(void* arg);
	static unsigned int WINAPI ReleaseThread(void* arg);

private:
	inline bool HandleRecvCP(CLanSession* pSession, int recvBytes);
	inline bool HandleSendCP(CLanSession* pSession, int sendBytes);
	inline bool RecvPost(CLanSession* pSession);
	inline bool SendPost(CLanSession* pSession);
	inline bool SendCheck(CLanSession* pSession);

private:
	inline CLanSession* AcquireSessionUsage(unsigned __int64 sessionID);
	inline void ReleaseSessionUsage(CLanSession* pSession);
	inline void IncrementUseCount(CLanSession* pSession);
	inline void DecrementUseCount(CLanSession* pSession);

private:
	void SleepForFixedSend();

public: // For Profilings
	long _Idx = -1;
	long _NetUpdate[2] = { 0, 0 };
	DWORD _netStart = 0;
	DWORD _netSum = 0;

private:
	wchar_t _IP[10];
	short _port;
	long _sendTime;
	bool _nagle;
	int _numOfThreads;
	int _numOfRunnings;

private:
	SOCKET _listenSock;
	volatile long _networkAlive = 0;

private:
	HANDLE _acceptThread;
	HANDLE _releaseThread;
	HANDLE _sendThread;
	HANDLE* _networkThreads;
	HANDLE _hNetworkCP;

private:
	long _releaseSignal = 0;
	CLockFreeQueue<unsigned __int64>* _pReleaseQ;
	ValidFlag _releaseFlag;

public: // TO-DO: private
#define __ID_BIT__ 47
	CLanSession* _sessions[dfSESSION_MAX] = { nullptr, };
	CLockFreeStack<long> _emptyIdx;
	volatile __int64 _sessionID = 0;
	unsigned __int64 _indexMask = 0;
	unsigned __int64 _idMask = 0;

public:
	CLockFreeQueue<CLanJob*>* _pJobQueues[dfJOB_QUEUE_CNT] = { nullptr, };
	CTlsPool<CLanJob>* _pJobPool;

public:
	DWORD _oldTick;

public:
	inline void UpdateMonitorData()
	{
		long recvCnt = InterlockedExchange(&_recvCnt, 0);
		long sendCnt = InterlockedExchange(&_sendCnt, 0);
		long acceptCnt = InterlockedExchange(&_acceptCnt, 0);
		long disconnectCnt = InterlockedExchange(&_disconnectCnt, 0);

		_acceptTotal += acceptCnt;
		_disconnectTotal += disconnectCnt;
		_acceptTPS = acceptCnt;
		_disconnectTPS = disconnectCnt;
		_recvTPS = recvCnt;
		_sendTPS = sendCnt;
	}

	inline long GetSessionCount() { return _sessionCnt; }
	inline long GetAcceptTotal() { return _acceptTotal; }
	inline long GetDisconnectTotal() { return _disconnectTotal; }

	inline long GetRecvTPS() { return _recvTPS; }
	inline long GetSendTPS() { return _sendTPS; }
	inline long GetAcceptTPS() { return _acceptTPS; }
	inline long GetDisconnectTPS() { return _disconnectTPS; }

public:
	CMonitorManager* _mm = nullptr;

private:
	
	long _acceptTotal = 0;
	long _disconnectTotal = 0;

	long _acceptTPS = 0;
	long _disconnectTPS = 0;
	long _recvTPS = 0;
	long _sendTPS = 0;

	volatile long _sessionCnt = 0;
	volatile long _acceptCnt;
	volatile long _disconnectCnt;
	volatile long _recvCnt;
	volatile long _sendCnt;
};
#endif