#pragma once
#include "CNetServer.h"
#include "CSector.h"
#include "CPlayer.h"

#include "CLockFreeStack.h"
#include "CLockFreePool.h"
#include "CJob.h"

#include "Protocol.h"
#include "ErrorCode.h"
#include <unordered_map>
using namespace std;

#include <synchapi.h>
#pragma comment(lib, "Synchronization.lib")

class CChattingServer : public CNetServer
{
public:
	CChattingServer() {};
	~CChattingServer() { Terminate(); };

public:
	bool Initialize();
	void Terminate();

private:
	void OnInitialize();
	void OnTerminate();
	void OnThreadTerminate(wchar_t* threadName);
	void OnError(int errorCode, wchar_t* errorMsg);
	void OnDebug(int debugCode, wchar_t* debugMsg);

private:
	bool OnConnectRequest();
	void OnAcceptClient(unsigned __int64 sessionID);
	void OnReleaseClient(unsigned __int64 sessionID);
	void OnRecv(unsigned __int64 sessionID, CPacket* packet);
	void OnSend(unsigned __int64 sessionID, int sendSize);

private:
	void HandleRecv(CPlayer* pPlayer, CPacket* packet);

private:
	static unsigned int WINAPI UpdateThread(void* arg);
	static unsigned int WINAPI MonitorThread(void* arg);
	static unsigned int WINAPI TimeoutThread(void* arg);

private:
	void ReqSendUnicast(CPacket* packet, unsigned __int64 sessionID);
	void ReqSendAroundSector(CPacket* packet, CSector* centerSector, CPlayer* pExpPlayer = nullptr);
	
private:
	inline void HandleCSPacket_REQ_LOGIN(CPacket* CSpacket, CPlayer* player);
	inline void HandleCSPacket_REQ_SECTOR_MOVE(CPacket* CSpacket, CPlayer* player);
	inline void HandleCSPacket_REQ_MESSAGE(CPacket* CSpacket, CPlayer* player);
	inline void HandleCSPacket_REQ_HEARTBEAT(CPlayer* player);

private:
	inline void GetCSPacket_REQ_LOGIN(CPacket* packet, __int64& accountNo, wchar_t*& ID, wchar_t*& nickname, char*& sessionKey);
	inline void GetCSPacket_REQ_SECTOR_MOVE(CPacket* packet, __int64& accountNo, WORD& sectorX, WORD& sectorY);
	inline void GetCSPacket_REQ_MESSAGE(CPacket* packet, __int64& accountNo, WORD& messageLen, wchar_t*& message);
	
private:
	inline void SetSCPacket_RES_LOGIN(CPacket* packet, BYTE status, __int64 accountNo);
	inline void SetSCPacket_RES_SECTOR_MOVE(CPacket* packet, __int64 accountNo, WORD sectorX, WORD sectorY);
	inline void SetSCPacket_RES_MESSAGE(CPacket* packet, __int64 accountNo, wchar_t* ID, wchar_t* nickname, WORD messageLen, wchar_t* message);

private:
	class ThreadArg
	{
	public:
		ThreadArg(CChattingServer* pServer, int threadIdx)
			:_pServer(pServer), _threadIdx(threadIdx) {};
	public:
		CChattingServer* _pServer;
		int _threadIdx;
	};

private:
	bool _serverAlive = true;
	int _updateThreadCnt = 0;
	int _playerPerThread = 0;
	HANDLE* _updateThread;
	HANDLE _monitorThread; 
	HANDLE _timeoutThread;
	
private:
	CSector _sectors[dfSECTOR_CNT_Y][dfSECTOR_CNT_X];

private:
	__int64 _playerIDGenerator = 0;
	unordered_map<unsigned __int64, CPlayer*> _playersMap;
	CPlayer** _players;
	CLockFreeStack<int> _usablePlayerIdx;
	SRWLOCK _playersLock;

private:
	long* _signal;
	CLockFreePool<CJob>* _pJobPool;

private: // For Monitor
	long _totalAccept = 0;
	long _totalDisconnect = 0;
	long _updateThreadWakeTPS = 0;
	long _handlePacketTPS = 0;
	long _jobQSize = 0;
};

