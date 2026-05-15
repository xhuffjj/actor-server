# 回调协程化方案

## 目标

当前服务里的 Lua 回调入口都是直接 `lua_pcall`：

- `OnServiceMsg`
- `OnAcceptMsg`
- `OnSocketData`
- `OnSocketClose`

这意味着这些回调必须一次执行到底，不能在中途 `yield`，因此下面这种写法目前做不到：

```lua
function OnSocketData(fd, buff)
    sunnet.Sleep(1000)
    sunnet.Write(fd, "echo:" .. buff)
end
```

本方案的目标是把这些回调整体改成“作为 Lua coroutine 运行”，让回调本身可以直接调用 `sunnet.Sleep(ms)` 挂起，等定时器到了再恢复执行。


## 为什么不能只把 `lua_pcall` 换成 `lua_resume`

不能简单把当前的 `lua_pcall` 替换成 `lua_resume`，原因是：

1. `lua_resume` 只能恢复 coroutine，不能直接拿主 `lua_State` 当成可挂起回调执行上下文。
2. 要支持 `sunnet.Sleep(ms)`，必须在回调进入前先创建一个新的 coroutine。
3. coroutine 挂起后，C++ 侧要保存它的引用，否则 Lua GC 会把它回收。
4. 定时器到期后，C++ 还要能根据 `session` 找回这条 coroutine 并恢复它。

所以真正需要的是一层统一的“Lua 回调调度器”，而不是局部替换某个 API。


## 总体设计

每次 C++ 收到一个事件消息，不再直接：

```cpp
lua_getglobal(luaState, "OnSocketData");
lua_pushinteger(luaState, fd);
lua_pushlstring(luaState, buff, len);
lua_pcall(luaState, 2, 0, 0);
```

而是统一改成：

1. 基于服务主 `luaState` 创建一个新的 Lua coroutine。
2. 把对应的 Lua 回调函数和参数压入该 coroutine 的栈。
3. 通过 `lua_resume` 启动这个 coroutine。
4. 如果回调正常结束，释放 coroutine 引用。
5. 如果回调执行到 `sunnet.Sleep(ms)` 并 `yield`，保存 coroutine 引用，等待后续定时器恢复。


## 建议增加的 Service 内部能力

建议在 `Service` 里增加下面几类状态和工具函数。

### 1. coroutine 等待表

```cpp
uint32_t nextSession = 1;
std::unordered_map<uint32_t, int> sessionCoRefs;
```

- `nextSession` 用来生成等待中的 session。
- `sessionCoRefs` 保存 `session -> luaL_ref`。
- `int` 是 coroutine 在线程注册表里的引用，防止 coroutine 被 GC。

### 2. Lua registry 上下文

当前已经把 `__service_id` 塞进了 registry。

建议再放一个：

```cpp
lua_pushlightuserdata(luaState, this);
lua_setfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
```

这样 `LuaApi::Sleep` 可以从当前 Lua 状态反查所属的 `Service*`。

### 3. 统一回调入口

建议把当前分散在 `OnServiceMsg` / `OnAcceptMsg` / `OnSocketData` / `OnSocketClose` 里的 Lua 调用逻辑，收敛成一个统一辅助函数，例如：

```cpp
int Service::StartCallbackCoroutine(const char* funcName, int nargs);
int Service::ResumeCoroutineRef(int ref, int nargs);
uint32_t Service::NewSession();
void Service::WaitSession(uint32_t session, int ref);
```

这里的重点不是函数名，而是把“创建 coroutine / resume / 错误处理 / unref / yield 后保留”这一套流程集中到一处。


## 回调如何作为 coroutine 启动

### 基本流程

以 `OnSocketData(fd, buff)` 为例。

1. 在主 `luaState` 上调用 `lua_newthread(luaState)` 创建 coroutine。
2. 立即把这个 coroutine 用 `luaL_ref(luaState, LUA_REGISTRYINDEX)` 挂到注册表，得到 `coRef`。
3. 往 coroutine 栈里压入目标 Lua 函数和参数。
4. 调用 `lua_resume(co, luaState, nargs)`。

Lua 5.3.5 的接口是：

```cpp
int lua_resume(lua_State *co, lua_State *from, int nargs);
```

这个项目当前 vendored 的 Lua 是 `3rd/lua-5.3.5`，所以实现必须按 Lua 5.3 写，不能按 Lua 5.4 的接口写。

### 返回值处理

如果 `lua_resume` 返回：

- `LUA_OK`
  - 回调执行完成。
  - 释放 `coRef`。

- `LUA_YIELD`
  - 说明回调内部主动挂起了。
  - 不释放 `coRef`。
  - coroutine 之后由 `session -> coRef` 这条链路恢复。

- 其他错误码
  - 读取 coroutine 栈顶错误信息。
  - 打日志。
  - 释放 `coRef`。


## `sunnet.Sleep(ms)` 的设计

### 行为

`sunnet.Sleep(ms)` 只能在 coroutine 里调用。

不允许在主 `lua_State` 上直接调用，否则应该报错。

### C++ 侧逻辑

`LuaApi::Sleep` 大致做这几步：

1. 从 registry 取出 `__service_ptr`，拿到当前 `Service*`。
2. 检查 `ms` 参数是否合法。
3. 检查当前 `lua_State* L` 是不是 coroutine。
4. 生成一个新的 `session`。
5. 对当前 coroutine 做 `lua_pushthread(L)` + `luaL_ref(L, LUA_REGISTRYINDEX)`，得到 `waitRef`。
6. 把 `session -> waitRef` 存到 `sessionCoRefs`。
7. 调 `Sunnet::Timeout(serviceId, session, ms)` 注册定时器。
8. 返回 `lua_yield(L, 0)`。

伪代码：

```cpp
int LuaApi::Sleep(lua_State* L) {
    Service* srv = GetServiceFromRegistry(L);
    uint32_t ms = luaL_checkinteger(L, 1);

    if (lua_pushthread(L) == 1) {
        return luaL_error(L, "sunnet.Sleep must run in coroutine");
    }
    lua_pop(L, 1);

    uint32_t session = srv->NewSession();

    lua_pushthread(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    srv->WaitSession(session, ref);
    Sunnet::get_instance()->Timeout(srv->id, session, ms);

    return lua_yield(L, 0);
}
```


## 定时器消息如何恢复 coroutine

### 消息类型

建议补完整 `TimerMsg`，至少包含：

```cpp
class TimerMsg : public BaseMsg {
public:
    uint32_t session;
};
```

### 恢复流程

当 `Service::OnMsg` 收到 `BaseMsg::TYPE::TIMER`：

1. 取出 `session`。
2. 从 `sessionCoRefs` 找到对应 `ref`。
3. 从注册表里 `lua_rawgeti(luaState, LUA_REGISTRYINDEX, ref)` 取回 coroutine。
4. `lua_tothread(luaState, -1)` 拿到 `lua_State* co`。
5. 从主状态栈上把这个 coroutine 弹掉。
6. 再次调用 `lua_resume(co, luaState, 0)` 恢复执行。

恢复后的处理规则和第一次启动一致：

- `LUA_OK`：执行完，`luaL_unref`，从 `sessionCoRefs` 删除。
- `LUA_YIELD`：说明这条 coroutine 又进入了新的等待，比如再次 `Sleep`。
- 其他错误：打日志，`luaL_unref`，删除等待项。

注意一点：

恢复前要先把旧 `session` 从 `sessionCoRefs` 里删掉。因为 coroutine 这次恢复后如果再次 `Sleep`，会登记一个新的 `session`。旧项不能残留。


## 哪些回调建议协程化

第一版建议只协程化这些运行期回调：

- `OnServiceMsg`
- `OnAcceptMsg`
- `OnSocketData`
- `OnSocketClose`

`OnInit` 和 `OnExit` 第一版不建议支持 `yield`。

原因：

1. `OnInit` 如果挂起，服务生命周期会变复杂，服务可能处于“已创建但未真正初始化完成”的中间状态。
2. `OnExit` 如果挂起，资源回收和关闭时序会难控制。

先把事件回调跑通，边界最清楚。


## 与 `Fork + Sleep` 方案的区别

之前的保守方案是：

```lua
function OnSocketData(fd, buff)
    sunnet.Fork(function()
        sunnet.Sleep(1000)
        sunnet.Write(fd, "echo:" .. buff)
    end)
end
```

这套方案的优点是改动小，能快速落地。

而本方案更进一步，直接允许：

```lua
function OnSocketData(fd, buff)
    sunnet.Sleep(1000)
    sunnet.Write(fd, "echo:" .. buff)
end
```

区别在于：

- `Fork + Sleep` 方案里，回调本身仍然不能 `yield`。
- 回调协程化方案里，回调本身就是 coroutine 入口。
- 后者业务代码更自然，但对调度器要求更高。


## 最关键的语义变化

这是本方案必须明确写清楚的点。

当 `OnSocketData(fd, buff)` 可以挂起后，同一个服务不会因为这一条回调 `Sleep` 就整体卡住。worker 线程会继续处理该服务后续收到的其他消息。

这意味着：

1. 一个 `fd` 的多次 `OnSocketData` 可能并发交错执行。
2. `OnServiceMsg` 和 `OnSocketData` 之间也可能交错。
3. Lua 业务层不能再默认“回调串行且一次执行到底”。

这是能力提升，同时也是语义变化。

### 如果业务需要严格顺序

如果某个连接必须严格按顺序处理，Lua 层要自己做顺序控制，例如：

- 给每个 `fd` 做一个消息队列。
- 或者做一个“连接忙碌标记”，前一个 coroutine 没结束前，后续消息只入队不立即处理。

也就是说：

- C++ 层负责“回调可挂起”
- Lua 层负责“同一业务对象是否要串行化”

这个边界是合理的，不建议第一版把“按 fd 自动串行调度 coroutine”一起塞进 C++，那会把模型搞复杂很多。


## 建议的落地顺序

### 第 1 步

补完整定时器消息结构和 `Sunnet::Timeout(serviceId, session, ms)`。

### 第 2 步

在 `Service` 中加：

- `nextSession`
- `sessionCoRefs`
- registry 里的 `__service_ptr`

### 第 3 步

实现 `LuaApi::Sleep(lua_State* L)`。

先只支持“在 coroutine 中挂起并等定时器恢复”。

### 第 4 步

把 `OnSocketData` 改成 coroutine 入口，先单点跑通。

这是最适合做第一条验证链路的入口，因为好测试。

### 第 5 步

把 `OnServiceMsg`、`OnAcceptMsg`、`OnSocketClose` 也统一切到同一套 coroutine 回调启动逻辑。


## 第一版测试建议

建议直接用 `service/chat/init.lua` 或 `service/ping/init.lua` 做一个最小验证：

```lua
function OnSocketData(fd, buff)
    print("step1", fd, buff)
    sunnet.Sleep(1000)
    print("step2", fd, buff)
    sunnet.Write(fd, "echo:" .. buff)
end
```

验证点：

1. `step1` 会立即打印。
2. 1 秒后 `step2` 打印。
3. 这 1 秒内服务仍能处理其他消息。
4. coroutine 恢复后不会崩溃。
5. coroutine 结束后引用会被正确释放。


## 不建议第一版一起做的事情

第一版不要一起做这些：

- `sunnet.Call / Ret`
- `sunnet.Fork`
- `userdata` 连接对象
- 自动按 fd 串行调度 coroutine
- `OnInit` / `OnExit` 可挂起

原因很简单：这些都能复用同一套 coroutine 基础设施，但不是把“事件回调本身协程化”跑通所必需的。


## 总结

如果目标是让下面这种代码成立：

```lua
function OnSocketData(fd, buff)
    sunnet.Sleep(1000)
    sunnet.Write(fd, "echo:" .. buff)
end
```

那最合适的做法不是继续要求业务手动 `Fork`，而是：

1. 所有运行期 Lua 回调都用 coroutine 启动。
2. `sunnet.Sleep(ms)` 在 coroutine 里 `yield`。
3. 定时器到期后通过 `session -> coroutine ref` 恢复。
4. 保持“C++ 负责可挂起，Lua 负责业务串行语义”的边界。

这是比 `Fork + Sleep` 更自然的一步，但也确实比前一个方案更重。第一版应当只做最小链路，不要把 RPC、userdata、自动串行调度一起塞进来。
