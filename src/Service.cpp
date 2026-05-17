#include "Service.h"
#include "Sunnet.h"
#include <iostream>
#include<unistd.h>
#include<string.h>
#include "ConnWriter.h"
#include <string>
#include "LuaApi.h"
Service::Service(){
    pthread_spin_init(&queueLock,PTHREAD_PROCESS_PRIVATE);
    pthread_spin_init(&inGlobalLock,PTHREAD_PROCESS_PRIVATE);
}
Service::~Service(){
    pthread_spin_destroy(&queueLock);
    pthread_spin_destroy(&inGlobalLock);
}
//从p到end之间读出一个uint32保存到v，并把p往后移4位，static表示只是这个cpp文件内部可见的工具函数，而且也不需要this，
static bool ReadUint32(char* &p,char* end,uint32_t &v){
    if(p+sizeof(uint32_t)>end){
        return false;
    }
    v=*(uint32_t*)p;
    p+=sizeof(uint32_t);
    return true;
}
//从序列化字符串解析出一个参数压入栈
static bool UnpackValue(lua_State* luaState, char*& p,char* end,int depth){
    if(p >= end){
        return false;
    }
    uint8_t type=*(uint8_t*)p;//参数类型
    p+=1;

        if(type==LuaApi::TYPE_INTEGER){
            if(p + sizeof(lua_Integer) > end){
                return false;
            }
            lua_Integer v=*(lua_Integer*) p;
            lua_pushinteger(luaState,v);

            p+=sizeof(lua_Integer);
            return true;
        }else if(type==LuaApi::TYPE_NUMBER){
             if(p + sizeof(lua_Number) > end){
                return false;
            }
            lua_Number v = *(lua_Number*)p;
            lua_pushnumber(luaState, v);

            p += sizeof(lua_Number);
            return true;
        }else if(type==LuaApi::TYPE_STRING){

            uint32_t len;
            //如果长度不够uint32或者不够字符串了
            if(!ReadUint32(p,end,len)||p+len>end){
                return false;
            }
            lua_pushlstring(luaState,p,len);
            p+=len;
            return true;
        }else if(type==LuaApi::TYPE_TABLE){
            uint32_t pairCount;
            if(!ReadUint32(p,end,pairCount)){
                return false;
            }
            lua_createtable(luaState,0,pairCount);
            for(uint32_t i=0;i<pairCount;i++){
                if(!UnpackValue(luaState,p,end,depth+1)){//解析键
                    return false;
                }
                //返回这里后p指向序列化的值最低地址
                 if(!UnpackValue(luaState,p,end,depth+1)){//解析值
                    return false;
                }
                lua_settable(luaState,-3);//键值插入表
            }
            return true;
        }
        //暂未支持的类型
        return false;
}

//启动协程
void Service::StartCallbackCoroutine(int nargs,RpcContext* rpcCtx){
    lua_State* co = lua_newthread(luaState);

    //把协程引用移到函数和参数前面，保存协程在栈防止被gc回收
    lua_insert(luaState, lua_gettop(luaState) - nargs-1);
    
    //把函数和参数移到协程栈
    lua_xmove(luaState, co, nargs+1);
    //如果有rpc上下文，保存
    if(rpcCtx != nullptr){
        coRpcContexts[co] = *rpcCtx;
    }
    
    int status = lua_resume(co, luaState, nargs);
    if(status == LUA_OK){
        //协程处理rpc完毕，删除rpc上下文
        coRpcContexts.erase(co);
        lua_pop(luaState, 1);
    }else if(status == LUA_YIELD){
        // co 之前挂在主栈上保活，现在可以弹掉了。
        // 阻塞方法已经把它登记进 sessionCoRefs 和 registry。
        lua_pop(luaState, 1);
    }else{
        //协程处理rpc异常结束，删除rpc上下文
        coRpcContexts.erase(co);
        std::cout << "start callback coroutine failed"<< " "<< lua_tostring(co, -1)<< std::endl;
        lua_pop(co, 1);
        lua_pop(luaState, 1);
    }

}

void Service::PushMsg(std::shared_ptr<BaseMsg> msg){
    pthread_spin_lock(&queueLock);
    {
    msgQueue.push(msg);
    }
    pthread_spin_unlock(&queueLock);
}

std::shared_ptr<BaseMsg> Service::PopMsg(){
    std::shared_ptr<BaseMsg>msg=NULL;
    pthread_spin_lock(&queueLock);
    {
        if(!msgQueue.empty()){
            msg=msgQueue.front();
            msgQueue.pop();
        }
    }

    pthread_spin_unlock(&queueLock);
    return msg;
}

bool Service::OnInit(){

    std::cout<<"["<<id<<"] OnInit"<<std::endl;
    luaState=luaL_newstate();
    if(luaState==nullptr){
        return false;
    }
    luaL_openlibs(luaState);

    //把sunnet的api注册进lua全局空间
    LuaApi::Register(luaState);

    // 把当前服务id存到registry，Lua脚本改不到,LuaApi::killservice里面要判断使用
    lua_pushinteger(luaState, id);
    lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_id");
    // LuaApi::Sleep 需要通过 registry 找回当前 Service，保存当前服务地址
    lua_pushlightuserdata(luaState, this);
    lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    std::string filename="../service/"+*type+"/init.lua";
    int isok=luaL_dofile(luaState,filename.data());
    if(isok==1){//成功返回0，失败返回1
        std::cout<<"run lua file: "<<lua_tostring(luaState,-1)<<std::endl;//启动失败，错误在栈顶
        lua_close(luaState);
        luaState=nullptr;
        return false;
    }

    lua_getglobal(luaState,"OnInit");//把名为OnInit的变量压栈
    lua_pushinteger(luaState,id);
    isok=lua_pcall(luaState,1,0,0);
    if(isok!=0){//返回值不为0，失败
        std::cout<<"call lua OnInit failed"<<lua_tostring(luaState,-1)<<std::endl;
        lua_close(luaState);
        luaState=nullptr;
        return false;
    }
    return true;
}

void Service::OnMsg(std::shared_ptr<BaseMsg>msg){
    Sunnet* inst=Sunnet::get_instance();

    if(msg->type==BaseMsg::TYPE::SERVICE){
        std::shared_ptr<ServiceMsg>m= std::dynamic_pointer_cast<ServiceMsg>(msg);//专门转化智能指针的
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

void Service::OnExit(){
    std::cout<<"["<<id<<"] OnExit"<<std::endl;
    lua_getglobal(luaState,"OnExit");//把名为OnExit的变量压栈
    int isok=lua_pcall(luaState,0,0,0);
    if(isok!=0){//返回值不为0，失败
        std::cout<<"call lua OnExit failed"<<lua_tostring(luaState,-1)<<std::endl;
    }
    lua_close(luaState);
}

bool Service::processMsg(){
    std::shared_ptr<BaseMsg>msg= PopMsg();
    if (msg){
        OnMsg(msg);

        if (is_Exiting){// OnExit()放到这里执行，不要放在Sunnet::KillService里面
            OnExit();
            return false;
        }

        return true;
    }else{
        return false;
    }
    
}

void Service::processMsgs(int max){
    for(int i=0;i<max;i++){
        bool succ=processMsg();
        if(not succ){
            break;
        }
    }
}

void Service::SetInGlobal(bool isIn){
    pthread_spin_lock(&inGlobalLock);
    {
        inGlobal=isIn;
    }
    pthread_spin_unlock(&inGlobalLock);
}


void Service::OnServiceMsg(std::shared_ptr<ServiceMsg> msg){
    if(msg->kind == ServiceMsg::RET){
        OnRetMsg(msg);
        return;
    }

    int top = lua_gettop(luaState);//记录栈顶索引

    lua_getglobal(luaState,"OnServiceMsg");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }
    
    lua_pushinteger(luaState, msg->session);
    lua_pushinteger(luaState,msg->sourse);

    char* p=msg->buff.get();
    char* end=msg->buff.get()+msg->size;
    uint8_t argc=(uint8_t)p[0];//参数个数
    p+=1;

    

    for(uint8_t i=0;i<argc;i++){
        
        if(!UnpackValue(luaState,p,end,0)){
            std::cout<<"unknown  or broken msg arg type"<<std::endl;
            //Lua 栈恢复到之前记录的高度
            lua_settop(luaState, top);
            return;
        }

    }
    //如果是call类型的服务消息，要记录当前协程正在执行的rpc上下文
    //rpc返回时，根据记录的上下文发ret消息
    if(msg->kind == ServiceMsg::CALL){
        RpcContext ctx;
        ctx.source = msg->sourse;
        ctx.session = msg->session;
        ctx.reted = false;
        StartCallbackCoroutine(2 + argc, &ctx);
    }else{
        StartCallbackCoroutine(2 + argc, nullptr);
    }
     
}

void Service::OnAcceptMsg(std::shared_ptr<SocketAcceptMsg>msg){
    std::cout<<"OnAcceptMsg,fd: "<<msg->clientfd<<std::endl;
    Sunnet *inst=Sunnet::get_instance();
    //为通信套接字创建写缓冲区,并保存映射关系到哈希表
    std::shared_ptr<ConnWriter> w=std::make_shared<ConnWriter>();
    w->conn=inst->GetConn(msg->clientfd);
    writers.emplace(msg->clientfd,w);

    // 调 Lua: OnAcceptMsg(listenfd, clientfd)
    lua_getglobal(luaState, "OnAcceptMsg");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }

    lua_pushinteger(luaState, msg->listenfd);
    lua_pushinteger(luaState, msg->clientfd);

    StartCallbackCoroutine(2, nullptr);
}

void Service::OnRWMsg(std::shared_ptr<SocketRWMsg>msg){

    auto conn = msg->conn;
    //如果此连接已经被关闭，fd被重新分配，但是消息队列仍然有这个连接残留的读写通知消息，这里可以拦住，防止已关闭的连接干扰新连接
    if(!conn || conn->closed){
        return;
    }

    int fd=conn->fd;
    Sunnet* inst=Sunnet::get_instance();
    if(msg->isRead&& (writers[fd]->isClosing==false||msg->isCloseReq==true)){
        //如果连接在延迟关闭状态，不再处理普通读的请求消息，只处理要求关闭的那个读消息
        const int BUFFSIZE=512;
        char buff[BUFFSIZE];
        int len=0;
        do{
            len=read(fd,buff,BUFFSIZE);
            if(len>0){
                OnSocketData(fd,buff,len);
            }
        }while(len==BUFFSIZE);

    
         if(len<=0&&errno!=EAGAIN){//如果epoll那里套接字发生错误，或者读被半关闭了，会发rwmsg,这里会关闭
            if(!conn->closed){//不能重复删除关闭连接
                conn->closed=true;
                OnSocketClose(fd);
                inst->Closeconn(fd);
            }
         }
    }
    if(msg->isWrite){
        if(!conn->closed){//连接如果清理了，就没必要写了，而且此时写缓冲区对象这时肯定不存在了
            OnSocketWritable(fd);
        }
    }
}

void Service::OnSocketData(int fd,const char* buff,int len){
    std::cout<<"OnSocketData ,fd: "<<fd<<" ,buff: "<<buff<<std::endl;
    // 调 Lua: OnSocketData(fd, buff)
    lua_getglobal(luaState, "OnSocketData");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }

    lua_pushinteger(luaState, fd);
    lua_pushlstring(luaState, buff, len); // 这里一定要用 pushlstring，不能假设 buff 以 \0 结尾

    int isok = lua_pcall(luaState, 2, 0, 0);
    if(isok != 0){
        std::cout << "call lua OnSocketData failed " << lua_tostring(luaState, -1) << std::endl;
        lua_pop(luaState, 1);
    }
}
void Service::OnSocketWritable(int fd){
    std::cout<<"OnSocketWritable"<<fd<<std::endl;
    //可写，回调fd对应缓冲区的写函数
    std::shared_ptr<ConnWriter>w=writers[fd];
    w->OnWriteable();
}
void Service::OnSocketClose(int fd){
    std::cout<<"OnSocketClose"<<fd<<std::endl;
    //把fd和写缓冲区从哈希表删去
    writers.erase(fd);

     // 调 Lua: OnSocketClose(fd)
    lua_getglobal(luaState, "OnSocketClose");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }

    lua_pushinteger(luaState, fd);

    StartCallbackCoroutine(1, nullptr);
    
}

bool Service::WriteConn(int fd,std::shared_ptr<char> buff,int len){
    auto it=writers.find(fd);
    if (it==writers.end()){
        return false;

    }
    it->second->EntireWrite(buff,len);
    return true;

}

bool Service::LingerClose(int fd){
    auto it = writers.find(fd);
    if(it == writers.end() || !it->second){
        return false;
    }
    it->second->LingerClose();
    return true;
}

void Service::OnTimerMsg(std::shared_ptr<TimerMsg> msg){
    //先找挂起的协程coRef
    auto it = sessionCoRefs.find(msg->session);
    if(it == sessionCoRefs.end()){
        std::cout << "[" << id << "] timer session not found " << msg->session << std::endl;
        return;
    }
    //取出然后清除挂起信息
    int coRef = it->second;
    sessionCoRefs.erase(it);

    ResumeCoroutineRef(coRef,0);
}

uint32_t Service::NewSession(){
    uint32_t session = nextSession++;
    if(nextSession == 0){//如果32位满了就重置为1，不用0，作为保留值
        nextSession = 1;
    }
    return session;
}

uint32_t Service::SleepCurrentCoroutine(lua_State* co, int ms){
    uint32_t session=NewSession();

    //协程对象保存进register表
    lua_pushthread(co);
    int coRef=luaL_ref(co,LUA_REGISTRYINDEX);

    //coRef保存到出让表
    sessionCoRefs[session]=coRef;

    //设置定期器
    if(ms >= 0){
        Sunnet::get_instance()->Timeout(id, session, (uint32_t)ms);
    }

    return session;
}

int Service::ResumeCoroutineRef(int coRef, int nargs){
    //先从register表把协程引用拿出来
    lua_rawgeti(luaState,LUA_REGISTRYINDEX, coRef);
    lua_State* co=lua_tothread(luaState,-1);
    lua_pop(luaState, 1);
    
    if(co == nullptr){
        std::cout << "resume coroutine failed, co is null" << std::endl;
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
        return LUA_ERRRUN;
    }

    //resume协程
    int status=lua_resume(co,luaState,nargs);
    if(status == LUA_OK){
        //协程处理rpc完毕，删除rpc上下文（如果有）
        coRpcContexts.erase(co);
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }else if(status == LUA_YIELD){
        // 阻塞api已经给这个协程在register表里面新加了引用，这个旧 ref 可以释放。
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }else{
        //协程处理rpc异常结束，删除rpc上下文
        coRpcContexts.erase(co);
        std::cout << "resume coroutine failed"<< " "<< lua_tostring(co, -1)<< std::endl;
        lua_pop(co, 1);
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }
    return status;
}

void Service::OnRetMsg(std::shared_ptr<ServiceMsg> msg){
    //先找挂起的协程coRef
    auto it = sessionCoRefs.find(msg->session);
    if(it == sessionCoRefs.end()){
        std::cout << "[" << id << "] ret session not found " << msg->session << std::endl;
        return;
    }
    //取出然后清除挂起信息
    int coRef = it->second;
    sessionCoRefs.erase(it);
    //从register表拿协程栈
    lua_rawgeti(luaState, LUA_REGISTRYINDEX, coRef);
    lua_State* co = lua_tothread(luaState, -1);
    lua_pop(luaState, 1);

    int top = lua_gettop(co);

    char* p = msg->buff.get();
    char* end = msg->buff.get() + msg->size;
    uint8_t argc = (uint8_t)p[0];
    p += 1;
    //把返回值一一压栈
    for(uint8_t i=0;i<argc;i++){
        if(!UnpackValue(co, p, end, 0)){
            std::cout << "unknown or broken ret arg type" << std::endl;
            //清理前面压入的参数，恢复栈
            lua_settop(co, top);    
            //压入错误信息
            lua_pushnil(co);
            lua_pushstring(co, "broken ret arg");
            ResumeCoroutineRef(coRef, 2);
            return;
        }
    }
    ResumeCoroutineRef(coRef, argc);
}