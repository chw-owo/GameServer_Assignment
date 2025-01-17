#pragma once
#include "DebugQ.h"
#include <windows.h>

template<typename T>
class LockFreeStack
{
private:
	struct Node
	{
		T _data;
		Node* _next;
	};

private:
	Node* _pTop = nullptr;

public:
	void Push(T data)
	{
		if (_debugQManager.GetStop()) return;
		Node* pNew = new Node;
		pNew->_data = data;

		for(;;)
		{
			Node* pTopLocal = _pTop;
			pNew->_next = pTopLocal;

			if (_debugQManager.GetStop()) return;
			if (InterlockedCompareExchange64((LONG64*)&_pTop, (LONG64)pNew, (LONG64)pTopLocal))
			{
				_debugQManager.EnqueueLog(__LINE__, ((nanoseconds)(system_clock::now() - g_Start)).count());
				break;
			}
		} 
	}

	void Pop()
	{
		if (_debugQManager.GetStop()) return;

		for(;;)
		{
			Node* pTopLocal = _pTop;
			Node* pPop = pTopLocal;
			Node* pNext = pPop->_next;

			if (_debugQManager.GetStop()) return;
			if (InterlockedCompareExchange64((LONG64*)&_pTop, (LONG64)pNext, (LONG64)pTopLocal))
			{
				_debugQManager.EnqueueLog(__LINE__, ((nanoseconds)(system_clock::now() - g_Start)).count());
				delete pPop;	
				_debugQManager.EnqueueLog(__LINE__, ((nanoseconds)(system_clock::now() - g_Start)).count());
				break;
			}
		} 
	}
};



