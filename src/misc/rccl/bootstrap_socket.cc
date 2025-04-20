#include "bootstrap_socket.h"

// anpNcclSocketInit
ncclResult_t anpNcclSocketInit(anpNcclSocket* socket, anpNcclSocketAddress* address, unsigned long flags, anpNcclSocketType type, unsigned int volatile* progress, int nonBlocking) {
    if (type != anpNcclSocketTypePluginBootstrap) {
        return ncclInternalError;
    }
    socket->fd = ::socket(AF_INET, SOCK_STREAM, 0); // Use ::socket to avoid potential conflicts
    if (socket->fd == -1) {
        return ncclInternalError;
    }

    int opt = 1;
    if (setsockopt(socket->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        close(socket->fd);
        return ncclInternalError;
    }

    if (nonBlocking) {
        fcntl(socket->fd, F_SETFL, O_NONBLOCK);
    }
    socket->addr = *address;

    return ncclSuccess;
}

// anpNcclSocketListen
ncclResult_t anpNcclSocketListen(anpNcclSocket* socket) {
    anpNcclSocketAddress addr = socket->addr;
    socklen_t addrlen = sizeof(addr);
    if (bind(socket->fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        printf("\n bind failed %d", socket->fd);
        return ncclInternalError;
    }
    if (listen(socket->fd, SOMAXCONN) == -1) {
        return ncclInternalError;
    }
    //printf("\n listen_socket_fd %d", socket->fd);
    return ncclSuccess;
}

// anpNcclSocketAccept
ncclResult_t anpNcclSocketAccept(anpNcclSocket* clientSocket, anpNcclSocket* listenSocket) {
    struct sockaddr_in clientAddr;
    //printf("\n Accept: listenSocket_fd %d", listenSocket->fd);
    fflush(stdout);
    socklen_t clientAddrLen = sizeof(clientAddr);
    clientSocket->fd = accept(listenSocket->fd, (struct sockaddr*)&(clientSocket->addr), &clientAddrLen);
    //printf("\n Accept: client_fd %d", clientSocket->fd);
    if (clientSocket->fd == -1) {
        return ncclInternalError;
    }
    return ncclSuccess;
}

// anpNcclSocketConnect
ncclResult_t anpNcclSocketConnect(anpNcclSocket* socket) {
    anpNcclSocketAddress addr = socket->addr;
    socklen_t addrlen = sizeof(addr);
    //if(getpeername(socket->fd, (struct sockaddr*)&addr, &addrlen) == -1 && errno == ENOTCONN){
        if (connect(socket->fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            return ncclInternalError;
        }
    //}
    return ncclSuccess;
}

// anpNcclSocketSend
ncclResult_t anpNcclSocketSend(anpNcclSocket* socket, void* data, int size) {
    ssize_t sent = send(socket->fd, data, size, 0);
    if (sent == -1 || sent != size) {
        return ncclInternalError;
    }
    return ncclSuccess;
}

// anpNcclSocketRecv
ncclResult_t anpNcclSocketRecv(anpNcclSocket* socket, void* data, int size) {
    ssize_t received = recv(socket->fd, data, size, 0);
    if (received == -1) {
        printf("\n no data received");
        return ncclInternalError;
    }
    if (received != size) {
        printf("\n data size mismatch received %ld, expected %u", received, size);
        return ncclInternalError;
    }
    return ncclSuccess;
}

// anpNcclSocketClose
ncclResult_t anpNcclSocketClose(anpNcclSocket* socket) {
    close(socket->fd);
    return ncclSuccess;
}
