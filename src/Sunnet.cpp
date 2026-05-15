#include <iostream>
#include "Sunnet.h"
#include "Worker.h"
#include "Service.h"
#include "SocketWorker.h"
#include "TimerWorker.h"
#include <unistd.h>
#include<fcntl.h>
#include <sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include <signal.h>
using namespace std;
//饿汉式
Sunnet* Sunnet::inst = new Sunnet();


//获取实例
Sunnet* Sunnet::get_instance(){
    return inst;
}

//开启系统
void Sunnet::Start(){
    cout<<"Hello Sunnet"<<endl;
    signal(SIGPIPE,SIG_IGN);
    pthread_rwlock_init(&serviceLock,NULL);
    pthread_spin_init(&globalLock,PTHREAD_PROCESS_PRIVATE);
    pthread_mutex_init(&sleepMtx,NULL);
    pthread_cond_init(&sleepCond,NULL);
    pthread_rwlock_init(&connsLock,NULL);
    pthread_mutex_init(&timerMtx, NULL);
    //设置条件变量属性，使用单调时钟
    pthread_condattr_t timerCondAttr;
    pthread_condattr_init(&timerCondAttr);
    pthread_condattr_setclock(&timerCondAttr,CLOCK_MONOTONIC);
    pthread_cond_init(&timerCond, &timerCondAttr);
    pthread_condattr_destroy(&timerCondAttr);
    //开启worker
    StartWorker();
    //开启socket线程
    StartSocket();
    //开启timer线程
    StartTimer();
}

//开启工作线程
void Sunnet::StartWorker(){
    for(int i=0;i<WORKER_NUM;i++){
        cout<<"start worker thread:"<<i<<endl;
        Worker* worker=new Worker();
        worker->id=i;
        worker->eachNum=2<<i;
        thread *wt=new thread(*worker,inst);
        workers.push_back(worker);
        workerThreads.push_back(wt);
    }
}

//等待线程退出
void Sunnet::Wait(){
    for(int i=0;i<workerThreads.size();i++){
        if(workerThreads[i]){
            workerThreads[i]->join();
        }
    }
    
}

uint32_t Sunnet::NewService(std::shared_ptr<string>type){
    std::shared_ptr<Service> srv=std::make_shared<Service>();
    srv->type=type;
    pthread_rwlock_wrlock(&serviceLock);
    {
        srv->id=maxId;
        maxId++;
        services.emplace(srv->id,srv);
    }
    pthread_rwlock_unlock(&serviceLock);
    //用户注册的初始化函数
    if(!srv->OnInit()){
        pthread_rwlock_wrlock(&serviceLock);
        {
            services.erase(srv->id);
        }
        pthread_rwlock_unlock(&serviceLock);
        return (uint32_t)-1;//这里返回一个不正常的服务号，代表失败，因为我们是无符号整数，不能返回-1
    }
    return srv->id;
}

std::shared_ptr<Service> Sunnet::GetService(uint32_t id){
    std::shared_ptr<Service> srv=NULL;
    pthread_rwlock_rdlock(&serviceLock);
    {
        unordered_map<uint32_t,std::shared_ptr<Service>>::iterator iter= services.find(id);
        if (iter!=services.end()){
            srv=iter->second;
        }
    }
    pthread_rwlock_unlock(&serviceLock);

    return srv;
}

//sunnet系统要求只能由服务自己在消息处理函数删除自身。
//这是因为没有对onExit和is_Exiting=true加锁，线程不安全
void Sunnet::KillService(uint32_t id){
    std::shared_ptr<Service>srv= GetService(id);
    if(!srv){
        return;
    }

    //退出前要执行用户注册的退出函数，
    //但是不要在这里执行
    //OnExit里面会lua_close，因为killService是通过lua_pcall启动的Lua函数调用的，killService返回到lua层OnService，再返回到c++层的OnService的lua_pcall会导致luaState为空，会导致lua_pcall保存
    //等从c++的OnService返回到processmsg再执行OnExit
    srv->is_Exiting=true;//标记正在退出

    pthread_rwlock_wrlock(&serviceLock);
    {
        if(srv->name){//有名字的话把名字删了
            names.erase(*srv->name);
        }
        services.erase(id);
    }
    pthread_rwlock_unlock(&serviceLock);
}

//弹出全局队列
std::shared_ptr<Service>  Sunnet::PopGlobalQueue(){
    std::shared_ptr<Service> srv=NULL;
    pthread_spin_lock(&globalLock);
    {
        if(!globalQueue.empty()){
            srv= globalQueue.front();
            globalQueue.pop();
            globalLen--;
        }
        
    }
    pthread_spin_unlock(&globalLock);
    return srv;
}

//压入全局队列
void Sunnet::PushGlobalQueue(std::shared_ptr<Service> srv){
    pthread_spin_lock(&globalLock);
    {
        globalQueue.push(srv);
        globalLen++;
    }
    pthread_spin_unlock(&globalLock);
}

//发送消息
bool Sunnet::Send(uint32_t toId,std::shared_ptr<BaseMsg> msg){
    std::shared_ptr<Service>toSrv= GetService(toId);
    if(toSrv==NULL){
        cout<<"send failed ,toSrv not exist ,toid: "<<toId<<endl;
        return false;
    }
    toSrv->PushMsg(msg);

    //检测是否位于全局队列
    bool hasPush=false;
    pthread_spin_lock(&toSrv->inGlobalLock);
    {
        if(toSrv->inGlobal==false){//读并写inGlobal是最小临界区
            PushGlobalQueue(toSrv);//同时操作局部锁和全局锁时，最好让全局锁包裹在局部锁，优先减小全局锁的临界区
            toSrv->inGlobal=true;
            hasPush=true;
        }
    }
    pthread_spin_unlock(&toSrv->inGlobalLock);

    if(hasPush){
        checkAndWeakUp();
    }
    return true;
}

std::shared_ptr<BaseMsg> Sunnet::MakeMsg(uint32_t sourse,char* buff,int len){
    std::shared_ptr<ServiceMsg> msg=make_shared<ServiceMsg>();
    msg->type=BaseMsg::TYPE::SERVICE;
    msg->sourse=sourse;
    //让智能指针接管c字符串，c字符串没有析构函数。智能指针通过delete销毁对象，所以这里可以通过智能指针管理c风格字符串，无需重写智能指针的销毁方法
    msg->buff=shared_ptr<char>(buff);
    msg->size=len;
    return msg;

}

void Sunnet::checkAndWeakUp(){
    //send后如果插入了全局队列，调用这个函数
    //因为send非常高频，这里的sleepCount就不加锁了
    //因为这里的sleepCount读时没有加锁。有可能不准
    //有可能出现该唤醒时未唤醒（sleepCount读小了）或者不该唤醒时唤醒（sleepCount读大了）
    //不过，不影响系统正确性，无非就是让下次send操作队列后再唤醒，和空转一次罢了。
    if(sleepCount==0){//没有需要唤醒的
        return;
    }
    if(WORKER_NUM-sleepCount<=globalLen){//如果当前的要处理的服务数比醒着的线程多
        cout<<"weak up"<<endl;
        pthread_cond_signal(&sleepCond);
    }
}

void Sunnet::WorkerWait(){
    pthread_mutex_lock(&sleepMtx);
    {
        sleepCount++;
        pthread_cond_wait(&sleepCond,&sleepMtx);
        sleepCount--;
    }
    pthread_mutex_unlock(&sleepMtx);
}

void Sunnet::StartSocket(){
    socketWorker=new SocketWorker();
    socketWorker->init();
    socketThread=new thread(*socketWorker);
}

int Sunnet::AddConn(int fd,uint32_t id,Conn::TYPE type){
    std::shared_ptr<Conn>conn=std::make_shared<Conn>();
    conn->fd=fd;
    conn->type=type;
    conn->serviceId=id;

    pthread_rwlock_wrlock(&connsLock);
    {
        Conns.emplace(fd,conn);
    }
    pthread_rwlock_unlock(&connsLock);
    return fd;
}

bool Sunnet::RemoveConn(int fd){
    int result;
    pthread_rwlock_wrlock(&connsLock);
    {
        result=Conns.erase(fd);
    }
    pthread_rwlock_unlock(&connsLock);
    return result==1;
}

std::shared_ptr<Conn> Sunnet::GetConn(int fd){
    std::shared_ptr<Conn> conn=NULL;
    pthread_rwlock_rdlock(&connsLock);
    {
        std::unordered_map<uint32_t,std::shared_ptr<Conn>>::iterator it= Conns.find(fd);
        if(it!=Conns.end()){
            conn=it->second;
        }
    }
    pthread_rwlock_unlock(&connsLock);
    return conn;
}


int Sunnet::Listen(uint32_t port,uint32_t serviceId){
    //创建socket
    int listenfd=socket(AF_INET,SOCK_STREAM,0);
    if(listenfd<0) {
        cout<<"listen error ,listenfd <=0"<<endl;
        return -1;
    }
    //设置非阻塞
    fcntl(listenfd,F_SETFL,O_NONBLOCK);
    //绑定
    struct sockaddr_in addr;
    addr.sin_family=AF_INET;
    addr.sin_port=htons(port);
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    int r=bind(listenfd,(struct sockaddr*)&addr,sizeof(addr));
    if (r==-1){
        cout<<"listen error ,bind fail"<<endl;
        return -1;
    }
    //监听
    r=listen(listenfd,64);
    if (r<0){
        return -1;
    }
    //把套接字加到管理结构
    AddConn(listenfd,serviceId,Conn::TYPE::LISTEN);
    //描述符加入epoll集合，这里操作的是SocketWorker线程对象方法，注意线程安全
    socketWorker->AddEvent(listenfd);
    return listenfd;
}

void Sunnet::Closeconn(uint32_t fd){
    //从conns哈希表删掉
    bool succ=RemoveConn(fd);
    //关闭fd
    close(fd);
    //从epoll监听集合删去,这里操作的是SocketWorker线程对象方法，注意线程安全
    if(succ){
         socketWorker->RemoveEvent(fd);
    }
}

void Sunnet::ModifyEvent(int fd,bool epollout){
    socketWorker->ModifyEvent(fd,epollout);
}

bool Sunnet::Name(uint32_t id,std::shared_ptr<std::string> name){
    std::shared_ptr<Service> srv=GetService(id);
    if(!srv||!name){
        return false;
    }

    string n=*name;
    
    pthread_rwlock_wrlock(&serviceLock);
    {
        if(names.find(n)!=names.end()){//重名
            pthread_rwlock_unlock(&serviceLock);
            return false;
        }
        names[n]=id;
        srv->name=name;
    }
    pthread_rwlock_unlock(&serviceLock);
    return true;
}

uint32_t Sunnet::QueryName(const std::string &name){
    uint32_t id=(uint32_t)-1;
    pthread_rwlock_rdlock(&serviceLock);
    {
        std::unordered_map<std::string,uint32_t>::iterator it=names.find(name);
        if(it!=names.end()){
            id=it->second;
        }
    }
    pthread_rwlock_unlock(&serviceLock);
    return id;
}

std::shared_ptr<std::string> Sunnet::GetServiceName(uint32_t id){
    std::shared_ptr<Service> srv = GetService(id);
    if(!srv){
        return nullptr;
    }
    return srv->name;
}

bool Sunnet::Write(uint32_t serviceId,int fd,std::shared_ptr<char>buff,int len){
    auto srv=GetService(serviceId);
    if(!srv){
        return false;
    }
    return srv->WriteConn(fd,buff,len);
}


bool Sunnet::LingerClose(uint32_t serviceId, int fd){
    auto srv = GetService(serviceId);
    if(!srv){
        return false;
    }
    return srv->LingerClose(fd);
}

void Sunnet::StartTimer(){
    timerWorker = new TimerWorker();
    timerThread = new thread(*timerWorker);
}


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