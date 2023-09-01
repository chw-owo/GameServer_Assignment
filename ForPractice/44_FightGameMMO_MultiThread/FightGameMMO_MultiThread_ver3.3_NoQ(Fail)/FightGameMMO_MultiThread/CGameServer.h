#pragma once
#include "CLanServer.h"
#include "CObjectPool.h"
#include "CSector.h"
#include "CPlayer.h"
#include "CProcessCpu.h"
#include "CProcessorCpu.h"
#include "Protocol.h"

#include <unordered_map>
using namespace std;

class CGameServer : public CLanServer
{
private:
	CGameServer();
	~CGameServer();
	static CGameServer _CGameServer;

public:
	static CGameServer* GetInstance();

private:
	bool OnConnectRequest();
	void OnAcceptClient(__int64 sessionID);
	void OnReleaseClient(__int64 sessionID);
	void OnRecv(__int64 sessionID, CPacket* packet);
	void OnSend(__int64 sessionID, int sendSize);
	void OnError(int errorCode, wchar_t* errorMsg);

private:
	static unsigned int WINAPI UpdateThread(void* arg);
	static unsigned int WINAPI MonitorThread(void* arg);

private:
	void ReqSendUnicast(CPacket* packet, __int64 sessionID);
	void ReqSendOneSector(CPacket* packet, CSector* sector, CPlayer* pExpPlayer = nullptr);
	void ReqSendAroundSector(CPacket* packet, CSector* centerSector, CPlayer* pExpPlayer = nullptr);

private:
	bool SkipForFixedFrame();
	void LogicUpdate();

private:
	void CreatePlayer(__int64 sessionID);
	void SendPlayerState(CPlayer* pPlayer);
	void SetPlayerDead(CPlayer* pPlayer);
	void DeleteDeadPlayer(CPlayer* pPlayer);

	bool CheckMovable(short x, short y);
	void UpdatePlayerMove(CPlayer* pPlayer);
	void SetSector(CPlayer* pPlayer);
	void UpdateSector(CPlayer* pPlayer, short direction);
	void SetSectorsAroundInfo();

private:
	// About Packet ====================================================
private:
	// Handle System Job
	inline void Handle_ACCEPT(__int64 sessionID);
	inline void Handle_RELEASE(__int64 sessionID);

	// Handle CS Packet
	inline void HandleCSPacket_MOVE_START(CPacket* recvPacket, CPlayer* pPlayer);
	inline void HandleCSPacket_MOVE_STOP(CPacket* recvPacket, CPlayer* pPlayer);
	inline void HandleCSPacket_ATTACK1(CPacket* recvPacket, CPlayer* pPlayer);
	inline void HandleCSPacket_ATTACK2(CPacket* recvPacket, CPlayer* pPlayer);
	inline void HandleCSPacket_ATTACK3(CPacket* recvPacket, CPlayer* pPlayer);
	inline void HandleCSPacket_ECHO(CPacket* recvPacket, CPlayer* pPlayer);

	// Handle CS Packet
	inline void Handle_MOVE_START_STATE(CPlayer* pPlayer);
	inline void Handle_MOVE_STOP_STATE(CPlayer* pPlayer);
	inline void Handle_ATTACK1_STATE(CPlayer* pPlayer);
	inline void Handle_ATTACK2_STATE(CPlayer* pPlayer);
	inline void Handle_ATTACK3_STATE(CPlayer* pPlayer);
	inline void Handle_DAMAGED_STATE(CPlayer* pPlayer);
	inline void Handle_SYNC_STATE(CPlayer* pPlayer);
	inline void Handle_ECHO_STATE(CPlayer* pPlayer);

	// Get Data from CS Packet
	inline void GetCSPacket_MOVE_START(CPacket* pPacket, unsigned char& moveDirection, short& x, short& y);
	inline void GetCSPacket_MOVE_STOP(CPacket* pPacket, unsigned char& direction, short& x, short& y);
	inline void GetCSPacket_ATTACK1(CPacket* pPacket, unsigned char& direction, short& x, short& y);
	inline void GetCSPacket_ATTACK2(CPacket* pPacket, unsigned char& direction, short& x, short& y);
	inline void GetCSPacket_ATTACK3(CPacket* pPacket, unsigned char& direction, short& x, short& y);
	inline void GetCSPacket_ECHO(CPacket* pPacket, int& time);

	// Set Game Data from Packet Data
	inline void SetPlayerMoveStart(CPlayer* pPlayer, unsigned char& moveDirection, short& x, short& y);
	inline void SetPlayerMoveStop(CPlayer* pPlayer, unsigned char& direction, short& x, short& y);
	inline void SetPlayerAttack1(CPlayer* pPlayer, CPlayer*& pDamagedPlayer, unsigned char& direction, short& x, short& y);
	inline void SetPlayerAttack2(CPlayer* pPlayer, CPlayer*& pDamagedPlayer, unsigned char& direction, short& x, short& y);
	inline void SetPlayerAttack3(CPlayer* pPlayer, CPlayer*& pDamagedPlayer, unsigned char& direction, short& x, short& y);
	inline void SetPlayerDamaged(CPlayer* pPlayer, int attackerID);
	inline void SetPlayerEcho(CPlayer* pPlayer, int time);

	// Set Data on SC Packet
	inline int SetSCPacket_CREATE_MY_CHAR(CPacket* pPacket, int ID, unsigned char direction, short x, short y, unsigned char hp);
	inline int SetSCPacket_CREATE_OTHER_CHAR(CPacket* pPacket, int ID, unsigned char direction, short x, short y, unsigned char hp);
	inline int SetSCPacket_DELETE_CHAR(CPacket* pPacket, int ID);
	inline int SetSCPacket_MOVE_START(CPacket* pPacket, int ID, unsigned char moveDirection, short x, short y);
	inline int SetSCPacket_MOVE_STOP(CPacket* pPacket, int ID, unsigned char direction, short x, short y);
	inline int SetSCPacket_ATTACK1(CPacket* pPacket, int ID, unsigned char direction, short x, short y);
	inline int SetSCPacket_ATTACK2(CPacket* pPacket, int ID, unsigned char direction, short x, short y);
	inline int SetSCPacket_ATTACK3(CPacket* pPacket, int ID, unsigned char direction, short x, short y);
	inline int SetSCPacket_DAMAGE(CPacket* pPacket, int attackID, int damageID, unsigned char damageHP);
	inline int SetSCPacket_SYNC(CPacket* pPacket, int ID, short x, short y);
	inline int SetSCPacket_ECHO(CPacket* pPacket, int time);

private:
	HANDLE _updateThread;
	HANDLE _monitorThread;

private:
	CObjectPool<CPlayer>* _pPlayerPool;
	CSector _sectors[dfSECTOR_CNT_Y][dfSECTOR_CNT_X];
	int _sectorCnt[dfMOVE_DIR_MAX] =
					{ dfVERT_SECTOR_NUM, dfDIAG_SECTOR_NUM,
					  dfVERT_SECTOR_NUM, dfDIAG_SECTOR_NUM,
					  dfVERT_SECTOR_NUM, dfDIAG_SECTOR_NUM,
					  dfVERT_SECTOR_NUM, dfDIAG_SECTOR_NUM };

	int _playerID = 0;
	int _timeGap = 1000 / dfFPS;

private:
	unordered_map<__int64, CPlayer*> _playerSessionID;
	CPlayer* _players[dfPLAYER_MAX] = { nullptr, };
	int _usableIDs[dfPLAYER_MAX];
	int _usableIDCnt = 0;

private:
	// For Monitor
	int _logicFPS = 0;
	int _syncCnt = 0;
	int _deadCnt = 0;
	int _timeoutCnt = 0;
	int _connectEndCnt = 0;
	int _checkPointsIdx = 0;
	int _checkPoints[dfMONITOR_CHECKPOINT] 
		= { 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 5500, 6000 };

	CProcessCpu _processCPUTime;
	CProcessorCpu _processorCPUTime;
};
