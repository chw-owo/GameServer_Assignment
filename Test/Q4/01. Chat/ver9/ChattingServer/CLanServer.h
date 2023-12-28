#pragma once
#include "Config.h"
#ifdef LANSERVER

#include "CLockFreeStack.h"
#include "CLockFreeQueue.h"
#include "CLanSession.h"
#include "CRecvLanPacket.h"
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
	bool NetworkInitialize(
		const wchar_t* IP, short port, int numOfThreads, int numOfRunnings, bool nagle, bool monitorServer = false);
	bool NetworkTerminate();

protected:
	bool Disconnect(unsigned __int64 sessionID);
	bool SendPacket(unsigned __int64 sessionID, CLanPacket* packet);
	void EnqueueJob(unsigned __int64 sessionID, CLanJob* job);
	CLanJob* DequeueJob(unsigned __int64 sessionID);

protected:
	virtual void OnInitialize() = 0;
	virtual void OnTerminate() = 0;
	virtual void OnThreadTerminate(wchar_t* threadName) = 0;

protected:
	virtual bool OnConnectRequest() = 0;
	virtual void OnAcceptClient(unsigned __int64 sessionID) = 0;
	virtual void OnReleaseClient(unsigned __int64 sessionID) = 0;
	virtual void OnRecv(unsigned __int64 sessionID, CRecvLanPacket* packet) = 0;
	virtual void OnSend(unsigned __int64 sessionID, int sendSize) = 0;
	virtual void OnError(int errorCode, wchar_t* errorMsg) = 0;
	virtual void OnDebug(int debugCode, wchar_t* debugMsg) = 0;

	// Called in Network Library
private:
	static unsigned int WINAPI AcceptThread(void* arg);
	static unsigned int WINAPI NetworkThread(void* arg);
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

public: // For Profilings
	long _Idx = -1;
	long _NetUpdate[2] = { 0, 0 };
	DWORD _netStart = 0;
	DWORD _netSum = 0;

private:
	wchar_t _IP[10];
	short _port;
	bool _nagle;
	int _numOfThreads;
	int _numOfRunnings;

private:
	SOCKET _listenSock;
	volatile long _networkAlive = 0;

private:
	HANDLE _acceptThread;
	HANDLE _releaseThread;
	HANDLE* _networkThreads;
	HANDLE _hNetworkCP;

private:
	long _releaseSignal = 0;
	CLockFreeQueue<unsigned __int64>* _pReleaseQ;

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
	ValidFlag _releaseFlag;

	// For Monitor
protected:
	inline void UpdateMonitorData()
	{
		long acceptCnt = 0;
		long disconnectCnt = 0;
		long recvCnt = 0;
		long sendCnt = 0;

		for (int i = 1; i < _tlsMax + 1; i++)
		{
			acceptCnt += InterlockedExchange(&_acceptCnts[i], 0);
			disconnectCnt += InterlockedExchange(&_disconnectCnts[i], 0);
			recvCnt += InterlockedExchange(&_recvCnts[i], 0);
			sendCnt += InterlockedExchange(&_sendCnts[i], 0);
		}

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
	long _sessionCnt = 0;
	long _acceptTotal = 0;
	long _disconnectTotal = 0;

	long _acceptTPS = 0;
	long _disconnectTPS = 0;
	long _recvTPS = 0;
	long _sendTPS = 0;

#define dfTHREAD_MAX 16
	volatile long _acceptCnts[dfTHREAD_MAX] = { 0, };
	volatile long _disconnectCnts[dfTHREAD_MAX] = { 0, };
	volatile long _recvCnts[dfTHREAD_MAX] = { 0, };
	volatile long _sendCnts[dfTHREAD_MAX] = { 0, };
	volatile long _sessionCnts[dfTHREAD_MAX] = { 0, };

	long _tlsMax = 0;
	long _tlsIdx;
};
#endif