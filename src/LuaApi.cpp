#include"LuaApi.h"
#include"Sunnet.h"
#include <cstring>
#include <memory>
#include <string>
#include<iostream>
#include <Msg.h>
#include "Service.h"
//Send用，序列化参数，把lua层压入栈的参数序列化成字符串
static bool PackValue(lua_State* luaState,int idx,std::string &out,int depth){
    if(depth>32){
          std::cout<<"LuaApi:send failed, table too deep"<<std::endl;
          return false;
    }

    idx=lua_absindex(luaState,idx);//固定一下索引，后面递归会传入负索引

    if(lua_isinteger(luaState,idx)){
            out.push_back((char)LuaApi::TYPE_INTEGER);
            lua_Integer v=lua_tointeger(luaState,idx);
            out.append((char*)&v,sizeof(lua_Integer));
            return true;
        }else if(lua_isnumber(luaState,idx)){
            out.push_back((char)LuaApi::TYPE_NUMBER);
            lua_Number v=lua_tonumber(luaState,idx);
            out.append((char*)&v,sizeof(lua_Number));
            return true;
        }
        else if(lua_isstring(luaState,idx)){
            out.push_back((char)LuaApi::TYPE_STRING);

            size_t len;//32位平台32位，64位平台64位
            const char* s=lua_tolstring(luaState,idx,&len);
            uint32_t n=(uint32_t) len;
            out.append((char*) &n,sizeof(uint32_t));
            out.append(s,len);
            return true;
        }
        else if(lua_istable(luaState,idx)){
            out.push_back((char)LuaApi::TYPE_TABLE);

            uint32_t pairCount=0;//table键值对数
            size_t countPos=out.size();//记录pairCount在序列化字符串中的位置
            out.append((char*)&pairCount,sizeof(pairCount));//占位

            lua_pushnil(luaState);
            while (lua_next(luaState,idx)!=0)
            {
                
                if(!(PackValue(luaState,-2,out,depth+1))){//递归序列化键
                    lua_pop(luaState,2);
                    return false;
                }
                if(!(PackValue(luaState,-1,out,depth+1))){//递归序列化值
                    lua_pop(luaState,2);
                    return false;
                }
                lua_pop(luaState,1);//pop掉value,剩key作为lua_next下一个输入
                pairCount++;
            }
            
            //回填pairCount
            memcpy(&out[countPos],&pairCount,sizeof(uint32_t));
            return true;

        }
        
        std::cout<<"LuaApi:send failed, arg type not support"<<std::endl;
        return false;
        

}

int LuaApi::NewService(lua_State* luaState){
    int num= lua_gettop(luaState);//获取栈顶的索引

    if(lua_isstring(luaState,1)==0||num!=1){//1为是，0为不是
        lua_pushinteger(luaState,-1);//压入-1
        return 1;//1个返回值
    }
    size_t len;
    const char* type= lua_tolstring(luaState,1,&len);//从索引1拿字符串
    //char *newstr=new char[len+1];//要加\0
    //newstr[len]='\0';
    //type字符串的内存由lua gc 管理，这里拷贝一份，防止lua回收掉内存
    //memcpy(newstr,type,len);
    //直接用lua传来的构造string，相当于拷贝了一份
    auto t=std::make_shared<std::string>(type,len);
    Sunnet* inst=Sunnet::get_instance();
    
    uint32_t id= inst->NewService(t);
    if(id==(uint32_t)-1){
        lua_pushinteger(luaState,-1);
    }else{
        lua_pushinteger(luaState,id);
    }
    return 1;
}


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

int LuaApi::KillService(lua_State *luaState){
    int num=lua_gettop(luaState);
    if (num!=1||lua_isinteger(luaState,-1)==0){
        lua_pushinteger(luaState,-1);
        return 1;
    }

    uint32_t target_id= lua_tointeger(luaState,1);
    
    //查一下当前服务的id
    lua_getfield(luaState,LUA_REGISTRYINDEX,"__service_id");
    uint32_t selfId=lua_tointeger(luaState,-1);
    if(selfId!=target_id){
        lua_pushinteger(luaState, -1);
        return 1;
    }

    Sunnet *inst=Sunnet::get_instance();
    //killservice不要关闭luaState，不然会导致这里返回后对luaState的操作报错
    inst->KillService(target_id);

    lua_pushinteger(luaState,1);
    return 1;
}

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


    /*lua参数序列化格式
        [arg_count:1字节]
        [type:1字节][data...]
        [type:1字节][data...]
        ...

        其中string将长度(4字节)放data前面
        table格式：
        [type = TABLE:1字节]
        [pair_count:4字节]
        [key1]
        [value1]
        [key2]
        [value2]
        ...

    */

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
    msg->buff=std::shared_ptr<char>(newBuff,[](char* p){delete []p;});
    msg->size=out.size();
    msg->session=0;

    Sunnet* inst=Sunnet::get_instance();
    inst->Send(target_id,msg);
    //lua层无返回值
    return 0;

}

int LuaApi::Call(lua_State *luaState){
    int num=lua_gettop(luaState);
    if(num < 2){
        return luaL_error(luaState, "sunnet.Call(target, ...) need target and args");
    }
    //检查是否为协程调用call
    int isMainThread = lua_pushthread(luaState);
    lua_pop(luaState, 1);
    if(isMainThread){
        return luaL_error(luaState, "sunnet.Call must be called in coroutine");
    }

    //拿服务指针
    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    Service* srv = (Service*)lua_touserdata(luaState, -1);
    lua_pop(luaState, 1);
    if(srv == nullptr){
        return luaL_error(luaState, "sunnet.Call missing service ptr");
    }
    //取得目标服务id
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
    //序列化参数
    std::string out;
    out.push_back((char)(num-1));

    for(int i=2;i<=num;i++){
        if(PackValue(luaState,i,out,0)==false){
            return luaL_error(luaState, "sunnet.Call pack args failed");
        }
    }
    //登记挂起信息
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
        //发送失败，移除挂起信息，抛出lua错误
        auto it = srv->sessionCoRefs.find(session);
        luaL_unref(luaState, LUA_REGISTRYINDEX, it->second);
        srv->sessionCoRefs.erase(it);
        return luaL_error(luaState, "sunnet.Call send failed");
    }

    return lua_yield(luaState, 0);
}


int LuaApi::Ret(lua_State *luaState){
    int num=lua_gettop(luaState);
    //拿本服务指针
    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    Service* srv = (Service*)lua_touserdata(luaState, -1);
    lua_pop(luaState, 1);
    if(srv == nullptr){
        return luaL_error(luaState, "sunnet.Ret missing service ptr");
    }
    //拿本协程正在处理的rpc上下文
    auto it = srv->coRpcContexts.find(luaState);
    if(it == srv->coRpcContexts.end()){
        return luaL_error(luaState, "sunnet.Ret must be called in RPC request");
    }
    //禁止重复ret
    if(it->second.reted){
        return luaL_error(luaState, "sunnet.Ret already called");
    }
    //序列化返回值
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

int LuaApi::Fork(lua_State *luaState){
    //fork的协程会立刻执行
    int num=lua_gettop(luaState);
    if(num < 1 || !lua_isfunction(luaState, 1)){
        return luaL_error(luaState, "sunnet.Fork(fn, ...) need function");
    }
    //协程函数参数数量
    int nargs = num - 1;

    lua_State* co = lua_newthread(luaState);

    //移动协程对象到fn 和参数前面
    lua_insert(luaState,lua_gettop(luaState)-num);
    
    //移动函数和参数到新协程栈
    lua_xmove(luaState, co, num);

    int status = lua_resume(co, luaState, nargs);
    
    if(status == LUA_OK || status == LUA_YIELD){
        //抛出协程，如果是LUA_YIELD,则已经在register表里面保活协程了，栈此处不再保留引用
        lua_pop(luaState, 1);
        return 0;
    }else{
        
        lua_pop(luaState, 1);
        return luaL_error(luaState, "sunnet.Fork failed %s", lua_tostring(co, -1));
    }

}

int LuaApi::Name(lua_State* luaState){
    int num=lua_gettop(luaState);
    //1个参数：给本服务命名该参数
    //2个参数，给id(参数2)命名(参数1)

    uint32_t id;
    size_t len;
    const char* nameStr;
    if(num==1){
        if(lua_isstring(luaState,1) == 0){
            lua_pushinteger(luaState, -1);
            return 1;
        }
        lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_id");
        id = lua_tointeger(luaState, -1);
        nameStr = lua_tolstring(luaState,1,&len);
    }else if(num==2){
        if(lua_isstring(luaState,1) == 0 || lua_isinteger(luaState,2) == 0){
            lua_pushinteger(luaState, -1);
            return 1;
        }
        nameStr = lua_tolstring(luaState,1,&len);
        id=lua_tointeger(luaState,2);
    }else{
        lua_pushinteger(luaState, -1);
        return 1;
    }

    std::shared_ptr<std::string>name= std::make_shared<std::string>(nameStr,len);
    bool ok=Sunnet::get_instance()->Name(id,name);
    lua_pushinteger(luaState,ok?1:-1);
    return 1;
}


int LuaApi::SelfName(lua_State *luaState){
    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_id");
    uint32_t selfId = lua_tointeger(luaState, -1);
    lua_pop(luaState, 1);

    auto name = Sunnet::get_instance()->GetServiceName(selfId);
    if(!name){
        lua_pushnil(luaState);
        return 1;
    }

    lua_pushlstring(luaState, name->c_str(), name->size());
    return 1;
}

int LuaApi::Write(lua_State *luaState ){
    int num = lua_gettop(luaState);
    if(num != 2 || lua_isinteger(luaState, 1) == 0 || lua_isstring(luaState, 2) == 0){
        lua_pushinteger(luaState, -1);
        return 1;
    }//参数1文件描述符，参数2字符串

    int fd = (int)lua_tointeger(luaState, 1);

    size_t len = 0;
    const char* s = lua_tolstring(luaState, 2, &len);

    char* raw = new char[len];
    memcpy(raw, s, len);
    std::shared_ptr<char> buff(raw, [](char* p){ delete[] p; });

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_id");
    uint32_t selfId = (uint32_t)lua_tointeger(luaState, -1);
    lua_pop(luaState, 1);

    bool ok = Sunnet::get_instance()->Write(selfId, fd, buff, (int)len);
    lua_pushinteger(luaState, ok ? 1 : -1);
    return 1;
}


int LuaApi::Listen(lua_State* luaState){
    int num = lua_gettop(luaState);
    if(num != 1 || lua_isinteger(luaState, 1) == 0){
        lua_pushinteger(luaState, -1);
        return 1;
    }

    uint32_t port = (uint32_t)lua_tointeger(luaState, 1);

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_id");
    uint32_t selfId = (uint32_t)lua_tointeger(luaState, -1);
    lua_pop(luaState, 1);

    int listenfd = Sunnet::get_instance()->Listen(port, selfId);
    lua_pushinteger(luaState, listenfd);
    return 1;
}

int LuaApi::CloseConn(lua_State* luaState){
    int num = lua_gettop(luaState);
    if(num != 1 || lua_isinteger(luaState, 1) == 0){
        lua_pushinteger(luaState, -1);
        return 1;
    }

    int fd = (int)lua_tointeger(luaState, 1);

    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_id");
    uint32_t selfId = (uint32_t)lua_tointeger(luaState, -1);
    lua_pop(luaState, 1);

    bool ok = Sunnet::get_instance()->LingerClose(selfId, fd);
    lua_pushinteger(luaState, ok ? 1 : -1);
    return 1;
}

int LuaApi::Sleep(lua_State *luaState){
    int num = lua_gettop(luaState);
    if(num != 1 || lua_isinteger(luaState, 1) == 0){
        //抛出lua error
        return luaL_error(luaState, "sunnet.Sleep(ms) need integer ms");
    }
    int isMainThread = lua_pushthread(luaState);
    lua_pop(luaState, 1);
    if(isMainThread){//如果是主程，返回1，如果是协程，返回0
        return luaL_error(luaState, "sunnet.Sleep must be called in coroutine");
    }

    uint32_t ms = (uint32_t)lua_tointeger(luaState, 1);
    //拿service的指针
    lua_getfield(luaState, LUA_REGISTRYINDEX, "__service_ptr");
    Service* srv = (Service*)lua_touserdata(luaState, -1);
    lua_pop(luaState, 1);
    //登记挂起信息，并设置定时器
    srv->SleepCurrentCoroutine(luaState, ms);
    //yield
    return lua_yield(luaState, 0);
}