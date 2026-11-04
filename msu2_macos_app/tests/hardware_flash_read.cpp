// Read-only protocol compatibility probe, never part of offline test.sh.
#include "msu2app/serial_guard.hpp"
#include "msu2app/flash.hpp"
#include "msu2/session.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
class Trace final:public msu2::Transport{
    msu2::SerialTransport inner;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    void trace(const char* kind,const std::uint8_t* p,std::size_t n){std::cout<<std::dec<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" "<<kind<<" "<<n<<" ";for(std::size_t i=0;i<n;++i)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(p[i]);std::cout<<std::dec<<std::endl;}
public:
    explicit Trace(const std::string& p):inner(p){}
    std::size_t read(std::uint8_t* p,std::size_t n,msu2::Milliseconds t)override{auto size=inner.read(p,n,t);if(size)trace("RX",p,size);return size;}
    void write_all(const std::uint8_t* p,std::size_t n,msu2::Milliseconds t)override{trace("TX",p,n);inner.write_all(p,n,t);}
    void discard_input()override{std::cout<<"DISCARD_INPUT\n";inner.discard_input();}
};
int main(int argc,char** argv){if(argc!=2){std::cerr<<"usage: hardware_flash_read /dev/cu.PORT\n";return 2;}try{
    msu2app::SerialGuard guard(argv[1]);Trace transport(argv[1]);msu2::Session session(transport);int banner=session.handshake();std::cout<<"banner="<<banner<<std::endl;
    for(auto address:{0u,256u,979456u,1005056u,1048575u}){auto value=msu2app::read_flash_bytes(transport,address,1);std::cout<<"verified_read_address="<<address<<" value="<<unsigned(value[0])<<std::endl;}
    std::cout<<"READ_ONLY_PASS 5 addresses; no erase/write/display commands\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
