/*
 * Copyright 2016, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <autoconf.h>
#include <stdio.h>

#include <sel4/sel4.h>
#include <sel4bench/arch/sel4bench.h>

#include <benchmark.h>
#include <scheduler.h>

#define NOPS ""
#define __SWINUM(x) ((x) & 0x00ffffff)

#include <arch/signal.h>
#define N_LOW_ARGS 5
#define N_HIGH_ARGS 4
#define N_YIELD_ARGS 2

void
abort(void)
{
    benchmark_finished(EXIT_FAILURE);
}

void
__arch_putchar(int c)
{
    benchmark_putchar(c);
}

void 
high_fn(int argc, char **argv) {

    assert(argc == N_HIGH_ARGS);
    seL4_CPtr produce = (seL4_CPtr) atol(argv[0]);
    seL4_CPtr done_ep = (seL4_CPtr) atol(argv[1]);
    volatile ccnt_t *start = (volatile ccnt_t *) atol(argv[2]);
    seL4_CPtr consume = (seL4_CPtr) atol(argv[3]);

    for (int i = 0; i < N_RUNS; i++) {
        DO_REAL_SIGNAL(produce);
        /* we're running at high prio, read the cycle counter */ 
        SEL4BENCH_READ_CCNT(*start);
        DO_REAL_WAIT(consume);
    }

    /* signal completion */
    seL4_Send(done_ep, seL4_MessageInfo_new(0, 0, 0, 0));
    /* block */
    seL4_Wait(produce, NULL);
}

void 
low_fn(int argc, char **argv)
{
    assert(argc == N_LOW_ARGS);
    seL4_CPtr produce = (seL4_CPtr) atol(argv[0]);
    volatile ccnt_t *start = (volatile ccnt_t *) atol(argv[1]);
    ccnt_t *results = (ccnt_t *) atol(argv[2]);
    seL4_CPtr done_ep = (seL4_CPtr) atol(argv[3]);
    seL4_CPtr consume = (seL4_CPtr) atol(argv[4]);
    
    for (int i = 0; i < N_RUNS; i++) {
        ccnt_t end;
        DO_REAL_WAIT(produce);
        SEL4BENCH_READ_CCNT(end);
        results[i] = (end - *start);
        DO_REAL_SIGNAL(consume);
    }

    /* signal completion */
    seL4_Send(done_ep, seL4_MessageInfo_new(0, 0, 0, 0));
    /* block */
    seL4_Wait(produce, NULL);
}

static void 
yield_fn(int argc, char **argv) {

   assert(argc == N_YIELD_ARGS);

   seL4_CPtr ep = (seL4_CPtr) atol(argv[0]);
   volatile ccnt_t *end = (volatile ccnt_t *) atol(argv[1]);

   for (int i = 0; i < N_RUNS; i++) {
       SEL4BENCH_READ_CCNT(*end);
       seL4_Yield();
   }

   seL4_Send(ep, seL4_MessageInfo_new(0, 0, 0, 0));
}

static void 
benchmark_yield(seL4_CPtr ep, ccnt_t *results, volatile ccnt_t *end)
{
    ccnt_t start;
    /* run the benchmark */
    for (int i = 0; i < N_RUNS; i++) {
        SEL4BENCH_READ_CCNT(start);
        seL4_Yield();
        results[i] = (*end - start);
    }

    benchmark_wait_children(ep, "yielder", 1);
}


static void 
benchmark_yield_thread(env_t *env, seL4_CPtr ep, ccnt_t *results) 
{
    sel4utils_thread_t thread;
    volatile ccnt_t end;
    char args_strings[N_YIELD_ARGS][WORD_STRING_SIZE];
    char *argv[N_YIELD_ARGS];

    benchmark_configure_thread(env, ep, seL4_MaxPrio, "yielder", &thread);
    sel4utils_create_word_args(args_strings, argv, N_YIELD_ARGS, ep, (seL4_Word) &end);
    sel4utils_start_thread(&thread, yield_fn, (void *) N_YIELD_ARGS, (void *) argv, 1);

    benchmark_yield(ep, results, &end);

    sel4utils_clean_up_thread(&env->vka, &env->vspace, &thread);
}

static void
benchmark_yield_process(env_t *env, seL4_CPtr ep, ccnt_t *results)
{
    sel4utils_process_t process;
    void *start;
    void *remote_start;
    seL4_CPtr remote_ep;
    char args_strings[N_YIELD_ARGS][WORD_STRING_SIZE];
    char *argv[N_YIELD_ARGS];
    UNUSED int error;
    cspacepath_t path;

    /* allocate a page to share for the start cycle count */
    start = vspace_new_pages(&env->vspace, seL4_AllRights, 1, seL4_PageBits);
    assert(start != NULL);
  
    benchmark_shallow_clone_process(env, &process, seL4_MaxPrio, yield_fn, "yield process"); 

    /* share memory for shared variable */
    remote_start = vspace_share_mem(&env->vspace, &process.vspace, start, 1, seL4_PageBits, 
                                  seL4_AllRights, 1);
    assert(remote_start != NULL);

    /* copy ep cap */
    vka_cspace_make_path(&env->vka, ep, &path);
    remote_ep = sel4utils_copy_cap_to_process(&process, path);
    assert(remote_ep != seL4_CapNull);
 
    sel4utils_create_word_args(args_strings, argv, N_YIELD_ARGS, remote_ep, (seL4_Word) remote_start);
    
    error = sel4utils_spawn_process(&process, &env->vka, &env->vspace, N_YIELD_ARGS, argv, 1);
    assert(error == seL4_NoError);

    benchmark_yield(ep, results, (volatile ccnt_t *) start);

    sel4utils_destroy_process(&process, &env->vka);
}     
    
static void
benchmark_prio_threads(env_t *env, seL4_CPtr ep, seL4_CPtr produce, seL4_CPtr consume, 
                       seL4_Word prio, ccnt_t *results)
{
    sel4utils_thread_t high, low;
    char high_args_strings[N_HIGH_ARGS][WORD_STRING_SIZE];
    char *high_argv[N_HIGH_ARGS];
    char low_args_strings[N_LOW_ARGS][WORD_STRING_SIZE];
    char *low_argv[N_LOW_ARGS];
    ccnt_t start;
    UNUSED int error;

    benchmark_configure_thread(env, ep, prio, "high", &high);
    benchmark_configure_thread(env, ep, seL4_MinPrio, "low", &low);
     
    sel4utils_create_word_args(high_args_strings, high_argv, N_HIGH_ARGS, produce, 
                               ep, (seL4_Word) &start, consume);
    sel4utils_create_word_args(low_args_strings, low_argv, N_LOW_ARGS, produce, 
                               (seL4_Word) &start, (seL4_Word) results, ep, consume);

    error = sel4utils_start_thread(&low, low_fn, (void *) N_LOW_ARGS, (void *) low_argv, 1);
    assert(error == seL4_NoError);
    error = sel4utils_start_thread(&high, high_fn, (void *) N_HIGH_ARGS, (void *) high_argv, 1);
    assert(error == seL4_NoError);

    benchmark_wait_children(ep, "children of scheduler benchmark", 2);
        
    sel4utils_clean_up_thread(&env->vka, &env->vspace, &high);
    sel4utils_clean_up_thread(&env->vka, &env->vspace, &low);
}   

static void
benchmark_prio_processes(env_t *env, seL4_CPtr ep, seL4_CPtr produce, seL4_CPtr consume, 
                         seL4_Word prio, ccnt_t *results)
{
    sel4utils_process_t high;
    sel4utils_thread_t low;
    char high_args_strings[N_HIGH_ARGS][WORD_STRING_SIZE];
    char *high_argv[N_HIGH_ARGS];
    char low_args_strings[N_LOW_ARGS][WORD_STRING_SIZE];
    char *low_argv[N_LOW_ARGS];
    void *start, *remote_start;
    seL4_CPtr remote_ep, remote_produce, remote_consume;
    UNUSED int error;
    cspacepath_t path;

    /* allocate a page to share for the start cycle count */
    start = vspace_new_pages(&env->vspace, seL4_AllRights, 1, seL4_PageBits);
    assert(start != NULL);
  
    benchmark_shallow_clone_process(env, &high, prio, high_fn, "high"); 
    /* run low in the same thread as us so we don't have to copy the results across */
    benchmark_configure_thread(env, ep, seL4_MinPrio, "low", &low);

    /* share memory for shared variable */
    remote_start = vspace_share_mem(&env->vspace, &high.vspace, start, 1, seL4_PageBits, seL4_AllRights, 1);
    assert(remote_start != NULL);

    /* copy ep cap */
    vka_cspace_make_path(&env->vka, ep, &path);
    remote_ep = sel4utils_copy_cap_to_process(&high, path);
    assert(remote_ep != seL4_CapNull);
    
    /* copy ntfn cap */
    vka_cspace_make_path(&env->vka, produce, &path);
    remote_produce = sel4utils_copy_cap_to_process(&high, path);
    assert(remote_produce != seL4_CapNull);

    /* copy ntfn cap */
    vka_cspace_make_path(&env->vka, consume, &path);
    remote_consume = sel4utils_copy_cap_to_process(&high, path);
    assert(remote_consume != seL4_CapNull);

    sel4utils_create_word_args(high_args_strings, high_argv, N_HIGH_ARGS, remote_produce, 
                               remote_ep, (seL4_Word) remote_start, remote_consume);
    sel4utils_create_word_args(low_args_strings, low_argv, N_LOW_ARGS, produce, 
                               (seL4_Word) start, (seL4_Word) results, ep, consume);

    error = sel4utils_start_thread(&low, low_fn, (void *) N_LOW_ARGS, (void *) low_argv, 1);
    assert(error == seL4_NoError);
    error = sel4utils_spawn_process(&high, &env->vka, &env->vspace, N_HIGH_ARGS, high_argv, 1);
    assert(error == seL4_NoError);

    benchmark_wait_children(ep, "children of scheduler benchmark", 2);
        
    sel4utils_clean_up_thread(&env->vka, &env->vspace, &low);
    sel4utils_destroy_process(&high, &env->vka);
}   

void
measure_signal_overhead(seL4_CPtr ntfn, ccnt_t *results)
{
    ccnt_t start, end;
    for (int i = 0; i < N_RUNS; i++) {
        SEL4BENCH_READ_CCNT(start);
        DO_NOP_SIGNAL(ntfn);
        SEL4BENCH_READ_CCNT(end);
        results[i] = (end - start);
    }
}

void
measure_yield_overhead(ccnt_t *results)
{
    ccnt_t start, end;
    for (int i = 0; i < N_RUNS; i++) {
        SEL4BENCH_READ_CCNT(start);
        SEL4BENCH_READ_CCNT(end);
        results[i] = (end - start);
    }
}

int
main(int argc, char **argv)
{
    env_t *env;
    UNUSED int error;
    vka_object_t done_ep, produce, consume;
    scheduler_results_t *results;

    env = benchmark_get_env(argc, argv, sizeof(scheduler_results_t));
    results = (scheduler_results_t *) env->results;

    sel4bench_init();

    error = vka_alloc_endpoint(&env->vka, &done_ep);
    assert(error == seL4_NoError);
    
    error = vka_alloc_notification(&env->vka, &produce);
    assert(error == seL4_NoError);

    error = vka_alloc_notification(&env->vka, &consume);
    assert(error == seL4_NoError);
    
    /* measure overhead */    
    measure_signal_overhead(produce.cptr, results->overhead_signal);
    measure_yield_overhead(results->overhead_yield);
    /* for the master seL4 kernel, the only thing that effects the length of a
     * schedule call is how far apart the two prios are that we are switching between.
     *
     * So for this benchmark we record the amount of time taken for a 
     * seL4_Signal to take place, causing a higher priority thread to run.
     *
     * Since the scheduler hits a different word in the 2nd level bitmap every wordBits
     * priority, we only test each wordBits prio.
     */
    for (int i = 0; i < N_PRIOS; i++) {
        uint8_t prio = seL4_MinPrio + 1 + (i * seL4_WordBits);
        benchmark_prio_threads(env, done_ep.cptr, produce.cptr, consume.cptr, prio,
                               results->thread_results[i]);
        benchmark_prio_processes(env, done_ep.cptr, produce.cptr, consume.cptr, prio,
                                 results->process_results[i]);
    }

    /* thread yield benchmarks */
    benchmark_yield_thread(env, done_ep.cptr, results->thread_yield);   
    benchmark_yield_process(env, done_ep.cptr, results->process_yield);   
    
    /* done -> results are stored in shared memory so we can now return */
    benchmark_finished(EXIT_SUCCESS);
    return 0;
}
