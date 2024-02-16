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
	_tlsIdx = TlsAlloc();
}

bool CLanServer::NetworkInitialize(const wchar_t* IP, short port, long sendTime, int numOfThreads, int numOfRunnings, bool nagle, bool monitorServer)
{
	// Option Setting ====================================================

	wcscpy_s(_IP, 10, IP);
	_port = port;
	_sendTime = sendTime;
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
	_pReleaseQ = new CLockFreeQueue<unsigned __int64>;

	// Network Setting ===================================================

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

	// Create IOCP for Network
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

	// Create Network Thread
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

	// Create Release Thread
	_releaseThread = (HANDLE)_beginthreadex(NULL, 0, ReleaseThread, this, 0, nullptr);
	if (_releaseThread == NULL)
	{
		int err = WSAGetLastError();
		wchar_t stErrMsg[dfERR_MAX];
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Create Release Thread Error, %d", _T(__FUNCTION__), __LINE__, err);
		OnError(ERR_CREATE_RELEASE_THREAD, stErrMsg);
		return false;
	}

	if (_sendTime != 0)
	{
		_sendThread = (HANDLE)_beginthreadex(NULL, 0, SendThread, this, 0, nullptr);
		if (_sendThread == NULL)
		{
			int err = WSAGetLastError();
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Create Send Thread Error, %d", _T(__FUNCTION__), __LINE__, err);
			OnError(ERR_CREATE_SEND_THREAD, stErrMsg);
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

	// Terminate Network Threads
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
		swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Socket for Wake Connect Error, %d", _T(__FUNCTION__), __LINE__, err);
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

	// ::printf("Disconnect\n");

	pSession->_disconnect = true;
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_recvComplOvl);
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_sendComplOvl);
	CancelIoEx((HANDLE)pSession->_sock, (LPOVERLAPPED)&pSession->_sendPostOvl);

	ReleaseSessionUsage(pSession);
	return true;
}

bool CLanServer::SendPacket(unsigned __int64 sessionID, CLanSendPacket* packet)
{
	// ::printf("Send Packet\n");

	CLanSession* pSession = AcquireSessionUsage(sessionID);
	if (pSession == nullptr) return false;

	if (packet->IsHeaderEmpty())
	{
		short payloadSize = packet->GetPayloadSize();
		stLanHeader header;
		header._len = payloadSize;

		int putRet = packet->PutHeaderData((char*)&header, dfLANHEADER_LEN);
		if (putRet != dfLANHEADER_LEN)
		{
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: CNetPacket PutHeaderData Error", _T(__FUNCTION__), __LINE__);
			OnError(ERR_PACKET_PUT_HEADER, stErrMsg);
			ReleaseSessionUsage(pSession);
			return false;
		}
	}

	pSession->_sendBuf.Enqueue(packet);
	if (_sendTime == 0 && SendCheck(pSession))
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
	CLanServer* pNetServer = (CLanServer*)arg;
	SOCKADDR_IN clientaddr;
	int addrlen = sizeof(clientaddr);

	int idx = (int)TlsGetValue(pNetServer->_tlsIdx);
	if (idx == 0)
	{
		idx = InterlockedIncrement(&pNetServer->_tlsMax);
		bool ret = TlsSetValue(pNetServer->_tlsIdx, (LPVOID)idx);
		if (ret == 0)
		{
			int err = GetLastError();
			// ::printf("%d\n", err);
			__debugbreak();
		}
	}

	for (;;)
	{
		// Accept
		SOCKET client_sock = accept(pNetServer->_listenSock, (SOCKADDR*)&clientaddr, &addrlen);
		if (client_sock == INVALID_SOCKET)
		{
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Accept Error", _T(__FUNCTION__), __LINE__);
			pNetServer->OnError(ERR_ACCEPT, stErrMsg);
			break;
		}
		if (pNetServer->_networkAlive == 1) break;

		pNetServer->_acceptCnts[idx]++;
		long sessionCnt = InterlockedIncrement(&pNetServer->_sessionCnt);

		if (sessionCnt > dfSESSION_MAX)
		{
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Session Max", _T(__FUNCTION__), __LINE__);
			pNetServer->OnError(DEB_SESSION_MAX, stErrMsg);

			closesocket(client_sock);
			pNetServer->_disconnectCnts[idx]++;
			InterlockedDecrement(&pNetServer->_sessionCnt);
			continue;
		}

		if (pNetServer->_emptyIdx.GetUseSize() == 0)
		{
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: No Empty Index", _T(__FUNCTION__), __LINE__);
			pNetServer->OnError(DEB_SESSION_MAX, stErrMsg);

			closesocket(client_sock);
			pNetServer->_disconnectCnts[idx]++;
			InterlockedDecrement(&pNetServer->_sessionCnt);
			continue;
		}

		WCHAR addr[dfADDRESS_LEN] = { L'0', };
		DWORD size = dfADDRESS_LEN;
		WSAAddressToStringW((SOCKADDR*)&clientaddr, sizeof(clientaddr), NULL, addr, &size);

		unsigned __int64 sessionID = InterlockedIncrement64(&pNetServer->_sessionID);
		sessionID &= pNetServer->_idMask;
		long idx = pNetServer->_emptyIdx.Pop();
		unsigned __int64 sessionIdx = ((unsigned __int64)idx) << __ID_BIT__;
		sessionID |= sessionIdx;
		CLanSession* pSession = pNetServer->_sessions[(long)idx];

		pNetServer->IncrementUseCount(pSession);
		pSession->Initialize(sessionID, client_sock, clientaddr);

		// ::printf("Accept: %d\n", pSession->_validFlag._useCount);

		CreateIoCompletionPort((HANDLE)pSession->_sock, pNetServer->_hNetworkCP, (ULONG_PTR)pSession->GetID(), 0);
		pNetServer->RecvPost(pSession);
		pNetServer->OnAcceptClient(sessionID, addr);
		// ::printf("%d", __LINE__);
		pNetServer->DecrementUseCount(pSession);
	}

	wchar_t stErrMsg[dfERR_MAX];
	swprintf_s(stErrMsg, dfERR_MAX, L"Accept Thread (%d)", GetCurrentThreadId());
	pNetServer->OnThreadTerminate(stErrMsg);

	return 0;
}

unsigned int __stdcall CLanServer::NetworkThread(void* arg)
{
	CLanServer* pNetServer = (CLanServer*)arg;
	int threadID = GetCurrentThreadId();
	NetworkOverlapped* pNetOvl = new NetworkOverlapped;

	long idx = InterlockedIncrement(&pNetServer->_Idx);

	for (;;)
	{
		__int64 sessionID;
		DWORD cbTransferred;

		int GQCSRet = GetQueuedCompletionStatus(pNetServer->_hNetworkCP,
			&cbTransferred, (PULONG_PTR)&sessionID, (LPOVERLAPPED*)&pNetOvl, INFINITE);

		if (pNetServer->_networkAlive == 1) break;
		
		// ::printf("Get IOCP\n");
		CLanSession* pSession = pNetServer->AcquireSessionUsage(sessionID);
		if (pSession == nullptr) continue;

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
					pNetServer->OnError(ERR_GQCS_RET0, stErrMsg);
				}
			}

			// ::printf("IO Fail\n");
			// ::printf("%d", __LINE__);
			pNetServer->DecrementUseCount(pSession);
		}
		else
		{
			switch (pNetOvl->_type)
			{
			case NET_TYPE::RECV_COMPLETE:
				pNetServer->HandleRecvCP(pSession, cbTransferred);
				// ::printf("Recv Success\n");
				// ::printf("%d", __LINE__);
				pNetServer->DecrementUseCount(pSession);
				break;

			case NET_TYPE::SEND_COMPLETE:
				pNetServer->HandleSendCP(pSession, cbTransferred);
				// ::printf("Send Success\n");
				// ::printf("%d", __LINE__);
				pNetServer->DecrementUseCount(pSession);
				break;

			case NET_TYPE::SEND_POST:
				pNetServer->SendPost(pSession);
				break;

			}
		}

		pNetServer->ReleaseSessionUsage(pSession);
	}

	delete pNetOvl;
	wchar_t stErrMsg[dfERR_MAX];
	swprintf_s(stErrMsg, dfERR_MAX, L"Network Thread (%d)", threadID);
	pNetServer->OnThreadTerminate(stErrMsg);

	return 0;
}

unsigned int __stdcall CLanServer::SendThread(void* arg)
{
	CLanServer* pNetServer = (CLanServer*)arg;
	pNetServer->_oldTick = timeGetTime();

	for (;;)
	{
		pNetServer->SleepForFixedSend();

		for (int i = 0; i < dfSESSION_MAX; i++)
		{
			unsigned __int64 sessionID = pNetServer->_sessions[i]->GetID();
			CLanSession* pSession = pNetServer->AcquireSessionUsage(sessionID);
			if (pSession == nullptr) continue;

			if (pNetServer->SendCheck(pSession))
			{
				pNetServer->SendPost(pSession);
			}
			pNetServer->ReleaseSessionUsage(pSession);
		}
	}

	wchar_t stErrMsg[dfERR_MAX];
	swprintf_s(stErrMsg, dfERR_MAX, L"Send Thread");
	pNetServer->OnThreadTerminate(stErrMsg);

	return 0;
}

void CLanServer::SleepForFixedSend()
{
	if ((timeGetTime() - _oldTick) < _sendTime)
		Sleep(_sendTime - (timeGetTime() - _oldTick));
	_oldTick += _sendTime;
}

unsigned int __stdcall CLanServer::ReleaseThread(void* arg)
{
	CLanServer* pNetServer = (CLanServer*)arg;
	long undesired = 0;

	int idx = (int)TlsGetValue(pNetServer->_tlsIdx);
	if (idx == 0)
	{
		idx = InterlockedIncrement(&pNetServer->_tlsMax);
		bool ret = TlsSetValue(pNetServer->_tlsIdx, (LPVOID)idx);
		if (ret == 0)
		{
			int err = GetLastError();
			// ::printf("%d\n", err);
			__debugbreak();
		}
	}

	for (;;)
	{
		WaitOnAddress(&pNetServer->_releaseSignal, &undesired, sizeof(long), INFINITE);
		if (pNetServer->_networkAlive == 1) break;

		while (pNetServer->_pReleaseQ->GetUseSize() > 0)
		{
			unsigned __int64 sessionID = pNetServer->_pReleaseQ->Dequeue();
			if (sessionID == 0) break;

			// ::printf("Release\n");

			unsigned __int64 sessionIdx = sessionID & pNetServer->_indexMask;
			sessionIdx >>= __ID_BIT__;
			CLanSession* pSession = pNetServer->_sessions[(long)sessionIdx];
			if (pSession == nullptr) continue;

			SOCKET sock = pSession->_sock;
			pSession->Terminate();
			closesocket(sock);
			pNetServer->_emptyIdx.Push(sessionIdx);

			pNetServer->_disconnectCnts[idx]++;
			InterlockedDecrement(&pNetServer->_sessionCnt);

			pNetServer->OnReleaseClient(sessionID);
			InterlockedDecrement(&pNetServer->_releaseSignal);
		}
	}

	return 0;
}

bool CLanServer::HandleRecvCP(CLanSession* pSession, int recvBytes)
{
	CLanRecvPacket* recvBuf = pSession->_recvBuf;
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
		stLanHeader* header = (stLanHeader*)recvBuf->GetPayloadReadPtr();

		if (dfLANHEADER_LEN + header->_len > useSize) break;

		int moveReadRet1 = recvBuf->MovePayloadReadPos(dfLANHEADER_LEN);
		if (moveReadRet1 != dfLANHEADER_LEN)
		{
			Disconnect(pSession->GetID());
			wchar_t stErrMsg[dfERR_MAX];
			swprintf_s(stErrMsg, dfERR_MAX, L"%s[%d]: Recv Buffer MoveReadPos Error\n", _T(__FUNCTION__), __LINE__);
			OnError(ERR_RECVBUF_MOVEREADPOS, stErrMsg);
			return false;
		}

		CLanMsg* recvMsg = CLanMsg::Alloc(recvBuf, header->_len);
		recvBuf->AddUsageCount(1);
		OnRecv(pSession->GetID(), recvMsg);
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

	pSession->_recvBuf = CLanRecvPacket::Alloc();
	pSession->_recvBuf->AddUsageCount(1);
	pSession->_recvBuf->CopyRecvBuf(recvBuf);
	CLanRecvPacket::Free(recvBuf);

	int idx = (int)TlsGetValue(_tlsIdx);
	if (idx == 0)
	{
		idx = InterlockedIncrement(&_tlsMax);
		bool ret = TlsSetValue(_tlsIdx, (LPVOID)idx);
		if (ret == 0)
		{
			int err = GetLastError();
			// ::printf("%d\n", err);
			__debugbreak();
		}
	}

	_recvCnts[idx] += cnt;
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

	// ::printf("Recv Request: %d\n", pSession->_validFlag._useCount);

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
			// ::printf("Recv Fail\n");
			// ::printf("%d ", __LINE__);
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
		CLanSendPacket* packet = pSession->_tempBuf.Dequeue();
		if (packet == nullptr) break;
		CLanSendPacket::Free(packet);
	}

	OnSend(pSession->GetID(), sendBytes);
	InterlockedExchange(&pSession->_sendFlag, 0);
	if (_sendTime == 0 && SendCheck(pSession))
	{
		SendPost(pSession);
	}

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
	int cnt = 0;
	int useSize = pSession->_sendBuf.GetUseSize();

	for (; cnt < useSize; cnt++)
	{
		if (cnt == dfWSASENDBUF_CNT) break;
		CLanSendPacket* packet = pSession->_sendBuf.Dequeue();
		if (packet == nullptr) break;

		pSession->_wsaSendbuf[cnt].buf = packet->GetLanPacketReadPtr();
		pSession->_wsaSendbuf[cnt].len = packet->GetLanPacketSize();
		pSession->_tempBuf.Enqueue(packet);
	}
	pSession->_sendCount = cnt;

	DWORD sendBytes;
	ZeroMemory(&pSession->_sendComplOvl._ovl, sizeof(pSession->_sendComplOvl._ovl));

	if (pSession->_disconnect)
	{
		InterlockedExchange(&pSession->_sendFlag, 0);
		return false;
	}

	int idx = (int)TlsGetValue(_tlsIdx);
	if (idx == 0)
	{
		idx = InterlockedIncrement(&_tlsMax);
		bool ret = TlsSetValue(_tlsIdx, (LPVOID)idx);
		if (ret == 0)
		{
			int err = GetLastError();
			// ::printf("%d\n", err);
			__debugbreak();
		}
	}

	_sendCnts[idx] += cnt;

	IncrementUseCount(pSession);
	int sendRet = WSASend(pSession->_sock, pSession->_wsaSendbuf,
		cnt, &sendBytes, 0, (LPOVERLAPPED)&pSession->_sendComplOvl, NULL);

	// ::printf("Send Request\n");

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
			// ::printf("%d ", __LINE__);
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
	if (sessionID == MAXULONGLONG)
	{
		return nullptr;
	}

	unsigned __int64 idx = sessionID & _indexMask;
	idx >>= __ID_BIT__;
	CLanSession* pSession = _sessions[(long)idx];

	IncrementUseCount(pSession);

	if (pSession->_validFlag._releaseFlag == 1)
	{
		// ::printf("%d ", __LINE__);
		DecrementUseCount(pSession);
		return nullptr;
	}

	if (pSession->GetID() != sessionID)
	{
		// ::printf("%d ", __LINE__);
		DecrementUseCount(pSession);
		return nullptr;
	}

	return pSession;

}

void CLanServer::ReleaseSessionUsage(CLanSession* pSession)
{
	// ::printf("%d ", __LINE__);
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
			_pReleaseQ->Enqueue(pSession->GetID());
			InterlockedIncrement(&_releaseSignal);
			WakeByAddressSingle(&_releaseSignal);
			return;
		}
	}
}


#endif
