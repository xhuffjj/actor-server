# 回调协程化实现方案

这份文档写“可以照抄”的第一版协程化代码。

目标是先让服务消息回调可以直接 `yield`：

```lua
function OnServiceMsg(source, msg)
    print("step1", source, msg)
    sunnet.Sleep(1000)
    print("step2", source, msg)
end
```

这一步基于你已经做好的定时器：

- `Sunnet::Timeout(serviceId, session, ms)`
- `TimerMsg`
- `Service::OnTimerMsg(...)`


## 第一版范围

第一版只做：

- `OnServiceMsg`
- `OnAcceptMsg`
- `OnSocketClose`
- `sunnet.Sleep(ms)`
- `TimerMsg` 恢复 coroutine

第一版不做：

- `OnInit` 可挂起
- `OnExit` 可挂起
- `OnSocketData` 可挂起
- `sunnet.Fork`
- `sunnet.Call/Ret`
- 取消定时器
- 按 fd 自动串行执行


## 关键语义

回调协程化以后，某个回调调用 `sunnet.Sleep(ms)` 会挂起当前 coroutine，但不会卡住整个服务。

服务还能继续处理后续消息。

第一版先不协程化 `OnSocketData`，所以 socket 数据回调仍然是当前 `lua_pcall` 语义，不能在里面直接 `sunnet.Sleep(ms)`。


## Lua 版本

项目使用的是 `3rd/lua-5.3.5`。

所以这份代码使用 Lua 5.3 的接口：

```cpp
int lua_resume(lua_State *L, lua_State *from, int nargs);
```

不要写 Lua 5.4 的 `lua_resume(..., int *nresults)` 版本。


## 要改哪些文件

修改：

- [include/Service.h](/home/a/桌面/sunnet/include/Service.h:1)
- [src/Service.cpp](/home/a/桌面/sunnet/src/Service.cpp:1)
- [include/LuaApi.h](/home/a/桌面/sunnet/include/LuaApi.h:1)
- [src/LuaApi.cpp](/home/a/桌面/sunnet/src/LuaApi.cpp:1)


## 1. 修改 `include/Service.h`

在 `Service` 里增加协程等待表和几个工具函数。

建议把消息处理函数整理到一组里。

```cpp
#pragma once

extern "C"{
    #include "lua.h"
    #include "lauxlib.h"
    #include "lualib.h"
}

#include <queue>
#include <memory>
#include <string>
#include <unordered_map>
#include "Msg.h"

class Sunnet;
class ConnWriter;

class Service{
public:
    uint32_t id;
    std::shared_ptr<std::string>type;
    std::shared_ptr<std::string>name;
    bool is_Exiting=false;

    std::queue<std::shared_ptr<BaseMsg>>msgQueue;
    pthread_spinlock_t queueLock;

    bool inGlobal=false;
    pthread_spinlock_t inGlobalLock;

    void SetInGlobal(bool isIn);

    Service();
    ~Service();

    bool OnInit();
    void OnMsg(std::shared_ptr<BaseMsg>);
    void OnExit();

    void PushMsg(std::shared_ptr<BaseMsg>);
    bool processMsg();
    void processMsgs(int max);

private:
    std::shared_ptr<BaseMsg> PopMsg();

private:
    void OnServiceMsg(std::shared_ptr<ServiceMsg> msg);
    void OnAcceptMsg(std::shared_ptr<SocketAcceptMsg>msg);
    void OnRWMsg(std::shared_ptr<SocketRWMsg>msg);
    void OnTimerMsg(std::shared_ptr<TimerMsg> msg);

private:
    void OnSocketData(int fd,const char* buff,int len);
    void OnSocketWritable(int fd);
    void OnSocketClose(int fd);

private:
    std::unordered_map<int,std::shared_ptr<ConnWriter>>writers;

private:
    lua_State *luaState;

    uint32_t nextSession = 1;
    std::unordered_map<uint32_t, int> sessionCoRefs;

    uint32_t NewSession();
    int ResumeCoroutineRef(int coRef, int nargs);

public:
    bool WriteConn(int fd,std::shared_ptr<char> buff,int len);
    bool LingerClose(int fd);

    uint32_t SleepCurrentCoroutine(lua_State* co, uint32_t ms);
};
```

### 说明

- `nextSession` 负责生成等待编号。
- `sessionCoRefs` 保存 `session -> coroutine ref`。
- `SleepCurrentCoroutine(...)` 给 `LuaApi::Sleep` 调用。


## 2. 修改 `Service::OnInit`

在 [src/Service.cpp](/home/a/桌面/sunnet/src/Service.cpp:106) 的 `OnInit()` 里，除了存 `__service_id`，还要存 `__service_ptr`。

找到这段：

```cpp
lua_pushinteger(luaState, id);
lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_id");
```

下面加：

```cpp
lua_pushlightuserdata(luaState, this);
lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
```

完整片段：

```cpp
// 把当前服务id存到registry，Lua脚本改不到
lua_pushinteger(luaState, id);
lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_id");

// LuaApi::Sleep 需要通过 registry 找回当前 Service
lua_pushlightuserdata(luaState, this);
lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
```


## 3. 在 `src/Service.cpp` 增加协程工具函数

建议放在 `Service::SetInGlobal(...)` 后面，`OnServiceMsg(...)` 前面。

```cpp
uint32_t Service::NewSession(){
    uint32_t session = nextSession++;
    if(nextSession == 0){
        nextSession = 1;
    }
    return session;
}

int Service::ResumeCoroutineRef(int coRef, int nargs){
    lua_rawgeti(luaState, LUA_REGISTRYINDEX, coRef);
    lua_State* co = lua_tothread(luaState, -1);
    lua_pop(luaState, 1);

    if(co == nullptr){
        std::cout << "resume coroutine failed, co is null" << std::endl;
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
        return LUA_ERRRUN;
    }

    int status = lua_resume(co, luaState, nargs);
    if(status == LUA_OK){
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }else if(status == LUA_YIELD){
        // Sleep 已经登记了新的 session，这个旧 ref 可以释放。
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }else{
        const char* err = lua_tostring(co, -1);
        std::cout << "resume coroutine failed";
        if(err){
            std::cout << " " << err;
        }
        std::cout << std::endl;
        lua_pop(co, 1);
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }
    return status;
}

uint32_t Service::SleepCurrentCoroutine(lua_State* co, uint32_t ms){
    uint32_t session = NewSession();

    lua_pushthread(co);
    int coRef = luaL_ref(co, LUA_REGISTRYINDEX);

    sessionCoRefs[session] = coRef;
    Sunnet::get_instance()->Timeout(id, session, ms);
    return session;
}
```

### 重要说明

这里 `luaL_ref(co, LUA_REGISTRYINDEX)` 是可以的，因为 coroutine 和主 `luaState` 共享 registry。


## 4. 增加一个启动 Lua 回调 coroutine 的 helper

继续在 `src/Service.cpp` 里加一个文件内 helper。

建议放在 `UnpackValue(...)` 后面。

```cpp
static void StartCallbackCoroutine(lua_State* luaState, int nargs){
    lua_State* co = lua_newthread(luaState);
    lua_insert(luaState, lua_gettop(luaState) - nargs);

    lua_xmove(luaState, co, nargs + 1);

    int status = lua_resume(co, luaState, nargs);
    
    }else if(status == LUA_YIELD){
        // co 之前挂在主栈上保活，现在可以弹掉了。
        // Sleep 已经把它登记进 sessionCoRefs 和 registry。
        lua_pop(luaState, 1);
    }else{
        const char* err = lua_tostring(co, -1);
        std::cout << "start callback coroutine failed";
        if(err){
            std::cout << " " << err;
        }
        std::cout << std::endl;
        lua_pop(co, 1);
        lua_pop(luaState, 1);
    }
}
```

### 这个 helper 怎么用

调用前，先把函数和参数压到主 `luaState` 上。

例如：

```cpp
lua_getglobal(luaState, "OnAcceptMsg");
if(!lua_isfunction(luaState, -1)){
    lua_pop(luaState, 1);
    return;
}

lua_pushinteger(luaState, msg->listenfd);
lua_pushinteger(luaState, msg->clientfd);
StartCallbackCoroutine(luaState, 2);
```

helper 内部会：

1. 创建 coroutine。
2. 把 coroutine 对象插到函数前面。
3. 把主栈上的函数和参数整体移动到 coroutine 栈。
4. `lua_resume` 启动。

### 为什么这里不需要启动时 `luaL_ref`

这里不需要一上来就把 `co` 存进 registry。

因为：

- `lua_newthread(luaState)` 创建出来的 coroutine 对象会先压在主 `luaState` 的栈上。
- 只要这个对象还留在主栈里，Lua 就认为它仍然有引用，不会被 GC 回收。
- 所以第一次 `lua_resume` 前，直接靠主栈保活就够了。

真正需要 `luaL_ref` 的时机，是 coroutine 调用了 `sunnet.Sleep(ms)` 并 `yield` 之后。

因为这时 `StartCallbackCoroutine(...)` 会把主栈上的 coroutine 对象弹掉，后续只能靠：

- `sessionCoRefs[session]`
- registry 里的 `coRef`

把它留到定时器恢复那一刻。


## 5. 改 `OnServiceMsg`

把原来的 `lua_pcall` 替换成 `StartCallbackCoroutine(...)`。

```cpp
void Service::OnServiceMsg(std::shared_ptr<ServiceMsg> msg){
    int top = lua_gettop(luaState);

    lua_getglobal(luaState, "OnServiceMsg");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }

    lua_pushinteger(luaState,msg->sourse);

    char* p=msg->buff.get();
    char* end=msg->buff.get()+msg->size;
    uint8_t argc=(uint8_t)p[0];
    p+=1;

    for(uint8_t i=0;i<argc;i++){
        if(!UnpackValue(luaState,p,end,0)){
            std::cout<<"unknown  or broken msg arg type"<<std::endl;
            lua_settop(luaState, top);
            return;
        }
    }

    StartCallbackCoroutine(luaState, 1 + argc);
}
```


## 6. 改 `OnAcceptMsg`

```cpp
void Service::OnAcceptMsg(std::shared_ptr<SocketAcceptMsg>msg){
    std::cout<<"OnAcceptMsg,fd: "<<msg->clientfd<<std::endl;
    Sunnet *inst=Sunnet::get_instance();

    std::shared_ptr<ConnWriter> w=std::make_shared<ConnWriter>();
    w->conn=inst->GetConn(msg->clientfd);
    writers.emplace(msg->clientfd,w);

    lua_getglobal(luaState, "OnAcceptMsg");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }

    lua_pushinteger(luaState, msg->listenfd);
    lua_pushinteger(luaState, msg->clientfd);
    StartCallbackCoroutine(luaState, 2);
}
```


## 7. `OnSocketData` 第一版不改

`OnSocketData` 保持当前 `lua_pcall` 版本，不接 `StartCallbackCoroutine(...)`。

原因是 socket 数据回调很容易出现同一个 fd 的多次数据交错执行。第一版先让服务消息、连接建立、连接关闭这几个入口可挂起。

如果 Lua 里写：

```lua
function OnSocketData(fd, buff)
    sunnet.Sleep(1000)
end
```

第一版应该报错，因为它仍然不是 coroutine 上下文。


## 8. 改 `OnSocketClose`

```cpp
void Service::OnSocketClose(int fd){
    std::cout<<"OnSocketClose"<<fd<<std::endl;
    writers.erase(fd);

    lua_getglobal(luaState, "OnSocketClose");
    if(!lua_isfunction(luaState, -1)){
        lua_pop(luaState, 1);
        return;
    }

    lua_pushinteger(luaState, fd);
    StartCallbackCoroutine(luaState, 1);
}
```


## 9. 改 `OnTimerMsg`

现在 `OnTimerMsg` 不再只是打日志，而是用 `session` 恢复 coroutine。

```cpp
void Service::OnTimerMsg(std::shared_ptr<TimerMsg> msg){
    auto it = sessionCoRefs.find(msg->session);
    if(it == sessionCoRefs.end()){
        std::cout << "[" << id << "] timer session not found " << msg->session << std::endl;
        return;
    }

    int coRef = it->second;
    sessionCoRefs.erase(it);

    ResumeCoroutineRef(coRef, 0);
}
```


## 10. 修改 `include/LuaApi.h`

新增 `Sleep` 声明：

```cpp
static int Sleep(lua_State *luaState);
```

建议改成：

```cpp
class LuaApi{
public:
    static void Register(lua_State *luaState );
    static int NewService(lua_State *luaState );
    static int KillService(lua_State *luaState );
    static int Send(lua_State *luaState );

    enum ArgType {
        TYPE_STRING = 1,
        TYPE_INTEGER = 2,
        TYPE_NUMBER = 3,
        TYPE_TABLE = 4,
    };

    static int Listen(lua_State *luaState );
    static int CloseConn(lua_State *luaState );
    static int Write(lua_State *luaState );
    static int Name(lua_State* luaState);
    static int SelfName(lua_State *luaState);
    static int Sleep(lua_State *luaState);
};
```


## 11. 修改 `src/LuaApi.cpp`

### 先 include `Service.h`

文件头部加：

```cpp
#include "Service.h"
```

### 注册 `Sleep`

在 `LuaApi::Register(...)` 里加：

```cpp
{"Sleep", Sleep},
```

完整位置：

```cpp
static const struct luaL_Reg lualibs[]={
    {"NewService",NewService},
    {"KillService",KillService},
    {"Send",Send},
    {"Name",Name},
    {"SelfName",SelfName},
    {"Write", Write},
    {"Listen", Listen},
    {"CloseConn", CloseConn},
    {"Sleep", Sleep},

    {NULL,NULL}
};
```

### 增加 `LuaApi::Sleep`

建议放到文件末尾。

```cpp
int LuaApi::Sleep(lua_State *luaState){
    int num = lua_gettop(luaState);
    if(num != 1 || lua_isinteger(luaState, 1) == 0){
        return luaL_error(luaState, "sunnet.Sleep(ms) need integer ms");
    }

    int isMainThread = lua_pushthread(luaState);
    lua_pop(luaState, 1);
    if(isMainThread){
        return luaL_error(luaState, "sunnet.Sleep must be called in coroutine");
    }

    uint32_t ms = (uint32_t)lua_tointeger(luaState, 1);

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    Service* srv = (Service*)lua_touserdata(luaState, -1);
    lua_pop(luaState, 1);

    if(srv == nullptr){
        return luaL_error(luaState, "sunnet.Sleep missing service ptr");
    }

    srv->SleepCurrentCoroutine(luaState, ms);
    return lua_yield(luaState, 0);
}
```


## 12. Lua 测试代码

可以先在 `service/ping/init.lua` 或任意服务的 `OnServiceMsg` 里测试：

```lua
function OnServiceMsg(source, msg)
    print("step1", source, msg)
    sunnet.Sleep(1000)
    print("step2", source, msg)
end
```

预期：

- 收到服务消息后立刻打印 `step1`
- 1 秒后打印 `step2`


## 13. 编译

如果新增或删除过 `.cpp` 文件，先重新配置：

```bash
cmake -S . -B build
```

然后编译：

```bash
cmake --build build
```


## 14. 第一版常见错误

### 1. `attempt to yield from outside a coroutine`

说明某个 Lua 回调没有通过 `StartCallbackCoroutine(...)` 启动，仍然是 `lua_pcall`。

检查：

- `OnServiceMsg`
- `OnAcceptMsg`
- `OnSocketClose`

### 2. `sunnet.Sleep missing service ptr`

说明 `OnInit()` 里没有设置：

```cpp
lua_pushlightuserdata(luaState, this);
lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
```

### 3. timer session not found

可能原因：

- 定时器重复触发
- `sessionCoRefs` 被提前删了
- coroutine 结束或报错后还有旧 timer 消息回来

第一版先打印日志即可。

### 4. 在 OnSocketData 里 Sleep 报错

这是第一版预期行为。

第一版没有协程化 `OnSocketData`，所以它仍然不能直接 `sunnet.Sleep(ms)`。


## 15. 一个小修正建议

你当前 [src/TimerWorker.cpp](/home/a/桌面/sunnet/src/TimerWorker.cpp:10) 里应该保持这种写法：

```cpp
ts.tv_nsec += (long)(ms%1000)* 1000000L;
```

不要写成：

```cpp
ts.tv_nsec = (long)(ms%1000)* 1000000L;
```

因为 `clock_gettime(CLOCK_MONOTONIC, &ts)` 已经把当前纳秒放进了 `ts.tv_nsec`，这里要在当前时间基础上加上剩余毫秒对应的纳秒。
