#pragma once

#include <MSWSock.h>
#include <WinSock2.h>
#include <Windows.h>

#define DEFAULT_PORT        "778"
#define MAX_BUFF_SIZE       8192
#define MAX_WORKER_THREAD   16

typedef enum _IO_OPERATION
{
    ClientIoAccept,
    ClientIoRead,
    ClientIoWrite
} IO_OPERATION, * PIO_OPERATION;

// data to be associated for every I/O operation on a socket
typedef struct _PER_IO_CONTEXT
{
    WSAOVERLAPPED               Overlapped;
    char                        Buffer[MAX_BUFF_SIZE];
    WSABUF                      wsabuf;
    int                         nTotalBytes;
    int                         nSentBytes;
    IO_OPERATION                IOOperation;
    SOCKET                      SocketAccept;
    struct _PER_IO_CONTEXT*     pIOContextForward;
} PER_IO_CONTEXT, *PPER_IO_CONTEXT;

// For AcceptEx, the IOCP completion key is the PER_SOCKET_CONTEXT for the listening socket,
// so we need to another field SocketAccept in PER_IO_CONTEXT. When the outstanding
// AcceptEx completes, this field is our connection socket handle.
//
//
// data to be associated with every socket added to the IOCP
//
typedef struct _PER_SOCKET_CONTEXT
{
    SOCKET                      Socket;
    LPFN_ACCEPTEX               fnAcceptEx;

    //
    //linked list for all outstanding i/o on the socket
    //
    PPER_IO_CONTEXT             pIOContext;
    struct _PER_SOCKET_CONTEXT* pCtxtBack;
    struct _PER_SOCKET_CONTEXT* pCtxtForward;
} PER_SOCKET_CONTEXT, * PPER_SOCKET_CONTEXT;

BOOL CreateListenSocket(void);
SOCKET CreateSocket(void);
BOOL CreateAcceptSocket(BOOL fUpdateIOCP);
PPER_SOCKET_CONTEXT UpdateCompletionPort(SOCKET sd, IO_OPERATION ClientIo, BOOL bAddToList);
PPER_SOCKET_CONTEXT CtxtAllocate(SOCKET sd, IO_OPERATION ClientIO);