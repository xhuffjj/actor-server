# sunnet

本项目是在 [luopeiyu/million_game_server](https://github.com/luopeiyu/million_game_server) 的 `sunnet` 基础上进行二次开发的 C++/Lua 服务框架实验项目。

在已有 C++/Lua 服务框架之上，本项目扩展了 Lua 参数序列化、定时器、Lua coroutine 挂起与恢复、服务间 RPC 调用，以及非阻塞 Socket 写缓冲与优雅关闭能力。

## 功能特性

- 基于 C++ 的服务管理、全局消息队列和 worker 线程调度。
- 使用 Lua 编写具体服务逻辑，每个服务从 `service/<type>/init.lua` 加载。
- 实现服务间消息序列化协议，支持 `integer`、`number`、`string`、`table` 参数传递。
- 扩展 `SEND`、`CALL`、`RET` 三类服务消息，支持同步风格 RPC。
- 实现最小堆定时器，并接入 Lua coroutine 恢复机制。
- 支持 `sunnet.Sleep(ms)` 非阻塞挂起当前 coroutine。
- 完善非阻塞 Socket 写缓冲，处理部分写入、`EAGAIN`、`EPOLLOUT` 注册和缓冲区排空后的优雅关闭。

## 目录结构

```text
.
├── 3rd/lua-5.3.5/   # Lua 5.3.5 源码与静态库
├── docs/            # 设计与实现记录
├── include/         # C++ 头文件
├── service/         # Lua 服务脚本
│   ├── main/
│   ├── db/
│   ├── ping/
│   └── chat/
├── src/             # C++ 源码
└── CMakeLists.txt
```

## 编译

依赖：

- Linux
- `g++`
- `cmake`
- `make`

Ubuntu / Debian 可安装基础构建工具：

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

本仓库使用 `3rd/lua-5.3.5/src/liblua.a`。如果该文件不存在，可以重新编译 Lua：

```bash
make -C 3rd/lua-5.3.5 linux
```

编译项目：

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

生成的可执行文件为：

```text
build/sunnet
```

## 运行

注意：服务脚本路径目前按运行目录拼接为 `../service/<type>/init.lua`，所以需要进入 `build/` 目录运行：

```bash
cd build
./sunnet
```

程序启动后会创建 `main` 服务，加载：

```text
service/main/init.lua
```

当前示例中，`main` 服务会创建 `db` 服务，并通过 `sunnet.Call("db", "get", 1001)` 测试 RPC。程序会持续运行，停止时按 `Ctrl+C`。

如果要测试 `chat` 网络服务，可以在 `service/main/init.lua` 中调用 `testchat()`，然后重新运行程序。`chat` 服务默认监听 `8002` 端口，可用 `nc` 连接测试：

```bash
nc 127.0.0.1 8002
```

## Lua 服务约定

每个服务类型对应一个目录：

```text
service/<type>/init.lua
```

例如：

```lua
local db = sunnet.NewService("db")
```

会加载：

```text
service/db/init.lua
```

Lua 服务可实现以下回调：

```lua
function OnInit(id)
    -- 服务创建时调用
end

function OnExit()
    -- 服务退出时调用
end

function OnServiceMsg(session, source, ...)
    -- 收到服务间消息时调用
    -- session == 0 表示普通 Send 消息
    -- session ~= 0 表示 Call 请求，可用 sunnet.Ret(...) 回复
end

function OnAcceptMsg(listenfd, clientfd)
    -- 有新连接接入时调用
end

function OnSocketData(fd, buff)
    -- 收到 socket 数据时调用
end

function OnSocketClose(fd)
    -- socket 关闭时调用
end
```

`OnServiceMsg`、`OnAcceptMsg`、`OnSocketClose` 以及 `sunnet.Fork` 启动的函数运行在 Lua coroutine 中，可以使用 `sunnet.Sleep` / `sunnet.Call`。当前 `OnSocketData` 仍使用普通 Lua 调用，不要在里面直接 `sunnet.Sleep`。

## Lua API

### `sunnet.NewService(type)`

创建一个服务。

```lua
local id = sunnet.NewService("db")
```

返回服务 id，失败返回 `-1`。

### `sunnet.KillService(id)`

销毁服务。当前实现只允许服务销毁自己。

```lua
sunnet.KillService(serviceId)
```

成功返回 `1`，失败返回 `-1`。

### `sunnet.Name(name[, id])`

给服务注册名字。

```lua
sunnet.Name("main")
sunnet.Name("db", dbId)
```

只传 `name` 时给当前服务命名；传 `name, id` 时给指定服务命名。成功返回 `1`，失败返回 `-1`。

### `sunnet.SelfName()`

获取当前服务名。

```lua
local name = sunnet.SelfName()
```

有名字时返回字符串，否则返回 `nil`。

### `sunnet.Send(target, ...)`

发送普通服务消息，不等待返回值。

```lua
sunnet.Send("db", "set", 1001, { name = "alice" })
sunnet.Send(2, "hello")
```

`target` 可以是服务 id，也可以是通过 `sunnet.Name` 注册的服务名。

### `sunnet.Call(target, ...)`

发送 RPC 请求，并挂起当前 coroutine，直到目标服务 `sunnet.Ret(...)` 返回。

```lua
local ok, user = sunnet.Call("db", "get", 1001)
```

`sunnet.Call` 必须在 coroutine 中调用。当前可在 `OnServiceMsg`、`OnAcceptMsg`、`OnSocketClose` 或 `sunnet.Fork` 创建的函数中使用。

### `sunnet.Ret(...)`

回复当前正在处理的 RPC 请求。

```lua
function OnServiceMsg(session, source, cmd, id)
    if session ~= 0 and cmd == "get" then
        sunnet.Ret(1, { id = id, name = "alice" })
    end
end
```

`sunnet.Ret` 只能在处理 `CALL` 请求的 `OnServiceMsg` coroutine 中调用。

### `sunnet.Fork(fn, ...)`

启动一个新的 Lua coroutine 并立即执行。

```lua
sunnet.Fork(function()
    sunnet.Sleep(1000)
    local ok, user = sunnet.Call("db", "get", 1001)
    print(ok, user.name)
end)
```

`Fork` 中可以使用 `sunnet.Sleep` 和 `sunnet.Call`。

### `sunnet.Sleep(ms)`

非阻塞挂起当前 coroutine，指定毫秒后恢复。

```lua
sunnet.Sleep(1000)
```

`sunnet.Sleep` 必须在 coroutine 中调用，不会阻塞 C++ worker 线程。

### `sunnet.Listen(port)`

监听 TCP 端口。

```lua
local listenfd = sunnet.Listen(8002)
```

成功返回监听 fd，失败返回 `-1`。

### `sunnet.Write(fd, data)`

向 socket 写入数据。

```lua
sunnet.Write(fd, "hello\n")
```

底层会走非阻塞写缓冲，支持部分写入和 `EAGAIN` 后等待 `EPOLLOUT`。成功返回 `1`，失败返回 `-1`。

### `sunnet.CloseConn(fd)`

优雅关闭连接。

```lua
sunnet.CloseConn(fd)
```

如果写缓冲区还有数据，会等待缓冲区排空后再关闭。成功返回 `1`，失败返回 `-1`。

## 消息序列化

服务间消息参数序列化格式：

```text
[arg_count:1 byte]
[type:1 byte][data...]
[type:1 byte][data...]
...
```

当前支持的参数类型：

| 类型 | 说明 |
| --- | --- |
| `integer` | Lua integer |
| `number` | Lua number |
| `string` | 4 字节长度 + 字符串内容 |
| `table` | 4 字节键值对数量 + key/value 递归序列化 |

`table` 的 key 当前支持 `integer` 和 `string`，value 支持上述已支持类型。序列化递归深度限制为 32。

服务消息类型：

| 类型 | 说明 |
| --- | --- |
| `SEND` | 普通异步消息，`session = 0` |
| `CALL` | RPC 请求，携带调用方等待的 `session` |
| `RET` | RPC 返回，用原始 `session` 恢复调用方 coroutine |

## RPC 示例

调用方：

```lua
sunnet.Fork(function()
    local ok, user = sunnet.Call("db", "get", 1001)
    print("[lua] call ret", ok, user.id, user.name)
end)
```

被调用方：

```lua
function OnServiceMsg(session, source, cmd, id)
    if session ~= 0 then
        if cmd == "get" then
            sunnet.Sleep(1000)
            sunnet.Ret(1, { id = id, name = "alice" })
            return
        end

        sunnet.Ret(0, "unknown cmd")
    end
end
```

## 来源说明

本项目基于 [luopeiyu/million_game_server](https://github.com/luopeiyu/million_game_server) 中的 `sunnet` 进行二次开发。发布或继续分发时，请保留原项目来源说明，并确认原项目许可证要求。
