#pragma once

extern "C"{
    #include "lua.h"
    #include "lauxlib.h"
    #include "lualib.h"
}

#include <queue>
#include<memory>

#include<string>
#include"Msg.h"
#include<unordered_map>
class Sunnet;

class ConnWriter;

class Service{
    public:
    uint32_t id;//唯一id
    std::shared_ptr<std::string>type;//服务类型名
    std::shared_ptr<std::string>name;//由用户注册的服务名
    //是否正在退出
    bool is_Exiting=false;
    //消息队列
    std::queue<std::shared_ptr<BaseMsg>>msgQueue;
    //锁
    pthread_spinlock_t queueLock;
    
    //是否在全局队列
    bool inGlobal=false;
    pthread_spinlock_t inGlobalLock;

    //安全设置inGlobal
    void SetInGlobal(bool isIn);

    Service();
    ~Service();

    //回调函数（编写服务逻辑）
    bool OnInit();//创建服务回调
    void OnMsg(std::shared_ptr<BaseMsg>);//收到消息时回调
    void OnExit();//退出服务时触发

    //插入消息
    void PushMsg(std::shared_ptr<BaseMsg>);
    //执行消息
    bool processMsg();
    void processMsgs(int max);
    


    private:
    //取出一条消息
    std::shared_ptr<BaseMsg> PopMsg();

    private:
    //OnMsg中用，处理不同类型消息
    void OnServiceMsg(std::shared_ptr<ServiceMsg> msg);//处理服务间消息
    void OnAcceptMsg(std::shared_ptr<SocketAcceptMsg>msg);//处理接受连接的通知消息
    void OnRWMsg(std::shared_ptr<SocketRWMsg>msg);//处理读写通知消息
    

    //下面的方法是要用网络模块才会用到的
    //由用户编写：收到数据回调/可写/关闭连接前的清理工作
    void OnSocketData(int fd,const char* buff,int len);
    void OnSocketWritable(int fd);
    void OnSocketClose(int fd);

    //同样，只有网络模块用到
    private:
    std::unordered_map<int,std::shared_ptr<ConnWriter>>writers;//写缓冲区表，fd->对应的写缓冲区

    private:
    lua_State *luaState;

    public:
    bool WriteConn(int fd,std::shared_ptr<char> buff,int len);//封装一下connwriter的EntireWrite
    bool LingerClose(int fd);//封装一下connwriter的lingerClose

    private:
    void OnTimerMsg(std::shared_ptr<TimerMsg> msg);
    void OnRetMsg(std::shared_ptr<ServiceMsg> msg);//处理ret消息
    public:
    uint32_t nextSession = 1;//协程出让号,递增
    //暂停的协程表
    std::unordered_map<uint32_t, int> sessionCoRefs;//[出让号]->协程引用

    uint32_t NewSession();//分配新的出让号
    //LuaApi::sleep调用，获得新的出让号，保存协程对象到register表，获得的coRef保存到出让表。返回新的出让号
    //同时设置定时器
    //保存到出让表是resume协程时要根据出让号查找协程引用
    //保存到register表是防止lua gc释放协程引用
    uint32_t SleepCurrentCoroutine(lua_State* co, int ms);
    //resume协程，并把协程引用从它的register表删除，返回执行状态
    int ResumeCoroutineRef(int coRef, int nargs);
    
    struct RpcContext{
        uint32_t source = 0;
        uint32_t session = 0;
        bool reted = false;
    };
    //正在处理 CALL 请求的 coroutine 上下文: coroutine -> rpc context
    std::unordered_map<lua_State*, RpcContext> coRpcContexts;
    //启动协程
    void StartCallbackCoroutine(int nargs, RpcContext* rpcCtx);
};