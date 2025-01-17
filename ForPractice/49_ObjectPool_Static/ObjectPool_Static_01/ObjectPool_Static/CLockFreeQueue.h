#pragma once
#include "CTlsPool.h"

#define __USESIZE_64BIT__ 47
unsigned __int64 _addressMask = 0;
unsigned int _keyMask = 0;
volatile __int64 _key = 0;

template <typename T>
class CLockFreeQueue
{
private: 
    // For protect ABA

public:
    CLockFreeQueue()
    { 
        _QID = InterlockedIncrement(&_IDSupplier);
        
        _keyMask = 0b11111111111111111;
        _addressMask = 0b11111111111111111;
        _addressMask <<= __USESIZE_64BIT__;
        _addressMask = ~_addressMask;

        int idx = 0;
        QueueNode* node = _pPool->Alloc(1, _queueDebugIdx, _QID, _QID, this, idx);
        node->_next = 0;
        node->_ID = _QID;

        //For Protect ABA 
        unsigned __int64 ret = (unsigned __int64)InterlockedIncrement64(&_key);
        unsigned __int64 key = ret;
        key <<= __USESIZE_64BIT__;
        __int64 pNode = (__int64)node;
        pNode &= _addressMask;
        pNode |= key;

        _head = pNode;
        _tail = _head;
        _size = 0;
    }

public:
    class QueueNode
    {
        friend CLockFreeQueue;

    private:
        T _data = 0;
        __int64 _next = 0;
        long _ID = 0;

    public: 
        struct NodeDebugData
        {
        public:
            NodeDebugData() : _idx(-1), _threadID(-1), _line(-1), _ID(-1), _data(-1), _bucketId(-1),
                _compKey(-1), _exchKey(-1), _realKey(-1), _compAddress(-1), _exchAddress(-1), _realAddress(-1) {}

            void SetData(int idx, int threadID, int line, int ID, int data, int bucketId,
                int exchKey, __int64 exchAddress, int compKey, __int64 compAddress, int realKey, __int64 realAddress)
            {
                _exchKey = exchKey;
                _exchAddress = exchAddress;

                _compKey = compKey;
                _compAddress = compAddress;

                _realKey = realKey;
                _realAddress = realAddress;

                _idx = idx;
                _threadID = threadID;
                _line = line;
                _ID = ID;
                _data = data;
                _bucketId = bucketId;
            }

        public:
            int _idx;
            int _threadID;
            int _line;
            int _ID;
            int _data;
            int _bucketId;

            int _compKey;
            __int64 _compAddress;
            int _exchKey;
            __int64 _exchAddress;
            int _realKey;
            __int64 _realAddress;
        };

    public:
#define dfNODE_DEBUG_MAX 10000
        inline void LeaveLog(int line, int idx, int ID, int data, int bucketId,
            unsigned __int64 exchange, unsigned __int64 comperand, unsigned __int64 real)
        {
            LONG debugIdx = InterlockedIncrement(&_nodeDebugIdx);

            int exchKey = (exchange >> __USESIZE_64BIT__) & _keyMask;
            int compKey = (comperand >> __USESIZE_64BIT__) & _keyMask;
            int realKey = (real >> __USESIZE_64BIT__) & _keyMask;
            __int64 exchAddress = exchange & _addressMask;
            __int64 compAddress = comperand & _addressMask;
            __int64 realAddress = real & _addressMask;

            _nodeDebugArray[debugIdx % dfNODE_DEBUG_MAX].SetData(idx, GetCurrentThreadId(), line, ID, data, bucketId,
                exchKey, exchAddress, compKey, compAddress, realKey, realAddress);
        }

        NodeDebugData _nodeDebugArray[dfNODE_DEBUG_MAX];
        volatile long _nodeDebugIdx = 0;
    };



private:
    long _QID;

public:
    static long _IDSupplier;
    static CTlsPool<QueueNode>* _pPool;

private:
    __int64 _head;
    __int64 _tail;
    long _size;

public:
    void Enqueue(T data)
    {
        int bucketIdx = 0;
        QueueNode* node = _pPool->Alloc(1, _queueDebugIdx, _QID, _QID, this, bucketIdx);
        int idx = LeaveLog(1, 0, (__int64)node, 0, 0, _QID);
        node->LeaveLog(1, idx, _QID, data, bucketIdx, 0, 0, 0);

        node->_data = data;
        node->_next = NULL;
        node->_ID = _QID;

        unsigned __int64 key = (unsigned __int64)InterlockedIncrement64(&_key);
        key <<= __USESIZE_64BIT__;
        __int64 newNode = (__int64)node;
        newNode &= _addressMask;
        newNode |= key;

        while (true)
        {
            int size = _size;
            __int64 tail = _tail;
            QueueNode* tailNode = (QueueNode*)(tail & _addressMask);
            __int64 next = tailNode->_next;

            if (next != NULL)
            {
                InterlockedCompareExchange64(&_tail, next, tail);
                LeaveLog(2, size, next, tail, 0, ((QueueNode*)(tail & _addressMask))->_data);
            }
            else
            {
                if (InterlockedCompareExchange64(&tailNode->_next, newNode, next) == next)
                {
                    // LeaveLog(1, size, newNode, next, 0, (__int64)data);                    
                    InterlockedCompareExchange64(&_tail, newNode, tail);
                    // LeaveLog(2, size, newNode, tail, 0, (__int64)data);
                    node->LeaveLog(2, _queueDebugIdx, _QID, (__int64)data, 0, newNode, tail, 0);

                    break;
                }
            }
        }

        InterlockedExchangeAdd(&_size, 1);
    }

    T Dequeue()
    {
        T data = 0;
        int size = _size;
        if (size == 0)
        {
            // LeaveLog(3, size, 0, 0, 0, 0);
            return data;
        }

        while (true)
        {
            size = _size;
            __int64 tail = _tail;
            __int64 head = _head;
            QueueNode* headNode = (QueueNode*)(head & _addressMask);
            __int64 next = headNode->_next;

            if (next == NULL)
            {
                // LeaveLog(4, size, 0, 0, 0, 0);
                return data;
            }

            if (head == tail)
            {
                InterlockedCompareExchange64(&_tail, next, tail);
                LeaveLog(5, size, next, tail, 0, ((QueueNode*)(tail & _addressMask))->_data);
            }

            if (InterlockedCompareExchange64(&_head, next, head) == head)
            {
                QueueNode* node = (QueueNode*)(next & _addressMask);
                data = node->_data;
  
                // int idx = LeaveLog(6, size, next, head, 0, data);      
                
                int bucketIdx = 0;
                _pPool->Free(headNode, 6, _queueDebugIdx, _QID, node->_ID, this, bucketIdx);
                headNode->LeaveLog(6, _queueDebugIdx, _QID, data, bucketIdx, next, head, 0);

                if (data != _QID) 
                {    
                    int idx = LeaveLog(99999999999, 0, 0, 0, 0, 0);
                    headNode->LeaveLog(99999999999, _queueDebugIdx, 0, 0, 0, 0, 0, 0);
                    _pPool->LeaveLog(9999999999, _queueDebugIdx, 0, 0, nullptr, nullptr);
                    __debugbreak();
                }

                break;
            }
        }

        InterlockedExchangeAdd(&_size, -1);
        return data;
    }

public:
    int GetUseSize() { return _size; }

private: // For Log
    class QueueDebugData
    {
        friend CLockFreeQueue;

    private:
        QueueDebugData() : _idx(-1), _threadID(-1), _line(-1), _size(-1), _data(-1),
            _compKey(-1), _exchKey(-1), _realKey(-1), _compAddress(-1), _exchAddress(-1), _realAddress(-1) {}

        void SetData(int idx, int threadID, int line, int size,
            int exchKey, __int64 exchAddress,
            int compKey, __int64 compAddress,
            int realKey, __int64 realAddress,
            __int64 data)
        {
            _exchKey = exchKey;
            _exchAddress = exchAddress;

            _compKey = compKey;
            _compAddress = compAddress;

            _realKey = realKey;
            _realAddress = realAddress;

            _idx = idx;
            _threadID = threadID;
            _line = line;
            _size = size;
            _data = data;
        }

    private:
        int _idx;
        int _threadID;
        int _line;
        int _size;

        int _compKey;
        __int64 _compAddress;

        int _exchKey;
        __int64 _exchAddress;

        int _realKey;
        __int64 _realAddress;

        __int64 _data;
    };

private:
#define dfQUEUE_DEBUG_MAX 10000

    inline int LeaveLog(int line, int size,
        unsigned __int64 exchange, unsigned __int64 comperand, unsigned __int64 real, unsigned __int64 data)
    {
        LONG idx = InterlockedIncrement(&_queueDebugIdx);

        int exchKey = (exchange >> __USESIZE_64BIT__) & _keyMask;
        int compKey = (comperand >> __USESIZE_64BIT__) & _keyMask;
        int realKey = (real >> __USESIZE_64BIT__) & _keyMask;
        __int64 exchAddress = exchange & _addressMask;
        __int64 compAddress = comperand & _addressMask;
        __int64 realAddress = real & _addressMask;

        _queueDebugArray[idx % dfQUEUE_DEBUG_MAX].SetData(idx, GetCurrentThreadId(), line, size,
            exchKey, exchAddress, compKey, compAddress, realKey, realAddress, data);

        return idx;
    }

    QueueDebugData _queueDebugArray[dfQUEUE_DEBUG_MAX];
    volatile long _queueDebugIdx = -1;
};

template<typename T>
long CLockFreeQueue<T>::_IDSupplier = 0;

template<typename T>
CTlsPool<typename CLockFreeQueue<T>::QueueNode>* CLockFreeQueue<T>::_pPool = new CTlsPool<CLockFreeQueue<T>::QueueNode>(0, false);