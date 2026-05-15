#include <iostream>
#include "SocketWorker.h"
#include <unistd.h>
#include "Sunnet.h"
#include <cstring>
#include<fcntl.h>
#include<sys/socket.h>
#include"Msg.h"
void SocketWorker::init(){
    std::cout<<"SocketWorker Init"<<std::endl;
    epollfd=epoll_create(8);
    
}

void SocketWorker::operator()(){
    while(true){
        const int EVENT_SIZE=64;
        struct epoll_event events [EVENT_SIZE];
        int eventCount=epoll_wait(epollfd,events,EVENT_SIZE,-1);
        for(int i=0;i<eventCount;i++){
            epoll_event ev=events[i];
            OnEvent(ev);
        }
    }
}

//这个是在工作线程里面做的
void SocketWorker::AddEvent(int fd){
    std::cout<<"AddEvent fd: "<<fd<<std::endl;
    struct epoll_event ev;
    ev.data.fd=fd;
    ev.events=EPOLLET|EPOLLIN;
    if(epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&ev)==-1){
        std::cout<<"AddEvent epoll_ctl Failed:"<<strerror(errno)<<std::endl;
    }
}

void SocketWorker::ModifyEvent(int fd,bool epollout){
    std::cout<<"ModifyEvent fd: "<<fd<<" "<<epollout<<std::endl;
    epoll_event ev;
    ev.data.fd=fd;
    if(epollout){
        ev.events=EPOLLIN|EPOLLOUT|EPOLLET;
    }else{
        ev.events=EPOLLIN|EPOLLET;
    }
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&ev);
}

void SocketWorker::RemoveEvent(int fd){
    std::cout<<"RemoveEvent fd: "<<fd<<std::endl;
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,NULL);
}

void  SocketWorker::OnEvent(epoll_event ev){
    int fd=ev.data.fd;
    Sunnet* inst=Sunnet::get_instance();
    std::shared_ptr<Conn>conn= inst->GetConn(fd);
    if (conn==NULL){
        std::cout<<"OnEvent failed ,conn==NULL"<<std::endl;
        return;
    }
    bool isread=ev.events& EPOLLIN;
    bool iswrite=ev.events& EPOLLOUT;
    bool iserror=ev.events& EPOLLERR;
    if(conn->type==Conn::TYPE::LISTEN){
        if(isread){
            OnAccept(conn);
        }
    }else{
        if(iswrite||isread){
            OnRW(conn,isread,iswrite);
        }
        if(iserror){
            std::cout<<"OnError fd:"<<conn->fd<<std::endl;
        }
    }

    
}
void  SocketWorker::OnAccept(std::shared_ptr<Conn> conn){
    std::cout<<"OnAccept fd:"<<conn->fd<<std::endl;
    //接受连接
    int clientfd=accept(conn->fd,NULL,NULL);
    if(clientfd<0){
        std::cout<<"accept error"<<std::endl;
    }
    fcntl(clientfd,F_SETFL,O_NONBLOCK);

    //保存到管理结构
    Sunnet* inst=Sunnet::get_instance();
    inst->AddConn(clientfd,conn->serviceId,Conn::TYPE::CLIENT);
    //添加到epoll监听集合
    struct epoll_event ev;
    ev.data.fd=clientfd;
    ev.events=EPOLLET|EPOLLIN;
    if(epoll_ctl(epollfd,EPOLL_CTL_ADD,clientfd,&ev)==-1){
        std::cout<<"OnAccept epoll_ctl Failed:"<<strerror(errno)<<std::endl;
    }
    //通知服务
    std::shared_ptr<SocketAcceptMsg> msg= std::make_shared<SocketAcceptMsg>();
    msg->type=BaseMsg::TYPE::SOCKET_ACCEPT;
    msg->listenfd=conn->fd;
    msg->clientfd=clientfd;
    inst->Send(conn->serviceId,msg);
}
void  SocketWorker::OnRW(std::shared_ptr<Conn>conn,bool r,bool w){
    std::cout<<"OnRW fd:"<<conn->fd<<std::endl;
    std::shared_ptr<SocketRWMsg>msg=std::make_shared<SocketRWMsg>();
    msg->type=BaseMsg::TYPE::SOCKET_RW;
    msg->isRead=r;
    msg->isWrite=w;
    msg->conn = conn;
    Sunnet* inst=Sunnet::get_instance();
    inst->Send(conn->serviceId,msg);
}