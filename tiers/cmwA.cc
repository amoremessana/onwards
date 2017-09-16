#include"account_info.hh"
#include<close_socket.hh>
#include<connect_wrapper.hh>
#include<ErrorWords.hh>
#include<File.hh>
#include<FILE_wrapper.hh>
#include<IO.hh>
#include<marshalling_integer.hh>
#include"message_ids.hh"
#include<poll_wrapper.hh>
#include<ReceiveBuffer.hh>
#include<ReceiveBufferCompressed.hh>
#include<ReceiveBufferStack.hh>
#include<setDirectory.hh>
#include<string_plus.hh>
#include<syslog_wrapper.hh>
#include<SendBufferCompressed.hh>
#include<SendBufferStack.hh>
#include<udp_stuff.hh>
#include"zz.middle_front.hh"

#include<memory> //unique_ptr
#include<vector>
#include<assert.h>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h> //strtol
#include<string.h>
#include<errno.h>
#include<netinet/in.h> //sockaddr_in6,socklen_t
#include<unistd.h> //pread

using namespace ::cmw;

int32_t previous_updatedtime;
int32_t current_updatedtime;

struct cmwRequest{
  ::sockaddr_in6 front;
  ::socklen_t frontlen=sizeof(front);
  int32_t latest_update;
  marshalling_integer const accountNbr;
  fixed_string_120 path;
  char const* middlefile;
  int fd;

  cmwRequest ()=default;

  template<class R>
  explicit cmwRequest (ReceiveBuffer<R>& buf):accountNbr(buf),path(buf){
    char* const pos=::strrchr(path(),'/');
    if(nullptr==pos)throw failure("cmwRequest didn't find a /");
    *pos='\0';
    middlefile=pos+1;
    setDirectory(path());
    char lastrun[60];
    ::snprintf(lastrun,sizeof(lastrun),"%s.lastrun",middlefile);
    fd=::open(lastrun,O_RDWR);
    previous_updatedtime=0;
    if(fd>=0){
      if(::pread(fd,&previous_updatedtime,sizeof(previous_updatedtime),0)==-1){
        ::close(fd);
        throw failure("pread ")<<errno;
      }
    }else{
      fd=::open(lastrun,O_RDWR|O_CREAT,S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
      if(fd<0)throw failure("open ")<<lastrun<<" "<<errno;
    }
    current_updatedtime=previous_updatedtime;
  }

  void save_lastruntime ()const
  {Write(fd,&latest_update,sizeof(latest_update));}

  void Marshal (SendBuffer& buf,bool=false)const{
    accountNbr.Marshal(buf);
    auto index=buf.ReserveBytes(1);
    if(MarshalFile(middlefile,buf))buf.Receive(index,true);
    else{
      buf.Receive(index,false);
      buf.Receive(middlefile);
      buf.InsertNull();
    }

    char line[100];
    int8_t updatedFiles=0;
    index=buf.ReserveBytes(sizeof(updatedFiles));
    FILE_wrapper Fl{middlefile,"r"};
    while(::fgets(line,sizeof(line),Fl.Hndl)){
      if('/'==line[0]&&'/'==line[1])continue;
      char const* tok=::strtok(line,"\n ");
      if(!::strcmp("message-lengths",tok))break;
      if(MarshalFile(tok,buf))++updatedFiles;
    }
    buf.Receive(index,updatedFiles);
  }

  ~cmwRequest (){::close(fd);}
};

#include"zz.middle_back.hh"

class cmwAmbassador{
  ReceiveBufferCompressed<
#ifdef CMW_ENDIAN_BIG
    LeastSignificantFirst
#else
    SameFormat
#endif
  > cmwBuf;

  SendBufferCompressed cmwSendbuf;
  SendBufferStack<> localsendbuf;
  ::std::vector<cmwAccount> accounts;
  ::std::vector<::std::unique_ptr<cmwRequest>> pendingRequests;
  ::pollfd fds[2];
  int loginPause;

  void login ();
  void reset (char const*);
  bool sendData ();
public:
  cmwAmbassador (char const*);
};

void cmwAmbassador::login (){
  for(;;){
    fds[0].fd=cmwSendbuf.sock_=cmwBuf.sock_=
       connect_wrapper("70.56.166.91",
#ifdef CMW_ENDIAN_BIG
                       "56790");
#else
                       "56789");
#endif
    if(fds[0].fd!=-1)break;
    ::printf("connect_wrapper %d\n",errno);
    poll_wrapper(nullptr,0,loginPause);
  }

  middle_back::Marshal(cmwSendbuf,Login,accounts);
  while(!cmwSendbuf.Flush());
  while(!cmwBuf.GotPacket());
  if(cmwBuf.GiveBool())set_nonblocking(fds[0].fd);
  else throw failure("Login:")<<cmwBuf.GiveString_view();
}

void cmwAmbassador::reset (char const* explanation){
  middle_front::Marshal(localsendbuf,false,string_plus{explanation});
  for(auto& r:pendingRequests){
    if(r.get())localsendbuf.Send((::sockaddr*)&r->front,r->frontlen);
  }
  pendingRequests.clear();
  close_socket(cmwBuf.sock_);
  cmwBuf.Reset();
  cmwSendbuf.CompressedReset();
  login();
}

bool cmwAmbassador::sendData (){
  try{return cmwSendbuf.Flush();}catch(::std::exception const& e){
    syslog_wrapper(LOG_ERR,"Problem sending data to CMW: %s",e.what());
    reset("Problem sending data to CMW");
  }
  return true;
}

#define CHECK_FIELD_NAME(fieldname)\
  ::fgets(lineBuf,sizeof(lineBuf),Fl.Hndl);\
  if(::strcmp(fieldname,::strtok(lineBuf," ")))\
    throw failure("Expected ")<<fieldname;

cmwAmbassador::cmwAmbassador (char const* configfile):cmwBuf(1100000)
  ,cmwSendbuf(1000000)
{
  char lineBuf[120];
  FILE_wrapper Fl(configfile,"r");
  while(::fgets(lineBuf,sizeof(lineBuf),Fl.Hndl)){
    char const* tok=::strtok(lineBuf," ");
    if(!::strcmp("Account-number",tok)){
      auto num=::strtol(::strtok(nullptr,"\n "),0,10);
      CHECK_FIELD_NAME("Password");
      accounts.emplace_back(num,::strtok(nullptr,"\n "));
    }else{
      if(accounts.empty())throw failure("An account number is required.");
      if(!::strcmp("UDP-port-number",tok))break;
      else throw failure("UDP-port-number is required.");
    }
  }
  fds[1].fd=localsendbuf.sock_=udp_server(::strtok(nullptr,"\n "));
#ifdef __linux__
  set_nonblocking(fds[1].fd);
#endif

  CHECK_FIELD_NAME("Login-attempts-interval-in-milliseconds");
  loginPause=::strtol(::strtok(nullptr,"\n "),0,10);
  CHECK_FIELD_NAME("Keepalive-interval-in-milliseconds");
  int keepaliveInterval=::strtol(::strtok(nullptr,"\n "),0,10);

  login();
  fds[0].events=fds[1].events=POLLIN;
  for(;;){
    if(0==poll_wrapper(fds,2,keepaliveInterval)){
      try{
        if(!pendingRequests.empty())throw failure("No reply from CMW");
        middle_back::Marshal(cmwSendbuf,Keepalive);
        fds[0].events|=POLLOUT;
        pendingRequests.push_back(nullptr);
      }catch(::std::exception const& e){
        syslog_wrapper(LOG_ERR,"Keepalive: %s",e.what());
        reset("Keepalive problem");
      }
      continue;
    }

    localsendbuf.Reset();
    if(fds[0].revents&POLLIN){
      try{
        if(cmwBuf.GotPacket()){
          do{
            assert(!pendingRequests.empty());
            if(pendingRequests.front().get()){
              auto const& request=*pendingRequests.front();
              if(cmwBuf.GiveBool()){
                setDirectory(request.path.c_str());
                empty_container<File>{cmwBuf};
                request.save_lastruntime();
                middle_front::Marshal(localsendbuf,true);
              }else middle_front::Marshal(localsendbuf,false,
                                 string_plus{"CMW:",cmwBuf.GiveString_view()});
              localsendbuf.Send((::sockaddr*)&request.front,request.frontlen);
              localsendbuf.Reset();
            }
            pendingRequests.erase(::std::begin(pendingRequests));
          }while(cmwBuf.NextMessage());
        }
      }catch(connection_lost const& e){
        syslog_wrapper(LOG_ERR,"Got end of stream notice: %s",e.what());
        reset("CMW stopped before your request was processed");
        continue;
      }catch(::std::exception const& e){
        syslog_wrapper(LOG_ERR,"Problem handling reply from CMW %s",e.what());
        assert(!pendingRequests.empty());
        if(pendingRequests.front().get()){
          auto const& request=*pendingRequests.front();
          middle_front::Marshal(localsendbuf,false,string_plus{e.what()});
          localsendbuf.Send((::sockaddr*)&request.front,request.frontlen);
        }
        pendingRequests.erase(::std::begin(pendingRequests));
      }
    }

    if(fds[0].revents&POLLOUT&&sendData())fds[0].events=POLLIN;

    if(fds[1].revents&POLLIN){
      auto& request=
              *pendingRequests.emplace_back(::std::make_unique<cmwRequest>());
      bool gotAddr=false;
      try{
        ReceiveBufferStack<SameFormat>
            rbuf(fds[1].fd,(::sockaddr*)&request.front,&request.frontlen);
        gotAddr=true;
	new (&request)cmwRequest(rbuf);
        middle_back::Marshal(cmwSendbuf,Generate,request);
        request.latest_update=current_updatedtime;
      }catch(::std::exception const& e){
        syslog_wrapper(LOG_ERR,"Accept request: %s",e.what());
        if(gotAddr){
          middle_front::Marshal(localsendbuf,false,string_plus{e.what()});
          localsendbuf.Send((::sockaddr*)&request.front,request.frontlen);
        }
	pendingRequests.pop_back();
        continue;
      }
      if(!sendData())fds[0].events|=POLLOUT;
    }
  }
}

int main (int ac,char** av){
  try{
    ::openlog(*av,LOG_PID|LOG_NDELAY,LOG_USER);
    if(ac!=2)throw failure("Usage: ")<<*av<<" config-file-name";
    cmwAmbassador(*(av+1));
  }catch(::std::exception const& e){
    syslog_wrapper(LOG_ERR,"Program ending: %s",e.what());
  }catch(...){
    syslog_wrapper(LOG_ERR,"Unknown exception!");
  }
  return EXIT_FAILURE;
}