#pragma once
#include "CommonProtocol.h"
#include "ErrorCode.h"

#include "CSystemLog.h"
#include "CNetServer.h"
#include "CTlsPool.h"
#include "CUser.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
using namespace std;

class CMonitorServer;
class CLoginServer : public CNetServer
{
	friend CMonitorServer;

public:
	CLoginServer() {}
	~CLoginServer() { Terminate(); }

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
	bool OnConnectRequest(WCHAR addr[dfADDRESS_LEN]);
	void OnAcceptClient(unsigned __int64 sessionID, WCHAR addr[dfADDRESS_LEN]);
	void OnReleaseClient(unsigned __int64 sessionID);
	void OnRecv(unsigned __int64 sessionID, CPacket* packet);
	void OnSend(unsigned __int64 sessionID, int sendSize);

private:
	static unsigned int WINAPI TimeoutThread(void* arg);

private:
	void ReqSendUnicast(CPacket* packet, unsigned __int64 sessionID);

private:
	void GetDataFromMysql();
	void SetDataToRedis();

private:
	BYTE CheckAccountNoValid(__int64 accountNo);
	void GetDataforUser(__int64 accountNo, WCHAR*& ID, WCHAR*& nickname);
	void GetIPforClient(CUser* user, WCHAR* gameIP, WCHAR* chatIP);
	
private:
	inline void HandleCSPacket_REQ_LOGIN(CPacket* CSpacket, CUser* user);
	inline void GetCSPacket_REQ_LOGIN(CPacket* packet, __int64& accountNo, char*& sessionKey);
	inline void SetSCPacket_RES_LOGIN(
		CPacket* packet, __int64 accountNo, BYTE status, WCHAR*& ID, WCHAR*& nickname,
		WCHAR*& gameIP, unsigned short gamePort, WCHAR*& chatIP, unsigned short chatPort);

private:
	bool _serverAlive = true;
	HANDLE _timeoutThread;

private:
	SRWLOCK _setLock;
	SRWLOCK _mapLock;
	unordered_map<unsigned __int64, CUser*> _usersMap;
	unordered_set<__int64> _accountNos;
	CTlsPool<CUser>* _pUserPool;

private:
	struct cmp
	{
		bool operator()(const WCHAR* a, const WCHAR* b) const
		{
			return wcscmp(a, b);
		}
	};

	map<WCHAR*, WCHAR*, cmp> _LanIPs;
	WCHAR* _WanIPs;

private:
	volatile long _handlePacketTPS = 0;
};

