# Call / Ret / Fork 实现代码

这份文档写第一版可以照抄的代码。

第一版目标：

- `sunnet.Fork(fn, ...)`
- `sunnet.Call(target, ...)`
- `sunnet.Ret(...)`
- 复用现在的 `sessionCoRefs`
- 不做 `Call` 超时
- 不做 `skynet.response()` 那种异步 response 闭包


## 1. 修改 `include/Msg.h`

把 `ServiceMsg` 替换成这个版本。

```cpp
class ServiceMsg:public BaseMsg
{
public:
    enum KIND{
        SEND = 1,
        CALL = 2,
        RET = 3,
    };

    uint8_t kind = SEND;
    uint32_t sourse;
    uint32_t session = 0;
    std::shared_ptr<char>buff;//消息
    size_t size;//消息长度
    ~ServiceMsg(){};
};
```

含义：

- `SEND`：普通 `sunnet.Send`
- `CALL`：`sunnet.Call` 发出的请求
- `RET`：`sunnet.Ret` 发出的回复

Lua 层 `OnServiceMsg` 统一使用这个签名：

```lua
function OnServiceMsg(session, source, ...)
end
```

其中：

- `session == 0`：普通 `Send`
- `session ~= 0`：`Call` 请求，可以调用 `sunnet.Ret(...)`


## 2. 修改 `include/Sunnet.h`

把 `Send` 声明从 `void` 改成 `bool`。

```cpp
bool Send(uint32_t toId,std::shared_ptr<BaseMsg>msg);//发送消息
```


## 3. 修改 `src/Sunnet.cpp`

把 `Sunnet::Send` 替换成这个版本。

```cpp
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
```

为什么要改成 `bool`：

`sunnet.Call` 发送失败时不能挂起 coroutine，否则永远没人恢复它。


## 4. 修改 `include/Service.h`

在 `Service` 里增加 RPC 上下文表。

建议在 `class Service` 的 `public:` 区域增加：

```cpp
struct RpcContext{
    uint32_t source = 0;
    uint32_t session = 0;
    bool reted = false;
};
```

在消息处理函数声明里增加 `OnRetMsg`。

```cpp
void OnRetMsg(std::shared_ptr<ServiceMsg> msg);
```

在协程相关字段附近改成下面这样。

```cpp
public:
    uint32_t nextSession = 1;//协程出让号,递增

    //暂停的协程表: session -> registry coroutine ref
    std::unordered_map<uint32_t, int> sessionCoRefs;

    //正在处理 CALL 请求的 coroutine 上下文: coroutine -> rpc context
    std::unordered_map<lua_State*, RpcContext> coRpcContexts;

    uint32_t NewSession();//分配新的出让号
    uint32_t SleepCurrentCoroutine(lua_State* co, int ms);
    int ResumeCoroutineRef(int coRef, int nargs);

private:
    void StartCallbackCoroutine(int nargs, RpcContext* rpcCtx);
```

注意：

`coRpcContexts` 的 key 是 `lua_State*`，也就是具体 coroutine。

不能用一个全局 `currentSession`，因为同一个服务里可以同时挂起多个 coroutine。


## 5. 修改 `src/Service.cpp`

### 5.1 删除文件内 static 版本

删掉现在这个文件内 helper：

```cpp
static void StartCallbackCoroutine(lua_State* luaState, int nargs){
    ...
}
```

下面改成 `Service` 成员函数。


### 5.2 增加 `Service::StartCallbackCoroutine`

建议放在 `Service::SetInGlobal(...)` 后面。

```cpp
void Service::StartCallbackCoroutine(int nargs, RpcContext* rpcCtx){
    lua_State* co = lua_newthread(luaState);

    // 把 co 插到函数和参数前面，让主栈暂时保活 coroutine。
    lua_insert(luaState, lua_gettop(luaState) - nargs - 1);

    // 把函数和参数移动到 coroutine 栈。
    lua_xmove(luaState, co, nargs + 1);

    if(rpcCtx != nullptr){
        coRpcContexts[co] = *rpcCtx;
    }

    int status = lua_resume(co, luaState, nargs);
    if(status == LUA_OK){
        coRpcContexts.erase(co);
        lua_pop(luaState, 1);
    }else if(status == LUA_YIELD){
        lua_pop(luaState, 1);
    }else{
        std::cout << "start callback coroutine failed";
        const char* err = lua_tostring(co, -1);
        if(err){
            std::cout << " " << err;
        }
        std::cout << std::endl;
        lua_pop(co, 1);
        coRpcContexts.erase(co);
        lua_pop(luaState, 1);
    }
}
```


### 5.3 替换 `Service::OnServiceMsg`

```cpp
void Service::OnServiceMsg(std::shared_ptr<ServiceMsg> msg){
    if(msg->kind == ServiceMsg::RET){
        OnRetMsg(msg);
        return;
    }

    int top = lua_gettop(luaState);

    lua_getglobal(luaState,"OnServiceMsg");
    if(!lua_isfunction(luaState, -1)){
        lua_settop(luaState, top);
        return;
    }

    lua_pushinteger(luaState,msg->kind == ServiceMsg::CALL ? msg->session : 0);
    lua_pushinteger(luaState,msg->sourse);

    char* p=msg->buff.get();
    char* end=msg->buff.get()+msg->size;
    uint8_t argc=(uint8_t)p[0];//参数个数
    p+=1;

    for(uint8_t i=0;i<argc;i++){
        if(!UnpackValue(luaState,p,end,0)){
            std::cout<<"unknown  or broken msg arg type"<<std::endl;
            lua_settop(luaState, top);
            return;
        }
    }

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
```

这里给 Lua 多传了一个 `session`：

- `SEND` 消息传 `0`
- `CALL` 消息传真实 `msg->session`

这样 Lua 层可以直接判断这次消息是不是 RPC 请求。


### 5.4 修改 `OnAcceptMsg`

最后一行改成：

```cpp
StartCallbackCoroutine(2, nullptr);
```

也就是完整 Lua 调用部分是：

```cpp
// 调 Lua: OnAcceptMsg(listenfd, clientfd)
lua_getglobal(luaState, "OnAcceptMsg");
if(!lua_isfunction(luaState, -1)){
    lua_pop(luaState, 1);
    return;
}

lua_pushinteger(luaState, msg->listenfd);
lua_pushinteger(luaState, msg->clientfd);

StartCallbackCoroutine(2, nullptr);
```


### 5.5 修改 `OnSocketClose`

最后一行改成：

```cpp
StartCallbackCoroutine(1, nullptr);
```

也就是完整 Lua 调用部分是：

```cpp
// 调 Lua: OnSocketClose(fd)
lua_getglobal(luaState, "OnSocketClose");
if(!lua_isfunction(luaState, -1)){
    lua_pop(luaState, 1);
    return;
}

lua_pushinteger(luaState, fd);

StartCallbackCoroutine(1, nullptr);
```


### 5.6 增加 `Service::OnRetMsg`

建议放在 `OnTimerMsg` 前面。

```cpp
void Service::OnRetMsg(std::shared_ptr<ServiceMsg> msg){
    auto it = sessionCoRefs.find(msg->session);
    if(it == sessionCoRefs.end()){
        std::cout << "[" << id << "] ret session not found " << msg->session << std::endl;
        return;
    }

    int coRef = it->second;
    sessionCoRefs.erase(it);

    lua_rawgeti(luaState, LUA_REGISTRYINDEX, coRef);
    lua_State* co = lua_tothread(luaState, -1);
    lua_pop(luaState, 1);


    int top = lua_gettop(co);

    char* p = msg->buff.get();
    char* end = msg->buff.get() + msg->size;
    uint8_t argc = (uint8_t)p[0];
    p += 1;

    for(uint8_t i=0;i<argc;i++){
        if(!UnpackValue(co, p, end, 0)){
            std::cout << "unknown or broken ret arg type" << std::endl;
            lua_settop(co, top);
            lua_pushnil(co);
            lua_pushstring(co, "broken ret arg");
            ResumeCoroutineRef(coRef, 2);
            return;
        }
    }

    ResumeCoroutineRef(coRef, argc);
}
```


### 5.7 修改 `Service::SleepCurrentCoroutine`

把 `ms` 参数从 `uint32_t` 改成 `int`。

`ms >= 0` 时设置定时器，用于 `sunnet.Sleep(ms)`。

`ms < 0` 时只保存 coroutine，不设置定时器，用于 `sunnet.Call(...)` 等待 `RET`。

```cpp
uint32_t Service::SleepCurrentCoroutine(lua_State* co, int ms){
    uint32_t session=NewSession();

    //协程对象保存进register表
    lua_pushthread(co);
    int coRef=luaL_ref(co,LUA_REGISTRYINDEX);

    //coRef保存到出让表
    sessionCoRefs[session]=coRef;

    if(ms >= 0){
        Sunnet::get_instance()->Timeout(id, session, (uint32_t)ms);
    }
    return session;
}
```


### 5.8 修改 `Service::ResumeCoroutineRef`

把你的 `ResumeCoroutineRef` 替换成这个版本。

```cpp
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

    //resume协程，nargs 个参数必须已经在 co 栈上
    int status=lua_resume(co,luaState,nargs);
    if(status == LUA_OK){
        coRpcContexts.erase(co);
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }else if(status == LUA_YIELD){
        // 阻塞api已经给这个协程在register表里面新加了引用，这个旧 ref 可以释放。
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }else{
        std::cout << "resume coroutine failed";
        const char* err = lua_tostring(co, -1);
        if(err){
            std::cout << " " << err;
        }
        std::cout << std::endl;
        lua_pop(co, 1);
        coRpcContexts.erase(co);
        luaL_unref(luaState, LUA_REGISTRYINDEX, coRef);
    }
    return status;
}
```


### 5.9 修改 `Service::OnExit`

`OnExit` 不需要额外清理 `sessionCoRefs` 和 `coRpcContexts`。

原因：

- `lua_close(luaState)` 会关闭整个 Lua VM，registry 里的 coroutine 引用会一起销毁。
- `Service` 对象释放时，`sessionCoRefs` 和 `coRpcContexts` 这两个 C++ 容器会自己析构。

保持这样即可：

```cpp
void Service::OnExit(){
    std::cout<<"["<<id<<"] OnExit"<<std::endl;
    lua_getglobal(luaState,"OnExit");//把名为OnInit的变量压栈
    int isok=lua_pcall(luaState,0,0,0);
    if(isok!=0){//返回值不为0，失败
        std::cout<<"call lua OnExit failed"<<lua_tostring(luaState,-1)<<std::endl;
    }

    lua_close(luaState);
    luaState = nullptr;
}
```


## 6. 修改 `include/LuaApi.h`

增加声明：

```cpp
static int Call(lua_State *luaState);
static int Ret(lua_State *luaState);
static int Fork(lua_State *luaState);
```

完整尾部可以改成：

```cpp
static int Listen(lua_State *luaState );
static int CloseConn(lua_State *luaState );
static int Write(lua_State *luaState );
static int Name(lua_State* luaState);
static int SelfName(lua_State *luaState);
static int Sleep(lua_State *luaState);
static int Call(lua_State *luaState);
static int Ret(lua_State *luaState);
static int Fork(lua_State *luaState);
```


## 7. 修改 `src/LuaApi.cpp`

### 7.1 修改 `LuaApi::Register`

加三个 API。

```cpp
void LuaApi::Register(lua_State* luaState){
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
        {"Call", Call},
        {"Ret", Ret},
        {"Fork", Fork},
        {NULL,NULL}
    };
    luaL_newlib(luaState,lualibs);
    lua_setglobal(luaState,"sunnet");
}
```


### 7.2 修改 `LuaApi::Send`

保持你现在的 `Send` 写法，只在消息初始化时新增：

```cpp
msg->kind=ServiceMsg::SEND;
msg->session=0;
```

完整函数：

```cpp
int LuaApi::Send(lua_State* luaState){
    int num=lua_gettop(luaState);
    if(num<2){
        std::cout<<"LuaApi:send failed ,num err"<<std::endl;
        return 0;
    }
    uint32_t target_id=(uint32_t)-1;

    if(lua_isinteger(luaState,1)){
        target_id=lua_tointeger(luaState,1);
    }else if(lua_isstring(luaState,1)){
        size_t nameLen;
        const char* nameStr = lua_tolstring(luaState,1,&nameLen);
        std::string name(nameStr, nameLen);
        target_id = Sunnet::get_instance()->QueryName(name);//不存在的话返回(uint32_t)-1，Sunnet->send里面会打印日志
    }else{
        std::cout<<"LuaApi:send failed, arg1 err"<<std::endl;
        return 0;
    }

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_id");
    uint32_t selfId = lua_tointeger(luaState, -1);
    lua_pop(luaState, 1);

    std::string out;
    out.push_back((char)(num-1));

    for(int i=2;i<=num;i++){
        if(PackValue(luaState,i,out,0)==false){
            return 0;
        }
    }

    char* newBuff = new char[out.size()];
    memcpy(newBuff, out.data(), out.size());

    auto msg= std::make_shared<ServiceMsg>();
    msg->type=BaseMsg::TYPE::SERVICE;
    msg->kind=ServiceMsg::SEND;
    msg->sourse=selfId;
    msg->session=0;
    msg->buff=std::shared_ptr<char>(newBuff,[](char* p){delete []p;});
    msg->size=out.size();

    Sunnet* inst=Sunnet::get_instance();
    inst->Send(target_id,msg);
    //lua层无返回值
    return 0;
}
```


### 7.3 增加 `LuaApi::Call`

建议放在 `LuaApi::Sleep` 后面。

```cpp
int LuaApi::Call(lua_State *luaState){
    int num=lua_gettop(luaState);
    if(num < 2){
        return luaL_error(luaState, "sunnet.Call(target, ...) need target and args");
    }

    int isMainThread = lua_pushthread(luaState);
    lua_pop(luaState, 1);
    if(isMainThread){
        return luaL_error(luaState, "sunnet.Call must be called in coroutine");
    }

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    Service* srv = (Service*)lua_touserdata(luaState, -1);
    lua_pop(luaState, 1);
    if(srv == nullptr){
        return luaL_error(luaState, "sunnet.Call missing service ptr");
    }

    uint32_t targetId=(uint32_t)-1;
    if(lua_isinteger(luaState,1)){
        targetId=(uint32_t)lua_tointeger(luaState,1);
    }else if(lua_isstring(luaState,1)){
        size_t nameLen;
        const char* nameStr = lua_tolstring(luaState,1,&nameLen);
        std::string name(nameStr, nameLen);
        targetId = Sunnet::get_instance()->QueryName(name);
    }else{
        return luaL_error(luaState, "sunnet.Call target must be service id or name");
    }

    std::string out;
    out.push_back((char)(num-1));

    for(int i=2;i<=num;i++){
        if(PackValue(luaState,i,out,0)==false){
            return luaL_error(luaState, "sunnet.Call pack args failed");
        }
    }

    uint32_t session = srv->SleepCurrentCoroutine(luaState, -1);

    char* newBuff = new char[out.size()];
    memcpy(newBuff, out.data(), out.size());

    auto msg = std::make_shared<ServiceMsg>();
    msg->type = BaseMsg::TYPE::SERVICE;
    msg->kind = ServiceMsg::CALL;
    msg->sourse = srv->id;
    msg->session = session;
    msg->buff = std::shared_ptr<char>(newBuff, [](char* p){ delete []p; });
    msg->size = out.size();

    bool ok = Sunnet::get_instance()->Send(targetId, msg);
    if(!ok){
        auto it = srv->sessionCoRefs.find(session);
        if(it != srv->sessionCoRefs.end()){
            luaL_unref(luaState, LUA_REGISTRYINDEX, it->second);
            srv->sessionCoRefs.erase(it);
        }
        return luaL_error(luaState, "sunnet.Call send failed");
    }

    return lua_yield(luaState, 0);
}
```


### 7.4 增加 `LuaApi::Ret`

```cpp
int LuaApi::Ret(lua_State *luaState){
    int num=lua_gettop(luaState);

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    Service* srv = (Service*)lua_touserdata(luaState, -1);
    lua_pop(luaState, 1);
    if(srv == nullptr){
        return luaL_error(luaState, "sunnet.Ret missing service ptr");
    }

    auto it = srv->coRpcContexts.find(luaState);
    if(it == srv->coRpcContexts.end()){
        return luaL_error(luaState, "sunnet.Ret must be called in RPC request");
    }

    if(it->second.reted){
        return luaL_error(luaState, "sunnet.Ret already called");
    }

    std::string out;
    out.push_back((char)num);

    for(int i=1;i<=num;i++){
        if(PackValue(luaState,i,out,0)==false){
            return luaL_error(luaState, "sunnet.Ret pack args failed");
        }
    }

    char* newBuff = new char[out.size()];
    memcpy(newBuff, out.data(), out.size());

    auto msg = std::make_shared<ServiceMsg>();
    msg->type = BaseMsg::TYPE::SERVICE;
    msg->kind = ServiceMsg::RET;
    msg->sourse = srv->id;
    msg->session = it->second.session;
    msg->buff = std::shared_ptr<char>(newBuff, [](char* p){ delete []p; });
    msg->size = out.size();

    bool ok = Sunnet::get_instance()->Send(it->second.source, msg);
    if(!ok){
        return luaL_error(luaState, "sunnet.Ret send failed");
    }

    it->second.reted = true;
    return 0;
}
```


### 7.5 增加 `LuaApi::Fork`

```cpp
int LuaApi::Fork(lua_State *luaState){
    int num=lua_gettop(luaState);
    if(num < 1 || !lua_isfunction(luaState, 1)){
        return luaL_error(luaState, "sunnet.Fork(fn, ...) need function");
    }

    int nargs = num - 1;

    lua_State* co = lua_newthread(luaState);

    // 把 co 插到 fn 和参数前面，让当前栈暂时保活 coroutine。
    lua_insert(luaState, lua_gettop(luaState) - nargs - 1);

    // 把 fn 和参数移动到新 coroutine 栈。
    lua_xmove(luaState, co, nargs + 1);

    int status = lua_resume(co, luaState, nargs);
    if(status == LUA_OK || status == LUA_YIELD){
        lua_pop(luaState, 1);
        lua_pushinteger(luaState, 1);
        return 1;
    }

    const char* err = lua_tostring(co, -1);
    std::string errMsg = err ? err : "";
    lua_pop(co, 1);
    lua_pop(luaState, 1);
    return luaL_error(luaState, "sunnet.Fork failed %s", errMsg.c_str());
}
```


## 8. Lua 测试

### 8.1 `Fork` 测试

放在任意已经协程化的 `OnServiceMsg` 里。

```lua
function OnServiceMsg(session, source, msg)
    sunnet.Fork(function()
        sunnet.Sleep(1000)
        print("[lua] fork wake up", msg)
    end)

    print("[lua] OnServiceMsg return immediately")
end
```

预期：

- 先打印 `OnServiceMsg return immediately`
- 1 秒后打印 `fork wake up`


### 8.2 `Call / Ret` 测试

创建一个 `service/db/init.lua`。

```lua
local serviceId

function OnInit(id)
    serviceId = id
    print("[lua] db OnInit id:" .. id)
    sunnet.Name("db")
end

function OnServiceMsg(session, source, cmd, id)
    if cmd == "get" then
        sunnet.Sleep(1000)
        sunnet.Ret(true, { id = id, name = "alice" })
        return
    end

    sunnet.Ret(false, "unknown cmd")
end

function OnExit()
    print("[lua] db OnExit")
end
```

调用方服务：

```lua
function OnServiceMsg(session, source, msg)
    local ok, user = sunnet.Call("db", "get", 1001)
    print("[lua] call ret", ok, user.id, user.name)
end
```

预期：

- 调用方发起 `Call`
- `db` 服务 `Sleep(1000)`
- 1 秒后 `Ret`
- 调用方 coroutine 恢复，打印 `call ret true 1001 alice`


## 9. 编译

```bash
cmake --build build
```

如果 CMake 没重新扫描过新增文件，不涉及这次改动，因为没有新增 `.cpp`。


## 10. 第一版限制

- `sunnet.Call` 只能在 coroutine 里调用。
- `sunnet.Ret` 只能在 `CALL` 请求对应的 coroutine 里调用。
- `sunnet.Fork` 里的 coroutine 不继承外层 `Ret` 上下文。
- 被调用方不 `Ret`，调用方会一直挂起。
- 服务退出时，已经发出去的 `CALL/RET` 消息不会自动撤销。
