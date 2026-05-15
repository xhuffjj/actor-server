#pragma once

#include<memory>
class Service;
class Sunnet;//只前向声明，源文件再包含头文件

class Worker{
    public:
    int id;//编号
    int eachNum;//每次处理多少消息
    void operator()(Sunnet *inst);//仿函数
    private:
    void CheckAndPutGlobal(std::shared_ptr<Service> srv);////检查还有没有消息，还有的话还要返回全局队列
};

