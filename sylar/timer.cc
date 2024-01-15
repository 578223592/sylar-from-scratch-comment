#include "timer.h"
#include "macro.h"
#include "util.h"

namespace sylar {

bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const {
    if (!lhs && !rhs) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (!rhs) {
        return false;
    }
    if (lhs->m_next < rhs->m_next) {
        return true;
    }
    if (rhs->m_next < lhs->m_next) {
        return false;
    }
    return lhs.get() < rhs.get();
}

Timer::Timer(uint64_t ms, std::function<void()> cb,
             bool recurring, TimerManager *manager)
    : m_recurring(recurring)
    , m_ms(ms)
    , m_cb(cb)
    , m_manager(manager) {
    m_next = sylar::GetElapsedMS() + m_ms;
}

Timer::Timer(uint64_t next)
    : m_next(next) {
}

bool Timer::cancel() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (m_cb) {
        m_cb    = nullptr;
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

bool Timer::refresh() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    m_next = sylar::GetElapsedMS() + m_ms;
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

bool Timer::reset(uint64_t ms, bool from_now) {
    if (ms == m_ms && !from_now) {
        return true;
    }
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    uint64_t start = 0;
    if (from_now) {
        start = sylar::GetElapsedMS();
    } else {
        start = m_next - m_ms;
    }
    m_ms   = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;
}

TimerManager::TimerManager() {
    m_previouseTime = sylar::GetElapsedMS();
}

TimerManager::~TimerManager() {
}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutexType::WriteLock lock(m_mutex);
    addTimer(timer, lock);
    return timer;
}

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock();
    if (tmp) {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

uint64_t TimerManager::getNextTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    m_shouldTriggerAtFrontFuncFlag = true;
    if (m_timers.empty()) {
        return ~0ull;
    }

    const Timer::ptr &next = *m_timers.begin();
    uint64_t now_ms        = sylar::GetElapsedMS();
    if (now_ms >= next->m_next) {
        return 0;
    } else {
        return next->m_next - now_ms;
    }
}

void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
    const uint64_t now_ms = sylar::GetElapsedMS();
    std::vector<Timer::ptr> expired;
    {
        RWMutexType::ReadLock lock(m_mutex);
        if (m_timers.empty()) {
            return;
        }
    }
    RWMutexType::WriteLock lock(m_mutex);
    if (m_timers.empty()) {
        return;
    }
    bool rollover = false;
    if (SYLAR_UNLIKELY(IsClockRollover(now_ms))) {
        // 使用clock_gettime(CLOCK_MONOTONIC_RAW)，应该不可能出现时间回退的问题
        rollover = true;
    }
    if (!rollover && ((*m_timers.begin())->m_next > now_ms)) {
        return;
    }

    const Timer::ptr now_timer(new Timer(now_ms));
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer); //如果发生调时，就触发所有的定时器事件
    while (it != m_timers.end() && (*it)->m_next == now_ms) {
        ++it;
    }
    expired.insert(expired.begin(), m_timers.begin(), it); // 如果时间被调了，会触发全部的定时器事件
    m_timers.erase(m_timers.begin(), it);
    cbs.reserve(expired.size());

    for (auto &timer : expired) {
        cbs.push_back(timer->m_cb);
        if (timer->m_recurring) {
            timer->m_next = now_ms + timer->m_ms;
            m_timers.insert(timer);
        } else {
            timer->m_cb = nullptr;
        }
    }
}

void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock &lock) {
    const auto it            = m_timers.insert(val).first;
    const bool insertAtFront = (it == m_timers.begin()) && !m_shouldTriggerAtFrontFuncFlag;
    if (insertAtFront) {
        m_shouldTriggerAtFrontFuncFlag = false;
    }
    lock.unlock();

    if (insertAtFront) {
        onTimerInsertedAtFront();
    }
}

bool TimerManager::IsClockRollover(uint64_t now_ms) {
    bool rollover = false;
    if (now_ms < m_previouseTime &&
        now_ms < (m_previouseTime - 60 * 60 * 1000)) {
        rollover = true;
    }
    m_previouseTime = now_ms;
    return rollover;
}

bool TimerManager::hasTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    return !m_timers.empty();
}

}
