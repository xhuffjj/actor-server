#pragma once


#include <vector>
#include<string>
#include<memory>
#include<unordered_map>
#include<thread>
#include<queue>
#include <chrono>
#include "Conn.h"
class Service;
class BaseMsg;
class Worker;//只前向声明，源文件再包含头文件
class SocketWorker;
class TimerWorker;
struct TimerNode {//定时事件节点
    std::chrono::steady_clock::time_point expire;
    uint32_t serviceId;//到期时发给的服务id
    uint32_t session;//这次超时阻塞对应的出让编号
};
struct TimerCompare {//定时最小堆排序仿函数
    bool operator()(const TimerNode& a, const TimerNode& b) const {
        return a.expire > b.expire;//降序
    }
};

//Sunnet管理着众线程和线程对象
class Sunnet{
public: 
    static Sunnet* get_instance();
    void Start();//启动Sunnet实例
    void Wait();//等待线程退出

public:
    std::unordered_map<uint32_t,std::shared_ptr<Service>>services;
    std::unordered_map<std::string, uint32_t> names;//服务名字表:名字->服务id
    uint32_t maxId=0;//最大服务id
    pthread_rwlock_t serviceLock;//读写锁
    //增删服务
    uint32_t NewService(std::shared_ptr<std::string>type);
    void KillService(uint32_t id);//仅限服务自己调用，防止竟态
    bool Name(uint32_t id, std::shared_ptr<std::string> name);//命名
    uint32_t QueryName(const std::string& name);//名字查服务id
    std::shared_ptr<std::string> GetServiceName(uint32_t id);//id查名字

private:
//查找服务
    std::shared_ptr<Service> GetService(uint32_t id);

private:
    int WORKER_NUM=3;//工作线程数
    std::vector<Worker*>workers;//线程对象数组
    std::vector<std::thread*>workerThreads;//线程数组
    static Sunnet *inst;
    
    void StartWorker();//开启工作线程

private:
    std::queue<std::shared_ptr<Service>>globalQueue;//全局服务队列
    int globalLen;
    pthread_spinlock_t globalLock;

public:
    bool Send(uint32_t toId,std::shared_ptr<BaseMsg>msg);//发送消息
    void PushGlobalQueue(std::shared_ptr<Service>);
    std::shared_ptr<Service> PopGlobalQueue();

    //仅测试用，创建一个msg
    std::shared_ptr<BaseMsg> MakeMsg(uint32_t sourse,char* buff,int len); 

private:
    pthread_mutex_t sleepMtx;//条件变量配套锁，保护sleepCount，并且让睡眠计数和睡眠成为原子操作
    //现在这个sleepMtx并没有保护globalQueue，有可能丢失唤醒（在睡眠前队列被插入了元素然后唤醒）
    //不过，由于send的高频性这个问题不大
    pthread_cond_t sleepCond;//条件变量，全局队列为空就让线程睡眠
    int sleepCount=0;//睡眠的线程数

public:
    //唤醒工作线程
    void checkAndWeakUp();
    //让工作线程等待
    void WorkerWait();

private:
    SocketWorker *socketWorker;
    std::thread* socketThread;
    //开启Socket线程
    void StartSocket();

private:
    std::unordered_map<uint32_t,std::shared_ptr<Conn>>Conns;//管理所有套接字的哈希表
    pthread_rwlock_t connsLock;//send客户端时要读这个哈希表，是高频操作，写操作创建连接相对低频，用读写锁。

public:
    int AddConn(int fd,uint32_t id,Conn::TYPE type);//添加连接
    bool RemoveConn(int fd);//删除连接
    std::shared_ptr<Conn> GetConn(int fd);//查连接

public:
    int Listen(uint32_t port,uint32_t serviceId);//服务要求epoll监听指定端口
    void ModifyEvent(int fd,bool epollout);
    void Closeconn(uint32_t fd);//关闭fd

public:
    //封装一下service的writeConn
    bool Write(uint32_t serviceId, int fd, std::shared_ptr<char> buff, int len);
    //封装一下service的LingerClose
    bool LingerClose(uint32_t serviceId, int fd);

public:
    //<存放元素，存储结构，排序规则>,最小堆定时器
    std::priority_queue<TimerNode, std::vector<TimerNode>, TimerCompare> timerQueue;
    pthread_mutex_t timerMtx;//保护堆的锁，使操作堆和操作条件变量成为原子操作
    pthread_cond_t timerCond;//新定时器插入时唤醒 timer 线程
    TimerWorker* timerWorker;
    std::thread* timerThread;
    void StartTimer();

public:
    //服务设置超时
    void Timeout(uint32_t serviceId, uint32_t session, uint32_t ms);
};
