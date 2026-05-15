#include "ConnWriter.h"
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include"Sunnet.h"
#include "Msg.h"
//对系统调用write的封装,由用户主动调用
void ConnWriter::EntireWrite(std::shared_ptr<char>buff,int len){
    if(isClosing){//正在关闭，处于延迟关闭状态，不再接受业务的写了
        std::cout<<"EntireWrite fail,Beacuse isClosing"<<std::endl;
        return;
    }

    if(objs.empty()){
        EntireWriteWhenEmpty(buff,len);
    }
    else{
        EntireWriteWhenNotEmpty(buff, len);
    }

}

void ConnWriter::EntireWriteWhenEmpty(std::shared_ptr<char>buff,int len){
    int fd=conn->fd;
    char* s=buff.get();
    int n=0;
    do {//如果是中断，重试
        n = write(fd, s, len);
    } while (n < 0 && errno == EINTR);
    if(n<0 && errno==EINTR){}
    std::cout << "EntireWriteWhenEmpty write n=" << n << std::endl;
    if(n>0&&n==len){
        //全写完了
        return;
    }
    if((n>0&&n<len) ||(n<0&&errno==EAGAIN) ){//缓冲区满
        std::shared_ptr<WriteObject>obj= std::make_shared<WriteObject>();
        if(n>0){
            obj->start=n;
        }else{
            obj->start=0;
        }
        
        obj->buff=buff;
        obj->len=len;
        objs.push_back(obj);
        Sunnet* inst=Sunnet::get_instance();
        inst->ModifyEvent(fd,true);
        return;
    }
    //真发生了错误
    std::cout << "EntireWrite write error " <<  std::endl;
}

void ConnWriter::EntireWriteWhenNotEmpty(std::shared_ptr<char>buff,int len){
    std::shared_ptr<WriteObject>obj= std::make_shared<WriteObject>();
    obj->start=0;
    obj->buff=buff;
    obj->len=len;
    objs.push_back(obj);
}

//收到epoll线程的可写通知的回调，这里面实现了优雅的写完后再关闭
void ConnWriter::OnWriteable(){
    if(conn->closed){//如果连接已关闭
        return;
    }
    //不停发送，直到失败
    while(WriteFrontObj()){}

    int fd=conn->fd;
    if(objs.empty()){
        Sunnet *inst=Sunnet::get_instance();
        inst->ModifyEvent(fd,false);

         if(isClosing){
            std::cout<<"linger close conn"<<std::endl;
            shutdown(fd,SHUT_RD);
            std::shared_ptr<SocketRWMsg> msg=std::make_shared<SocketRWMsg>();//发一条读消息触发
            msg->conn=conn;
            msg->isRead=true;
            msg->type=BaseMsg::TYPE::SOCKET_RW;
            msg->isCloseReq=true;
            inst->Send(conn->serviceId,msg);
        }
    }

}
//写完完整的一条返回true
bool ConnWriter::WriteFrontObj(){
    if(objs.empty()){
        return false;
    }
    int fd=conn->fd;
    std::shared_ptr<WriteObject> obj=objs.front();
    char *s=obj->buff.get() + obj->start;
    int len=obj->len-obj->start;
    int n=0;
    do {
        n = write(fd, s, len);
    } while (n < 0 && errno == EINTR);
    std::cout << "WriteFrontObj write n=" << n << std::endl;
    if(n>0&&n==len){
        objs.pop_front();//全部写完，这条出队
        return true;
    }
    if((n>0&&n<len)||(n<0&&errno==EAGAIN)){
        if(n>0){
            obj->start+=n;
        }
        
        return false;
    }
    //真发生了错误
    std::cout << "WriteFrontObj write error " <<  std::endl;
    return false;
}

void ConnWriter::LingerClose(){
    if(isClosing){
        return;
    }
    isClosing=true;
    int fd=conn->fd;
    
    //如果缓冲区队列空，立刻发消息提醒关闭
    if(objs.empty()){
        shutdown(fd,SHUT_RD);//关闭读端
        Sunnet *inst=Sunnet::get_instance();
        std::cout<<"linger close conn"<<std::endl;
        std::shared_ptr<SocketRWMsg> msg=std::make_shared<SocketRWMsg>();
        msg->conn=conn;
        msg->isRead=true;
        msg->type=BaseMsg::TYPE::SOCKET_RW;
        msg->isCloseReq=true;
        inst->Send(conn->serviceId,msg);
    }

    //如果缓冲区队列不空，等OnWriteable()里面发消息关闭
}

