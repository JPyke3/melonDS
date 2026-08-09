#include "DrmLeaseSocket.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

DrmLeaseSocket::~DrmLeaseSocket()
{
#ifdef __linux__
    if (leaseFd >= 0)
        close(leaseFd);

    if (socketFd >= 0)
        close(socketFd);
#endif
}

int DrmLeaseSocket::TakeLeaseFd()
{
    const int fd = leaseFd;
    leaseFd = -1;
    return fd;
}

bool DrmLeaseSocket::Connect(const std::string& socketPath)
{
#ifndef __linux__
    std::fprintf(stderr, "[drm-lease] DRM leasing is only supported on Linux\n");
    return false;
#else
    socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFd < 0)
    {
        std::fprintf(stderr, "[drm-lease] socket(): %s\n", std::strerror(errno));
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    if (socketPath.size() >= sizeof(address.sun_path))
    {
        std::fprintf(stderr, "[drm-lease] socket path is too long\n");
        return false;
    }

    std::memcpy(
        address.sun_path,
        socketPath.c_str(),
        socketPath.size() + 1
    );

    std::fprintf(
        stderr,
        "[drm-lease] connecting to '%s'...\n",
        socketPath.c_str()
    );

    if (::connect(
            socketFd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) < 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] connect(): %s\n",
            std::strerror(errno)
        );
        return false;
    }

    // Gamescope sends one normal byte ('L') plus one FD through SCM_RIGHTS.
    char data = 0;
    iovec iov{};
    iov.iov_base = &data;
    iov.iov_len = sizeof(data);

    char control[CMSG_SPACE(sizeof(int))] = {};

    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t received = recvmsg(
        socketFd,
        &message,
        MSG_CMSG_CLOEXEC
    );

    if (received <= 0)
    {
        std::fprintf(
            stderr,
            "[drm-lease] recvmsg(): %s\n",
            received < 0 ? std::strerror(errno) : "connection closed"
        );
        return false;
    }

    cmsghdr* cmsg = CMSG_FIRSTHDR(&message);

    if (cmsg == nullptr ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS)
    {
        std::fprintf(stderr, "[drm-lease] response contained no DRM FD\n");
        return false;
    }

    std::memcpy(
        &leaseFd,
        CMSG_DATA(cmsg),
        sizeof(leaseFd)
    );

    std::fprintf(
        stderr,
        "[drm-lease] received DRM lease fd %d\n",
        leaseFd
    );

    return leaseFd >= 0;
#endif
}

