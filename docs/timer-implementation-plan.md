# 定时器实现方案

这份文档只写“定时器本体”怎么落代码，不把协程恢复和 `sunnet.Sleep` 一起塞进来。

目标是先跑通这条链：

1. `Sunnet::Timeout(serviceId, session, ms)`
2. 定时器线程等待到期
3. 构造 `TimerMsg`
4. `Send(serviceId, msg)`
5. `Service::OnMsg()` 收到 `TIMER`


## 现状

你当前代码里已经有：

- `BaseMsg::TYPE::TIMER`，见 [include/Msg.h](/home/a/桌面/sunnet/include/Msg.h:8)
- `TimerMsg`，见 [include/Msg.h](/home/a/桌面/sunnet/include/Msg.h:44)

你当前代码里还没有：

- `TimerWorker`
- `Sunnet::Timeout`
- `Service::OnTimerMsg`
- `OnMsg()` 里的 `TIMER` 分支


## 设计决定

### 1. 定时器状态属于 `Sunnet`

`timerQueue`、`timerMtx`、`timerCond` 都放在 `Sunnet` 里，作为 `Sunnet` 成员。

原因：

- `Timeout(...)` 是 `Sunnet` 对外能力
- `TimerWorker` 只是消费这份全局定时器状态
- 这和你现在 `globalQueue`、`services`、`Conns` 的放置方式一致

### 2. 定时器线程也用仿函数

你现在已有线程入口风格：

- `Worker` 用 `operator()(Sunnet *inst)`
- `SocketWorker` 用 `operator()()`

所以定时器线程也保持这个风格，新增一个 `TimerWorker`：

```cpp
class TimerWorker {
public:
    void operator()();
};
```

不要直接写成员函数线程入口：

```cpp
new std::thread(&Sunnet::TimerLoop, this);
```

功能上可以，但和你现在项目风格不一致。

### 3. 第一版不用 `timerStop`

你当前 `Worker` 和 `SocketWorker` 都是无限循环，没有完整退出流程。

所以第一版定时器线程也先按同样风格做，不加 `timerStop`，避免文档和项目现状不一致。


## 要改哪些文件

### 新增

- [include/TimerWorker.h](/home/a/桌面/sunnet/include/TimerWorker.h:1)
- [src/TimerWorker.cpp](/home/a/桌面/sunnet/src/TimerWorker.cpp:1)

### 修改

- [include/Sunnet.h](/home/a/桌面/sunnet/include/Sunnet.h:1)
- [src/Sunnet.cpp](/home/a/桌面/sunnet/src/Sunnet.cpp:1)
- [include/Service.h](/home/a/桌面/sunnet/include/Service.h:1)
- [src/Service.cpp](/home/a/桌面/sunnet/src/Service.cpp:1)


## 代码怎么写

### 1. 新增 `include/TimerWorker.h`

```cpp
#pragma once

class TimerWorker{
public:
    void operator()();
};
```


### 2. 修改 `include/Sunnet.h`

你现在的 [include/Sunnet.h](/home/a/桌面/sunnet/include/Sunnet.h:1) 已经开始有 timer 字段了，但还不完整，而且缺少 `<chrono>`。

建议整理成下面这样。

```cpp
#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <thread>
#include <queue>
#include <chrono>
#include "Conn.h"

class Service;
class BaseMsg;
class Worker;
class SocketWorker;
class TimerWorker;

struct TimerNode{
    std::chrono::steady_clock::time_point expire;
    uint32_t serviceId;
    uint32_t session;
};

struct TimerCompare{
    bool operator()(const TimerNode& a, const TimerNode& b) const{
        return a.expire > b.expire;
    }
};

class Sunnet{
public:
    static Sunnet* get_instance();
    void Start();
    void Wait();

public:
    std::unordered_map<uint32_t,std::shared_ptr<Service>>services;
    std::unordered_map<std::string, uint32_t> names;
    uint32_t maxId=0;
    pthread_rwlock_t serviceLock;

    uint32_t NewService(std::shared_ptr<std::string>type);
    void KillService(uint32_t id);
    bool Name(uint32_t id, std::shared_ptr<std::string> name);
    uint32_t QueryName(const std::string& name);
    std::shared_ptr<std::string> GetServiceName(uint32_t id);

private:
    std::shared_ptr<Service> GetService(uint32_t id);

private:
    int WORKER_NUM=3;
    std::vector<Worker*>workers;
    std::vector<std::thread*>workerThreads;
    static Sunnet *inst;

    void StartWorker();

private:
    std::queue<std::shared_ptr<Service>>globalQueue;
    int globalLen=0;
    pthread_spinlock_t globalLock;

public:
    void Send(uint32_t toId,std::shared_ptr<BaseMsg>msg);
    void PushGlobalQueue(std::shared_ptr<Service>);
    std::shared_ptr<Service> PopGlobalQueue();
    std::shared_ptr<BaseMsg> MakeMsg(uint32_t sourse,char* buff,int len);

private:
    pthread_mutex_t sleepMtx;
    pthread_cond_t sleepCond;
    int sleepCount=0;

public:
    void checkAndWeakUp();
    void WorkerWait();

private:
    SocketWorker *socketWorker;
    std::thread* socketThread;
    void StartSocket();

private:
    std::unordered_map<uint32_t,std::shared_ptr<Conn>>Conns;
    pthread_rwlock_t connsLock;

public:
    int AddConn(int fd,uint32_t id,Conn::TYPE type);
    bool RemoveConn(int fd);
    std::shared_ptr<Conn> GetConn(int fd);

public:
    int Listen(uint32_t port,uint32_t serviceId);
    void ModifyEvent(int fd,bool epollout);
    void Closeconn(uint32_t fd);

public:
    bool Write(uint32_t serviceId, int fd, std::shared_ptr<char> buff, int len);
    bool LingerClose(uint32_t serviceId, int fd);

public:
    std::priority_queue<TimerNode, std::vector<TimerNode>, TimerCompare> timerQueue;
    pthread_mutex_t timerMtx;
    pthread_cond_t timerCond;
    TimerWorker* timerWorker;
    std::thread* timerThread;

    void StartTimer();

public:
    void Timeout(uint32_t serviceId, uint32_t session, uint32_t ms);
};
```

### 这里有几个点



### 3. 新增 `src/TimerWorker.cpp`

```cpp
#include "TimerWorker.h"
#include "Sunnet.h"
#include "Msg.h"
#include <ctime>

static timespec MakeMonotonicDeadline(uint64_t ms){
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;

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

        while(inst->timerQueue.empty()){
            pthread_cond_wait(&inst->timerCond, &inst->timerMtx);
        }

        while(!inst->timerQueue.empty()){
            TimerNode node = inst->timerQueue.top();
            auto now = std::chrono::steady_clock::now();

            if(node.expire <= now){
                inst->timerQueue.pop();
                pthread_mutex_unlock(&inst->timerMtx);

                std::shared_ptr<TimerMsg> msg = std::make_shared<TimerMsg>();
                msg->type = BaseMsg::TYPE::TIMER;
                msg->session = node.session;
                inst->Send(node.serviceId, msg);

                pthread_mutex_lock(&inst->timerMtx);
                continue;
            }

            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(node.expire - now).count();
            timespec ts = MakeMonotonicDeadline((uint64_t)diff);

            pthread_cond_timedwait(&inst->timerCond, &inst->timerMtx, &ts);
            break;
        }

        pthread_mutex_unlock(&inst->timerMtx);
    }
}
```

### 这里的关键点

- `timerQueue` 用 `steady_clock`
- `timerCond` 会被初始化成 `CLOCK_MONOTONIC`
- `pthread_cond_timedwait` 要吃绝对时间，所以这里转成 `CLOCK_MONOTONIC` 的绝对 `timespec`
- 被新定时器 `signal` 唤醒后，直接回到外层重新检查堆顶

第一版这样够用了。


### 4. 修改 `src/Sunnet.cpp`

#### 先补 include

文件头部加上：

```cpp
#include "TimerWorker.h"
```

#### `Start()` 里初始化 timer 锁和条件变量

把 [src/Sunnet.cpp](/home/a/桌面/sunnet/src/Sunnet.cpp:22) 的 `Start()` 改成：

```cpp
void Sunnet::Start(){
    cout<<"Hello Sunnet"<<endl;
    signal(SIGPIPE,SIG_IGN);
    pthread_rwlock_init(&serviceLock,NULL);
    pthread_spin_init(&globalLock,PTHREAD_PROCESS_PRIVATE);
    pthread_mutex_init(&sleepMtx,NULL);
    pthread_cond_init(&sleepCond,NULL);
    pthread_rwlock_init(&connsLock,NULL);
    pthread_mutex_init(&timerMtx, NULL);

    pthread_condattr_t timerCondAttr;
    pthread_condattr_init(&timerCondAttr);
    pthread_condattr_setclock(&timerCondAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&timerCond, &timerCondAttr);
    pthread_condattr_destroy(&timerCondAttr);

    StartWorker();
    StartSocket();
    StartTimer();
}
```

`timerCond` 必须显式设置成 `CLOCK_MONOTONIC`，因为 `TimerWorker` 里给 `pthread_cond_timedwait` 传的是 `CLOCK_MONOTONIC` 算出来的绝对时间。

如果这里用默认初始化，`pthread_cond_timedwait` 默认按 `CLOCK_REALTIME` 解释 `timespec`，系统时间调整会影响等待语义。

#### 新增 `StartTimer()`

```cpp
void Sunnet::StartTimer(){
    timerWorker = new TimerWorker();
    timerThread = new thread(*timerWorker);
}
```

#### 新增 `Timeout(...)`

```cpp
void Sunnet::Timeout(uint32_t serviceId, uint32_t session, uint32_t ms){
    TimerNode node;
    node.expire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    node.serviceId = serviceId;
    node.session = session;

    pthread_mutex_lock(&timerMtx);
    {
        bool wake = timerQueue.empty() || node.expire < timerQueue.top().expire;
        timerQueue.push(node);
        if(wake){
            pthread_cond_signal(&timerCond);
        }
    }
    pthread_mutex_unlock(&timerMtx);
}
```

### `Wait()` 先不用管 timerThread

你现在 [src/Sunnet.cpp](/home/a/桌面/sunnet/src/Sunnet.cpp:51) 的 `Wait()` 只 `join` worker 线程，socket 线程本身也没 join。

所以第一版定时器也先不强行补完整停机流程，保持和当前工程一致。


### 5. 修改 `include/Service.h`

在 private 回调区域加一个：

```cpp
void OnTimerMsg(std::shared_ptr<TimerMsg> msg);
```

也就是改成：

```cpp
private:
    void OnServiceMsg(std::shared_ptr<ServiceMsg> msg);
    void OnAcceptMsg(std::shared_ptr<SocketAcceptMsg>msg);
    void OnRWMsg(std::shared_ptr<SocketRWMsg>msg);
    void OnTimerMsg(std::shared_ptr<TimerMsg> msg);
```


### 6. 修改 `src/Service.cpp`

#### 在 `OnMsg()` 里加 `TIMER` 分支

把 [src/Service.cpp](/home/a/桌面/sunnet/src/Service.cpp:145) 的 `OnMsg()` 改成：

```cpp
void Service::OnMsg(std::shared_ptr<BaseMsg>msg){
    if(msg->type==BaseMsg::TYPE::SERVICE){
        std::shared_ptr<ServiceMsg>m= std::dynamic_pointer_cast<ServiceMsg>(msg);
        OnServiceMsg(m);
    }else if(msg->type==BaseMsg::TYPE::SOCKET_ACCEPT){
        std::shared_ptr<SocketAcceptMsg>m=std::dynamic_pointer_cast<SocketAcceptMsg>(msg);
        OnAcceptMsg(m);
    }else if(msg->type==BaseMsg::TYPE::SOCKET_RW){
        std::shared_ptr<SocketRWMsg>m=std::dynamic_pointer_cast<SocketRWMsg>(msg);
        OnRWMsg(m);
    }else if(msg->type==BaseMsg::TYPE::TIMER){
        std::shared_ptr<TimerMsg>m=std::dynamic_pointer_cast<TimerMsg>(msg);
        OnTimerMsg(m);
    }
}
```

#### 新增 `OnTimerMsg()`

```cpp
void Service::OnTimerMsg(std::shared_ptr<TimerMsg> msg){
    std::cout << "[" << id << "] OnTimerMsg session=" << msg->session << std::endl;
}
```

第一版这里只打印，不恢复 coroutine。


## 第一版测试方法

先不要接 Lua。

先在 [src/Service.cpp](/home/a/桌面/sunnet/src/Service.cpp:106) 的 `OnInit()` 里临时加一条测试：

```cpp
Sunnet::get_instance()->Timeout(id, 123, 1000);
```

建议放在 `OnInit` Lua 回调成功之后：

```cpp
    isok=lua_pcall(luaState,1,0,0);
    if(isok!=0){
        std::cout<<"call lua OnInit failed"<<lua_tostring(luaState,-1)<<std::endl;
        lua_close(luaState);
        luaState=nullptr;
        return false;
    }

    Sunnet::get_instance()->Timeout(id, 123, 1000);
    return true;
```

然后运行程序，预期看到：

```text
[0] OnTimerMsg session=123
```

如果能看到，说明这条链已经通了：

- timer 线程启动成功
- `Timeout()` 成功入堆
- timer 到期成功出堆
- `TimerMsg` 成功投递
- `Service::OnMsg()` 成功分发到 `OnTimerMsg()`


## 这一步先别做什么

这一步不要一起做：

- `sunnet.Sleep(ms)`
- coroutine 恢复
- 取消定时器
- 周期定时器
- Lua `OnTimeout(...)`

先把底层定时器链条单独跑通，再往上接。


## 你现在最该做的顺序

1. 先按这份文档把 `TimerWorker.h/.cpp` 建出来
2. 把 `Sunnet.h/.cpp` 的 timer 字段和 `Timeout()` 补上
3. 把 `Service.h/.cpp` 的 `OnTimerMsg()` 接上
4. 用 `OnInit()` 里临时 `Timeout(id, 123, 1000)` 验证
5. 通过后再删掉这个临时测试
6. 下一步再接 `session -> coroutine ref` 和 `sunnet.Sleep(ms)`

这一步不要贪多，先把“1 秒后能收到 `TimerMsg`”做实。
