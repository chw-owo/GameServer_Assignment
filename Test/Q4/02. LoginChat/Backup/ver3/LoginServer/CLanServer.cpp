#include "CLanServer.h"
#ifdef LANSERVER

#include "ErrorCode.h"
#include <stdio.h>
#include <tchar.h>

// 17 bit for Idx (MAX 131072)
// 47 bit for Id (MAX 140737488355328)

CLanServer::CLanServer()
{
	_indexMask = 0b11111111111111111;
	_indexMask <<= __ID_BIT__;
	_idMask = ~_indexMask;

	_releaseFlag._useCount = 0;
	_releaseFlag._releaseFlag = 1;
}

bool CLanServer::NetworkInitialize(const wchar_t* IP, short port, int numOfThreads, int numOfRunnings, bool nagle, bool monitorServer)
{
	// Option Setting ====================================================

	wcscpy_s(_IP, 10, IP);
	_port = port;
	_numOfThreads = numOfThreads;
	_numOfRunnings = numOfRunnings;
	_nagle = nagle;
	_mm = new CMonitorManager(monitorServer);

	// Resource Setting ===================================================

	for (int i = 0; i < dfSESSION_MAX; i++)
	{
		_emptyIdx.Push(i);
		_sessions[i] = new CLanSession;
	}

	for (int i = 0; i < dfJOB_QUEUE_CNT; i++)
	{
		_pJobQueues[i] = new CLockFreeQueue<CLanJob*>;
	}
	_pJobPool = new CTlsPool<CLanJob>(dfJOB_DEF, true);

	// Lanwork Setting ===================================================

	// Initialize Winsock
	WSADATA wsa;
	int startRet = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (startRet != 0)
	{
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: WSAStartup Error", _T(__FUNCTION__), __LINE__);
		OnError(ERR_WSASTARTUP, stErrMsg);
		return false;
	}

	// Create Listen Sock
	_listenSock = socket(AF_INET, SOCK_STREAM, 0);
	if (_listenSock == INVALID_SOCKET)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Listen sock is INVALID, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_LISTENSOCK_INVALID, stErrMsg);
		return false;
	}

	// Set Linger Option
	LINGER optval;
	optval.l_onoff = 1;
	optval.l_linger = 0;
	int optRet = setsockopt(_listenSock, SOL_SOCKET, SO_LINGER, (char*)&optval, sizeof(optval));
	if (optRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Set Linger Option Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_SET_LINGER, stErrMsg);
		return false;
	}

	// Set SndBuf0 Option for Async
	int sndBufSize = 0;
	optRet = setsockopt(_listenSock, SOL_SOCKET, SO_SNDBUF, (char*)&sndBufSize, sizeof(sndBufSize));
	if (optRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Set SendBuf Option Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_SET_SNDBUF_0, stErrMsg);
		return false;
	}

	// Listen Socket Bind
	SOCKADDR_IN serveraddr;
	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(_port);
	int bindRet = bind(_listenSock, (SOCKADDR*)&serveraddr, sizeof(serveraddr));
	if (bindRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Listen Sock Bind Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_LISTENSOCK_BIND, stErrMsg);
		return false;
	}

	// Start Listen
	int listenRet = listen(_listenSock, SOMAXCONN);
	if (listenRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Listen Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_LISTEN, stErrMsg);
		return false;
	}

	// Thread Setting ===================================================

	// Create IOCP for Lanwork
	_hNetworkCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, _numOfRunnings);
	if (_hNetworkCP == NULL)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Create IOCP Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_CREATE_IOCP, stErrMsg);
		return false;
	}

	// Create Accept Thread
	_acceptThread = (HANDLE)_beginthreadex(NULL, 0, AcceptThread, this, 0, nullptr);
	if (_acceptThread == NULL)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Create Accept Thread Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_CREATE_ACCEPT_THREAD, stErrMsg);
		return false;
	}

	// Create Lanwork Thread
	_networkThreads = new HANDLE[_numOfThreads];
	for (int i = 0; i < _numOfThreads; i++)
	{
		_networkThreads[i] = (HANDLE)_beginthreadex(NULL, 0, NetworkThread, this, 0, nullptr);
		if (_networkThreads[i] == NULL)
		{
			int err = WSAGetLastError();
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Create Network Thread Error, %d", _T(__FUNCTION__), __LINE__, err);
			OnError(ERR_CREATE_NETWORK_THREAD, stErrMsg);
			return false;
		}
	}

	OnInitialize();

	return true;
}

bool CLanServer::NetworkTerminate()
{
	if (InterlockedExchange(&_networkAlive, 1) != 0)
	{
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: LanServer Already Terminate", _T(__FUNCTION__), __LINE__);
		OnError(ERR_ALREADY_TERMINATE, stErrMsg);
		return false;
	}

	// Terminate Lanwork Threads
	for (int i = 0; i < _numOfThreads; i++)
		PostQueuedCompletionStatus(_hNetworkCP, 0, 0, 0);
	WaitForMultipleObjects(_numOfThreads, _networkThreads, true, INFINITE);

	// Terminate Accept Thread
	SOCKADDR_IN serveraddr;
	ZeroMemory(&serveraddr, sizeof(serveraddr));
	InetPton(AF_INET, L"127.0.0.1", &serveraddr.sin_addr);
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(_port);

	// Awake Accept Thread if It's blocked
	SOCKET socktmp = socket(AF_INET, SOCK_STREAM, 0);
	if (socktmp == INVALID_SOCKET)
	{
		int err = ::WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Socket for Wake is INVALID, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_TEMPSOCK_INVALID, stErrMsg);
		return false;
	}

	int connectRet = connect(socktmp, (SOCKADDR*)&serveraddr, sizeof(serveraddr));
	if (connectRet == SOCKET_ERROR)
	{
		int err = ::WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Socket for Wake Connect Error, %d\n", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_TEMPSOCK_CONNECT, stErrMsg);
		return false;
	}
	WaitForSingleObject(_acceptThread, INFINITE);

	// Release All Session
	closesocket(socktmp);
	closesocket(_listenSock);
	for (int i = 0; i < dfSESSION_MAX; i++)
	{
		CLanSession* pSession = _sessions[i];
		closesocket(pSession->_sock);
		delete pSession;
	}

	WSACleanup();
	CloseHandle(_hNetworkCP);
	CloseHandle(_acceptThread);
	for (int i = 0; i < _numOfThreads; i++)
		CloseHandle(_networkThreads[i]);
	delete[] _networkThreads;

	OnTerminate();
}

bool CLanServer::Disconnect(unsigned __int64 sessionID)
{
	CLanSession* pSession = AcquireSessionUsage(sessionID);
	if (pSession == nullptr) return false;

	// ::printf("%d: Disconnect (%016llx - %016llx)\n", GetCurrentThreadId(), sessionID, pSession->GetID());

	pSession->_disconnect = true;
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_recvComplOvl);
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_sendComplOvl);
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_recvPostOvl);
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_sendPostOvl);

	ReleaseSessionUsage(pSession);
	return true;
}

bool CLanServer::SendPacket(unsigned __int64 sessionID, CLanPacket* packet)
{
	CLanSession* pSession = AcquireSessionUsage(sessionID);
	if (pSession == nullptr) return false;

	// ::printf("%d: Send LanPacket (%016llx - %016llx)\n", GetCurrentThreadId(), sessionID, pSession->GetID());

	if (packet->IsHeaderEmpty())
	{
		short payloadSize = packet->GetPayloadSize();
		stLanHeader header;
		header._len = payloadSize;

		int putRet = packet->PutHeaderData((char*)&header, dfLANHEADER_LEN);
		if (putRet != dfLANHEADER_LEN)
		{
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: CLanPacket PutHeaderData Error", _T(__FUNCTION__), __LINE__);
			OnError(ERR_PACKET_PUT_HEADER, stErrMsg);
			ReleaseSessionUsage(pSession);
			return false;
		}
	}

	pSession->_sendBuf.Enqueue(packet);
	if (SendCheck(pSession))
	{
		// SendPost(pSession);
		PostQueuedCompletionStatus(_hNetworkCP, 1, (ULONG_PTR)pSession->GetID(), (LPOVERLAPPED)&pSession->_sendPostOvl);
	}

	ReleaseSessionUsage(pSession);
	return true;
}

void CLanServer::EnqueueJob(unsigned __int64 sessionID, CLanJob* job)
{
	unsigned __int64 sessionIndex = sessionID & _indexMask;
	long idx = (sessionIndex >> __ID_BIT__) % dfJOB_QUEUE_CNT;
	_pJobQueues[idx]->Enqueue(job);
}

CLanJob* CLanServer::DequeueJob(unsigned __int64 sessionID)
{
	unsigned __int64 sessionIndex = sessionID & _indexMask;
	long idx = (sessionIndex >> __ID_BIT__) % dfJOB_QUEUE_CNT;
	if (_pJobQueues[idx]->GetUseSize() == 0) return nullptr;
	return _pJobQueues[idx]->Dequeue();
}

unsigned int __stdcall CLanServer::AcceptThread(void* arg)
{
	CLanServer* pLanServer = (CLanServer*)arg;
	SOCKADDR_IN clientaddr;
	int addrlen = sizeof(clientaddr);

	for (;;)
	{
		// Accept
		SOCKET client_sock = accept(pLanServer->_listenSock, (SOCKADDR*)&clientaddr, &addrlen);
		if (client_sock == INVALID_SOCKET)
		{
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Accept Error", _T(__FUNCTION__), __LINE__);
			pLanServer->OnError(ERR_ACCEPT, stErrMsg);
			break;
		}
		if (pLanServer->_networkAlive == 1) break;

		InterlockedIncrement(&pLanServer->_acceptCnt);
		long sessionCnt = InterlockedIncrement(&pLanServer->_sessionCnt);

		if (sessionCnt > dfSESSION_MAX)
		{
			closesocket(client_sock);
			InterlockedIncrement(&pLanServer->_disconnectCnt);
			InterlockedDecrement(&pLanServer->_sessionCnt);

			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Session Max", _T(__FUNCTION__), __LINE__);
			pLanServer->OnError(DEB_SESSION_MAX, stErrMsg);
			continue;
		}

		if (pLanServer->_emptyIdx.GetUseSize() == 0)
		{
			closesocket(client_sock);
			InterlockedIncrement(&pLanServer->_disconnectCnt);
			InterlockedDecrement(&pLanServer->_sessionCnt);

			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: No Empty Index", _T(__FUNCTION__), __LINE__);
			pLanServer->OnError(DEB_SESSION_MAX, stErrMsg);
			continue;
		}

		unsigned __int64 sessionID = InterlockedIncrement64(&pLanServer->_sessionID);
		sessionID &= pLanServer->_idMask;
		long idx = pLanServer->_emptyIdx.Pop();
		unsigned __int64 sessionIdx = ((unsigned __int64)idx) << __ID_BIT__;
		sessionID |= sessionIdx;
		CLanSession* pSession = pLanServer->_sessions[(long)idx];

		pLanServer->IncrementUseCount(pSession);
		pSession->Initialize(sessionID, client_sock, clientaddr);

		// ::printf("%d: Accept Success (%016llx - %016llx)\n", GetCurrentThreadId(), sessionID, pSession->GetID());

		CreateIoCompletionPort((HANDLE)pSession->_sock, pLanServer->_hNetworkCP, (ULONG_PTR)pSession->GetID(), 0);
		PostQueuedCompletionStatus(pLanServer->_hNetworkCP, 1, (ULONG_PTR)pSession->GetID(), (LPOVERLAPPED)&pSession->_recvPostOvl);
		pLanServer->OnAcceptClient(sessionID);

	}

	wchar_t stErrMsg[dfERR_MAX];
	swprintf_s(stErrMsg, dfERR_MAX, L"Accept Thread (%d)", GetCurrentThreadId());
	pLanServer->OnThreadTerminate(stErrMsg);

	return 0;
}

unsigned int __stdcall CLanServer::NetworkThread(void* arg)
{
	CLanServer* pLanServer = (CLanServer*)arg;
	int threadID = GetCurrentThreadId();
	NetworkOverlapped* pNetOvl = new NetworkOverlapped;

	for (;;)
	{
		__int64 sessionID;
		DWORD cbTransferred;

		int GQCSRet = GetQueuedCompletionStatus(pLanServer->_hNetworkCP,
			&cbTransferred, (PULONG_PTR)&sessionID, (LPOVERLAPPED*)&pNetOvl, INFINITE);

		if (pLanServer->_networkAlive == 1) break;
		if (pNetOvl->_type == NET_TYPE::RELEASE)
		{
			pLanServer->HandleRelease(sessionID);
			continue;
		}

		CLanSession* pSession = pLanServer->AcquireSessionUsage(sessionID);
		if (pSession == nullptr)
		{
			continue;
		}

		if (GQCSRet == 0 || cbTransferred == 0)
		{
			if (GQCSRet == 0)
			{
				int err = GetLastError();
				if (err != WSAECONNRESET && err != WSAECONNABORTED && err != WSAENOTSOCK && err != WSAEINTR &&
					err != ERROR_OPERATION_ABORTED && err != ERROR_SEM_TIMEOUT && err != ERROR_SUCCESS &&
					err != ERROR_CONNECTION_ABORTED && err != ERROR_NETNAME_DELETED)
				{
					wchar_t stErrMsg[dfERR_MAX];
					swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: GQCS return 0, %d", _T(__FUNCTION__), __LINE__, err);
					pLanServer->OnError(ERR_GQCS_RET0, stErrMsg);
				}
			}

		}
		else
		{
			switch (pNetOvl->_type)
			{
			case NET_TYPE::RECV_COMPLETE:
				// ::printf("RECV_COMPLETE\n");
				pLanServer->HandleRecvCP(pSession, cbTransferred);
				break;

			case NET_TYPE::SEND_COMPLETE:
				// ::printf("SEND_COMPLETE\n");
				pLanServer->HandleSendCP(pSession, cbTransferred);
				break;

			case NET_TYPE::RECV_POST:
				// ::printf("RECV_POST\n");
				pLanServer->RecvPost(pSession);
				break;

			case NET_TYPE::SEND_POST:
				// ::printf("SEND_POST\n");
				pLanServer->SendPost(pSession);
				pLanServer->IncrementUseCount(pSession);
				break;
			}
		}

		pLanServer->DecrementUseCount(pSession);
		pLanServer->ReleaseSessionUsage(pSession);
	}

	delete pNetOvl;
	wchar_t stErrMsg[dfERR_MAX];
	swprintf_s(stErrMsg, dfERR_MAX, L"Lanwork Thread (%d)", threadID);
	pLanServer->OnThreadTerminate(stErrMsg);

	return 0;
}


bool CLanServer::HandleRecvCP(CLanSession* pSession, int recvBytes)
{
	CLanPacket* recvBuf = pSession->_recvBuf;
	int moveWriteRet = recvBuf->MovePayloadWritePos(recvBytes);
	if (moveWriteRet != recvBytes)
	{
		Disconnect(pSession->GetID());
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Recv Buffer MoveWritePos Error", _T(__FUNCTION__), __LINE__);
		OnError(ERR_RECVBUF_MOVEWRITEPOS, stErrMsg);
		return false;
	}

	int cnt = 0;
	int useSize = recvBuf->GetPayloadSize();

	while (useSize > dfLANHEADER_LEN)
	{
		// ::printf("%016llx: Payload %d\n", pSession->GetID(), recvBuf->GetPayloadReadPos());

		stLanHeader* header = (stLanHeader*)recvBuf->GetPayloadReadPtr();
		if (dfLANHEADER_LEN + header->_len > useSize) break;

		int moveReadRet1 = recvBuf->MovePayloadReadPos(dfLANHEADER_LEN);
		if (moveReadRet1 != dfLANHEADER_LEN)
		{
			Disconnect(pSession->GetID());
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Recv Buffer MoveReadPos Error", _T(__FUNCTION__), __LINE__);
			OnError(ERR_RECVBUF_MOVEREADPOS, stErrMsg);
			return false;
		}

		// ::printf("%016llx: Header %d\n", pSession->GetID(), recvBuf->GetPayloadReadPos());

		CRecvLanPacket* recvLanPacket = CRecvLanPacket::Alloc(recvBuf);
		recvBuf->AddUsageCount(1);
		OnRecv(pSession->GetID(), recvLanPacket);
		cnt++;

		int moveReadRet2 = recvBuf->MovePayloadReadPos(header->_len);
		if (moveReadRet2 != header->_len)
		{
			Disconnect(pSession->GetID());
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Recv Buffer MoveReadPos Error", _T(__FUNCTION__), __LINE__);
			OnError(ERR_RECVBUF_MOVEREADPOS, stErrMsg);
			return false;
		}

		useSize = recvBuf->GetPayloadSize();
	}

	pSession->_recvBuf = CLanPacket::Alloc();
	pSession->_recvBuf->Clear();
	pSession->_recvBuf->AddUsageCount(1);

	pSession->_recvBuf->CopyRecvBuf(recvBuf);
	CLanPacket::Free(recvBuf);

	InterlockedAdd(&_recvCnt, cnt);
	RecvPost(pSession);
	return true;
}

bool CLanServer::RecvPost(CLanSession* pSession)
{
	DWORD flags = 0;
	DWORD recvBytes = 0;

	pSession->_wsaRecvbuf[0].buf = pSession->_recvBuf->GetPayloadWritePtr();
	pSession->_wsaRecvbuf[0].len = pSession->_recvBuf->GetRemainPayloadSize();

	ZeroMemory(&pSession->_recvComplOvl._ovl, sizeof(pSession->_recvComplOvl._ovl));

	if (pSession->_disconnect)
	{
		return false;
	}

	IncrementUseCount(pSession);
	int recvRet = WSARecv(pSession->_sock, pSession->_wsaRecvbuf,
		dfWSARECVBUF_CNT, &recvBytes, &flags, (LPOVERLAPPED)&pSession->_recvComplOvl, NULL);

	// ::printf("%d: Recv Request (%016llx)\n", GetCurrentThreadId(), pSession->GetID());

	if (recvRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err != ERROR_IO_PENDING)
		{
			if (err != WSAECONNRESET && err != WSAECONNABORTED && err != WSAEINTR)
			{
				wchar_t stErrMsg[dfERR_MAX];
				swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Recv Error, %d", _T(__FUNCTION__), __LINE__, err);
				OnError(ERR_RECV, stErrMsg);
			}
			DecrementUseCount(pSession);
			return false;
		}
		else if (pSession->_disconnect)
		{
			CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_recvComplOvl);
			return false;
		}
	}

	return true;
}

bool CLanServer::HandleSendCP(CLanSession* pSession, int sendBytes)
{
	for (int i = 0; i < pSession->_sendCount; i++)
	{
		CLanPacket* packet = pSession->_tempBuf.Dequeue();
		if (packet == nullptr) break;
		CLanPacket::Free(packet);
	}

	OnSend(pSession->GetID(), sendBytes);
	InterlockedExchange(&pSession->_sendFlag, 0);
	if (SendCheck(pSession)) SendPost(pSession);
	return true;
}

bool CLanServer::SendCheck(CLanSession* pSession)
{
	if (pSession->_sendBuf.GetUseSize() == 0) return false;
	if (InterlockedExchange(&pSession->_sendFlag, 1) == 1) return false;
	if (pSession->_sendBuf.GetUseSize() == 0)
	{
		InterlockedExchange(&pSession->_sendFlag, 0);
		return false;
	}
	return true;
}

bool CLanServer::SendPost(CLanSession* pSession)
{
	int idx = 0;
	int useSize = pSession->_sendBuf.GetUseSize();

	for (; idx < useSize; idx++)
	{
		if (idx == dfWSASENDBUF_CNT) break;
		CLanPacket* packet = pSession->_sendBuf.Dequeue();
		if (packet == nullptr) break;

		pSession->_wsaSendbuf[idx].buf = packet->GetLanPacketReadPtr();
		pSession->_wsaSendbuf[idx].len = packet->GetLanPacketSize();
		pSession->_tempBuf.Enqueue(packet);
	}
	pSession->_sendCount = idx;

	DWORD sendBytes;
	ZeroMemory(&pSession->_sendComplOvl._ovl, sizeof(pSession->_sendComplOvl._ovl));

	if (pSession->_disconnect)
	{
		InterlockedExchange(&pSession->_sendFlag, 0);
		return false;
	}

	InterlockedAdd(&_sendCnt, idx);
	IncrementUseCount(pSession);
	int sendRet = WSASend(pSession->_sock, pSession->_wsaSendbuf,
		idx, &sendBytes, 0, (LPOVERLAPPED)&pSession->_sendComplOvl, NULL);

	// ::printf("%d: Send Request (%016llx)\n", GetCurrentThreadId(), pSession->GetID());

	if (sendRet == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err != ERROR_IO_PENDING)
		{
			if (err != WSAECONNRESET && err != WSAECONNABORTED && err != WSAEINTR)
			{
				wchar_t stErrMsg[dfERR_MAX];
				swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Send Error, %d", _T(__FUNCTION__), __LINE__, err);
				OnError(ERR_SEND, stErrMsg);
			}
			InterlockedExchange(&pSession->_sendFlag, 0);
			DecrementUseCount(pSession);
			return false;
		}
		else if (pSession->_disconnect)
		{
			CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_sendComplOvl);
			InterlockedExchange(&pSession->_sendFlag, 0);
			return false;
		}
	}

	return true;
}

CLanSession* CLanServer::AcquireSessionUsage(unsigned __int64 sessionID)
{
	if (sessionID == -1)
	{
		return nullptr;
	}

	unsigned __int64 idx = sessionID & _indexMask;
	idx >>= __ID_BIT__;
	CLanSession* pSession = _sessions[(long)idx];

	IncrementUseCount(pSession);

	if (pSession->_validFlag._releaseFlag == 1)
	{
		DecrementUseCount(pSession);
		return nullptr;
	}

	if (pSession->GetID() != sessionID)
	{
		DecrementUseCount(pSession);
		return nullptr;
	}

	return pSession;

}

void CLanServer::ReleaseSessionUsage(CLanSession* pSession)
{
	DecrementUseCount(pSession);
}

void CLanServer::IncrementUseCount(CLanSession* pSession)
{
	InterlockedIncrement16(&pSession->_validFlag._useCount);
}

void CLanServer::DecrementUseCount(CLanSession* pSession)
{
	short ret = InterlockedDecrement16(&pSession->_validFlag._useCount);
	if (ret == 0)
	{
		if (InterlockedCompareExchange(&pSession->_validFlag._flag, _releaseFlag._flag, 0) == 0)
		{
			PostQueuedCompletionStatus(_hNetworkCP, 1, (ULONG_PTR)pSession->GetID(), (LPOVERLAPPED)&pSession->_releaseOvl);
			return;
		}
	}
}

void CLanServer::HandleRelease(unsigned __int64 sessionID)
{
	unsigned __int64 idx = sessionID & _indexMask;
	idx >>= __ID_BIT__;
	CLanSession* pSession = _sessions[(long)idx];

	SOCKET sock = pSession->_sock;
	pSession->Terminate();
	closesocket(sock);
	_emptyIdx.Push(idx);

	InterlockedIncrement(&_disconnectCnt);
	InterlockedDecrement(&_sessionCnt);

	OnReleaseClient(sessionID);
}

#endif

