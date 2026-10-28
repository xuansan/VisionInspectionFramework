#pragma once
#include <winsock2.h>
#include "model.hpp"
namespace vision::database {
inline int socket_wait(SOCKET socket,bool read,bool write,bool except,const Deadline& deadline) {
    deadline.check();
    if(socket==INVALID_SOCKET)throw Error("DB.CONNECTION");
    fd_set r,w,e;FD_ZERO(&r);FD_ZERO(&w);FD_ZERO(&e);
    if(read)FD_SET(socket,&r);if(write)FD_SET(socket,&w);if(except)FD_SET(socket,&e);
    timeval timeout{0,5000};
    const int result=select(0,read?&r:nullptr,write?&w:nullptr,except?&e:nullptr,&timeout);
    if(result==SOCKET_ERROR)throw Error("DB.CONNECTION");
    deadline.check();
    return (FD_ISSET(socket,&r)?1:0)|(FD_ISSET(socket,&w)?2:0)|(FD_ISSET(socket,&e)?4:0);
}
}
