#pragma once
#include <string>
#include <vector>
namespace msu2app {
std::vector<std::string> serial_aliases(const std::string& port);
std::string serial_lock_key(const std::string& port);
std::string default_lock_root();
class SerialGuard {
public:
    explicit SerialGuard(const std::string& port, const std::string& root=default_lock_root());
    ~SerialGuard();
    SerialGuard(const SerialGuard&)=delete;
    SerialGuard& operator=(const SerialGuard&)=delete;
private:
    int fd_=-1;
};
}
