/**
 * @file scheduler.cc
 * @brief 协程调度器实现
 * @version 0.1
 * @date 2021-06-15
 */
#include "scheduler.h"
#include "hook.h"
#include "macro.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

/// 当前线程的调度器，同一个调度器下的所有线程共享同一个实例
static thread_local Scheduler *t_scheduler = nullptr;

/// 当前线程的调度协程，每个线程都独有一份
/// 注意：调度器可以共享，调度协程不能共享
static thread_local Fiber *t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) {
    SYLAR_ASSERT(threads > 0);

    m_useCaller = use_caller;
    m_name      = name;

    if (use_caller) { // 猜想：目前感觉use_caller用于标记当前线程是否是主线程，主线程为true，否则为false
        --threads;
        sylar::Fiber::GetThis();
        SYLAR_ASSERT(GetThis() == nullptr);
        t_scheduler = this;

        /**
         * 个人：目前执行到这里的时候应该还是在主协程中
         * caller线程的主协程不会被线程的调度协程run进行调度，而且，线程的调度协程停止时，应该返回caller线程的主协程
         * 在user caller情况下，把caller线程的主协程暂时保存起来，等调度协程结束时，再resume caller协程
         */

        // todo：弄清t_scheduler_fiber和m_rootFiber的关系。
        //  目前的想法是一个线程就只有一个调度器，那么，m_rootFiber和t_scheduler_fiber是同一个对象，为什么还要搞两份呢？？？
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

        sylar::Thread::SetName(m_name);
        t_scheduler_fiber = m_rootFiber.get(); // 赋值给调度fiber，这里与上面两行的todo是一个问题
        m_rootThread      = sylar::GetThreadId();
        m_threadIds.push_back(m_rootThread);
    } else {
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler *Scheduler::GetThis() {
    return t_scheduler;
}
/**
 * @brief
 * @return  调度器的主协程
 */
Fiber *Scheduler::GetMainFiber() {
    return t_scheduler_fiber;
}

void Scheduler::setThis() {
    t_scheduler = this;
}

Scheduler::~Scheduler() {
    SYLAR_LOG_DEBUG(g_logger) << "Scheduler::~Scheduler()";
    SYLAR_ASSERT(m_stopping);
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
}

void Scheduler::start() {
    SYLAR_LOG_DEBUG(g_logger) << "start";
    MutexType::Lock lock(m_mutex);
    if (m_stopping) {
        SYLAR_LOG_ERROR(g_logger) << "Scheduler is stopped";
        return;
    }
    SYLAR_ASSERT(m_threads.empty());
    m_threads.resize(m_threadCount); // todo:thead_ids里面包含caller线程，但是m_threads里面不包含，感觉语义不清楚，可以尝试改进下
    for (size_t i = 0; i < m_threadCount; ++i) {
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                      m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
}



void Scheduler::tickle() {
    SYLAR_LOG_DEBUG(g_logger) << "ticlke";
}


/**
 * \brief idle任务，相当于啥也不做，直接yield，忙等待
 */
void Scheduler::idle() {
    SYLAR_LOG_DEBUG(g_logger) << "idle";
    while (!stopping()) {
        sylar::Fiber::GetThis()->yield();
    }
}
bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

void Scheduler::stop() {
    SYLAR_LOG_DEBUG(g_logger) << "stop";
    if (stopping()) {  //todo：虽然重载之后会调用子类（IO Manager）的stopping()方法，但是，在子类的stopping会调用父类的Scheduler的stop方法，在该方法中会判断
                        // m_stopping的值，岂不是永远不会退出了
        return;
    }
    m_stopping = true;

    /// 如果use caller，那只能由caller线程发起stop
    /// 反之，如果不是调度器所在的主线程
    if (m_useCaller) {
        SYLAR_ASSERT(this->GetThis() == this);
    } else {
        SYLAR_ASSERT(this->GetThis() != this);   //todo:这里是为何？不是所有的线程共享一个实例吗？？？
    }

    for (size_t i = 0; i < m_threadCount; i++) {
        tickle();  //todo: 这里tickle通知了其他线程，让它们也结束；但是发现没有让其他线程停止的逻辑呀。
    }

    if (m_rootFiber) {
        tickle();
    }

    /// 在use caller情况下，调度器协程结束时，应该返回caller协程
    if (m_rootFiber) {
        m_rootFiber->resume();
        SYLAR_LOG_DEBUG(g_logger) << "m_rootFiber end";
    }

    //todo ： 为什么要swap再停止线程呢？
    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }
    for (auto &i : thrs) {
        i->join();
    }
}
/**
 * \brief 真正的开始调度
 * 在start函数中创建了线程池，但是可以看到其他线程虽然传入的还是一个调度类，但是通过对thread_local变量的使用，可以做到不同
 */
void Scheduler::run() {
    SYLAR_LOG_DEBUG(g_logger) << "run";
    set_hook_enable(true); // todo hook 待看
    this->setThis();
    if (sylar::GetThreadId() != m_rootThread) {
        t_scheduler_fiber = sylar::Fiber::GetThis().get(); // 创建主thread_local变量
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this))); // 本质上其作用就是让出当前的运行，即yield
    Fiber::ptr cb_fiber = nullptr;

    ScheduleTask task{};
    while (true) {
        task.reset();
        bool tickleOtherThread = false; // 是否tickle其他线程进行任务调度
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            // 遍历所有调度任务
            while (it != m_tasks.end()) {
                if (it->thread != -1 && it->thread != sylar::GetThreadId()) {
                    // 指定了调度线程，但不是在当前线程上调度，标记一下需要通知其他线程进行调度，然后跳过这个任务，继续下一个
                    ++it;
                    tickleOtherThread = true;
                    continue;
                }
                // 找到一个未指定线程，或是指定了当前线程的任务
                SYLAR_ASSERT(it->fiber != nullptr || it->cb != nullptr);
                // if (it->fiber) {
                //     // 任务队列时的协程一定是READY状态，谁会把RUNNING或TERM状态的协程加入调度呢？
                //     SYLAR_ASSERT(it->fiber->getState() == Fiber::READY);
                // }

                // [BUG FIX]: hook IO相关的系统调用时，在检测到IO未就绪的情况下，会先添加对应的读写事件，再yield当前协程，等IO就绪后再resume当前协程
                // 多线程高并发情境下，有可能发生刚添加事件就被触发的情况，如果此时当前协程还未来得及yield，则这里就有可能出现协程状态仍为RUNNING的情况
                // 这里简单地跳过这种情况，以损失一点性能为代价，否则整个协程框架都要大改
                // todo:待查看为什么当前协程还未来得及yield，就被触发，难道不能保证yield再触发吗？？？
                if (it->fiber && it->fiber->getState() == Fiber::RUNNING) {
                    ++it;
                    continue;
                }

                // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                task = *it;
                it = m_tasks.erase(it);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
            tickleOtherThread |= (it != m_tasks.end());
        }

        if (tickleOtherThread) {
            tickle();
        }

        //todo 感觉这里可以优化，全部封装成fiber，而不是回调函数，因为在这里还要判断然后再封装
        //牺牲一点效率保证代码的统一性，即优化：ScheduleTask
        if (task.fiber) {
            // resume协程，resume返回时，协程要么执行完了，要么半路yield了，总之这个任务就算完成了，活跃线程数减一
            task.fiber->resume();
            --m_activeThreadCount;
            task.reset();
        } else if (task.cb) {
            if (cb_fiber) {
                cb_fiber->reset(task.cb);
            } else {
                cb_fiber.reset(new Fiber(task.cb));
            }
            task.reset();
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        } else {
            // 进到这个分支情况一定是任务队列空了，调度idle协程即可
            if (idle_fiber->getState() == Fiber::TERM) {
                // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那一定是调度器停止了
                SYLAR_LOG_DEBUG(g_logger) << "idle fiber term";
                break;
            }
            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
    }
    SYLAR_LOG_DEBUG(g_logger) << "Scheduler::run() exit";
}

} // end namespace sylar