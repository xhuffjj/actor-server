print("run lua init.lua")
local serivceId

local function testping()
    
    local ping1 = sunnet.NewService("ping");
    print("[lua] new serivce ping1:" .. ping1);
    sunnet.Name("ping1",ping1)
    local ping2 = sunnet.NewService("ping");
    print("[lua] new serivce ping2:" .. ping2);
     sunnet.Name("ping2",ping2)
    local pong = sunnet.NewService("ping");
    print("[lua] new serivce pong:" .. pong);
     sunnet.Name("pong", pong)

    
    sunnet.Send(ping1,"start")
    sunnet.Send(ping2,"start");
end


local function testchat()
    local chat=sunnet.NewService("chat")

end


local function testcall()
    local db = sunnet.NewService("db");
    sunnet.Name("db",db)
    sunnet.Fork(
        function ()
            sunnet.Sleep(1000)
            local ok, user = sunnet.Call("db","get", 1001)
            print("[lua] call ret", ok, user.id, user.name)
        end
    )
    
end

function OnInit(id)
    print("[lua] main OnInit id:" .. id)
    sunnet.Name("main")
    serivceId=id
    --testchat()
    --testfork()
    testcall()
end

function OnExit()
    print("[lua] main OnExit");
end


