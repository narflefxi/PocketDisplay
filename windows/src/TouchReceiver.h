#pragma once
#include <winsock2.h>
#include <cstdint>
#include <atomic>
#include <thread>

class TouchReceiver {
public:
    TouchReceiver() = default;
    ~TouchReceiver();

    bool Start(uint16_t port = 7778);
    void Stop();

private:
    void ReceiverLoop();
    void ApplyEvent(uint8_t type, float nx, float ny) const;

    SOCKET            sock_    = INVALID_SOCKET;
    std::thread       thread_;
    std::atomic<bool> running_{false};
};
