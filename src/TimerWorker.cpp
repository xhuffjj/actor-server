#include "TimerWorker.h"
#include "Sunnet.h"
#include "Msg.h"
#include <ctime>

static timespec MakeMonotonicDeadline (uint64_t ms){
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);

    ts.tv_sec+=ms/1000;
    ts.tv_nsec+=(long)(ms%1000)* 1000000L;
    //进位处理
    if(ts.tv_nsec >= 1000000000L){
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}
void TimerWorker::operator()(){

    Sunnet* inst = Sunnet::get_instance();
    while(true){
        pthread_mutex_lock(&inst->timerMtx);
        //空就睡
        while(inst->timerQueue.empty()){
            pthread_cond_wait(&inst->timerCond, &inst->timerMtx);
        }
        //醒来遍历所有超时的节点
        while(!inst->timerQueue.empty()){
            TimerNode node = inst->timerQueue.top();
            auto now = std::chrono::steady_clock::now();

            if(node.expire <= now){
                inst->timerQueue.pop();
                pthread_mutex_unlock(&inst->timerMtx);
                //给超时的服务发信息
                std::shared_ptr<TimerMsg> msg = std::make_shared<TimerMsg>();
                msg->type = BaseMsg::TYPE::TIMER;
                msg->session = node.session;
                inst->Send(node.serviceId, msg);
                pthread_mutex_lock(&inst->timerMtx);
                continue;
            }

            //如果这个不是超时的节点。而是新加到堆顶的节点导致我们被唤醒
            //那就重新计算睡眠时间，duration_cast将时间结构转为数字，指定为ms,.count取出里面的底层数字
            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(node.expire - now).count();
            timespec ts = MakeMonotonicDeadline((uint64_t)diff);
            //睡到下一个超时到
            pthread_cond_timedwait(&inst->timerCond, &inst->timerMtx, &ts);
            break;
        }
        pthread_mutex_unlock(&inst->timerMtx);
    }
}
