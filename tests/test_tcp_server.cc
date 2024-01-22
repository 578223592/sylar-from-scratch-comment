// ##控制不同服务器的开关宏
#define SERVER_3

#ifdef  SERVER_1
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 10
#define PORT 8888

int main() {
    int listen_fd, conn_fd, epoll_fd, event_count;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    struct epoll_event events[MAX_EVENTS], event;
    // 创建监听套接字
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1;
    }
    // 设置端口重用
    const int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // 设置服务器地址和端⼝
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    // 绑定监听套接字到服务器地址和端⼝
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        return -1;
    }
    // 监听连接
    if (listen(listen_fd, 5) == -1) {
        perror("listen");
        return -1;
    }
    // 创建 epoll 实例
    if ((epoll_fd = epoll_create1(0)) == -1) {
        perror("epoll_create1");
        return -1;
    }
    // 添加监听套接字到 epoll 实例中
    event.events  = EPOLLIN;
    event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        perror("epoll_ctl");
        return -1;
    }
    while (true) {
        // 等待事件发⽣
        event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (event_count == -1) {
            perror("epoll_wait");
            return -1;
        }
        // 处理事件
        for (int i = 0; i < event_count; i++) {
            if (events[i].data.fd == listen_fd) {
                // 有新连接到达
                conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                if (conn_fd == -1) {
                    perror("accept");
                    continue;
                }
                // 将新连接的套接字添加到 epoll 实例中
                event.events  = EPOLLIN;
                event.data.fd = conn_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) == -1) {
                    perror("epoll_ctl");
                    return -1;
                }
            } else {
                // 有数据可读
                char buf[1024];
                int len = read(events[i].data.fd, buf, sizeof(buf) - 1);
                if (len <= 0) {
                    // 发⽣错误或连接关闭，关闭连接
                    // printf("error,fd: %d\n",events[i].data.fd );
                    close(events[i].data.fd);
                } else {
                    buf[len] = '\0';
                    // printf("接收到消息：%s\n", buf);
                    // 发送回声消息给客户端
                    // write(events[i].data.fd, buf, len);
                    close(events[i].data.fd);
                }
            }
        }
    }
    // 关闭监听套接字和 epoll 实例
    close(listen_fd);
    close(epoll_fd);
    return 0;
}

#endif

#ifdef SERVER_2

#include "sylar/sylar.h"
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
static int sock_listen_fd = -1;
void test_accept();
void error(const char *msg) {
    printf("errno:%d\n",errno);
    perror(msg);
    printf("erreur...\n");
    exit(1);
}
void watch_io_read() {
    sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
}
void test_accept() {
    struct sockaddr_in addr; // maybe sockaddr_un;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    int fd        = accept(sock_listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
        std::cout << "fd = " << fd << "accept false" << std::endl;
    } else {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
            char buffer[1024];
            memset(buffer, 0, sizeof(buffer));
            while (true) {
                int ret = recv(fd, buffer, sizeof(buffer), 0);
                if (ret > 0) {
                    // printf("recv:%s\n", buffer);
                    ret = send(fd, buffer, ret, 0);
                }
                if (ret <= 0) {
                    if (errno == EAGAIN)
                        continue;
                    break;
                }
                // printf("close fd:%d\n", fd); //短连接，一次处理完毕之后，无论如何，关闭fd
                close(fd);
            }
        });
    }
    sylar::IOManager::GetThis()->schedule(watch_io_read);
}
void test_iomanager() {
    int portno = 12345;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    // setup socket
    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0) {
        const char *msg = "Error creating socket..";
        error(msg);
    }
    int yes = 1;
    // lose the pesky "address already in use" error message
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset((char *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(portno);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    // bind socket and listen for connections
    if (bind(sock_listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error("Error binding socket..\n");
    if (listen(sock_listen_fd, 1024) < 0) {
        error("Error listening..\n");
    }
    printf("epoll echo server listening for connections on port: %d\n", portno);
    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);
    sylar::IOManager iom;
    iom.addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
}

int main(int argc, char *argv[]) {
    test_iomanager();
    return 0;
}


#endif

#ifdef SERVER_3
 /**
 /**
  * @file test_tcp_server.cc
  * @brief TcpServer类测试
  * @version 0.1
  * @date 2021-09-18
  */
 #include "sylar/sylar.h"

 // static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

 /**
  * @brief 自定义TcpServer类，重载handleClient方法
  */
 class MyTcpServer : public sylar::TcpServer {
 protected:
     void handleClient(sylar::Socket::ptr client) override;
 };

 void MyTcpServer::handleClient(sylar::Socket::ptr client) {
     // SYLAR_LOG_INFO(g_logger) << "new client: " << client->toString();
     // static std::string buf;
     // buf.resize(4096);
     char buf[1024];
     client->recv(&buf[0], sizeof(buf)-1); // 这里有读超时，由tcp_server.read_timeout配置项进行配置，默认120秒
     // SYLAR_LOG_INFO(g_logger) << "recv: " << buf;
     client->close();
 }

 void run() {
     sylar::TcpServer::ptr server(new MyTcpServer); // 内部依赖shared_from_this()，所以必须以智能指针形式创建对象
     auto addr = sylar::Address::LookupAny("0.0.0.0:12345");
     SYLAR_ASSERT(addr != nullptr);
     std::vector<sylar::Address::ptr> addrs;
     addrs.emplace_back(addr);

     std::vector<sylar::Address::ptr> fails;
     while (!server->bind(addrs, fails)) {
        sleep(2);
     }

     // SYLAR_LOG_INFO(g_logger) << "bind success, " << server->toString();

     server->start();
 }

 int main(int argc, char *argv[]) {
     sylar::EnvMgr::GetInstance()->init(argc, argv);
     sylar::Config::LoadFromConfDir(sylar::EnvMgr::GetInstance()->getConfigPath());

    sylar::IOManager iom(1);
     iom.schedule(&run);

     return 0;
 }

#endif
