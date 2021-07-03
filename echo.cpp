#include <iostream>
#include <thread>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>

#include <boost/asio.hpp>

using namespace boost;

namespace Log {
    enum LogLevel {
        none = 0,
        warning = 1,
        info = 2,
        debug = 3
    };
    struct LogStruct {
        std::ostream &logout;
        LogLevel lv;
        LogStruct(LogLevel lv_ = debug):logout(std::cout),lv(lv_){};
        LogStruct(std::ostream& o, LogLevel lv_ = debug):logout(o),lv(lv_){};
        LogStruct(const LogStruct&) = delete;
        LogStruct(LogStruct&&) = delete;
    };
    static LogStruct Log(debug);
    template<LogLevel L>
    void Logging() {};
    template<LogLevel L, typename T>
    void Logging(const T &t) {
        if (L<=Log.lv) Log.logout << t << "\n";
    }
    template<LogLevel L, typename A, typename... T>
    void Logging(const A &a, T... t) {
        if (L<=Log.lv) {
            Log.logout << a << " ";
            Logging<L,T...>(t...);
        }
    }
}

// semaphore didn't appear until C++20
class Semaphore {
    public:
    Semaphore();
    void p();
    void v();
    private:
    size_t count;
    std::mutex mtx;
    std::condition_variable cond;
};

Semaphore::Semaphore():count(0){};
void Semaphore::p() {
    std::unique_lock<std::mutex> lock(mtx);
    if (count==0) {
        cond.wait(lock,[this](){return count>0;});
        --count;
    }
}
void Semaphore::v() {
    std::unique_lock<std::mutex> lock(mtx);
    ++count;
    if (count>0) cond.notify_one();
}


class Session:public std::enable_shared_from_this<Session> {
    public:
    using socket_type = asio::ip::tcp::socket;
    const size_t MAXBUFFERSIZE = 8192;
    public:
    Session(const std::shared_ptr<socket_type> &sock_);
    void run();
    void handleRead();
    void handleWrite();
    private:
    std::shared_ptr<socket_type> sock;
    std::string write_buffer;
    std::string read_buffer;
    size_t write_bytes;
    size_t read_bytes;
    std::mutex destroy_mtx;
    bool destroy_flag;
    void onRead(size_t read_bytes_);
    void onWrite(size_t write_bytes_);
    void destroy();
};

Session::Session(const std::shared_ptr<socket_type> &sock_):
    sock(sock_),write_bytes(0),read_bytes(0),
    write_buffer(MAXBUFFERSIZE,0),read_buffer(MAXBUFFERSIZE,0),destroy_flag(false) {
};

void Session::run() {
    if (!sock->is_open()) return;
    handleRead();
}
void Session::destroy() {
    std::lock_guard<std::mutex> lock(destroy_mtx);
    if (destroy_flag) {
        Log::Logging<Log::warning>("Session destroy:","already done");
        return;
    }
    destroy_flag = true;
    Log::Logging<Log::warning>("Session destroy");
    if (sock->is_open()) {
        system::error_code error;
        sock->cancel(error);
        if (error.value()) Log::Logging<Log::warning>("Session destroy:",error.message());
        sock->close(error);
        if (error.value()) Log::Logging<Log::warning>("Session destroy:",error.message());
    } else {
        Log::Logging<Log::warning>("Session destroy:","socket already closed");
    }
}

void Session::handleRead() {
    auto ptr = shared_from_this();
    sock->async_read_some(asio::buffer(read_buffer,MAXBUFFERSIZE),
        [this,ptr](const system::error_code &error, size_t read_bytes_){
            if (error.value()) {
                Log::Logging<Log::warning>("Session handleRead:",error.message());
                destroy();
                return;
            }
            onRead(read_bytes_);
        }
    );
}

void Session::onRead(size_t read_bytes_) {
    Log::Logging<Log::info>("onRead:read_bytes_,buffer.size",read_bytes_,read_buffer.size());
    write_buffer.swap(read_buffer);
    // read_buffer.clear();
    read_bytes += read_bytes_;
    Log::Logging<Log::debug>("to write:",write_buffer);
    handleWrite();
}

void Session::handleWrite() {
    auto ptr = shared_from_this();
    sock->async_write_some(asio::buffer(write_buffer,read_bytes - write_bytes),
        [this,ptr](const system::error_code &error, size_t write_bytes_) {
            if (error.value()) {
                Log::Logging<Log::warning>("Session run:",error.message());
                destroy();
                return;
            }
            onWrite(write_bytes_);
        }
    );
}

void Session::onWrite(size_t write_bytes_) {
    Log::Logging<Log::info>("onWrite:read_bytes_,buffer.size",write_bytes_,read_buffer.size());
    write_bytes += write_bytes_;
    if (write_bytes<read_bytes) {
        Log::Logging<Log::info>("onWrite","to write more");
        write_buffer = write_buffer.substr(write_bytes_);
        write_buffer.resize(MAXBUFFERSIZE,0);
        handleWrite();
        return;
    }
    Log::Logging<Log::info>("onWrite:","to read");
    handleRead();
}

class Server {
    public:
    using socket_type = Session::socket_type;
    const size_t LISTEN_BACKLOG = 128;
    // MAX threads
    const size_t MAX_THREADS = 4;
    Server(const std::string &ip, const uint16_t &port);
    void run();
    // run in child threads (multithreading)
    void subthreadRun();
    private:
    void handleAccept();
    void destroy();
    asio::io_context ioctx;
    asio::ip::tcp::endpoint ep;
    asio::ip::tcp::acceptor acceptor;
    // only destroy once
    std::mutex destroy_mtx;
    bool destroy_flag;
    // support for multithreading, main should be run before child threads can run
    Semaphore run_smph;
};

Server::Server(const std::string &ip, const uint16_t &port):
    ioctx(),ep(asio::ip::address::from_string(ip),port),acceptor(ioctx),
    destroy_flag(false){
};

void Server::destroy() {
    std::lock_guard<std::mutex> lock(destroy_mtx);
    if (destroy_flag) {
        Log::Logging<Log::warning>("Server destroy:","already destroyed");
        return;
    }
    destroy_flag = true;
    Log::Logging<Log::warning>("Server destroy");
    if (acceptor.is_open()) {
        system::error_code error;
        acceptor.cancel(error);
        if (error.value()) Log::Logging<Log::warning>("Server destroy:",error.message());
        acceptor.close(error);
        if (error.value()) Log::Logging<Log::warning>("Server destroy:",error.message());
    }
}
void Server::run() {
    system::error_code error;
    Log::Logging<Log::warning>("Server run:","to open");
    acceptor.open(asio::ip::tcp::v6(),error);
    if (error.value()) {
        Log::Logging<Log::warning>("Server run:",error.message());
        destroy();
        return;
    }
    acceptor.bind(ep,error);
    Log::Logging<Log::warning>("Server run:","to bind");
    if (error.value()) {
        Log::Logging<Log::warning>("Server run:",error.message());
        destroy();
        return;
    }
    Log::Logging<Log::warning>("Server run:","to listen");
    acceptor.listen(LISTEN_BACKLOG,error);
    if (error.value()) {
        Log::Logging<Log::warning>("Server run:",error.message());
        destroy();
        return;
    }
    handleAccept();
    for (int i=0;i<MAX_THREADS;++i) run_smph.v();
    ioctx.run();
}

void Server::subthreadRun(){
    run_smph.p();
    ioctx.run();
}

void Server::handleAccept() {
    auto sock = std::make_shared<socket_type>(ioctx);
    auto sess = std::make_shared<Session>(sock);
    acceptor.async_accept(*sock.get(),
        [this,sock,sess](const system::error_code &error) {
            if (error.value()) {
                Log::Logging<Log::warning>("handleAccept:",error.message());
                return;
            }
            sess->run();
            handleAccept();
        }
    );
}

int main(){
    Log::Log.lv = Log::info;

    /// 3 ///
    Server s("::",13579);
    Log::Logging<Log::warning>("server created");
    std::thread t([&s](){s.run();});
    std::thread t1([&s](){s.subthreadRun();});
    std::thread t2([&s](){s.subthreadRun();});
    t.join();
    Log::Logging<Log::info>("thread t", "terminated");
    t1.join();
    Log::Logging<Log::info>("thread t1", "terminated");
    t2.join();
    Log::Logging<Log::info>("thread t2", "terminated");
    
    /// 2 ///
    // Server s("::",13579);
    // Log::Logging<Log::warning>("server created");
    // s.run();
    
    /// 1 ///
    // Server s1("::ffff:127.0.0.1",13579),s2("::ffff:192.168.233.5",13579);
    // Log::Logging<Log::warning>("server created");
    // std::thread t1([&s1](){s1.run();});
    // std::thread t2([&s2](){s2.run();});
    // t1.join();
    // t2.join();
    return 0;
}
