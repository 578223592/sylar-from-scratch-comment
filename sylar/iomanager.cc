/**
 * @file iomanager.cc
 * @brief IO协程调度器实现
 * @version 0.1
 * @date 2021-06-16
 */

#include "iomanager.h"
#include "log.h"
#include "macro.h"
#include <fcntl.h>     // for fcntl()
#include <sys/epoll.h> // for epoll_xxx()
#include <unistd.h>    // for pipe()

#include <cstring>

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

enum EpollCtlOp {
};

static std::ostream &operator<<(std::ostream &os, const EpollCtlOp &op) {
    switch ((int)op) {
#define XX(ctl) \
    case ctl:   \
        return os << #ctl;
        XX(EPOLL_CTL_ADD);
        XX(EPOLL_CTL_MOD);
        XX(EPOLL_CTL_DEL);
#undef XX
    default:
        return os << (int)op;
    }
}

static std::ostream &operator<<(std::ostream &os, EPOLL_EVENTS events) {
    if (!events) {
        return os << "0";
    }
    bool first = true;
#define XX(E)          \
    if (events & E) {  \
        if (!first) {  \
            os << "|"; \
        }              \
        os << #E;      \
        first = false; \
    }
    XX(EPOLLIN);
    XX(EPOLLPRI);
    XX(EPOLLOUT);
    XX(EPOLLRDNORM);
    XX(EPOLLRDBAND);
    XX(EPOLLWRNORM);
    XX(EPOLLWRBAND);
    XX(EPOLLMSG);
    XX(EPOLLERR);
    XX(EPOLLHUP);
    XX(EPOLLRDHUP);
    XX(EPOLLONESHOT);
    XX(EPOLLET);
#undef XX
    return os;
}

IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(IOManager::Event event) {
    switch (event) {
    case IOManager::READ:
        return read;
    case IOManager::WRITE:
        return write;
    default:
        SYLAR_ASSERT2(false, "getContext");
    }
    throw std::invalid_argument("getContext invalid event");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(const IOManager::Event event) {
    // 待触发的事件必须已被注册过
    SYLAR_ASSERT(events & event);
    /**
     *  清除该事件，表示不再关注该事件了
     * 也就是说，注册的IO事件是一次性的，如果想持续关注某个socket fd的读写事件，那么每次触发事件之后都要重新添加
     */
    events = (Event)(events & ~event);
    // 调度对应的协程
    EventContext &ctx = getEventContext(event);
    if (ctx.cb) {
        ctx.scheduler->schedule(ctx.cb);
    } else {
        ctx.scheduler->schedule(ctx.fiber);
    }
    resetEventContext(ctx);
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
    : Scheduler(threads, use_caller, name) {
    m_epfd = epoll_create(5000);
    SYLAR_ASSERT(m_epfd > 0);

    int rt = pipe(m_tickleFds);
    SYLAR_ASSERT(!rt);

    // 关注pipe读句柄的可读事件，用于tickle协程
    epoll_event event;
    memset(&event, 0, sizeof(epoll_event));
    event.events  = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];

    // 非阻塞方式，配合边缘触发
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    SYLAR_ASSERT(!rt);

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    SYLAR_ASSERT(!rt);

    contextResize(32);

    start();
}

IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i]) {
            delete m_fdContexts[i];
        }
    }
}

void IOManager::contextResize(size_t size) {
    m_fdContexts.resize(size);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i] == nullptr) {
            m_fdContexts[i]     = new FdContext();
            m_fdContexts[i]->fd = i;
        }
    }
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    // 找到fd对应的FdContext，如果不存在，那就分配一个
    FdContext *fd_ctx = nullptr;
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        lock.unlock();
    } else {
        lock.unlock();
        RWMutexType::WriteLock lock2(m_mutex); // todo：待搞清楚这里为什么要扩容？ 可用的fd与缩影也没必要意义对应把感觉
        contextResize(fd * 1.5);               // 扩容
        fd_ctx = m_fdContexts[fd];
    }

    // 同一个fd不允许重复添加相同的事件
    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (SYLAR_UNLIKELY(fd_ctx->events & event)) {
        SYLAR_LOG_ERROR(g_logger) << "addEvent assert fd=" << fd
                                  << " event=" << (EPOLL_EVENTS)event
                                  << " fd_ctx.event=" << (EPOLL_EVENTS)fd_ctx->events;
        SYLAR_ASSERT(!(fd_ctx->events & event));
    }

    // 将新的事件加入epoll_wait，使用epoll_event的私有指针存储FdContext的位置
    int op = fd_ctx->events > Event::NONE ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events   = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    const int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt != 0) {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                  << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ") fd_ctx->events="
                                  << (EPOLL_EVENTS)fd_ctx->events;
        return -1;
    }

    // 待执行IO事件数加1
    ++m_pendingEventCount; // maybe bug： idle函数中触发事件的时候，读写分别都会使得m_pendingEventCount-1，但是这里添加事件（具体读写为止），只+1，可能有
                           //   还是说这里只会添加读写任务其中一项

    // 找到这个fd的event事件对应的EventContext，对其中的scheduler, cb, fiber进行赋值
    fd_ctx->events                     = (Event)(fd_ctx->events | event);
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event); // todo：是不是可以确保传入的只有读写中的一个，因为这里event如果同时有读写会出问题
    SYLAR_ASSERT(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);

    // 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当成回调执行体
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) {
        event_ctx.cb.swap(cb);
    } else {
        // 什么时候会传入一个空的回调呢，可以看下经典使用：connect_with_timeout的回调函数为空，即回到当时的协程
        event_ctx.fiber = Fiber::GetThis(); // 默认把当前协程作为回调函数
        SYLAR_ASSERT2(event_ctx.fiber->getState() == Fiber::RUNNING, "state=" << event_ctx.fiber->getState());
    }
    return 0;
}

bool IOManager::delEvent(int fd, Event event) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (SYLAR_UNLIKELY((fd_ctx->events & event) == Event::NONE)) {
        return false; // 该事件没注册监听，自然没法删除
    }

    // 清除指定的事件，表示不关心这个事件了，如果清除之后结果为0，则从epoll_wait中删除该文件描述符
    const Event reserveEvents = (Event)(fd_ctx->events & ~event); // 还要保留的事件
    int op                    = reserveEvents != Event::NONE ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | reserveEvents;
    epevent.data.ptr = fd_ctx;

    const int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                  << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ")";
        return false;
    }

    // 待执行事件数减1
    --m_pendingEventCount; // todo：后面同样要看看为什么只减一个事件
    // 重置该fd对应的event事件上下文
    fd_ctx->events                     = reserveEvents;
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event); //todo：与addEvent同样对称的问题
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

bool IOManager::cancelEvent(const  int fd, Event event) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (SYLAR_UNLIKELY(!(fd_ctx->events & event))) {
        return false;
    }

    // 删除事件
    const Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                  << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ")";
        return false;
    }

    // 删除之前触发一次事件
    fd_ctx->triggerEvent(event);
    // 活跃事件数减1
    --m_pendingEventCount;
    return true;
}

bool IOManager::cancelAll(int fd) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!fd_ctx->events) {
        return false;
    }

    // 删除全部事件
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;

    const int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                  << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
                                  << rt << " (" << errno << ") (" << strerror(errno) << ")";
        return false;
    }

    // 触发全部已注册的事件
    if (fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }
    if (fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    SYLAR_ASSERT(fd_ctx->events == Event::NONE);
    return true;
}

IOManager *IOManager::GetThis() {
    return dynamic_cast<IOManager *>(Scheduler::GetThis());
}

/**
 * 通知调度协程、也就是Scheduler::run()从idle中退出
 * Scheduler::run()每次从idle协程中退出之后，都会重新把任务队列里的所有任务执行完了再重新进入idle
 * 如果没有调度线程处理于idle状态，那也就没必要发通知了
 *
 * 突然感觉这里的设计和线程池还是有一些不一样，线程池是将任务全部一股脑的塞入，塞入之后工作线程自己去取，如果没有任务就沉睡，直到被唤醒
 * 这里的设计是调度器查看是否有空闲的线程，如果有空闲的线程，那么就通知它去处理任务，即工作线程能接触到的只有自己的任务，无法看到整个任务队列了
 * 区别好像不是很大，但是还是有一些不同的，这里可能就是调度器的作用体现了！！！
 * todo：后面可以考虑加入快慢任务这样，区分出快慢任务池
 * 这样感觉有点像pika的设计，设计快慢任务分离防止慢任务阻塞快任务
 */
void IOManager::tickle() {
    SYLAR_LOG_DEBUG(g_logger) << "tickle";
    if (!hasIdleThreads()) {
        return;
    }
    const int rt = write(m_tickleFds[1], "T", 1); // 为什么向这个管道写，就能tickle其他线程呢？ 因为其他线程会监听到fd，那么写入的时候就会触发epoll了（只会触发一个，因为epoll是线程安全的）
    SYLAR_ASSERT(rt == 1);
}

bool IOManager::stopping() {
    uint64_t timeout = 0;
    return stopping(timeout);
}

bool IOManager::stopping(uint64_t &timeout) {
    // 对于IOManager而言，必须等所有待调度的IO事件都执行完了才可以退出
    // 增加定时器功能后，还应该保证没有剩余的定时器待触发
    timeout = getNextTimer();
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

/**
 * 调度器无调度任务时会阻塞idle协程上，对IO调度器而言，idle状态应该关注两件事，一是有没有新的调度任务，对应Schduler::schedule()，
 * 如果有新的调度任务，那应该立即退出idle状态，并执行对应的任务；二是关注当前注册的所有IO事件有没有触发，如果有触发，那么应该执行
 * IO事件对应的回调函数
 */

/**
 * 总结：IOmanager对于任务一共有两个东西需要完成：1.schedule调度，2.触发IO事件
 * 对于chedule调度，在run函数中完成；对于触发IO事件，IOManager在idle中完成。
 * 然后IO时间主要就是定时器，定时器有几种：1.真正的定时任务；2.sleep函数；3.read、write这样的会阻塞的任务
 */
void IOManager::idle() {
    SYLAR_LOG_DEBUG(g_logger) << "idle";

    // 一次epoll_wait最多检测256个就绪事件，如果就绪事件超过了这个数，那么会在下轮epoll_wati继续处理
    constexpr uint64_t MAX_EVNETS = 256;
    epoll_event *events           = new epoll_event[MAX_EVNETS]();
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {
        delete[] ptr; // lambda
    });

    while (true) {
        // 获取下一个定时器的超时时间，顺便判断调度器是否停止
        uint64_t next_timeout = 0;
        if (SYLAR_UNLIKELY(stopping(next_timeout))) {
            SYLAR_LOG_DEBUG(g_logger) << "name=" << getName() << "idle stopping exit";
            break;
        }

        // 阻塞在epoll_wait上，等待事件发生或定时器超时
        int rt{};
        do {
            // 默认超时时间5秒，如果下一个定时器的超时时间大于5秒，仍以5秒来计算超时，避免定时器超时时间太大时，epoll_wait一直阻塞
            // todo ： 虽然为了避免一直阻塞，但是一直阻塞有什么问题吗？
            static constexpr int MAX_TIMEOUT = 5000;
            if (next_timeout != ~0ull) { //~0ull是全1，应该是表示没有下一个定时器的触发时间
                next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
            } else {
                next_timeout = MAX_TIMEOUT;
            }
            rt = epoll_wait(m_epfd, events, MAX_EVNETS, (int)next_timeout);
            if (rt < 0 && errno == EINTR) { // 超时触发：即没有事件触发且errno为EINTR，EINTR表示中途被信号中断
                continue;
            }
            break; // 只有有事件触发才会跳出循环
        } while (true);

        // 收集所有已超时的定时器，执行回调函数
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if (!cbs.empty()) {
            for (const auto &cb : cbs) {
                schedule(cb); // 触发就是加入调度，因此是定时器时间到了才开始安排任务，而不是定时时间到就立即执行（好像没有谁可以保证后一点）
            }
            cbs.clear();
        }

        // 遍历所有发生的事件，根据epoll_event的私有指针找到对应的FdContext，进行事件处理
        for (int i = 0; i < rt; ++i) {
            epoll_event &event = events[i];
            if (event.data.fd == m_tickleFds[0]) {
                // ticklefd[0]用于通知协程调度，这时只需要把管道里的内容读完即可,不需要管具体内容
                // todo：看下为什么要唤醒，即不是通过定时器唤醒，而是通过tickle()唤醒
                uint8_t dummy[256];
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0)
                    ;
                continue;
            }

            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            FdContext::MutexType::Lock lock(fd_ctx->mutex);
            /**
             * EPOLLERR: 出错，比如写读端已经关闭的pipe ；                关闭的pipe相关知识：屏蔽sigpipe信号
             * EPOLLHUP: 套接字对端关闭
             * 出现这两种事件，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执行不到的情况，因此在触发的事件上添加读写事件
             */ 
            if (event.events & (EPOLLERR | EPOLLHUP)) { //如果有这两个事件，就将event添加读写事件中自己关心的事件
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            int real_events = Event::NONE;
            if (event.events & EPOLLIN) {
                real_events |= Event::READ;
            }
            if (event.events & EPOLLOUT) {
                real_events |= Event::WRITE;
            }

            // 说明触发的事件是不关注的事件，虽然没有关注这些事件，但是仍然有可能触发epoll，当触发这些事件的时候，选择跳过
            if ((fd_ctx->events & real_events) == Event::NONE) {
                continue;
            }

            // 剔除已经发生的事件，将剩下关心的事件重新加入epoll_wait
            // 剔除的原因是因为fiber（协程）都是一次性的，因此关心的事件触发一次之后就不再触发了
            const int leftEvents = (fd_ctx->events & ~real_events);
            int op          = leftEvents > 0 ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; //todo : 也可以使用EPOLLONESHOT来代替这个
            event.events    = EPOLLET | leftEvents;

            const int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << m_epfd << ", "
                                          << (EpollCtlOp)op << ", " << fd_ctx->fd << ", " << (EPOLL_EVENTS)event.events << "):"
                                          << rt2 << " (" << errno << ") (" << strerror(errno) << ")";
                continue;
            }

            // 处理已经发生的事件，也就是让调度器调度指定的函数或协程
            if (real_events & READ) {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // end for 全部的定时器事件

        /**
         * 一旦处理完所有的事件，idle协程yield，这样可以让调度协程(Scheduler::run)重新检查是否有新任务要调度
         * 上面triggerEvent实际也只是把对应的fiber重新加入调度，要执行的话还要等idle协程退出
         */ 
        Fiber::ptr cur = Fiber::GetThis();
        const auto rawPtr   = cur.get();
        cur.reset();

        rawPtr->yield();
    } // end while(true)
}

void IOManager::onTimerInsertedAtFront() {
    tickle();
}

} // end namespace sylar