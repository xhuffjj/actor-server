local serivceId

function OnInit(id)
    print("[lua] ping OnInit id:" .. id);
    serivceId=id
    
end

local function test1(source,buff,...)
    local integertest,numbertest=...
    if(source==0)then
        sunnet.Send("pong",buff .. "i",1,1.23456)
        return
    end
    print("[lua] ping OnServiceMsg id " .. serivceId .. " buff len:" .. string.len(buff) .. " integertest: " .. integertest .. " numbertest: " .. numbertest);
    if(string.len(buff)>50)then
        sunnet.KillService(serivceId)
        return
    end
    sunnet.Send(source,buff .. "i",integertest+1,numbertest+1)
end

function test2(source,...)
    local buff,tabletest=...
    if(source==0)then
        sunnet.Send("pong",buff .. "i",{x=1, y="abc"})
        return
    end
    print("[lua] ping OnServiceMsg id " .. serivceId .. " buff len:" .. string.len(buff) .. " table:x: " .. tabletest.x .. " table:y: " .. tabletest.y);
    if(string.len(buff)>50)then
        sunnet.KillService(serivceId)
        return
    end
    sunnet.Sleep(10000)
    sunnet.Send(source,buff .. "i",{x=1, y="abc"})
   
end





function OnServiceMsg(session,source,buff,...)
    --test2(source,buff,...)
end

function OnExit()
    print("[lua] ping OnExit")
end