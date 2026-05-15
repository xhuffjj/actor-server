#include<iostream>
#include "Sunnet.h"
#include <string>
#include<memory>
#include <unistd.h>
void test(){
    Sunnet *inst=Sunnet::get_instance();
    std::shared_ptr<std::string>pingType= std::make_shared<std::string>("ping");
    uint32_t ping1= inst->NewService(pingType);
    uint32_t ping2=inst->NewService(pingType);
    uint32_t pong=inst->NewService(pingType);
    std::shared_ptr<BaseMsg> msg1=inst->MakeMsg(ping1,new char[3]{'h','i','\0'},3);
    std::shared_ptr<BaseMsg> msg2=inst->MakeMsg(ping2,new char[6]{'h','e','l','l','o','\0'},6);
    inst->Send(pong,msg1);
    inst->Send(pong,msg2);
}

void TestSocketCtrl(){
    Sunnet *inst=Sunnet::get_instance();
    int fd=inst->Listen(8001,1);
    usleep(15*1000000);
    inst->Closeconn(fd);
}

void TestEcho(){
    std::shared_ptr<std::string>t=std::make_shared<std::string>("gateway");
    uint32_t gateway=Sunnet::get_instance()->NewService(t);
}

int main(){
    Sunnet* inst= Sunnet::get_instance();
    inst->Start();
    auto t=std::make_shared<std::string>("main");
    uint32_t mainId=inst->NewService(t);
    inst->Wait();
    return 0;
}