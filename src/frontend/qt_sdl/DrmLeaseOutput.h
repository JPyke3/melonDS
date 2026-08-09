#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class DrmLeaseOutput
{
public:
    explicit DrmLeaseOutput(int leaseFd) : leaseFd(leaseFd) {}
    ~DrmLeaseOutput();

    DrmLeaseOutput(const DrmLeaseOutput&) = delete;
    DrmLeaseOutput& operator=(const DrmLeaseOutput&) = delete;

    bool IsValid() const { return leaseFd >= 0; }
    int GetFd() const { return leaseFd; }

    void PrintResources() const;
    bool InitializeOutput();
    void PresentBottomScreen(const uint32_t* pixels);

private:
    int leaseFd = -1;

    void* scanoutMemory = nullptr;
    std::size_t scanoutSize = 0;

    uint32_t scanoutPitch = 0;
    uint32_t scanoutWidth = 0;
    uint32_t scanoutHeight = 0;

    bool outputActive = false;
    bool rotateLeft = false;
    bool loggedFirstFrame = false;

    std::vector<int> sourceXMap;
    std::vector<int> sourceYMap;
};
