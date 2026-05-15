#include "Worker.h"
#include <iostream>
#include<unistd.h>
#include"Sunnet.h"
#include"Service.h"
using namespace std;

void Worker::CheckAndPutGlobal(std::shared_ptr<Service> srv){
    if (srv->is_Exiting){//如果服务正在退出，就不再放回全局队列,服务失去所有引用，释放内存
        return;
    } 
    Sunnet *inst=Sunnet::get_instance();
    pthread_spin_lock(&(srv->queueLock));
    {
        if (!srv->msgQueue.empty()){
            //我们并未修改srv->isGlobal,此时仍为true
            //所以只将服务插入全局队列
            inst->PushGlobalQueue(srv);
        }else{
            //修改isGlobal为false
            srv->SetInGlobal(false);
        }
    }
    pthread_spin_unlock(&(srv->queueLock));
}

void Worker::operator()(Sunnet *inst){
    while(true){

        std::shared_ptr<Service> srv=inst->PopGlobalQueue();
        if (srv==NULL){
            inst->WorkerWait();
        }else{
            srv->processMsgs(eachNum);
            CheckAndPutGlobal(srv);//检查还有没有消息，还有的话还要放回全局队列
        }

    }
}