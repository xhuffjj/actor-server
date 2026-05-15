local serivceId
local conns={}

function OnInit(id)
    serivceId=id
    print("[lua] chat OnInit id: " .. id)
    sunnet.Listen(8002)
end

function OnExit()
    
end

function OnServiceMsg()
    
end

function OnAcceptMsg(listenfd,clientfd)
    print("[lua] chat OnAcceptMsg clientfd: " .. clientfd)
    conns[clientfd]=true
end

function OnSocketData(fd,buff)
    print("[lua] chat OnSocketData fd: " .. fd)
    for fd,_ in pairs(conns) do
        sunnet.Write(fd,buff)
    end
end

function OnSocketClose(fd)
    print("[lua] chat OnSocketClose fd: " .. fd)
    conns[fd]=nil
end

