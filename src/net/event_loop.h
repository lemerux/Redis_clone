#pragma once

#include "command/command.h"
#include "net/socket.h"
#include "resp/resp.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace redis_clone {

struct Connection {
    explicit Connection(Socket clientSocket, std::uint64_t id);

    Socket socket;
    Buffer input;
    Buffer output;
    std::uint64_t clientId;
    bool closeAfterReply = false;
    bool closing = false;
};

class EventLoop {
public:
    explicit EventLoop(unsigned short port);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void runOnce();

private:
    void acceptClients();
    void handleClientEvent(int fd, std::uint32_t events);
    void readFromClient(Connection& connection);
    void dispatchCommands(Connection& connection);
    void dispatchCommand(Connection& connection, const RespValue& value);
    void writeToClient(Connection& connection);
    void updateInterest(Connection& connection);
    void closeConnection(Connection& connection);
    void finishDeferredCloses();

    int epollFd_ = -1;
    Socket listener_;
    RespParser parser_;
    CommandTable commandTable_;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::vector<int> deferredCloses_;
    std::uint64_t nextClientId_ = 1;
};

}  // namespace redis_clone
