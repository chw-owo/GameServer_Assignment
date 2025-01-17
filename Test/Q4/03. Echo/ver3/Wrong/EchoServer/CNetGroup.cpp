#include "CNetGroup.h"
#include "CNetServer.h"
#include <tchar.h>

bool CNetGroup::Disconnect(unsigned __int64 sessionID)
{
	return _pNet->Disconnect(sessionID);
}

bool CNetGroup::SendPacket(unsigned __int64 sessionID, CNetSendPacket* packet, bool disconnect)
{
	return _pNet->SendPacket(sessionID, packet, disconnect);
}

void CNetGroup::MoveGroup(unsigned __int64 sessionID, CNetGroup* pGroup)
{
	_pNet->MoveGroup(sessionID, pGroup);
}

unsigned int __stdcall CNetGroup::UpdateThread(void* arg)
{
	CNetGroup* pGroup = (CNetGroup*)arg;
	pGroup->Initialize();
	pGroup->OnInitialize();

	if (pGroup->_fps > 0)
	{
		while (pGroup->GetAlive())
		{
			pGroup->NetworkUpdate();
			if (!pGroup->SkipForFixedFrame())
			{
				pGroup->Update();
				pGroup->OnUpdate();
			}
		}
	}
	else
	{
		while (pGroup->GetAlive())
		{
			pGroup->NetworkUpdate();
		}
	}

	pGroup->RemoveAllSessions();
	pGroup->Terminate();
	pGroup->OnTerminate();
	return 0;
}

bool CNetGroup::SkipForFixedFrame()
{
	static DWORD oldTick = GetTickCount64();
	if ((GetTickCount64() - oldTick) < (1000 / _fps))
		return true;
	oldTick += (1000 / _fps);
	return false;
}

void CNetGroup::NetworkUpdate()
{
	if (_fps == 0)
		WaitOnAddress(&_signal, &_undesired, sizeof(long), INFINITE);
	else
		WaitOnAddress(&_signal, &_undesired, sizeof(long), (1000 / _fps));

	while (_enterSessions.GetUseSize() > 0)
	{
		unsigned __int64 sessionID = _enterSessions.Dequeue();
		_sessions.push_back(sessionID);
		OnEnterGroup(sessionID);
		_enterCnt++;
		InterlockedDecrement(&_signal);
	}

	vector<unsigned __int64>::iterator it = _sessions.begin();
	for (; it != _sessions.end();)
	{
		unsigned __int64 sessionID = *it;
		CNetSession* pSession = _pNet->AcquireSessionUsage(sessionID);
		if (pSession == nullptr)
		{
			OnLeaveGroup(sessionID);
			it = _sessions.erase(it);
			_leaveCnt++;
			InterlockedDecrement(&_signal);
			continue;
		}

		EnterCriticalSection(&pSession->_groupLock);
		if (pSession->_pGroup != this)
		{
			OnLeaveGroup(sessionID);
			it = _sessions.erase(it);
			_leaveCnt++;
			InterlockedDecrement(&_signal);
		}
		else
		{
			while (pSession->_OnRecvQ->GetUseSize() > 0)
			{
				if (pSession->_pGroup != this) break;
				CNetMsg* msg;
				pSession->_OnRecvQ->Dequeue((char*)&msg, sizeof(msg));
				OnRecv(sessionID, msg);
				InterlockedDecrement(&_signal);
			}
			it++;
		}
		LeaveCriticalSection(&pSession->_groupLock);
		_pNet->ReleaseSessionUsage(pSession);
	}
	
	for (int i = 0; i < dfONSENDQ_CNT; i++)
	{
		while (_OnSendQs[i]->GetUseSize() > 0)
		{
			OnSend(_OnSendQs[i]->Dequeue());
			InterlockedDecrement(&_signal);
		}
	}
}

void CNetGroup::RemoveAllSessions()
{
	vector<unsigned __int64>::iterator it = _sessions.begin();
	for (; it != _sessions.end(); it++)
	{
		MoveGroup((*it), nullptr);
		InterlockedIncrement(&_leaveCnt);
	}
}

void CNetGroup::EnqueueOnSendQ(unsigned __int64 sessionID)
{
	unsigned __int64 sessionIdx = sessionID & _pNet->_indexMask;
	long idx = (sessionIdx >> __ID_BIT__) % dfONSENDQ_CNT;
	_OnSendQs[idx]->Enqueue(sessionID);
}
