#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

/*TODO:template不知道怎么用 */
template <typename T>
class threadpool
{
public:
    /*connPool是数据库连接池指针,thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    //插入队列,插入的是就绪事件listen
    bool append(T *request, int state);
    //插入队列,插入的是完成事件connect
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

//定义线程池中的参数
private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库连接池
    int m_actor_model;          //模型切换
};

//初始化线程,使其成功运行,但是分离回收
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    //创建线程池
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();

    //循环初始化线程
    for (int i = 0; i < thread_number; ++i)
    {
        //创建线程
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads; /*TODO:不知道*/
            throw std::exception();
        }

        //分离回收
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

//向请求队列中添加任务
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

//向请求队列中添加任务
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();

    //根据硬件，预先设置请求队列的最大值
    if (m_workqueue.size() >= m_max_requests) /*如果请求队列大于允许最大请求数,就结束*/
    {
        m_queuelocker.unlock(); /*TODO:为什么要unlock*/
        return false;
    }

    //添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock(); /*TODO:m_queuelocker有什么用*/

    //信号量提醒有任务处理
    m_queuestat.post(); /*TODO:m_queuestat会把状态调成post吗*/
    return true;
}

//创建线程同时进入工作模式
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

//执行任务
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        //信号量等待
        m_queuestat.wait();

        //被唤醒后,先加互斥锁
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        //处理队列事件
        T *request = m_workqueue.front();
        m_workqueue.pop_front();

        //解锁
        m_queuelocker.unlock();

        //如果队列中没有任务,跳到下一次循环
        if (!request)
            continue;

        if (1 == m_actor_model)
        {
            //TODO:m_state是人家插入队列时规定的
            if (0 == request->m_state)
            {
                //TODO
                if (request->read_once())
                {
                    request->improv = 1;
                    //获取数据库链接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //process进行处理
                    request->process();
                }

                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }

        /*TODO:应该是某个直接处理的模式,ET和LT那些*/
        else
        {
            //获取数据库链接
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            //process(模板类中的方法,这里是http类)进行处理
            request->process();
        }
    }
}
#endif
