#pragma once

#include <string>

class DrmLeaseSocket
{
public:
    DrmLeaseSocket() = default;
    ~DrmLeaseSocket();

    DrmLeaseSocket(const DrmLeaseSocket&) = delete;
    DrmLeaseSocket& operator=(const DrmLeaseSocket&) = delete;

    bool Connect(const std::string& socketPath);
    int TakeLeaseFd();

private:
    int socketFd = -1;
    int leaseFd = -1;
};
