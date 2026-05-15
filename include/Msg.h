#pragma once
#include <cstdint>
#include<memory>
class Conn;
//消息基类
class BaseMsg{
    public:
    enum TYPE{
        SERVICE=1,
        SOCKET_ACCEPT=2,
        SOCKET_RW=3,
        TIMER=4,
    };

    uint8_t type;//消息类型，和TYPE对应
    char load[999999]{};//调试用，用于放大内存泄漏的影响，快速发现泄漏
    virtual ~BaseMsg(){};
};

class ServiceMsg:public BaseMsg
{
public:
    enum KIND{
        SEND = 1,
        CALL = 2,
        RET = 3,
    };
    uint8_t kind = SEND;
    uint32_t sourse;
    uint32_t session = 0;
    std::shared_ptr<char>buff;//消息
    size_t size;//消息长度
    //ServiceMsg(/* args */);
    ~ServiceMsg(){};
};

class SocketAcceptMsg:public BaseMsg{
    public:
    int listenfd;
    int clientfd;
};

class SocketRWMsg:public BaseMsg{
    public:
    std::shared_ptr<Conn> conn;
    bool isRead=false;
    bool isWrite=false;
    bool isCloseReq=false;
};

class TimerMsg:public BaseMsg{
    public:
    uint32_t session;
};

