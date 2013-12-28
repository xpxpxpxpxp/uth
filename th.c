#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include "th.h"

#define NR_CPU      1
#define STACK_SIZE  8192
#define USE_ATOMIC_BY_SPIN

#ifdef USE_ATOMIC_BY_SPIN

static pthread_spinlock_t  g_atomic_lock;

static inline void atomic_bit_set(int *n, int mask)
{
    pthread_spin_lock(&g_atomic_lock);
    (*n) |= mask;
    pthread_spin_unlock(&g_atomic_lock);
}

static inline void atomic_bit_clear(int *n, int mask)
{
    pthread_spin_lock(&g_atomic_lock);
    (*n) &= ~mask;
    pthread_spin_unlock(&g_atomic_lock);
}

static inline int atomic_bit_test_and_set(int *n, int mask)
{
    int last;

    pthread_spin_lock(&g_atomic_lock);
    last = *n;
    (*n) |= mask;
    pthread_spin_unlock(&g_atomic_lock);
    
    return last;
}

#endif

typedef void (*ctx_func_t)(void);

static struct cpu   g_cpu[NR_CPU];
int                 g_end = 0;

static inline void ts_adjust(struct timespec *ts)
{
    if (ts->tv_nsec >= 1000000000UL) {
        ldiv_t  q = ldiv(ts->tv_nsec, 1000000000L);
        ts->tv_sec += q.quot;
        ts->tv_nsec =  q.rem;
    }
}

static inline int ts_after(struct timespec *a, struct timespec *b)
{
    return  (a->tv_sec>b->tv_sec) ||
            (a->tv_sec==b->tv_sec && a->tv_nsec>b->tv_nsec);
}

static inline void get_wait_ts(struct cpu *cpu, struct timespec *ts)
{
    struct evt   *evt;

    assert(!l_empty(&cpu->sleep));

    evt = l_entry(l_first(&cpu->sleep), struct evt, node);
    *ts = evt->sleep;
}

static inline void append_to_ready_unlocked(struct th *th, struct cpu *cpu)
{
    l_add_tail(&th->node, &cpu->ready);
    atomic_bit_set(&th->state, TH_READY);
}

static inline void append_to_ready(struct th *th, struct cpu *cpu)
{
    pthread_spin_lock(&cpu->spin);
    l_add_tail(&th->node, &cpu->ready);
    atomic_bit_set(&th->state, TH_READY);
    pthread_spin_unlock(&cpu->spin);
}

static inline struct th *pop_from_ready(struct cpu *cpu)
{
    struct th   *th; 

    th = l_entry(l_first(&cpu->ready), struct th, node);
    pthread_spin_lock(&cpu->spin);
    l_del(&th->node);
    pthread_spin_unlock(&cpu->spin);
    th->state |= TH_RUN;
    th->state &= ~TH_READY;
    return th;
}

static void do_event(struct cpu *cpu)
{
    struct timespec ts;
    struct evt      *evt;

    assert(sched_getcpu() == cpu->id);
    
    if (l_empty(&cpu->sleep))
        return;

    while (!l_empty(&cpu->sleep)) {
        evt = l_entry(l_first(&cpu->sleep), struct evt, node);
        clock_gettime(CLOCK_REALTIME, &ts);
        if (!ts_after(&ts, &evt->sleep)) {
            break;
        }
        l_del(&evt->node);
        evt->func(evt->arg1, evt->arg2, evt->arg3);
    }
}

static void sched(struct cpu *cpu)
{
    struct th   *old = cpu->run;

    do_event(cpu);
    while (l_empty(&cpu->ready)) {
        struct timespec ts0, ts1;

        clock_gettime(CLOCK_REALTIME, &ts0);
        ts0.tv_nsec += 4000000L;                /* HZ = 250 */
        ts_adjust(&ts0);
        
        if (!l_empty(&cpu->sleep))  {
            get_wait_ts(cpu, &ts1);
            if (ts_after(&ts0, &ts1)) {
                ts0 = ts1;
            }
        } 
        pthread_mutex_lock(&cpu->mutex); 
        pthread_cond_timedwait(&cpu->cond, &cpu->mutex, &ts0);
        pthread_mutex_unlock(&cpu->mutex); 
        do_event(cpu);
    }

    cpu->run = pop_from_ready(cpu);

    if (old==cpu->run)
        return;

    if (cpu->run->state & TH_SLEEP) {
        cpu->run->state &= ~TH_SLEEP;
        l_del(&cpu->run->evt.node);
    }
    
    if (old) {
        swap_ctx(&old->uc, &cpu->run->uc);
    } else {
        set_ctx(&cpu->run->uc);
    }
}

static void *cpu_func(void *arg)
{
    struct cpu      *cpu = (struct cpu *)arg; 
    unsigned long   nr_switch = 0;
    struct th       *th;
    cpu_set_t       cpuset;
    int             ret;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu->id, &cpuset);
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    assert(ret==0);

    get_ctx(&cpu->uc);
    if (nr_switch) {
        /* th exit */
        th = cpu->run;
        cpu->run = NULL;
        th->done = 1;
        pthread_cond_broadcast(&cpu->cond);
        th_cd_signal(&th->done_cd);
        /* th free in th_join() */
    }
    nr_switch ++;

    sched(cpu);

    return NULL;
}

static void init_cpu(void)
{
    int         i;
    struct cpu  *cpu;

#ifdef USE_ATOMIC_BY_SPIN
    pthread_spin_init(&g_atomic_lock, PTHREAD_PROCESS_PRIVATE);
#endif
    for (i=0; i<NR_CPU; i++) {
        cpu = &g_cpu[i];
        cpu->id = i;
        cpu->run = NULL;
        l_init(&cpu->ready);
        l_init(&cpu->sleep);
        pthread_mutex_init(&cpu->mutex, NULL);
        pthread_cond_init(&cpu->cond, NULL);
        pthread_spin_init(&cpu->spin, PTHREAD_PROCESS_PRIVATE);
        pthread_create(&cpu->pth, NULL, cpu_func, cpu);
    }
    usleep(10000);
}

struct th *th_create(int cpu_id, void (*func)(struct th *), void *arg)
{
    struct th   *th;
    char        *buf;
    struct cpu  *cpu = &g_cpu[cpu_id];

    buf = malloc(sizeof(struct th) + STACK_SIZE);
    assert(buf);

    bzero((char *)buf, sizeof(sizeof(struct th) + STACK_SIZE));
    th = (struct th *)buf;
    get_ctx(&th->uc);
    th->uc.uc_stack.ss_sp = buf + sizeof(struct th);
    th->uc.uc_stack.ss_size = STACK_SIZE;
    th->uc.uc_link = &cpu->uc;
    th->cpu = cpu_id;
    th->done = 0;
    th_cd_init(&th->done_cd);
    th->arg = arg;
    l_init(&th->node);
    l_init(&th->evt.node);
    make_ctx(&th->uc, (ctx_func_t)func, 1, th);
    append_to_ready(th, cpu);
    pthread_cond_signal(&cpu->cond);

    return th;
}

static struct th *th_self(void)
{
    struct cpu  *cpu = &g_cpu[sched_getcpu()];
    return cpu->run;
}


static void insert_to_sleep_q(struct th *th, unsigned long usec)
{
    struct evt      *evt;
    struct timespec *ts = &th->evt.sleep;
    struct cpu      *cpu = &g_cpu[th->cpu];
    struct lh       *lh; 

    assert(th->cpu == sched_getcpu());
    
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += usec * 1000;
    ts_adjust(ts);

    for (lh=cpu->sleep.next; lh!=&cpu->sleep; lh=lh->next) {
        evt = l_entry(lh, struct evt, node);
        if (!ts_after(ts, &evt->sleep))
            break;
    }
    /* insert before lh */
    l_add_raw(&th->evt.node, lh->prev, lh);
}

static void wakeup_by_sleep(void *arg1, long arg2, long arg3)
{
    struct th   *th = (struct th *)arg1;

    assert(th->cpu == sched_getcpu());

    th->state &= ~TH_SLEEP;
    append_to_ready(th, &g_cpu[th->cpu]);
    /* it is not necessary signal myself :) */
    //pthread_cond_signal(&g_cpu[th->cpu].cond);
}

int th_usleep(unsigned long usec)
{
    struct th       *th = th_self();
    struct evt      *evt;
    struct cpu      *cpu = &g_cpu[th->cpu];

    evt = &th->evt;
    evt->func = wakeup_by_sleep;
    evt->arg1 = th;
    insert_to_sleep_q(th, usec);
    th->state &= ~TH_RUN;
    th->state |= TH_SLEEP; 
    sched(cpu);

    return 0;
}


int th_yield(void)
{
    struct th   *th = th_self();
    struct cpu  *cpu = &g_cpu[th->cpu];

    pthread_spin_lock(&cpu->spin);
    if (l_empty(&cpu->ready)) {
        pthread_spin_unlock(&cpu->spin);
        return 0;
    }

    th->state &= ~TH_RUN;
    append_to_ready_unlocked(th, cpu);
    pthread_spin_unlock(&cpu->spin);

    sched(cpu);
    
    return 1;
}

static int I_am_uth(void)
{
    int         i;
    pthread_t   self = pthread_self();

    for (i=0; i<NR_CPU; i++) {
        if (g_cpu[i].pth == self)
            return 1;
    }
    return 0;
}


int th_join(struct th *th)
{
    struct cpu  *cpu = &g_cpu[th->cpu];
    int         uth = I_am_uth();

    pthread_mutex_lock(&cpu->mutex);
    while (!th->done) {
        if (uth) 
            th_cd_wait(&th->done_cd);
        else
            pthread_cond_wait(&cpu->cond, &cpu->mutex);
    }
    pthread_mutex_unlock(&cpu->mutex);

    free(th);
    return 0;
}

int th_lk_init(struct lk *lk)
{
    lk->locked = 0;
    l_init(&lk->wait);
    pthread_spin_init(&lk->spin, PTHREAD_PROCESS_PRIVATE);
    return 0;
}

int th_lk_trylock(struct lk *lk)
{
    pthread_spin_lock(&lk->spin);
    if (lk->locked) {
        pthread_spin_unlock(&lk->spin);
        return -1;
    }
    lk->locked = 1;
    pthread_spin_unlock(&lk->spin);

    return 0;
}

int th_lk_lock(struct lk *lk)
{
    pthread_spin_lock(&lk->spin);
    while (lk->locked) {
        struct th   *th = th_self();

        l_add_tail(&th->node, &lk->wait);
        th->state |= TH_WAIT_LK;
        th->state &= TH_RUN;
        pthread_spin_unlock(&lk->spin);
        sched(&g_cpu[th->cpu]);
        pthread_spin_lock(&lk->spin);
    }
    lk->locked = 1;
    pthread_spin_unlock(&lk->spin);
    return 0;
}

int th_lk_unlock(struct lk *lk)
{
    struct th   *th = NULL;

    pthread_spin_lock(&lk->spin);
    lk->locked = 0;
    if (!l_empty(&lk->wait)) {
        th = l_entry(l_first(&lk->wait), struct th, node);
        l_del(&th->node);
    }
    pthread_spin_unlock(&lk->spin);

    if (th) {
        struct cpu *cpu = &g_cpu[th->cpu];

        th->state &= ~TH_WAIT_LK;
        append_to_ready(th, cpu);
        pthread_cond_signal(&cpu->cond);
    }
    return 0;
}

int th_cd_init(struct cd *cd)
{
    cd->val = 0;
    l_init(&cd->wait);
    pthread_spin_init(&cd->spin, PTHREAD_PROCESS_PRIVATE);
    return 0;
}

int th_cd_wait(struct cd *cd)
{
    pthread_spin_lock(&cd->spin);
    while (cd->val<=0) {
        struct th   *th = th_self();
        
        l_add_tail(&th->node, &cd->wait);
        th->state |= TH_WAIT_CD; 
        th->state &= ~TH_RUN;
        pthread_spin_unlock(&cd->spin);
        sched(&g_cpu[th->cpu]);
        pthread_spin_lock(&cd->spin);
    }
    cd->val --;
    pthread_spin_unlock(&cd->spin);
    return 0;
}

static void wakeup_by_cd_sleep(void *arg1, long arg2, long arg3)
{
    struct th    *th = (struct th *)arg1;
    struct cd    *cd = (struct cd *)arg2;

    assert(th->cpu == sched_getcpu());

    pthread_spin_lock(&cd->spin);
    if (!(th->state & TH_WAIT_CD)) {
        pthread_spin_unlock(&cd->spin);
        /* 
         * NOTE!!! 
         * We already have been waken up by th_cd_signal or th_cd_bcast.
         * This means another thread hold me, and it is maybe modifying my state (th->state),
         * so, we must use atomic_bit_clear to avoid race.
         */
        atomic_bit_clear(&th->state, TH_SLEEP);
        return;
    }
    
    th->state &= ~TH_WAIT_CD;
    l_del(&th->node);
    pthread_spin_unlock(&cd->spin);

    th->wakeup_expired = 1;
    th->state &= ~TH_SLEEP;
    append_to_ready(th, &g_cpu[th->cpu]);
    /* it is not necessary signal myself :) */
    //pthread_cond_signal(&g_cpu[th->cpu].cond);
}



int th_cd_timedwait(struct cd *cd, unsigned long usec)
{
    pthread_spin_lock(&cd->spin);
    while (cd->val<=0) {
        struct th   *th = th_self();

        th->wakeup_expired = 0;
        l_add_tail(&th->node, &cd->wait);
        th->state |= TH_WAIT_CD; 
        th->state &= ~TH_RUN;
        pthread_spin_unlock(&cd->spin);

        /*
         * NOTE:
         * Once we have linked in cd->wait, we can be accessed by the other thread immediately. 
         * for example, by the thread calling th_cd_signal on another core ..
         * so, we must use atomic_bit_set to avoid race.
         */
        atomic_bit_set(&th->state, TH_SLEEP);
        th->evt.func = wakeup_by_cd_sleep;
        th->evt.arg1 = th;
        th->evt.arg2 = (long)cd;
        insert_to_sleep_q(th, usec);
        sched(&g_cpu[th->cpu]);
        if (th->wakeup_expired) {
            return 0;
        }
        pthread_spin_lock(&cd->spin);
    }
    cd->val --;
    pthread_spin_unlock(&cd->spin);
    return 1;
}

int th_cd_signal(struct cd *cd)
{
    struct th   *th = NULL;

    pthread_spin_lock(&cd->spin);
    cd->val ++;
    if (!l_empty(&cd->wait)) {
        th = l_entry(l_first(&cd->wait), struct th, node);
        l_del(&th->node);
        th->state &= ~TH_WAIT_CD;
    }
    pthread_spin_unlock(&cd->spin);

    if (th) {
        struct cpu  *cpu = &g_cpu[th->cpu];
        
        append_to_ready(th, cpu);
        pthread_cond_signal(&cpu->cond);
    }
    return 0;
}

int th_cd_bcast(struct cd *cd)
{
    pthread_spin_lock(&cd->spin);
    cd->val = 0;
    while (!l_empty(&cd->wait)) {
        struct th   *th = l_entry(l_first(&cd->wait), struct th, node);
        struct cpu  *cpu = &g_cpu[th->cpu];

        l_del(&th->node);
        th->state &= ~TH_WAIT_CD;
        pthread_spin_unlock(&cd->spin);
        append_to_ready(th, cpu);
        pthread_cond_signal(&cpu->cond);
        pthread_spin_lock(&cd->spin);
    }
    pthread_spin_unlock(&cd->spin);
    return 0;
}

void test_func1(struct th *th)
{
    char    *name = (char *)th->arg;
    int     i = 0;
   
    for (i=0; i<10; i++) {
        printf("name = %s, %d\n", name, i);
        th_usleep(1000000);
    }
}

void show_args(void *p1, void *p2)
{
    printf("p1 = %p, p2 = %p\n", p1, p2);
    while(1);
}

int main()
{
    struct th   *th_1;

    init_cpu();

    th_1 = th_create(0, test_func1, "test 1");
    th_join(th_1);

    printf("line: %d\n", __LINE__);
    while(1) {
        int     c;
        c = getchar();
        if (c=='q')
                break;
    }
    return 0;
}

