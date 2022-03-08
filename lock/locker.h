#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

//信号量
class sem
{
public:
    //默认初始化信号量
    sem()
    {
        //将0赋值给信号量时失败
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }

    //给定值初始化信号量
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }

    ~sem()
    {
        sem_destroy(&m_sem);
    }

    //信号量等待,信号量有值时才返回
    bool wait()
    {
        //阻塞直到信号量非0,然后-1后返回
        return sem_wait(&m_sem) == 0;
    }

    //有任务入列时,信号量值增加
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

//互斥锁
class locker
{
public:
    //初始化互斥锁
    locker()
    {
        //以默认模式初始化互斥锁失败
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }

    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    //加锁
    bool lock()
    {
        //被锁住则阻塞,否则加锁
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    //解锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    //互斥锁
    pthread_mutex_t m_mutex;
};

//条件变量
class cond
{
public:
    //初始化条件变量
    cond()
    {
        //以默认属性初始化条件变量
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }

    //回收条件变量
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    //等待条件变量,无条件
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //等待目标条件变量
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //成功为0,失败为错误码
        return ret == 0;
    }

    //等待条件变量,时间条件
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //&t时间内没有满足即返回ETIMEOUT
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }

    //唤醒一个等待目标条件变量的线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    //以广播的方式唤醒所有等待目标条件变量的线程
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //条件变量
    pthread_cond_t m_cond;
};

#endif
