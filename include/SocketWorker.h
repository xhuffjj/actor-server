
#pragma once
#include <sys/epoll.h>
#include<memory>
#include<Conn.h>
class Sunnet;

class SocketWorker{
    public:
    void init();//初始化
    void operator()();

    private:
    int epollfd;

    public:
    void AddEvent(int fd);
    void RemoveEvent(int fd);
    void ModifyEvent(int fd,bool epollout);
    private:
    void OnEvent(epoll_event ev);
    void OnAccept(std::shared_ptr<Conn> conn);
    void OnRW(std::shared_ptr<Conn>conn,bool r,bool w);
};