#include "Server.h"
#include "Main.h"
#include <stdio.h>

Server::Server()
{
	int err;
	int bindRet;
	int listenRet;
	int ioctRet;

	srand(0);
	CreateDirectory(L"ProfileBasic", NULL);
	
	_pSessionPool = new CObjectPool<Session>(dfSESSION_MAX, true);
	_pPlayerPool = new CObjectPool<Player>(dfSESSION_MAX, true);
	_pSPacketPool = new CObjectPool<SerializePacket>(dfSPACKET_MAX, false);

	// Initialize Winsock
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		g_bShutdown = true;
		return;
	}

	// Set Listen Socket
	_listensock = socket(AF_INET, SOCK_STREAM, 0);
	if (_listensock == INVALID_SOCKET)
	{
		err = WSAGetLastError();
		::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL, 
			L"%s[%d]: listen sock is INVALIED, %d\n", 
			_T(__FUNCTION__), __LINE__, err);
		g_bShutdown = true;
		return;
	}

	// Bind Socket
	SOCKADDR_IN serveraddr;
	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	InetPton(AF_INET, dfNETWORK_IP, &serveraddr.sin_addr);
	serveraddr.sin_port = htons(dfNETWORK_PORT);

	bindRet = bind(_listensock, (SOCKADDR*)&serveraddr, sizeof(serveraddr));
	if (bindRet == SOCKET_ERROR)
	{
		err = WSAGetLastError();
		::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: bind Error, %d\n",
			_T(__FUNCTION__), __LINE__, err);

		g_bShutdown = true;
		return;
	}

	// Listen
	listenRet = listen(_listensock, SOMAXCONN);
	if (listenRet == SOCKET_ERROR)
	{
		err = WSAGetLastError();
		::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: listen Error, %d\n",
			_T(__FUNCTION__), __LINE__, err);

		g_bShutdown = true;
		return;
	}

	// Set Non-Blocking Mode
	u_long on = 1;
	ioctRet = ioctlsocket(_listensock, FIONBIO, &on);
	if (ioctRet == SOCKET_ERROR)
	{
		err = WSAGetLastError();
		::printf("Error! Function %s Line %d: %d\n", __func__, __LINE__, err);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: ioct Error, %d\n",
			_T(__FUNCTION__), __LINE__, err);
		g_bShutdown = true;
		return;
	}

	_time.tv_sec = 0;
	_time.tv_usec = 0;

	::printf("Network Setting Complete!\n");

	for (int y = 0; y < dfSECTOR_CNT_Y; y++)
	{
		for (int x = 0; x < dfSECTOR_CNT_X; x++)
		{
			_sectors[y][x].InitializeSector(x, y);
		}
	}

	SetSectorsAroundInfo();

	_oldTick = GetTickCount64();
	::printf("Content Setting Complete!\n");

}

Server::~Server()
{	
	for (int i = 0; i < dfSESSION_MAX; i++)
	{
		if (_SessionMap[i] != nullptr)
		{
			closesocket(_SessionMap[i]->_socket);
			_pSessionPool->Free(_SessionMap[i]);
		}

		if (_PlayerMap[i] != nullptr)
			_pPlayerPool->Free(_PlayerMap[i]);
	}

	closesocket(_listensock);
	WSACleanup();

	delete _pSessionPool;
	delete _pPlayerPool;
	delete _pSPacketPool;
}

/*
Server* Server::GetInstance()
{
	static Server _server;
	return &_server;
}
*/

void Server::NetworkUpdate()
{ 
	PRO_BEGIN(L"Network");

	int rStopIdx = 0;
	int wStopIdx = 0;

	for (int i = 0; i < dfSESSION_MAX; i++)
	{
		if (_SessionMap[i] == nullptr) continue;

		_rSessions[rStopIdx++] = _SessionMap[i];
		if (_SessionMap[i]->_sendBuf.GetUseSize() > 0)
			_wSessions[wStopIdx++] = _SessionMap[i];
	}

	FD_SET rset;
	FD_SET wset;
	FD_ZERO(&rset);
	FD_ZERO(&wset);
	int rStartIdx = 0;
	int wStartIdx = 0;

	while ((rStopIdx - rStartIdx) >= (FD_SETSIZE - 1) &&
		(wStopIdx - wStartIdx) >= FD_SETSIZE)
	{
		SelectProc(rset, wset, rStartIdx, (FD_SETSIZE - 1), wStartIdx, FD_SETSIZE);
		rStartIdx += (FD_SETSIZE - 1);
		wStartIdx += FD_SETSIZE;
		FD_ZERO(&rset);
		FD_ZERO(&wset);
	}

	while((rStopIdx - rStartIdx) >= (FD_SETSIZE - 1) && 
		(wStopIdx - wStartIdx) < FD_SETSIZE)
	{
		SelectProc(rset, wset, rStartIdx, (FD_SETSIZE - 1), 0, 0);
		rStartIdx += (FD_SETSIZE - 1);
		FD_ZERO(&rset);
		FD_ZERO(&wset);
	}

	while ((rStopIdx - rStartIdx) < (FD_SETSIZE - 1) &&
		(wStopIdx - wStartIdx) >= FD_SETSIZE)
	{
		SelectProc(rset, wset, 0, 0, wStartIdx, FD_SETSIZE);
		wStartIdx += FD_SETSIZE;
		FD_ZERO(&rset);
		FD_ZERO(&wset);
	}

	SelectProc(rset, wset, rStartIdx, rStopIdx - rStartIdx, wStartIdx, wStopIdx - wStartIdx);

	PRO_END(L"Network");

	PRO_BEGIN(L"Delayed Disconnect");
	DisconnectDeadSession();
	PRO_END(L"Delayed Disconnect");	
}

void Server::ContentUpdate()
{
	if (SkipForFixedFrame()) return;

	PRO_BEGIN(L"Content");
	
	for (int i = 0; i < dfSESSION_MAX; i++)
	{
		if (_PlayerMap[i] == nullptr) continue;

		if (GetTickCount64() - _PlayerMap[i]->_pSession->_lastRecvTime 
				> dfNETWORK_PACKET_RECV_TIMEOUT)
		{
			_timeoutCnt++;
			SetSessionDead(_PlayerMap[i]->_pSession);
			continue;
		}

		if (_PlayerMap[i]->_move)
		{
			UpdatePlayerMove(_PlayerMap[i]);
		}
	}

	PRO_END(L"Content");
}

// About Network ==================================================

void Server::SelectProc(FD_SET rset, FD_SET wset, int rStartIdx, int rCount, int wStartIdx, int wCount)
{
	FD_SET(_listensock, &rset);
	for (int i = 0; i < wCount; i++)
		FD_SET(_wSessions[wStartIdx + i]->_socket, &wset);
	for (int i = 0; i < rCount; i++)
		FD_SET(_rSessions[rStartIdx + i]->_socket, &rset);

	// Select Socket Set
	int selectRet = select(0, &rset, &wset, NULL, &_time);
	if (selectRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: select Error, %d\n",
			_T(__FUNCTION__), __LINE__, err);
		g_bShutdown = true;
		return;
	}

	// Handle Selected Socket
	else if (selectRet > 0)
	{
		if (FD_ISSET(_listensock, &rset))
			AcceptProc();

		for (int i = 0; i < wCount; i++)
			if (FD_ISSET(_wSessions[wStartIdx + i]->_socket, &wset))
				SendProc(_wSessions[wStartIdx + i]);

		for (int i = 0; i < rCount; i++)
			if (FD_ISSET(_rSessions[rStartIdx + i]->_socket, &rset))
				RecvProc(_rSessions[rStartIdx + i]);
	}
}

void Server::AcceptProc()
{
	if (_usableCnt == 0 && _sessionIDs == dfSESSION_MAX)
	{
		::printf("Can't Accept More!\n");
		return;
	}
	
	int ID;
	if (_usableCnt == 0)
		ID = _sessionIDs++;
	else
		ID = _usableSessionID[--_usableCnt];

	SerializePacket* pRecvPacket = _pSPacketPool->Alloc();
	SerializePacket* pSendPacket = _pSPacketPool->Alloc();
	Session* pSession = _pSessionPool->Alloc(ID, pRecvPacket, pSendPacket);

	if (pSession == nullptr)
	{
		::printf("Error! Func %s Line %d: fail new\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: new Error, %d\n",
			_T(__FUNCTION__), __LINE__);
		g_bShutdown = true;	
		return;
	}

	int addrlen = sizeof(pSession->_addr);
	pSession->_socket = accept(_listensock, (SOCKADDR*)&pSession->_addr, &addrlen);
	if (pSession->_socket == INVALID_SOCKET)
	{
		int err = WSAGetLastError();
		::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: accept Error, %d\n",
			_T(__FUNCTION__), __LINE__, err);
		g_bShutdown = true;
		return;
	}

	pSession->_lastRecvTime = GetTickCount64();
	_SessionMap[pSession->_ID] = pSession;
	CreatePlayer(pSession);

	_acceptCnt++;
}

void Server::RecvProc(Session* pSession)
{
	pSession->_lastRecvTime = GetTickCount64();

	PRO_BEGIN(L"Network: Recv");
	int recvRet = recv(pSession->_socket,
		pSession->_recvBuf.GetWriteBufferPtr(),
		pSession->_recvBuf.DirectEnqueueSize(), 0);
	PRO_END(L"Network: Recv");

	if (recvRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err == WSAECONNRESET || err == WSAECONNABORTED)
		{
			SetSessionDead(pSession, true);
			return;
		}
		else if(err != WSAEWOULDBLOCK)
		{
			::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]: recv Error, %d\n",
				_T(__FUNCTION__), __LINE__, err);
			SetSessionDead(pSession);
			return;
		}
	}
	else if (recvRet == 0)
	{
		SetSessionDead(pSession, true);
		return;
	}

	int moveRet = pSession->_recvBuf.MoveWritePos(recvRet);
	if (recvRet != moveRet)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		g_bShutdown = true;
		return;
	}
	int useSize = pSession->_recvBuf.GetUseSize();

	Player* pPlayer = _PlayerMap[pSession->_ID];
	
	if (pPlayer == nullptr)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		g_bShutdown = true;
		return;
	}
	
	while (useSize > 0)
	{
		if (useSize <= dfHEADER_SIZE)
			break;

		st_PACKET_HEADER header;
		int peekRet = pSession->_recvBuf.Peek((char*)&header, dfHEADER_SIZE);
		if (peekRet != dfHEADER_SIZE)
		{
			::printf("Error! Func %s Line %d\n", __func__, __LINE__);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
			g_bShutdown = true;
			return;
		}

		if ((char) header.byCode != (char)dfPACKET_CODE)
		{
			::printf("Error! Wrong Header Code! %x - Func %s Line %d\n", 
				header.byCode, __func__, __LINE__);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]: Wrong Header Code, %x\n", 
				_T(__FUNCTION__), __LINE__, header.byCode);
			SetSessionDead(pSession);
			return;
		}

		if (useSize < dfHEADER_SIZE + header.bySize)
			break;

		int moveReadRet = pSession->_recvBuf.MoveReadPos(dfHEADER_SIZE);
		if (moveReadRet != dfHEADER_SIZE)
		{
			::printf("Error! Func %s Line %d\n", __func__, __LINE__);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
			g_bShutdown = true;
			return;
		}
		
		bool handlePacketRet = HandleCSPackets(pPlayer, header.byType);
		if (!handlePacketRet)
		{
			::printf("Error! Func %s Line %d: PlayerID %d, Session ID %d\n", 
				__func__, __LINE__, pPlayer->_ID, pSession->_ID);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]: Handle CS Packet Error\n", _T(__FUNCTION__), __LINE__);
			SetSessionDead(pSession);
			return;
		}

		useSize = pSession->_recvBuf.GetUseSize();
	}
}

void Server::SendProc(Session* pSession)
{
	PRO_BEGIN(L"Network: Send");
	int sendRet = send(pSession->_socket,
		pSession->_sendBuf.GetReadBufferPtr(),
		pSession->_sendBuf.DirectDequeueSize(), 0);
	PRO_END(L"Network: Send");

	if (sendRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err == WSAECONNRESET || err == WSAECONNABORTED)
		{
			SetSessionDead(pSession, true);
			return;
		}
		else if (err != WSAEWOULDBLOCK)
		{
			::printf("Error! Func %s Line %d: %d\n", __func__, __LINE__, err);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]: send Error, %d\n", 
				_T(__FUNCTION__), __LINE__, err);
			SetSessionDead(pSession);
			return;
		}
	}

	int moveRet = pSession->_sendBuf.MoveReadPos(sendRet);
	if (sendRet != moveRet)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		SetSessionDead(pSession);
		return;
	}
}

void Server::SetSessionDead(Session* pSession, bool connectEnd)
{
	if(pSession->_alive)
	{
		pSession->_alive = false;
		_disconnectedSessionIDs[_disconnectCnt] = pSession->_ID;
		_disconnectCnt++;	

		if (connectEnd)
			_connectEndCnt++;
		_disconnectMonitorCnt++;
	}
}

void Server::DisconnectDeadSession()
{
	for (int i = 0; i < _disconnectCnt; i++)
	{
		int ID = _disconnectedSessionIDs[i];

		Player* pPlayer = _PlayerMap[ID];
		if (pPlayer == nullptr)
		{
			::printf("Error! Func %s Line %d (ID: %d)\n", __func__, __LINE__, ID);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]: Session ID Find Error\n", _T(__FUNCTION__), __LINE__);
			g_bShutdown = true;
			return;
		}
		_PlayerMap[ID] = nullptr;

		// Remove from Sector
		vector<Player*>::iterator vectorIter = pPlayer->_pSector->_players.begin();
		for (; vectorIter < pPlayer->_pSector->_players.end(); vectorIter++)
		{
			if ((*vectorIter) == pPlayer)
			{
				pPlayer->_pSector->_players.erase(vectorIter);
				break;
			}
		}
		
		pPlayer->_pSession->_pSendPacket->Clear();
		int deleteRet = SetSCPacket_DELETE_CHAR(pPlayer->_pSession->_pSendPacket, pPlayer->_ID);
		EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), deleteRet, pPlayer->_pSector);
		_pPlayerPool->Free(pPlayer);
		
		Session* pSession = _SessionMap[ID];
		if (pSession == nullptr)
		{
			::printf("Error! Func %s Line %d (ID: %d)\n", __func__, __LINE__, ID);
			LOG(L"ERROR", SystemLog::ERROR_LEVEL,
				L"%s[%d]: Session ID Find Error\n", _T(__FUNCTION__), __LINE__);
			g_bShutdown = true;
			return;
		}
		_SessionMap[ID] = nullptr;

		closesocket(pSession->_socket);	
		_pSPacketPool->Free(pSession->_pRecvPacket);
		_pSPacketPool->Free(pSession->_pSendPacket);
		_pSessionPool->Free(pSession);

		_usableSessionID[_usableCnt++] = ID;
	}

	_disconnectCnt = 0;
}

void Server::EnqueueUnicast(char* msg, int size, Session* pSession)
{
	if (pSession == nullptr)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		g_bShutdown = true;
		return;
	}

	int enqueueRet = pSession->_sendBuf.Enqueue(msg, size);
	if (enqueueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		SetSessionDead(pSession);
	}
}

void Server::EnqueueOneSector(char* msg, int size, Sector* sector, Session* pExpSession)
{
	if (pExpSession == nullptr)
	{
		vector<Player*>::iterator playerIter = sector->_players.begin();
		for (; playerIter < sector->_players.end(); playerIter++)
		{
			if ((*playerIter) == nullptr)
			{
				::printf("Error! Func %s Line %d\n", __func__, __LINE__);
				LOG(L"ERROR", SystemLog::ERROR_LEVEL,
					L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
				g_bShutdown = true;
				return;
			}

			int enqueueRet = (*playerIter)->_pSession->_sendBuf.Enqueue(msg, size);
			if (enqueueRet != size)
			{
				::printf("Error! Func %s Line %d\n", __func__, __LINE__);
				LOG(L"ERROR", SystemLog::ERROR_LEVEL,
					L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
				SetSessionDead((*playerIter)->_pSession);
			}
			
		}
	}
	else
	{
		vector<Player*>::iterator playerIter = sector->_players.begin();
		for (; playerIter < sector->_players.end(); playerIter++)
		{
			if ((*playerIter)->_pSession != pExpSession)
			{
				if ((*playerIter) == nullptr)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					g_bShutdown = true;
					return;
				}

				int enqueueRet = (*playerIter)->_pSession->_sendBuf.Enqueue(msg, size);
				if (enqueueRet != size)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					SetSessionDead((*playerIter)->_pSession);
				}
			}
		}
	}
}

void Server::EnqueueAroundSector(char* msg, int size, Sector* centerSector, Session* pExpSession)
{
	if (pExpSession == nullptr)
	{
		for(int i = 0; i < dfAROUND_SECTOR_NUM; i++)
		{
			vector<Player*>::iterator playerIter 
				= centerSector->_around[i]->_players.begin();

			for (; playerIter < centerSector->_around[i]->_players.end(); playerIter++)
			{
				if ((*playerIter) == nullptr)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					g_bShutdown = true;
					return;
				}

				int enqueueRet = (*playerIter)->_pSession->_sendBuf.Enqueue(msg, size);
				if (enqueueRet != size)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					SetSessionDead((*playerIter)->_pSession);
				}
			}
		}
	}
	else
	{		
		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator playerIter 
				= centerSector->_around[i]->_players.begin();

			for (; playerIter < centerSector->_around[i]->_players.end(); playerIter++)
			{
				if ((*playerIter) == nullptr)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					g_bShutdown = true;
					return;
				}

				int enqueueRet = (*playerIter)->_pSession->_sendBuf.Enqueue(msg, size);
				if (enqueueRet != size)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					SetSessionDead((*playerIter)->_pSession);
				}
			}
		}

		vector<Player*>::iterator playerIter 
			= centerSector->_around[8]->_players.begin();
		for (; playerIter < centerSector->_around[8]->_players.end(); playerIter++)
		{
			if ((*playerIter)->_pSession != pExpSession)
			{
				if ((*playerIter) == nullptr)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					g_bShutdown = true;
					return;
				}

				int enqueueRet = (*playerIter)->_pSession->_sendBuf.Enqueue(msg, size);
				if (enqueueRet != size)
				{
					::printf("Error! Func %s Line %d\n", __func__, __LINE__);
					LOG(L"ERROR", SystemLog::ERROR_LEVEL,
						L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
					SetSessionDead((*playerIter)->_pSession);
				}
			}
		}
	}
}

// About Content ====================================================

bool Server::SkipForFixedFrame()
{
	static DWORD oldTick = GetTickCount64();
	if ((GetTickCount64() - oldTick) < (1000 / dfFPS))
		return true;
	oldTick += (1000 / dfFPS);
	return false;
}

bool Server::CheckMovable(short x, short y)
{
	if (x < dfRANGE_MOVE_LEFT || x > dfRANGE_MOVE_RIGHT ||
		y < dfRANGE_MOVE_TOP || y > dfRANGE_MOVE_BOTTOM)
		return false;

	return true;
}

void Server::UpdatePlayerMove(Player* pPlayer)
{
	switch (pPlayer->_moveDirection)
	{
	case dfPACKET_MOVE_DIR_LL:

		if (CheckMovable(pPlayer->_x - dfSPEED_PLAYER_X, pPlayer->_y))
			pPlayer->_x -= dfSPEED_PLAYER_X;

		if (pPlayer->_x < pPlayer->_pSector->_xPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_LL);

		break;

	case dfPACKET_MOVE_DIR_LU:
		
		if (CheckMovable(pPlayer->_x - dfSPEED_PLAYER_X, pPlayer->_y - dfSPEED_PLAYER_Y))
		{
			pPlayer->_x -= dfSPEED_PLAYER_X;
			pPlayer->_y -= dfSPEED_PLAYER_Y;
		}

		if (pPlayer->_x < pPlayer->_pSector->_xPosMin &&
				pPlayer->_y < pPlayer->_pSector->_yPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_LU);

		else if (pPlayer->_x < pPlayer->_pSector->_xPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_LL);
		
		else if(pPlayer->_y < pPlayer->_pSector->_yPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_UU);

		break;

	case dfPACKET_MOVE_DIR_UU:
		
		if (CheckMovable(pPlayer->_x, pPlayer->_y - dfSPEED_PLAYER_Y))
			pPlayer->_y -= dfSPEED_PLAYER_Y;

		if (pPlayer->_y < pPlayer->_pSector->_yPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_UU);

		break;

	case dfPACKET_MOVE_DIR_RU:
		
		if (CheckMovable(pPlayer->_x + dfSPEED_PLAYER_X, pPlayer->_y - dfSPEED_PLAYER_Y))
		{
			pPlayer->_x += dfSPEED_PLAYER_X;
			pPlayer->_y -= dfSPEED_PLAYER_Y;
		}

		if (pPlayer->_x > pPlayer->_pSector->_xPosMax &&
				pPlayer->_y < pPlayer->_pSector->_yPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_RU);

		else if (pPlayer->_x > pPlayer->_pSector->_xPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_RR);

		else if (pPlayer->_y < pPlayer->_pSector->_yPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_UU);

		break;

	case dfPACKET_MOVE_DIR_RR:

		if (CheckMovable(pPlayer->_x + dfSPEED_PLAYER_X, pPlayer->_y))
			pPlayer->_x += dfSPEED_PLAYER_X;

		if (pPlayer->_x > pPlayer->_pSector->_xPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_RR);

		break;

	case dfPACKET_MOVE_DIR_RD:
		
		if (CheckMovable(pPlayer->_x + dfSPEED_PLAYER_X, pPlayer->_y + dfSPEED_PLAYER_Y))
		{
			pPlayer->_x += dfSPEED_PLAYER_X;
			pPlayer->_y += dfSPEED_PLAYER_Y;
		}

		if (pPlayer->_x > pPlayer->_pSector->_xPosMax &&
				pPlayer->_y > pPlayer->_pSector->_yPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_RD);

		else if (pPlayer->_x > pPlayer->_pSector->_xPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_RR);
		
		else if (pPlayer->_y > pPlayer->_pSector->_yPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_DD);

		break;

	case dfPACKET_MOVE_DIR_DD:
		
		if (CheckMovable(pPlayer->_x, pPlayer->_y + dfSPEED_PLAYER_Y))
			pPlayer->_y += dfSPEED_PLAYER_Y;

		if (pPlayer->_y > pPlayer->_pSector->_yPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_DD);
		
		break;

	case dfPACKET_MOVE_DIR_LD:
		
		if (CheckMovable(pPlayer->_x - dfSPEED_PLAYER_X, pPlayer->_y + dfSPEED_PLAYER_Y))
		{
			pPlayer->_x -= dfSPEED_PLAYER_X;
			pPlayer->_y += dfSPEED_PLAYER_Y;
		}

		if (pPlayer->_x < pPlayer->_pSector->_xPosMin &&
				pPlayer->_y > pPlayer->_pSector->_yPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_LD);

		else if (pPlayer->_x < pPlayer->_pSector->_xPosMin)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_LL);

		else if (pPlayer->_y > pPlayer->_pSector->_yPosMax)
			UpdateSector(pPlayer, dfPACKET_MOVE_DIR_DD);

		break;
	}
}

void Server::SetSector(Player* pPlayer)
{
	int x = (pPlayer->_x / dfSECTOR_SIZE_X) + 2;
	int y = (pPlayer->_y / dfSECTOR_SIZE_Y) + 2;
	_sectors[y][x]._players.push_back(pPlayer);
	pPlayer->_pSector = &_sectors[y][x];
}

void Server::UpdateSector(Player* pPlayer, short direction)
{
	PRO_BEGIN(L"Content: Update Sector");

	vector<Player*>::iterator iter = pPlayer->_pSector->_players.begin();
	for (; iter < pPlayer->_pSector->_players.end(); iter++)
	{
		if (pPlayer == (*iter))
		{
			pPlayer->_pSector->_players.erase(iter);
			break;
		}
	}

	// Get Around Sector Data =======================================
	
	int sectorCnt = _sectorCnt[direction];
	Sector** inSector = pPlayer->_pSector->_new[direction];
	Sector** outSector = pPlayer->_pSector->_old[direction];
	Sector* newSector = pPlayer->_pSector->_around[direction];

	// Send Data About My Player ==============================================


	pPlayer->_pSession->_pSendPacket->Clear();
	int createMeToOtherRet = SetSCPacket_CREATE_OTHER_CHAR(pPlayer->_pSession->_pSendPacket,
		pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y, pPlayer->_hp);
	for (int i = 0; i < sectorCnt; i++)
		EnqueueOneSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), createMeToOtherRet, inSector[i]);

	pPlayer->_pSession->_pSendPacket->Clear();
	int MoveMeToOtherRet = SetSCPacket_MOVE_START(pPlayer->_pSession->_pSendPacket,
		pPlayer->_ID, pPlayer->_moveDirection, pPlayer->_x, pPlayer->_y);
	for (int i = 0; i < sectorCnt; i++)
		EnqueueOneSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), MoveMeToOtherRet, inSector[i]);
	
	pPlayer->_pSession->_pSendPacket->Clear();
	int deleteMeToOtherRet = SetSCPacket_DELETE_CHAR(pPlayer->_pSession->_pSendPacket, pPlayer->_ID);
	for (int i = 0; i < sectorCnt; i++)
		EnqueueOneSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), deleteMeToOtherRet, outSector[i]);

	// Send Data About Other Player ==============================================
	
	for (int i = 0; i < sectorCnt; i++)
	{	
		vector<Player*>::iterator iter = inSector[i]->_players.begin();
		for(; iter < inSector[i]->_players.end(); iter++)
		{
			pPlayer->_pSession->_pSendPacket->Clear();
			int createOtherRet = SetSCPacket_CREATE_OTHER_CHAR(pPlayer->_pSession->_pSendPacket,
				(*iter)->_ID, (*iter)->_direction, (*iter)->_x, (*iter)->_y, (*iter)->_hp);
			EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), createOtherRet, pPlayer->_pSession);

			if((*iter)->_move)
			{
				pPlayer->_pSession->_pSendPacket->Clear();
				int MoveOtherRet = SetSCPacket_MOVE_START(pPlayer->_pSession->_pSendPacket,
					(*iter)->_ID, (*iter)->_moveDirection, (*iter)->_x, (*iter)->_y);
				EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), MoveOtherRet, pPlayer->_pSession);
			}
		}
	}

	for (int i = 0; i < sectorCnt; i++)
	{
		vector<Player*>::iterator iter = outSector[i]->_players.begin();
		for (; iter < outSector[i]->_players.end(); iter++)
		{
			pPlayer->_pSession->_pSendPacket->Clear();
			int deleteOtherRet = SetSCPacket_DELETE_CHAR(pPlayer->_pSession->_pSendPacket, (*iter)->_ID);
			EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), deleteOtherRet, pPlayer->_pSession);
		}
	}

	pPlayer->_pSector = newSector;
	newSector->_players.push_back(pPlayer);
	

	PRO_END(L"Content: Update Sector");
}

void Server::CreatePlayer(Session* pSession)
{
	Player* pPlayer = _pPlayerPool->Alloc(pSession, _playerID++);
	_PlayerMap[pSession->_ID] = pPlayer;
	SetSector(pPlayer);
	
	pPlayer->_pSession->_pSendPacket->Clear();
	int createMeRet = SetSCPacket_CREATE_MY_CHAR(pPlayer->_pSession->_pSendPacket,
		pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y, pPlayer->_hp);
	EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), createMeRet, pPlayer->_pSession);

	pPlayer->_pSession->_pSendPacket->Clear();
	int createMeToOtherRet = SetSCPacket_CREATE_OTHER_CHAR(pPlayer->_pSession->_pSendPacket,
		pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y, pPlayer->_hp);
	EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(),
		createMeToOtherRet, pPlayer->_pSector, pPlayer->_pSession);

	for (int i = 0; i < 8; i++)
	{
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[i]->_players.begin();
		for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
		{
			pPlayer->_pSession->_pSendPacket->Clear();
			int createOtherRet = SetSCPacket_CREATE_OTHER_CHAR(pPlayer->_pSession->_pSendPacket,
				(*iter)->_ID, (*iter)->_direction, (*iter)->_x, (*iter)->_y, (*iter)->_hp);
			EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), createOtherRet, pPlayer->_pSession);
		}
	}

	vector<Player*>::iterator iter
		= pPlayer->_pSector->_around[8]->_players.begin();
	for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
	{
		if ((*iter) != pPlayer)
		{
			pPlayer->_pSession->_pSendPacket->Clear();
			int createOtherRet = SetSCPacket_CREATE_OTHER_CHAR(pPlayer->_pSession->_pSendPacket,
				(*iter)->_ID, (*iter)->_direction, (*iter)->_x, (*iter)->_y, (*iter)->_hp);
			EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), createOtherRet, pPlayer->_pSession);
		}
	}

	
}

// About Packet ====================================================

bool Server::HandleCSPackets(Player* pPlayer, BYTE type)
{
	switch (type)
	{
	case dfPACKET_CS_MOVE_START:
	{
		PRO_BEGIN(L"Network: MOVE START");
		bool ret = HandleCSPacket_MOVE_START(pPlayer);	
		PRO_END(L"Network: MOVE START");
		return ret;
	}
		break;

	case dfPACKET_CS_MOVE_STOP:
	{
		PRO_BEGIN(L"Network: MOVE STOP");
		bool ret = HandleCSPacket_MOVE_STOP(pPlayer);
		PRO_END(L"Network: MOVE STOP");
		return ret;
	}
		break;

	case dfPACKET_CS_ATTACK1:
	{
		PRO_BEGIN(L"Network: ATTACK1");
		bool ret = HandleCSPacket_ATTACK1(pPlayer);
		PRO_END(L"Network: ATTACK1");
		return ret;
	}
		break;

	case dfPACKET_CS_ATTACK2:
	{
		PRO_BEGIN(L"Network: ATTACK2");
		bool ret = HandleCSPacket_ATTACK2(pPlayer);
		PRO_END(L"Network: ATTACK2");
		return ret;
	}
		break;

	case dfPACKET_CS_ATTACK3:
	{
		PRO_BEGIN(L"Network: ATTACK3");
		bool ret = HandleCSPacket_ATTACK3(pPlayer);
		PRO_END(L"Network: ATTACK3");
		return ret;
	}
		break;
	
	case dfPACKET_CS_ECHO:
	{
		PRO_BEGIN(L"Network: ECHO");
		bool ret = HandleCSPacket_ECHO(pPlayer);
		PRO_END(L"Network: ECHO");
		return ret;
	}
		break;
	}

	::printf("Error! Func %s Line %d Case %d\n", __func__, __LINE__, type);
	LOG(L"ERROR", SystemLog::ERROR_LEVEL,
		L"%s[%d] No Switch Case, %d\n", _T(__FUNCTION__), __LINE__, type);
	return false;
}

bool Server::HandleCSPacket_MOVE_START(Player* pPlayer)
{
	BYTE moveDirection;
	short x;
	short y;

	pPlayer->_pSession->_pRecvPacket->Clear();
	bool getRet = GetCSPacket_MOVE_START(pPlayer->_pSession->_pRecvPacket, &(pPlayer->_pSession->_recvBuf), moveDirection, x, y);
	if(!getRet) return false;
	
	if (abs(pPlayer->_x - x) > dfERROR_RANGE || abs(pPlayer->_y - y) > dfERROR_RANGE)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int setRet = SetSCPacket_SYNC(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_x, pPlayer->_y);
		EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSession);
		x = pPlayer->_x;
		y = pPlayer->_y;
	}

	SetPlayerMoveStart(pPlayer, moveDirection, x, y);

	pPlayer->_pSession->_pSendPacket->Clear();
	int setRet = SetSCPacket_MOVE_START(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_moveDirection, pPlayer->_x, pPlayer->_y);
	EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSector, pPlayer->_pSession);
	
	return true;
}

bool Server::HandleCSPacket_MOVE_STOP(Player* pPlayer)
{
	BYTE direction;
	short x;
	short y;

	pPlayer->_pSession->_pRecvPacket->Clear();
	bool getRet = GetCSPacket_MOVE_STOP(pPlayer->_pSession->_pRecvPacket, &(pPlayer->_pSession->_recvBuf), direction, x, y);
	if (!getRet) return false;
	
	if (abs(pPlayer->_x - x) > dfERROR_RANGE || abs(pPlayer->_y - y) > dfERROR_RANGE)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int setRet = SetSCPacket_SYNC(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_x, pPlayer->_y);
		EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSession);
		x = pPlayer->_x;
		y = pPlayer->_y;
	}

	SetPlayerMoveStop(pPlayer, direction, x, y);

	pPlayer->_pSession->_pSendPacket->Clear();
	int setRet = SetSCPacket_MOVE_STOP(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y);
	EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSector, pPlayer->_pSession);
	
	return true;
}

bool Server::HandleCSPacket_ATTACK1(Player* pPlayer)
{
	BYTE direction;
	short x;
	short y;

	pPlayer->_pSession->_pRecvPacket->Clear();
	bool getRet = GetCSPacket_ATTACK1(pPlayer->_pSession->_pRecvPacket, &(pPlayer->_pSession->_recvBuf), direction, x, y);
	if (!getRet) return false;
	
	if (abs(pPlayer->_x - x) > dfERROR_RANGE || abs(pPlayer->_y - y) > dfERROR_RANGE)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int setRet = SetSCPacket_SYNC(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_x, pPlayer->_y);
		EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSession);
		x = pPlayer->_x;
		y = pPlayer->_y;
	}

	Player* damagedPlayer = nullptr;
	SetPlayerAttack1(pPlayer, damagedPlayer, direction, x, y);

	pPlayer->_pSession->_pSendPacket->Clear();
	int attackSetRet = SetSCPacket_ATTACK1(pPlayer->_pSession->_pSendPacket, 
		pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y);
	EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), attackSetRet, pPlayer->_pSector);

	if (damagedPlayer != nullptr)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int damageSetRet = SetSCPacket_DAMAGE(
			pPlayer->_pSession->_pSendPacket, pPlayer->_ID, damagedPlayer->_ID, damagedPlayer->_hp);
		EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), damageSetRet, damagedPlayer->_pSector);
	}

	return true;
}

bool Server::HandleCSPacket_ATTACK2(Player* pPlayer)
{
	BYTE direction;
	short x;
	short y;

	pPlayer->_pSession->_pRecvPacket->Clear();
	bool getRet = GetCSPacket_ATTACK2(pPlayer->_pSession->_pRecvPacket, &(pPlayer->_pSession->_recvBuf), direction, x, y);
	if (!getRet) return false;

	if (abs(pPlayer->_x - x) > dfERROR_RANGE || abs(pPlayer->_y - y) > dfERROR_RANGE)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int setRet = SetSCPacket_SYNC(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_x, pPlayer->_y);
		EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSession);
		x = pPlayer->_x;
		y = pPlayer->_y;
	}

	Player* damagedPlayer = nullptr;
	SetPlayerAttack2(pPlayer, damagedPlayer, direction, x, y);

	pPlayer->_pSession->_pSendPacket->Clear();
	int attackSetRet = SetSCPacket_ATTACK2(pPlayer->_pSession->_pSendPacket, 
		pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y);
	EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), attackSetRet, pPlayer->_pSector);

	if (damagedPlayer != nullptr)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int damageSetRet = SetSCPacket_DAMAGE(
			pPlayer->_pSession->_pSendPacket, pPlayer->_ID, damagedPlayer->_ID, damagedPlayer->_hp);
		EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), damageSetRet, damagedPlayer->_pSector);
	}

	
	return true;
}

bool Server::HandleCSPacket_ATTACK3(Player* pPlayer)
{
	BYTE direction;
	short x;
	short y;

	pPlayer->_pSession->_pRecvPacket->Clear();
	bool getRet = GetCSPacket_ATTACK3(pPlayer->_pSession->_pRecvPacket, &(pPlayer->_pSession->_recvBuf), direction, x, y);
	if (!getRet) return false;

	if (abs(pPlayer->_x - x) > dfERROR_RANGE || abs(pPlayer->_y - y) > dfERROR_RANGE)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int setRet = SetSCPacket_SYNC(pPlayer->_pSession->_pSendPacket, pPlayer->_ID, pPlayer->_x, pPlayer->_y);
		EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSession);
		x = pPlayer->_x;
		y = pPlayer->_y;
	}

	Player* damagedPlayer = nullptr;
	SetPlayerAttack3(pPlayer, damagedPlayer, direction, x, y);

	pPlayer->_pSession->_pSendPacket->Clear();
	int attackSetRet = SetSCPacket_ATTACK3(pPlayer->_pSession->_pSendPacket, 
		pPlayer->_ID, pPlayer->_direction, pPlayer->_x, pPlayer->_y);
	EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), attackSetRet, pPlayer->_pSector);

	if (damagedPlayer != nullptr)
	{
		pPlayer->_pSession->_pSendPacket->Clear();
		int damageSetRet = SetSCPacket_DAMAGE(
			pPlayer->_pSession->_pSendPacket, pPlayer->_ID, damagedPlayer->_ID, damagedPlayer->_hp);
		EnqueueAroundSector(pPlayer->_pSession->_pSendPacket->GetReadPtr(), damageSetRet, damagedPlayer->_pSector);
	}

	return true;
}

bool Server::HandleCSPacket_ECHO(Player* pPlayer)
{
	int time;

	pPlayer->_pSession->_pRecvPacket->Clear();
	bool getRet = GetCSPacket_ECHO(pPlayer->_pSession->_pRecvPacket, &(pPlayer->_pSession->_recvBuf), time);
	if (!getRet) return false;
	
	pPlayer->_pSession->_pSendPacket->Clear();
	int setRet = SetSCPacket_ECHO(pPlayer->_pSession->_pSendPacket, time);
	EnqueueUnicast(pPlayer->_pSession->_pSendPacket->GetReadPtr(), setRet, pPlayer->_pSession);
	
	return true;
}

bool Server::GetCSPacket_MOVE_START(SerializePacket* pPacket, RingBuffer* recvBuffer, BYTE& moveDirection, short& x, short& y)
{
	int size = sizeof(moveDirection) + sizeof(x) + sizeof(y);
	int dequeueRet = recvBuffer->Dequeue(pPacket->GetWritePtr(), size);
	if (dequeueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		return false;
	}
	pPacket->MoveWritePos(dequeueRet);

	*pPacket >> moveDirection;
	*pPacket >> x;
	*pPacket >> y;

	return true;
}

bool Server::GetCSPacket_MOVE_STOP(SerializePacket* pPacket, RingBuffer* recvBuffer, BYTE& direction, short& x, short& y)
{
	
	int size = sizeof(direction) + sizeof(x) + sizeof(y);
	int dequeueRet = recvBuffer->Dequeue(pPacket->GetWritePtr(), size);
	if (dequeueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		return false;
	}
	pPacket->MoveWritePos(dequeueRet);

	*pPacket >> direction;
	*pPacket >> x;
	*pPacket >> y;

	
	return true;
}

bool Server::GetCSPacket_ATTACK1(SerializePacket* pPacket, RingBuffer* recvBuffer, BYTE& direction, short& x, short& y)
{
	
	int size = sizeof(direction) + sizeof(x) + sizeof(y);
	int dequeueRet = recvBuffer->Dequeue(pPacket->GetWritePtr(), size);
	if (dequeueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		return false;
	}
	pPacket->MoveWritePos(dequeueRet);

	*pPacket >> direction;
	*pPacket >> x;
	*pPacket >> y;

	
	return true;
}

bool Server::GetCSPacket_ATTACK2(SerializePacket* pPacket, RingBuffer* recvBuffer, BYTE& direction, short& x, short& y)
{
	
	int size = sizeof(direction) + sizeof(x) + sizeof(y);
	int dequeueRet = recvBuffer->Dequeue(pPacket->GetWritePtr(), size);
	if (dequeueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		return false;
	}
	pPacket->MoveWritePos(dequeueRet);

	*pPacket >> direction;
	*pPacket >> x;
	*pPacket >> y;

	
	return true;
}

bool Server::GetCSPacket_ATTACK3(SerializePacket* pPacket, RingBuffer* recvBuffer, BYTE& direction, short& x, short& y)
{
	
	int size = sizeof(direction) + sizeof(x) + sizeof(y);
	int dequeueRet = recvBuffer->Dequeue(pPacket->GetWritePtr(), size);
	if (dequeueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		return false;
	}
	pPacket->MoveWritePos(dequeueRet);

	*pPacket >> direction;
	*pPacket >> x;
	*pPacket >> y;

	
	return true;
}

bool Server::GetCSPacket_ECHO(SerializePacket* pPacket, RingBuffer* recvBuffer, int& time)
{
	
	int size = sizeof(time);
	int dequeueRet = recvBuffer->Dequeue(pPacket->GetWritePtr(), size);
	if (dequeueRet != size)
	{
		::printf("Error! Func %s Line %d\n", __func__, __LINE__);
		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]\n", _T(__FUNCTION__), __LINE__);
		return false;
	}
	pPacket->MoveWritePos(dequeueRet);
	*pPacket >> time;

	
	return true;
}

void Server::SetPlayerMoveStart(Player* pPlayer, BYTE& moveDirection, short& x, short& y)
{
	pPlayer->_x = x;
	pPlayer->_y = y;
	pPlayer->_move = true;
	pPlayer->_moveDirection = moveDirection;

	switch (moveDirection)
	{
	case  dfPACKET_MOVE_DIR_LL:
	case  dfPACKET_MOVE_DIR_LU:
	case  dfPACKET_MOVE_DIR_LD:
		pPlayer->_direction = dfPACKET_MOVE_DIR_LL;
		break;

	case  dfPACKET_MOVE_DIR_RU:
	case  dfPACKET_MOVE_DIR_RR:
	case  dfPACKET_MOVE_DIR_RD:
		pPlayer->_direction = dfPACKET_MOVE_DIR_RR;
		break;
	}
	
}

void Server::SetPlayerMoveStop(Player* pPlayer, BYTE& direction, short& x, short& y)
{
	pPlayer->_x = x;
	pPlayer->_y = y;
	pPlayer->_move = false;
	pPlayer->_direction = direction;	
}

void Server::SetPlayerAttack1(Player* pPlayer, Player*& pDamagedPlayer, BYTE& direction, short& x, short& y)
{
	pPlayer->_x = x;
	pPlayer->_y = y;
	pPlayer->_direction = direction;

	if (direction == dfPACKET_MOVE_DIR_LL)
	{
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[8]->_players.begin();

		for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
		{
			if ((*iter) != pPlayer)
			{
				int dist = pPlayer->_x - (*iter)->_x;
				if (dist >= 0 && dist <= dfATTACK1_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK1_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK1_DAMAGE;

					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}

					return;
				}
			}
		}

		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator iter
				= pPlayer->_pSector->_around[i]->_players.begin();

			for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
			{
				int dist = pPlayer->_x - (*iter)->_x;
				if (dist >= 0 && dist <= dfATTACK1_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK1_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK1_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}

					return;
				}
			}
		}
	}
	else if (direction == dfPACKET_MOVE_DIR_RR)
	{
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[8]->_players.begin();
		for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
		{
			if((*iter) != pPlayer)
			{
				int dist = (*iter)->_x - pPlayer->_x;
				if (dist >= 0 && dist <= dfATTACK1_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK1_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK1_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}

		
		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator iter
				= pPlayer->_pSector->_around[i]->_players.begin();
			for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
			{
				int dist = (*iter)->_x - pPlayer->_x;
				if (dist >= 0 && dist <= dfATTACK1_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK1_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK1_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}
	}
	
	
}

void Server::SetPlayerAttack2(Player* pPlayer, Player*& pDamagedPlayer, BYTE& direction, short& x, short& y)
{
	pPlayer->_x = x;
	pPlayer->_y = y;
	pPlayer->_direction = direction;

	if (direction == dfPACKET_MOVE_DIR_LL)
	{	
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[8]->_players.begin();

		for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
		{
			if((*iter) != pPlayer)
			{
				int dist = pPlayer->_x - (*iter)->_x;
				if (dist >= 0 && dist <= dfATTACK2_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK2_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK2_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}

		
		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator iter
				= pPlayer->_pSector->_around[i]->_players.begin();
			for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
			{
				int dist = pPlayer->_x - (*iter)->_x;
				if (dist >= 0 && dist <= dfATTACK2_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK2_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK2_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}
	}
	else if (direction == dfPACKET_MOVE_DIR_RR)
	{
		
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[8]->_players.begin();

		for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
		{
			if((*iter) != pPlayer)
			{
				int dist = (*iter)->_x - pPlayer->_x;
				if (dist >= 0 && dist <= dfATTACK2_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK2_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK2_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
					}
					return;
				}
			}
		}

		
		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator iter
				= pPlayer->_pSector->_around[i]->_players.begin();
			for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
			{
				int dist = (*iter)->_x - pPlayer->_x;
				if (dist >= 0 && dist <= dfATTACK2_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK2_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK2_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}
	}
	
}

void Server::SetPlayerAttack3(Player* pPlayer, Player*& pDamagedPlayer, BYTE& direction, short& x, short& y)
{
	pPlayer->_x = x;
	pPlayer->_y = y;
	pPlayer->_direction = direction;

	if (direction == dfPACKET_MOVE_DIR_LL)
	{
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[8]->_players.begin();
		for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
		{
			if((*iter) != pPlayer)
			{
				int dist = pPlayer->_x - (*iter)->_x;
				if (dist >= 0 && dist <= dfATTACK3_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK3_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK3_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}
		
		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator iter 
				= pPlayer->_pSector->_around[i]->_players.begin();
			for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
			{
				int dist = pPlayer->_x - (*iter)->_x;
				if (dist >= 0 && dist <= dfATTACK3_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK3_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK3_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}
	}
	else if (direction == dfPACKET_MOVE_DIR_RR)
	{
		vector<Player*>::iterator iter 
			= pPlayer->_pSector->_around[8]->_players.begin();
		for (; iter < pPlayer->_pSector->_around[8]->_players.end(); iter++)
		{
			if((*iter)!= pPlayer)
			{
				int dist = (*iter)->_x - pPlayer->_x;
				if (dist >= 0 && dist <= dfATTACK3_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK3_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK3_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
						
					}
					return;
				}
			}
		}
		
		for (int i = 0; i < 8; i++)
		{
			vector<Player*>::iterator iter 
				= pPlayer->_pSector->_around[i]->_players.begin();
			for (; iter < pPlayer->_pSector->_around[i]->_players.end(); iter++)
			{
				int dist = (*iter)->_x - pPlayer->_x;
				if (dist >= 0 && dist <= dfATTACK3_RANGE_X &&
					abs((*iter)->_y - pPlayer->_y) <= dfATTACK3_RANGE_Y)
				{
					pDamagedPlayer = (*iter);
					pDamagedPlayer->_hp -= dfATTACK3_DAMAGE;
					
					if (pDamagedPlayer->_hp <= 0)
					{
						_deadCnt++;
						SetSessionDead(pDamagedPlayer->_pSession);
					}
					return;
				}
			}
		}
	}
	
}

void Server::SetSCPacket_HEADER(SerializePacket* pPacket, BYTE size, BYTE type)
{
	*pPacket << (BYTE)dfPACKET_CODE;
	*pPacket << size;
	*pPacket << type;
}

int Server::SetSCPacket_CREATE_MY_CHAR(SerializePacket* pPacket, int ID, BYTE direction, short x, short y, BYTE hp)
{
	int size = sizeof(ID) + sizeof(direction) + sizeof(x) + sizeof(y) + sizeof(hp);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_CREATE_MY_CHARACTER);
	
	*pPacket << ID;
	*pPacket << direction;
	*pPacket << x;
	*pPacket << y;
	*pPacket << hp;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_CREATE_OTHER_CHAR(SerializePacket* pPacket, int ID, BYTE direction, short x, short y, BYTE hp)
{
	int size = sizeof(ID) + sizeof(direction) + sizeof(x) + sizeof(y) + sizeof(hp);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_CREATE_OTHER_CHARACTER);

	*pPacket << ID;
	*pPacket << direction;
	*pPacket << x;
	*pPacket << y;
	*pPacket << hp;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_DELETE_CHAR(SerializePacket* pPacket, int ID)
{
	int size = sizeof(ID);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_DELETE_CHARACTER);

	*pPacket << ID;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_MOVE_START(SerializePacket* pPacket, int ID, BYTE moveDirection, short x, short y)
{
	int size = sizeof(ID) + sizeof(moveDirection) + sizeof(x) + sizeof(y);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_MOVE_START);

	*pPacket << ID;
	*pPacket << moveDirection;
	*pPacket << x;
	*pPacket << y;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_MOVE_STOP(SerializePacket* pPacket, int ID, BYTE direction, short x, short y)
{
	int size = sizeof(ID) + sizeof(direction) + sizeof(x) + sizeof(y);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_MOVE_STOP);

	*pPacket << ID;
	*pPacket << direction;
	*pPacket << x;
	*pPacket << y;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_ATTACK1(SerializePacket* pPacket, int ID, BYTE direction, short x, short y)
{
	int size = sizeof(ID) + sizeof(direction) + sizeof(x) + sizeof(y);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_ATTACK1);

	*pPacket << ID;
	*pPacket << direction;
	*pPacket << x;
	*pPacket << y;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_ATTACK2(SerializePacket* pPacket, int ID, BYTE direction, short x, short y)
{
	int size = sizeof(ID) + sizeof(direction) + sizeof(x) + sizeof(y);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_ATTACK2);

	*pPacket << ID;
	*pPacket << direction;
	*pPacket << x;
	*pPacket << y;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_ATTACK3(SerializePacket* pPacket, int ID, BYTE direction, short x, short y)
{
	int size = sizeof(ID) + sizeof(direction) + sizeof(x) + sizeof(y);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_ATTACK3);

	*pPacket << ID;
	*pPacket << direction;
	*pPacket << x;
	*pPacket << y;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_DAMAGE(SerializePacket* pPacket, int attackID, int damageID, BYTE damageHP)
{
	int size = sizeof(attackID) + sizeof(damageID) + sizeof(damageHP);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_DAMAGE);

	*pPacket << attackID;
	*pPacket << damageID;
	*pPacket << damageHP;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_SYNC(SerializePacket* pPacket, int ID, short x, short y)
{
	int size = sizeof(ID) + sizeof(x) + sizeof(y);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_SYNC);

	*pPacket << ID;
	*pPacket << x;
	*pPacket << y;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}

	_syncCnt++;
	return pPacket->GetDataSize();
}

int Server::SetSCPacket_ECHO(SerializePacket* pPacket, int time)
{
	int size = sizeof(time);
	SetSCPacket_HEADER(pPacket, size, dfPACKET_SC_ECHO);

	*pPacket << time;

	if (pPacket->GetDataSize() != dfHEADER_SIZE + size)
	{
		::printf("Create Packet Error, %d != %d: func %s, line %d\n",
			pPacket->GetDataSize(), dfHEADER_SIZE + size, __func__, __LINE__);

		LOG(L"ERROR", SystemLog::ERROR_LEVEL,
			L"%s[%d]: Create Packet Error, %d != %d\n",
			_T(__FUNCTION__), __LINE__, pPacket->GetDataSize(), dfHEADER_SIZE + size);
	}
	
	return pPacket->GetDataSize();
}

void Server::Sector::InitializeSector(short xIndex, short yIndex)
{
	_xIndex = xIndex;
	_yIndex = yIndex;

	if (_xIndex < 2 || _xIndex >= (dfSECTOR_CNT_X - 2) ||
		_yIndex < 2 || _yIndex >= (dfSECTOR_CNT_Y - 2))
		return;

	_xPosMin = (xIndex - 2) * dfSECTOR_SIZE_X ;
	_yPosMin = (yIndex - 2) * dfSECTOR_SIZE_Y;
	_xPosMax = (xIndex - 1) * dfSECTOR_SIZE_X;
	_yPosMax = (yIndex - 1) * dfSECTOR_SIZE_Y;
	_players.reserve(dfDEFAULT_PLAYERS_PER_SECTOR);
}

void Server::SetSectorsAroundInfo()
{
	for (int y = 2; y < dfSECTOR_CNT_Y - 2; y++)
	{
		for (int x = 2; x < dfSECTOR_CNT_X - 2; x++)
		{
			Sector* pSector = &_sectors[y][x];

			pSector->_around[dfPACKET_MOVE_DIR_LL] = &_sectors[y][x - 1];
			pSector->_around[dfPACKET_MOVE_DIR_LU] = &_sectors[y - 1][x - 1];
			pSector->_around[dfPACKET_MOVE_DIR_UU] = &_sectors[y - 1][x];
			pSector->_around[dfPACKET_MOVE_DIR_RU] = &_sectors[y - 1][x + 1];
			pSector->_around[dfPACKET_MOVE_DIR_RR] = &_sectors[y][x + 1];
			pSector->_around[dfPACKET_MOVE_DIR_RD] = &_sectors[y + 1][x + 1];
			pSector->_around[dfPACKET_MOVE_DIR_DD] = &_sectors[y + 1][x];
			pSector->_around[dfPACKET_MOVE_DIR_LD] = &_sectors[y + 1][x - 1];
			pSector->_around[8] = &_sectors[y][x];
			
			// dfPACKET_MOVE_DIR_LL

			pSector->_llNew[0] = &_sectors[y - 1][x - 2];
			pSector->_llNew[1] = &_sectors[y][x - 2];
			pSector->_llNew[2] = &_sectors[y + 1][x - 2];

			pSector->_llOld[0] = &_sectors[y - 1][x + 1];
			pSector->_llOld[1] = &_sectors[y][x + 1];
			pSector->_llOld[2] = &_sectors[y + 1][x + 1];

			// dfPACKET_MOVE_DIR_LU

			pSector->_luNew[0] = &_sectors[y - 2][x];
			pSector->_luNew[1] = &_sectors[y - 2][x - 1];
			pSector->_luNew[2] = &_sectors[y - 2][x - 2];
			pSector->_luNew[3] = &_sectors[y - 1][x - 2];
			pSector->_luNew[4] = &_sectors[y][x - 2];

			pSector->_luOld[0] = &_sectors[y + 1][x];
			pSector->_luOld[1] = &_sectors[y + 1][x - 1];
			pSector->_luOld[2] = &_sectors[y + 1][x + 1];
			pSector->_luOld[3] = &_sectors[y - 1][x + 1];
			pSector->_luOld[4] = &_sectors[y][x + 1];

			// dfPACKET_MOVE_DIR_UU

			pSector->_uuNew[0] = &_sectors[y - 2][x - 1];
			pSector->_uuNew[1] = &_sectors[y - 2][x];
			pSector->_uuNew[2] = &_sectors[y - 2][x + 1];

			pSector->_uuOld[0] = &_sectors[y + 1][x - 1];
			pSector->_uuOld[1] = &_sectors[y + 1][x];
			pSector->_uuOld[2] = &_sectors[y + 1][x + 1];

			// dfPACKET_MOVE_DIR_RU

			pSector->_ruNew[0] = &_sectors[y - 2][x];
			pSector->_ruNew[1] = &_sectors[y - 2][x + 1];
			pSector->_ruNew[2] = &_sectors[y - 2][x + 2];
			pSector->_ruNew[3] = &_sectors[y - 1][x + 2];
			pSector->_ruNew[4] = &_sectors[y][x + 2];

			pSector->_ruOld[0] = &_sectors[y][x - 1];
			pSector->_ruOld[1] = &_sectors[y - 1][x - 1];
			pSector->_ruOld[2] = &_sectors[y + 1][x - 1];
			pSector->_ruOld[3] = &_sectors[y + 1][x + 1];
			pSector->_ruOld[4] = &_sectors[y + 1][x];

			// dfPACKET_MOVE_DIR_RR

			pSector->_rrNew[0] = &_sectors[y - 1][x + 2];
			pSector->_rrNew[1] = &_sectors[y][x + 2];
			pSector->_rrNew[2] = &_sectors[y + 1][x + 2];

			pSector->_rrOld[0] = &_sectors[y - 1][x - 1];
			pSector->_rrOld[1] = &_sectors[y][x - 1];
			pSector->_rrOld[2] = &_sectors[y + 1][x - 1];

			// dfPACKET_MOVE_DIR_RD

			pSector->_rdNew[0] = &_sectors[y + 2][x];
			pSector->_rdNew[1] = &_sectors[y + 2][x + 1];
			pSector->_rdNew[2] = &_sectors[y + 2][x + 2];
			pSector->_rdNew[3] = &_sectors[y + 1][x + 2];
			pSector->_rdNew[4] = &_sectors[y][x + 2];

			pSector->_rdOld[0] = &_sectors[y - 1][x];
			pSector->_rdOld[1] = &_sectors[y - 1][x + 1];
			pSector->_rdOld[2] = &_sectors[y - 1][x - 1];
			pSector->_rdOld[3] = &_sectors[y + 1][x - 1];
			pSector->_rdOld[4] = &_sectors[y][x - 1];

			// dfPACKET_MOVE_DIR_DD

			pSector->_ddNew[0] = &_sectors[y + 2][x - 1];
			pSector->_ddNew[1] = &_sectors[y + 2][x];
			pSector->_ddNew[2] = &_sectors[y + 2][x + 1];

			pSector->_ddOld[0] = &_sectors[y - 1][x - 1];
			pSector->_ddOld[1] = &_sectors[y - 1][x];
			pSector->_ddOld[2] = &_sectors[y - 1][x + 1];

			// dfPACKET_MOVE_DIR_LD

			pSector->_ldNew[0] = &_sectors[y + 2][x];
			pSector->_ldNew[1] = &_sectors[y + 2][x - 1];
			pSector->_ldNew[2] = &_sectors[y + 2][x - 2];
			pSector->_ldNew[3] = &_sectors[y + 1][x - 2];
			pSector->_ldNew[4] = &_sectors[y][x - 2];

			pSector->_ldOld[0] = &_sectors[y][x + 1];
			pSector->_ldOld[1] = &_sectors[y + 1][x + 1];
			pSector->_ldOld[2] = &_sectors[y - 1][x + 1];
			pSector->_ldOld[3] = &_sectors[y - 1][x - 1];
			pSector->_ldOld[4] = &_sectors[y - 1][x];
		}
	}
}
