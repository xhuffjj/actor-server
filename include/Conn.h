#pragma once
#include <cstdint>
//对套接字做一层封装
class Conn{
    public:
    enum TYPE{
        LISTEN=1,
        CLIENT=2
    };
    uint8_t type;//套接字类型
    int fd;//套接字
    uint32_t serviceId;//和fd关联的服务id
    bool closed = false;//该连接对象是否已关闭
};