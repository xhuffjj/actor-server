local serviceId

function OnInit(id)
    serviceId = id
    print("[lua] db OnInit id:" .. id)
    sunnet.Name("db")
end

function OnServiceMsg(session, source, cmd, id)
    if session~=0 then
        if cmd == "get" then
            sunnet.Sleep(1000)
            sunnet.Ret(1, { id = id, name = "alice" })
            return
        end

        sunnet.Ret(0, "unknown cmd")
    end
    
end

function OnExit()
    print("[lua] db OnExit")
end