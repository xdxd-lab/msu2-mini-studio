#pragma once
#include <string>
#include <vector>
namespace msu2app {
std::vector<std::string> serial_aliases(const std::string& port);
std::string serial_lock_key(const std::string& port);
// Nonempty MSU2_SHARED_LOCK_ROOT overrides the compiled workspace path (used
// only while its guard helper exists); otherwise use the user's Application Support.
// Separate checkouts need an explicit shared root to coordinate reliably.
std::string default_lock_root();
// Holds flock and checks cu/tty owners before allowing a serial session.
// The file's PID is historical evidence, not proof of a currently held lock.
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
