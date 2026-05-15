# Call / Ret / Fork 架构设计

这份文档先写思路架构，不写完整照抄代码。

当前已经有：

- 回调协程化：`OnServiceMsg`、`OnAcceptMsg`、`OnSocketClose` 可以 `yield`
- 定时器：`Sunnet::Timeout(serviceId, session, ms)`
- 等待表：`Service::sessionCoRefs`
- `sunnet.Sleep(ms)`：保存当前 coroutine，定时器到期后 resume

下一步做：

- `sunnet.Call(target, ...)`
- `sunnet.Ret(...)`
- `sunnet.Fork(fn, ...)`


## 目标语义

### `sunnet.Call(target, ...)`

同步风格的服务调用。

Lua 写法：

```lua
local ok, data = sunnet.Call("db", "get_user", 1001)
print(ok, data)
```

执行语义：

1. 当前 coroutine 发送一个 RPC 请求给目标服务。
2. 当前 coroutine 挂起。
3. 目标服务处理完后调用 `sunnet.Ret(...)`。
4. 原 coroutine 被恢复。
5. `sunnet.Call(...)` 返回 `Ret` 传回来的参数。

第一版要求 `Call` 必须在 coroutine 里调用。

也就是说：

- 可以在协程化后的 `OnServiceMsg` 里调用
- 可以在 `sunnet.Fork(...)` 启动的 coroutine 里调用
- 不能在普通 `lua_pcall` 的 `OnInit` / `OnSocketData` 里直接调用


### `sunnet.Ret(...)`

回复当前 RPC 请求。

Lua 写法：

```lua
function OnServiceMsg(source, cmd, id)
    if cmd == "get_user" then
        sunnet.Ret(true, { id = id, name = "alice" })
        return
    end
end
```

执行语义：

1. 只能在处理 `CALL` 消息的 coroutine 里调用。
2. C++ 找到当前 coroutine 对应的请求上下文。
3. 把返回参数打包成 `RET` 消息。
4. 发回调用方服务。
5. 调用方收到 `RET` 后，用 session 找到挂起的 coroutine 并 resume。

第一版采用显式 `Ret`。

如果目标服务处理 `CALL` 后没有调用 `Ret`，调用方会一直等待。


### `sunnet.Fork(fn, ...)`

服务内启动一个后台 coroutine。

Lua 写法：

```lua
function OnServiceMsg(source, msg)
    sunnet.Fork(function()
        sunnet.Sleep(1000)
        print("later", msg)
    end)
end
```

执行语义：

1. C++ 创建一个新的 coroutine。
2. 把 `fn` 和参数移动到新 coroutine 栈。
3. `lua_resume` 启动。
4. 如果执行完，直接结束。
5. 如果 `Sleep` 或 `Call` 挂起，就进入已有等待表。

第一版 `Fork` 不继承 `Ret` 上下文。

也就是说，不能在 `Fork` 里的函数直接 `sunnet.Ret(...)` 回复外层请求。这样可以避免一个请求被多个 coroutine 重复回复。


## 核心设计

整个设计继续复用 `session`。

`session` 现在不只表示定时器等待，也表示 RPC 等待。

```text
session -> coroutine ref
```

等待来源分两类：

- `Sleep`：定时器到了恢复 coroutine
- `Call`：收到 `RET` 消息后恢复 coroutine

第一版可以继续用当前的：

```cpp
std::unordered_map<uint32_t, int> sessionCoRefs;
```

暂时不需要单独拆成 `timerSessionCoRefs` 和 `rpcSessionCoRefs`。


## 消息类型

现在的 `ServiceMsg` 只有：

```cpp
uint32_t sourse;
std::shared_ptr<char> buff;
size_t size;
```

要支持 `Call / Ret`，建议扩展成：

```cpp
class ServiceMsg: public BaseMsg{
public:
    enum KIND{
        SEND = 1,
        CALL = 2,
        RET = 3,
    };

    uint8_t kind = SEND;
    uint32_t sourse;
    uint32_t session = 0;
    std::shared_ptr<char> buff;
    size_t size;
};
```

三种消息含义：

- `SEND`：普通 `sunnet.Send(...)`，不需要返回值，`session = 0`
- `CALL`：`sunnet.Call(...)` 发出的请求，`session` 是调用方生成的等待编号
- `RET`：`sunnet.Ret(...)` 发出的回复，`session` 是原始 `CALL` 的等待编号

`BaseMsg::TYPE::SERVICE` 不变。

也就是说，外层仍然都是服务消息，只是在 `ServiceMsg` 里再区分 `SEND / CALL / RET`。


## Service 需要新增的状态

当前已经有：

```cpp
uint32_t nextSession = 1;
std::unordered_map<uint32_t, int> sessionCoRefs;
```

为了让 `Ret` 知道“当前 coroutine 正在处理哪个 CALL”，还需要一个 coroutine 上下文表。

建议：

```cpp
struct RpcContext{
    uint32_t source;
    uint32_t session;
    bool reted;
};

std::unordered_map<lua_State*, RpcContext> coRpcContexts;
```

含义：

- key 是当前 coroutine 的 `lua_State*`
- `source` 是请求方服务 id
- `session` 是请求方传来的等待编号
- `reted` 用来防止同一个请求重复 `Ret`

为什么不能只把 `source/session` 存到 registry 全局字段里：

```text
一个服务里可以同时跑多个 coroutine。
某个 CALL 处理过程中 Sleep 或 Call 后，服务可以继续处理下一个消息。
如果用全局字段保存当前 session，后面的消息会覆盖前面的上下文。
```

所以 `Ret` 上下文必须跟 coroutine 绑定。


## OnServiceMsg 分流

现在所有 `SERVICE` 消息都会走 Lua 的 `OnServiceMsg`。

扩展后，C++ 先看 `msg->kind`：

```text
SEND -> 启动 OnServiceMsg(source, ...)
CALL -> 启动 OnServiceMsg(source, ...)，并给这个 coroutine 绑定 RpcContext
RET  -> 不进 Lua OnServiceMsg，直接恢复等待中的 caller coroutine
```

### `SEND`

保持现在行为。

```text
ServiceMsg(kind=SEND, source=A, session=0, args)
        |
        v
目标服务 OnServiceMsg(source, ...)
```


### `CALL`

目标服务仍然进入 `OnServiceMsg(source, ...)`，但是 C++ 要记录这个 coroutine 正在处理 RPC 请求。

```text
ServiceMsg(kind=CALL, source=A, session=100, args)
        |
        v
目标服务创建 coroutine co
        |
        v
coRpcContexts[co] = { source=A, session=100, reted=false }
        |
        v
resume co -> Lua OnServiceMsg(source, ...)
```

以后 Lua 在这个 coroutine 里调用：

```lua
sunnet.Ret(...)
```

C++ 就能通过当前 `lua_State*` 找到 `source/session`。


### `RET`

`RET` 是给调用方恢复 coroutine 用的，不应该再进 Lua 的 `OnServiceMsg`。

```text
ServiceMsg(kind=RET, source=B, session=100, ret args)
        |
        v
调用方服务 sessionCoRefs[100] 找到 coRef
        |
        v
把 ret args 解包压到 co 栈
        |
        v
lua_resume(co, luaState, retArgc)
```

这时 Lua 侧：

```lua
local a, b = sunnet.Call(...)
```

会得到：

```lua
a, b
```


## Call 的执行链

`sunnet.Call(target, ...)` 发生在调用方服务。

流程：

```text
Lua coroutine 调用 sunnet.Call(target, ...)
        |
        v
C++ 检查当前是不是 coroutine
        |
        v
生成 session
        |
        v
把当前 coroutine 保存到 registry，得到 coRef
        |
        v
sessionCoRefs[session] = coRef
        |
        v
把参数打包成 ServiceMsg(kind=CALL, source=selfId, session=session)
        |
        v
Sunnet::Send(targetId, msg)
        |
        v
lua_yield(luaState, 0)
```

关键点：

- `Call` 和 `Sleep` 一样，都是保存当前 coroutine 后 `yield`
- `Call` 的恢复不是靠 timer，而是靠 `RET` 消息
- `Call` 恢复时要把返回值压到 coroutine 栈，再 `lua_resume(co, luaState, nret)`

第一版建议先不做超时。

后面可以加：

```lua
sunnet.Call(target, timeoutMs, ...)
```

或者：

```lua
sunnet.CallTimeout(target, timeoutMs, ...)
```

超时本质上就是同一个 session 同时挂 RPC 等待和 timer。


## Ret 的执行链

`sunnet.Ret(...)` 发生在被调用方服务。

流程：

```text
Lua coroutine 调用 sunnet.Ret(...)
        |
        v
C++ 用当前 lua_State* 查 coRpcContexts
        |
        v
找不到 -> 报错：Ret must be called in RPC request
        |
        v
如果 reted=true -> 报错：RPC request already returned
        |
        v
打包 Ret 参数
        |
        v
发送 ServiceMsg(kind=RET, source=selfId, session=ctx.session) 到 ctx.source
        |
        v
ctx.reted = true
```

`Ret` 发送后是否立刻结束当前 coroutine，由 Lua 自己决定。

第一版推荐 Lua 代码写：

```lua
sunnet.Ret(...)
return
```

不要在 `Ret` 后继续写复杂逻辑。


## Fork 的执行链

`sunnet.Fork(fn, ...)` 发生在当前服务内，不经过消息队列。

流程：

```text
Lua 调用 sunnet.Fork(fn, ...)
        |
        v
C++ 检查第一个参数是 function
        |
        v
lua_newthread
        |
        v
把 fn 和参数移动到新 coroutine 栈
        |
        v
lua_resume(co, luaState, nargs)
        |
        v
结束 / yield / error
```

如果 `Fork` 里的 coroutine 调用：

```lua
sunnet.Sleep(1000)
```

就走已有 `Sleep` 等待表。

如果调用：

```lua
sunnet.Call("db", ...)
```

就走新的 RPC 等待表。

`Fork` 本身不返回 coroutine 句柄。

第一版只返回：

```lua
1
```

表示启动成功。


## 需要调整的 C++ 结构

### 1. `ServiceMsg`

增加：

```cpp
uint8_t kind;
uint32_t session;
```

`Send` 设置：

```cpp
kind = ServiceMsg::SEND;
session = 0;
```

`Call` 设置：

```cpp
kind = ServiceMsg::CALL;
session = callerSession;
```

`Ret` 设置：

```cpp
kind = ServiceMsg::RET;
session = requestSession;
```


### 2. `Service`

增加：

```cpp
struct RpcContext;
std::unordered_map<lua_State*, RpcContext> coRpcContexts;
```

把当前文件内的：

```cpp
StartCallbackCoroutine(...)
```

建议改成 `Service` 成员函数。

原因是启动 coroutine 时需要：

- 给 `CALL` coroutine 绑定 `RpcContext`
- coroutine 结束或报错时清理 `coRpcContexts`
- coroutine yield 后保留上下文，等待后续恢复

成员函数更适合做这些事。


### 3. `LuaApi`

新增注册：

```cpp
{"Call", Call},
{"Ret", Ret},
{"Fork", Fork},
```

新增 API：

```cpp
static int Call(lua_State* luaState);
static int Ret(lua_State* luaState);
static int Fork(lua_State* luaState);
```

`Call` 和 `Ret` 都需要通过 registry 里的：

```cpp
__service_ptr
```

拿回当前 `Service*`。


### 4. 恢复 coroutine 时支持参数

当前 `ResumeCoroutineRef(coRef, nargs)` 只支持 `nargs = 0`。

RPC `RET` 需要：

1. 找到 coroutine
2. 把返回参数解包到 coroutine 栈
3. `lua_resume(co, luaState, nret)`

所以恢复逻辑要明确成：

```text
ResumeCoroutineRef 要么只负责 resume
要么新增一个专门处理 RET 的 ResumeRpcCoroutine
```

第一版建议新增一个专门的 `OnRetMsg(...)`，不要把所有逻辑塞进 `ResumeCoroutineRef`。


## 错误和边界

### `Call` 目标不存在

不能先保存 coroutine 再发送失败。

否则 caller 会永久挂起。

建议让 `Sunnet::Send(...)` 返回 `bool`：

```cpp
bool Sunnet::Send(uint32_t toId, std::shared_ptr<BaseMsg> msg);
```

发送失败时：

```lua
sunnet.Call(...)
```

直接抛 Lua 错误，不进入等待表。


### `Ret` 找不到上下文

说明当前 coroutine 不是 RPC 请求处理协程。

应该报错：

```text
sunnet.Ret must be called in RPC request
```


### 重复 `Ret`

同一个请求只允许回复一次。

如果 `reted == true`，直接报错。


### 被调用方不 Ret

第一版 caller 会一直等待。

这是设计上的限制。

后面加 timeout 后可以解决。


### 服务退出时仍有挂起 coroutine

当前 `Sleep` 已经会出现这个问题。

`Call` 也一样。

第一版可以先不做取消。

后面需要在 `OnExit` 前清理：

- `sessionCoRefs`
- `coRpcContexts`
- registry 里的 coroutine ref


## 推荐实现顺序

1. 扩展 `ServiceMsg::kind/session`
2. 让 `Sunnet::Send(...)` 返回 `bool`
3. 把 `StartCallbackCoroutine(...)` 改成 `Service` 成员函数
4. 增加 `coRpcContexts`
5. 实现 `sunnet.Fork(fn, ...)`
6. 实现 `sunnet.Call(target, ...)` 发送 `CALL` 并 yield
7. 实现 `sunnet.Ret(...)` 发送 `RET`
8. 在 `Service::OnServiceMsg(...)` 里分流 `SEND / CALL / RET`
9. 写 Lua 测试服务验证 `Call -> Ret -> resume`


## 第一版测试方式

`db` 服务：

```lua
function OnServiceMsg(source, cmd, id)
    if cmd == "get" then
        sunnet.Sleep(1000)
        sunnet.Ret(true, { id = id, name = "alice" })
        return
    end
end
```

`main` 或 `ping` 服务：

```lua
function OnServiceMsg(source, msg)
    local ok, user = sunnet.Call("db", "get", 1001)
    print(ok, user.id, user.name)
end
```

`Fork` 测试：

```lua
function OnServiceMsg(source, msg)
    sunnet.Fork(function()
        sunnet.Sleep(1000)
        print("fork wake up", msg)
    end)
end
```


## 总结

`Fork` 是服务内新开 coroutine。

`Sleep` 是 timer 恢复 coroutine。

`Call` 是发送 `CALL` 后挂起 coroutine。

`Ret` 是发送 `RET` 后让调用方恢复 coroutine。

它们共用同一个核心机制：

```text
session -> registry coroutine ref -> lua_resume
```

区别只在于谁来触发恢复：

- `Sleep`：timer 触发
- `Call`：`RET` 消息触发
- `Fork`：不等待外部触发，只负责启动新 coroutine
