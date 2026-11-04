#include "msu2app/serial_guard.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <iomanip>
#include <sstream>
extern char** environ;
namespace msu2app {
std::vector<std::string> serial_aliases(const std::string& port) {
    std::filesystem::path p(port); auto n=p.filename().string();
    if(n.rfind("cu.",0)==0 || n.rfind("tty.",0)==0) {
        auto suffix=n.substr(n.find('.')+1);
        return {(p.parent_path()/("cu."+suffix)).string(),(p.parent_path()/("tty."+suffix)).string()};
    }
    return {port};
}
std::string serial_lock_key(const std::string& port) {
    auto canonical=serial_aliases(port).front(); unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(canonical.data(),static_cast<CC_LONG>(canonical.size()),hash);
    std::ostringstream s; for(unsigned i=0;i<10;++i)s<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(hash[i]);
    return s.str();
}
std::string default_lock_root() {
    if(const char* p=std::getenv("MSU2_SHARED_LOCK_ROOT"))if(*p)return p;
#ifdef MSU2_WORKSPACE_ROOT
    if(std::filesystem::exists(std::filesystem::path(MSU2_WORKSPACE_ROOT)/"scripts/macos/serial_guard.py"))
        return std::string(MSU2_WORKSPACE_ROOT)+"/results/macos/.device-locks";
#endif
    const char* home=std::getenv("HOME");
    if(!home)throw std::runtime_error("找不到使用者資料夾，無法建立序列埠鎖");
    return std::string(home)+"/Library/Application Support/MSU2 Mini Studio/device-locks";
}
namespace {
void check_idle(const std::string& port) {
    if(!std::filesystem::exists(port))throw std::runtime_error("序列埠不存在："+port);
    auto paths=serial_aliases(port);
    std::vector<std::string> args{"/usr/sbin/lsof","-nP","--"};
    for(const auto& p:paths)if(std::filesystem::exists(p))args.push_back(p);
    std::vector<char*> argv; for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
    int pipes[2]; if(pipe(pipes))throw std::runtime_error("無法建立序列埠檢查管線");
    fcntl(pipes[0],F_SETFD,FD_CLOEXEC);fcntl(pipes[1],F_SETFD,FD_CLOEXEC);
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,pipes[1],STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,pipes[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,pipes[0]);
    posix_spawn_file_actions_addclose(&actions,pipes[1]);
    pid_t child; const int result=posix_spawn(&child,argv[0],&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);close(pipes[1]);
    if(result){close(pipes[0]);throw std::runtime_error("無法執行 lsof，未開啟序列埠");}
    fcntl(pipes[0],F_SETFL,O_NONBLOCK);
    std::string output;int status=0;bool reaped=false;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<deadline){
        char buf[2048];ssize_t n;while((n=read(pipes[0],buf,sizeof buf))>0)if(output.size()<32768)output.append(buf,n);
        if(waitpid(child,&status,WNOHANG)==child){reaped=true;break;}
        pollfd p{pipes[0],POLLIN,0};poll(&p,1,30);
    }
    if(!reaped){kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){} close(pipes[0]);throw std::runtime_error("序列埠擁有者檢查逾時；未開啟序列埠");}
    char buf[2048];ssize_t n;while((n=read(pipes[0],buf,sizeof buf))>0)if(output.size()<32768)output.append(buf,n);
    close(pipes[0]);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=1||!output.empty())
        throw std::runtime_error("序列埠忙碌或無法確認空閒；請等其他程式完成。"+output);
}
}
SerialGuard::SerialGuard(const std::string& port,const std::string& root) {
    std::filesystem::create_directories(root);
    const auto path=(std::filesystem::path(root)/(serial_lock_key(port)+".lock")).string();
    fd_=open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd_<0)throw std::runtime_error("無法開啟共用序列埠鎖");
    try {
        if(flock(fd_,LOCK_EX|LOCK_NB))throw std::runtime_error("其他測試持有共用序列埠鎖；請等候完成");
        check_idle(port);
        const std::string evidence="pid="+std::to_string(getpid())+" port="+port+" aliases_checked=cu,tty\n";
        if(ftruncate(fd_,0)||write(fd_,evidence.data(),evidence.size())!=static_cast<ssize_t>(evidence.size()))
            throw std::runtime_error("序列埠鎖紀錄寫入失敗");
    }catch(...){close(fd_);fd_=-1;throw;}
}
SerialGuard::~SerialGuard(){if(fd_>=0){flock(fd_,LOCK_UN);close(fd_);}}
}
