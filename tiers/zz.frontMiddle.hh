#pragma once
//Code generated by the C++ Middleware Writer version 1.14.
#include<Buffer.hh>

namespace frontMiddle{
void Marshal (::cmw::SendBuffer& buf
         ,::cmw::marshallingInt const& a
         , const char* b){
  try{
    buf.ReserveBytes(4);
    a.Marshal(buf);
    Receive(buf,b);
    buf.FillInSize(cmw::udp_packet_max);
  }catch(...){buf.Rollback();throw;}
}
}
