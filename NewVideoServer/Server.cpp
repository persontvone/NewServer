#define WIN32_LEAN_AND_MEAN
#include "Server.h"
#include <stdio.h>
#include <process.h>
#include <stdint.h>
#include <WS2tcpip.h>

#define xmalloc(s) HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(s))
#define xfree(p)   HeapFree(GetProcessHeap(),0,(p))

HANDLE g_hIOCP = INVALID_HANDLE_VALUE;
SOCKET g_listenSocket = INVALID_SOCKET;
HANDLE g_ThreadHandles[MAX_WORKER_THREAD];
WSAEVENT g_hCleanupEvent[1];
PPER_SOCKET_CONTEXT g_pListenSocketContext = NULL;
PPER_SOCKET_CONTEXT g_pContextList = NULL;
BOOL g_bEndServer = FALSE;			// set to TRUE on CTRL-C
BOOL g_bRestart = TRUE;				// set to TRUE to CTRL-BRK
BOOL g_bVerbose = FALSE;
CRITICAL_SECTION g_CriticalSection;

int main(int argc, char** argv)
{
	SYSTEM_INFO systemInfo;
	int iRet = 0;

	//WHAT?
	g_ThreadHandles[0] = (HANDLE)WSA_INVALID_EVENT;
	
	for (int i = 0; i < MAX_WORKER_THREAD; i++)
	{
		g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
	}
	GetSystemInfo(&systemInfo);
	DWORD dwThreadCount = systemInfo.dwNumberOfProcessors * 2;
	
	if ((g_hCleanupEvent[0] = WSACreateEvent()) == WSA_INVALID_EVENT)
	{
		printf("WSACreateEvent() failed: %d\n", WSAGetLastError());
		return WSAGetLastError();
	}

	WSADATA wsadata;
	if (WSAStartup(MAKEWORD(2, 2), &wsadata))
	{
		return WSAGetLastError();
	}

	while (g_bRestart)
	{
		g_bRestart = FALSE;
		g_bEndServer = FALSE;
		WSAResetEvent(g_hCleanupEvent[0]);

		__try
		{
			g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
			if (g_hIOCP == NULL)
			{
				printf("Cannot create CompletionPort. Error %d\n", WSAGetLastError());
				__leave;
			}
			for (DWORD dwCPU = 0; dwCPU < dwThreadCount; dwCPU++)
			{
				// Create worker threads to service the overlapped I/O requests.  The decision
				// to create 2 worker threads per CPU in the system is a heuristic.  Also,
				// note that thread handles are closed right away, because we will not need them
				// and the worker threads will continue to execute.

				uint32_t dwThreadId;
				HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, &WorkerThread, g_hIOCP, 0, &dwThreadId);
				if (hThread == NULL)
				{
					printf("CreateThread() failed to create worker thread: %d\n", GetLastError());
					__leave;
				}
				g_ThreadHandles[dwCPU] = hThread;
				hThread = INVALID_HANDLE_VALUE;
			}
			if (!CreateListenSocket())
				__leave;

			if (!CreateAcceptSocket(TRUE))
				__leave;
		}

		__finally
		{

		}
	}

}

BOOL CreateListenSocket(void)
{

	int nRet = 0;
	LINGER lingerStruct;
	struct addrinfo hints = { 0 };
	struct addrinfo* addrlocal = NULL;

	lingerStruct.l_onoff = 1;
	lingerStruct.l_linger = 0;

	//
	// Resolve the interface
	//
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(NULL, DEFAULT_PORT, &hints, &addrlocal) != 0)
	{
		printf("getaddrinfo() failed with error %d\n", WSAGetLastError());
		return(FALSE);
	}

	if (addrlocal == NULL)
	{
		printf("getaddrinfo() failed to resolve/convert the interface\n");
		return(FALSE);
	}

	g_listenSocket = CreateSocket();
	if (g_listenSocket == INVALID_SOCKET)
	{
		freeaddrinfo(addrlocal);
		return(FALSE);
	}

	nRet = bind(g_listenSocket, addrlocal->ai_addr, (int)addrlocal->ai_addrlen);
	if (nRet == SOCKET_ERROR)
	{
		printf("bind() failed: %d\n", WSAGetLastError());
		freeaddrinfo(addrlocal);
		return(FALSE);
	}

	nRet = listen(g_listenSocket, 5);
	if (nRet == SOCKET_ERROR)
	{
		printf("listen() failed: %d\n", WSAGetLastError());
		freeaddrinfo(addrlocal);
		return(FALSE);
	}

	freeaddrinfo(addrlocal);
	return(TRUE);
}

SOCKET CreateSocket(void)
{
	SOCKET sdSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (sdSocket == INVALID_SOCKET)
	{
		printf("WSASocket(sdSocket) failed: %d\n", WSAGetLastError());
		return(sdSocket);
	}
	//
	// Disable send buffering on the socket.  Setting SO_SNDBUF
	// to 0 causes winsock to stop buffering sends and perform
	// sends directly from our buffers, thereby save one memory copy.
	//
	// However, this does prevent the socket from ever filling the
	// send pipeline. This can lead to packets being sent that are
	// not full (i.e. the overhead of the IP and TCP headers is 
	// great compared to the amount of data being carried).
	//
	// Disabling the send buffer has less serious repercussions 
	// than disabling the receive buffer.
	//
	int nZero = 0;
	int nRet = setsockopt(sdSocket, SOL_SOCKET, SO_SNDBUF, (char*)&nZero, sizeof(nZero));
	if (nRet == SOCKET_ERROR)
	{
		printf("setsockopt(SNDBUF) failed: %d\n", WSAGetLastError());
		return(sdSocket);
	}
	return(sdSocket);
}

BOOL CreateAcceptSocket(BOOL fUpdateIOCP)
{
	int nRet = 0;
	DWORD dwRecvNumBytes = 0;
	DWORD bytes = 0;
	//
	// GUID to Microsoft specific extensions
	//
	GUID acceptex_guid = WSAID_ACCEPTEX;
	//
	//The context for listening socket uses the SockAccept member to store the
	//socket for client connection. 
	//
	if (fUpdateIOCP)
	{
		g_pListenSocketContext = UpdateCompletionPort(g_listenSocket, ClientIoAccept, FALSE);
		if (g_pListenSocketContext == NULL)
		{
			printf("failed to update listen socket to IOCP\n");
			return(FALSE);
		}
		// Load the AcceptEx extension function from the provider for this socket
		int nRet = WSAIoctl(g_listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&acceptex_guid, sizeof(acceptex_guid), &g_pListenSocketContext->fnAcceptEx,
			sizeof(g_pListenSocketContext->fnAcceptEx), &bytes, NULL, NULL);
		if (nRet == SOCKET_ERROR)
		{
			printf("failed to load AcceptEx: %d\n", WSAGetLastError());
			return (FALSE);
		}
	}

	g_pListenSocketContext->pIOContext->SocketAccept = CreateSocket();
	if (g_pListenSocketContext->pIOContext->SocketAccept == INVALID_SOCKET)
	{
		printf("failed to create new accept socket\n");
		return(FALSE);
	}

	//
	// pay close attention to these parameters and buffer lengths
	//
	nRet = g_pListenSocketContext->fnAcceptEx(g_listenSocket, g_pListenSocketContext->pIOContext->SocketAccept,
		(LPVOID)(g_pListenSocketContext->pIOContext->Buffer),
		MAX_BUFF_SIZE - (2 * (sizeof(SOCKADDR_STORAGE) + 16)),
		sizeof(SOCKADDR_STORAGE) + 16, sizeof(SOCKADDR_STORAGE) + 16,
		&dwRecvNumBytes,
		(LPOVERLAPPED) & (g_pListenSocketContext->pIOContext->Overlapped));
	if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
	{
		printf("AcceptEx() failed: %d\n", WSAGetLastError());
		return(FALSE);
	}

	return(TRUE);
}

PPER_SOCKET_CONTEXT UpdateCompletionPort(SOCKET sd, IO_OPERATION ClientIo, BOOL bAddToList) {

	PPER_SOCKET_CONTEXT lpPerSocketContext;

	lpPerSocketContext = CtxtAllocate(sd, ClientIo);
	if (lpPerSocketContext == NULL)
		return(NULL);

	g_hIOCP = CreateIoCompletionPort((HANDLE)sd, g_hIOCP, (DWORD_PTR)lpPerSocketContext, 0);
	if (g_hIOCP == NULL)
	{
		printf("CreateIoCompletionPort() failed: %d\n", GetLastError());
		if (lpPerSocketContext->pIOContext)
			xfree(lpPerSocketContext->pIOContext);
		xfree(lpPerSocketContext);
		return(NULL);
	}
	//The listening socket context (bAddToList is FALSE) is not added to the list.
	//All other socket contexts are added to the list.
	//
	if (bAddToList)
	{
		CtxtListAddTo(lpPerSocketContext);
	}

	if (g_bVerbose)
		printf("UpdateCompletionPort: Socket(%d) added to IOCP\n", lpPerSocketContext->Socket);

	return(lpPerSocketContext);
}

PPER_SOCKET_CONTEXT CtxtAllocate(SOCKET sd, IO_OPERATION ClientIO)
{
	PPER_SOCKET_CONTEXT lpPerSocketContext;
	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		printf("EnterCriticalSection raised an exception.\n");
		return NULL;
	}

	lpPerSocketContext = (PPER_SOCKET_CONTEXT)xmalloc(sizeof(PER_SOCKET_CONTEXT));
	if (lpPerSocketContext)
	{
		lpPerSocketContext->pIOContext = (PPER_IO_CONTEXT)xmalloc(sizeof(PER_IO_CONTEXT));
		if (lpPerSocketContext->pIOContext)
		{
			lpPerSocketContext->Socket = sd;
			lpPerSocketContext->pCtxtBack = NULL;
			lpPerSocketContext->pCtxtForward = NULL;
			lpPerSocketContext->pIOContext->Overlapped.Internal = 0;
			lpPerSocketContext->pIOContext->Overlapped.InternalHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.Offset = 0;
			lpPerSocketContext->pIOContext->Overlapped.OffsetHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.hEvent = NULL;
			lpPerSocketContext->pIOContext->IOOperation = ClientIO;
			lpPerSocketContext->pIOContext->pIOContextForward = NULL;
			lpPerSocketContext->pIOContext->nTotalBytes = 0;
			lpPerSocketContext->pIOContext->nSentBytes = 0;
			lpPerSocketContext->pIOContext->wsabuf.buf = lpPerSocketContext->pIOContext->Buffer;
			lpPerSocketContext->pIOContext->wsabuf.len = sizeof(lpPerSocketContext->pIOContext->Buffer);
			lpPerSocketContext->pIOContext->SocketAccept = INVALID_SOCKET;

			ZeroMemory(lpPerSocketContext->pIOContext->wsabuf.buf, lpPerSocketContext->pIOContext->wsabuf.len);
		}
		else
		{
			xfree(lpPerSocketContext);
			printf("HeapAlloc() PER_IO_CONTEXT failed: %d\n", GetLastError());
		}
	}
	else
	{
		printf("HeapAlloc() PER_SOCKET_CONTEXT failed: %d\n", GetLastError());
		return(NULL);
	}
	LeaveCriticalSection(&g_CriticalSection);

	return(lpPerSocketContext);
}