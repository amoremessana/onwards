#pragma once
#include"ErrorWords.hh"
#include"platforms.hh"
#include"wrappers.hh"
#include<fcntl.h>

namespace cmw{
inline auto tcp_server (char const* port){
  getaddrinfo_wrapper res(nullptr,port,SOCK_STREAM,AI_PASSIVE);
  for(auto r=res();r!=nullptr;r=r->ai_next){
    sock_type sock=::socket(r->ai_family,r->ai_socktype,0);
    if(-1==sock)continue;

    int on=1;
    if(::setsockopt(sock,SOL_SOCKET,SO_REUSEADDR
                    ,(char const*)&on,sizeof(on))<0){
      closeSocket(sock);
      throw failure("tcp_server setsockopt ")<<GetError();
    }

    if(::bind(sock,r->ai_addr,r->ai_addrlen)<0){
      closeSocket(sock);
      throw failure("tcp_server bind ")<<GetError();
    }

    if(::listen(sock,SOMAXCONN)<0){
      closeSocket(sock);
      throw failure("tcp_server listen ")<<GetError();
    }

    return sock;
  }
  throw failure("tcp_server");
}

inline auto accept_wrapper(sock_type s){
  auto nu=::accept(s,nullptr,nullptr);
  if(nu>=0)return nu;

  if(ECONNABORTED==GetError())return 0;
  throw failure("accept_wrapper ")<<GetError();
}

#if defined(__FreeBSD__)||defined(__linux__)
inline auto accept4_wrapper(sock_type s,int flags){
  ::sockaddr amb;
  ::socklen_t len=sizeof(amb);
  auto nu=::accept4(s,&amb,&len,flags);
  if(nu>=0)return nu;

  if(ECONNABORTED==GetError())return 0;
  throw failure("accept4_wrapper ")<<GetError();
}
#endif
}
