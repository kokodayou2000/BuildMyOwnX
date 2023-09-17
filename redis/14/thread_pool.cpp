#include <assert.h>
#include "thread_pool.h"

static void *worker(void *arg)
{
    ThreadPool *tp = (ThreadPool *)arg;
    while (true)
    {
        pthread_mutex_lock(&tp->mu);
        while (tp->queue.empty())
        {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }
        // 获取 job
        Work w = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mu);

        w.f(w.arg);
    }
    return NULL;
}

void trhead_pool_init(ThreadPool *tp, size_t num_threads)
{
    // 线程数量要大于0
    assert(num_threads > 0);
    // 初始化互斥锁
    int rv = pthread_mutex_init(&tp->mu, NULL);
    // 初始化条件变量
    rv = pthread_cond_init(&tp->not_empty, NULL);
    assert(rv == 0);
    // 设置大小
    tp->threads.resize(num_threads);
    // 为线程池的每一个线程初始化，绑定 worker
    for (size_t i = 0; i < num_threads; i++)
    {
        int rv = pthread_create(&tp->threads[i], NULL, &worker, tp);
        assert(rv == 0);
    }
}

void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg)
{
    Work w;
    w.f = f;
    w.arg = arg;
    
    pthread_mutex_lock(&tp->mu);
    // 向队列中添加job
    tp->queue.push_back(w);
    // 唤醒，表示队列不为空
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
}