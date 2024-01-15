/**
 * @file thread.cc
 * @brief 线程封装实现
 * @version 0.1
 * @date 2021-06-15
 */
#include "thread.h"

#include "log.h"
#include "util.h"

namespace sylar {

static thread_local Thread *t_thread          = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

Thread *Thread::GetThis() {
    return t_thread;
}

const std::string &Thread::GetName() {
    return t_thread_name;
}

void Thread::SetName(const std::string &name) {
    if (name.empty()) {
        return;
    }
    if (t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string &name)
    : m_cb(cb)
    , m_name(name) {
    if (name.empty()) {
        m_name = "UNKNOW";
    }
    const int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt) {
        SYLAR_LOG_ERROR(g_logger) << "pthread_create thread fail, rt=" << rt
                                  << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    m_semaphore.wait();
}

Thread::~Thread() {
    if (m_thread) {
        pthread_detach(m_thread);
    }
}

void Thread::join() {
    if (m_thread) {
        int rt = pthread_join(m_thread, nullptr);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger) << "pthread_join thread fail, rt=" << rt
                                      << " name=" << m_name;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void *Thread::run(void *arg) {
    Thread *thread = (Thread *)arg;
    t_thread       = thread;
    t_thread_name  = thread->m_name;
    thread->m_id   = sylar::GetThreadId();
    // 设置线程名字，注意，不能在外面设置，因为在外面的时候运行自定义Thread类的是主线程，只有到run方法中才是创建之后的线程
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());  // pthread库（POSIX线程库）中的pthread_setname_np函数，该函数用于设置线程的名称。这样在debug的时候可以看到有意义的线程名字

    std::function<void()> cb;
    cb.swap(thread->m_cb);

    thread->m_semaphore.notify(); //执行到这里代表Thread真正执行cb的初始化完毕了，thread初始化函数可以退出了

    cb();
    return 0;
}

} // namespace sylar
