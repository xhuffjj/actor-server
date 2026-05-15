#pragma once
#include <list>
#include <memory>

class Conn;

//没写完的要写的
class WriteObject{
    public:
    int start;//开始位置
    int len;//buff长度
    std::shared_ptr<char>buff;
};

//写缓冲区
class ConnWriter{
    public:
    std::shared_ptr<Conn> conn;//写缓冲区所属连接

    public:
    bool isClosing=false;//正在关闭的标志
    std::list<std::shared_ptr<WriteObject>>objs;

    public:
    void EntireWrite(std::shared_ptr<char>buff,int len);//封装write系统调用，队列空就write，否则推入队列
    void LingerClose();//延迟关闭，设置isclosing，半关闭读端，并且发消息通知本服务读，关闭连接统一在读那里做，一旦延迟关闭，就不再处理可读事件，只会处理可写事件，在sunnet系统中，可写事件的注册来源只有未写完的数据要写这一项
    void OnWriteable();//epoll通知可写时的回调


    private:
    void EntireWriteWhenEmpty(std::shared_ptr<char>buff,int len);//调用write写buff
    void EntireWriteWhenNotEmpty(std::shared_ptr<char>buff,int len);//buff插入队列
    bool WriteFrontObj();//从队列抛出字符，再write
};

