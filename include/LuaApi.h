#pragma once

extern "C"{
    #include<lua.h>
    #include<lauxlib.h>
}

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
    static int Call(lua_State *luaState);
    static int Ret(lua_State *luaState);
    static int Fork(lua_State *luaState);
};