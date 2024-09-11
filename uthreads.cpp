#include <queue>
#include <iostream>
#include <list>
#include "uthreads.h"
#include <istream>
#include <csetjmp>
#include <map>
#include <algorithm>
#include <string>
#include <signal.h>
#include <sys/time.h>


// ----- CONSTANTS  --------
#define MAX_THREAD_NUM 100 /* maximal number of threads */
#define STACK_SIZE 4096 /* stack size per thread (in bytes) */
#define MILL 1000000
#define FAILURE -1
#define SUCCESS 0
#define INVALID_QUANTUM "Quantum number is not a non-positive number"
#define INVALID_ID "Wrong ID "
#define INVALID_EXCEED_THREAD_AMOUNT "Exceeded max thread amount"
#define INVALID_MEMORY_ALLOCATION "Allocation failure"
#define INVALID_ENV "env setup failure"
#define INVALID_SIGACTION "sigaction error."
#define INVALID_SETTIMER "setitimer error."
#define INVALID_SETTIMER "setitimer error."
#define INVALID_SLEEP_THREAD1 "main thread cant call the sleep function."
#define INVALID_SLEEP_THREAD2 "num quantum's for sleeping ,ust be positive."
#define INVALID_ENTRY_POINT "it is an error to call this function with a null entry_point"


#define TERMINATE 3
#define N_TERMINATE 4
#define TIMER_SIGNAL 26

// ----- DECLARATIONS --------
using namespace std;

typedef void (*thread_entry_point)(void);

typedef class Thread *T;
struct sigaction sa = {0};
struct itimerval timer;
int quantum_time; // The max time for changing threads at the library;
int quantum_counter; // Counter fot the total number of quantum's since the library was initialized.
int current_running_thread; // Saving the current running thread.
sigset_t block_set;
bool is_block = false;

enum State {
    RUNNING, BLOCKED, READY
};

enum Action {
    UNBLOCK, BLOCK
};

// ----- FUNCTIONS DECLARATIONS  --------
void function_failure_printer(const std::string &);

void system_call_failure_printer(const std::string &);

void ids_priorities_start();

int get_next_ready_thread();

void sleeping_list_handler();

void timer_init();

int uthread_get_tid();

int uthread_get_total_quantums();

int uthread_get_quantums(int);

void signal_handler(Action);

// void init_block_set(sigset_t *set);

void handle_running_thread_termination(int tid, T cur_thread);

void handle_non_running_thread_termination(int tid, T cur_thread);

void block_current_running_thread(T cur_thread);

void block_ready_sleep_thread(T cur_thread, int tid);

void prepare_new_thread(T new_thread);


// ----- ADDRESS TRANSLATING --------
// function were copied from the demo_jump.c
/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
#ifdef __x86_64__
/* code for 64 bit Intel arch */
typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

address_t translate_address(address_t addr) {
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}
#endif

// ----- DATA STRUCTURES --------
//A min heep data structure that will let us get the next minimal number for the threed tid.
priority_queue<int, vector<int>, greater<int> > ids_priorities;
//A list to keep track of the sleeping threads
list<int> sleeping_threads;
//The queue for the ready threads.
queue<int> ready_queue;
//Hash map that saves <Thread: Thread id>
map<int, T> thread_map;


class Thread {
public:
    int state;
    int tid;
    int quantum_counter;
    int sleeping_timer;
    char *stack;
    sigjmp_buf env{};

    /**
     * CONSTRUCTOR
     */
    Thread(int id, thread_entry_point process) : state(READY), tid(id), quantum_counter(0), sleeping_timer(0) {
        //Init the thread environment.
        if (tid > 0) {
            sigsetjmp(env, 1);
            stack = new(nothrow) char[STACK_SIZE];
            if (stack == nullptr) {
                system_call_failure_printer(INVALID_MEMORY_ALLOCATION);
                exit(1);
            }

            address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
            address_t pc = (address_t) process;
            sigsetjmp(env, 1);
            (env->__jmpbuf)[JB_SP] = (long) translate_address(sp);
            (env->__jmpbuf)[JB_PC] = (long) translate_address(pc);
            if (sigemptyset(&env->__saved_mask) == -1) {
                system_call_failure_printer(INVALID_ENV);
                exit(1);
            }
        }

        if (tid == 0) {
            sigsetjmp(env, 1);
        }
    }

    /** Destructor */
    ~Thread() {
        if (tid != 0) {
            delete[] stack;
            stack = nullptr;
        }

    }

};

// ----- ADDITIONAL FUNCTIONS --------

/**
 * Switching between threads.
 */
void switch_threads(int switch_case) {

    signal_handler(BLOCK);
    sleeping_list_handler();

    if (switch_case != TERMINATE) { // Not terminate case
        //Checks if the current thread is not the dummy.
        if (current_running_thread >= 0) {
            int old_thread_id = current_running_thread; //Saving the old thread.
            T old_thread = thread_map[old_thread_id];

            if (old_thread->state != BLOCKED) {
                old_thread->state = READY; // Switching the state of the old thread.

                if (old_thread->sleeping_timer == 0) {
                    ready_queue.push(old_thread_id); //pushing the current thread to the end of the ready queue.
                } else {//pushing the current thread to sleeping list.
                    sleeping_threads.push_back(old_thread_id);
                }
            }

            int ret_val = sigsetjmp(old_thread->env, 1); //Saving the old thread environment.
            //Checks if the environment saving was successful.
            bool did_just_save_bookmark = ret_val == 0;
            if (!did_just_save_bookmark) {
                old_thread->state = RUNNING; // Switching the state of the old thread.
                if (switch_case != TIMER_SIGNAL)
                    signal_handler(UNBLOCK);
                return;
            }
        }
    }


    //Gets the next ready thread from the ready queue.
    int new_thread_id = get_next_ready_thread();
    T new_thread = thread_map[new_thread_id];
    prepare_new_thread(new_thread);
    signal_handler(UNBLOCK);
    siglongjmp(new_thread->env, 1);
}

void prepare_new_thread(T new_thread) {
    current_running_thread = new_thread->tid;
    new_thread->state = RUNNING;
    new_thread->quantum_counter++;
    quantum_counter++;
    timer_init();
}


/**
 * Blocking or unblocking the timer signals
 */
void signal_handler(Action action) {
    switch (action) {
        case BLOCK: {
            sigprocmask(SIG_BLOCK, &block_set, nullptr);
            break;
        }

        case UNBLOCK: {
            sigprocmask(SIG_UNBLOCK, &block_set, nullptr);
            break;
            }
        break;
    }
}

/**
 *
 * @param problem - The string of the massage that need to be printed.
 */
void function_failure_printer(const std::string &problem) {
    std::cerr << "thread library error: " << problem << std::endl;
}

/**
 *
 * @param problem - The string of the massage that need to be printed.
 */
void system_call_failure_printer(const std::string &problem) {
    std::cerr << "system error: " << problem << std::endl;
}

/**
 * @param tid To remove from the ready queue.
 */
void remove_from_queue(int tid) {
    for (long unsigned int  i = 0; i <= ready_queue.size(); ++i) {
        int cur_id = ready_queue.front();
        ready_queue.pop();
        if (cur_id != tid) {
            ready_queue.push(cur_id);
        }
    }
}

/**
 * Pushing all the ids to the min heep
 */
void ids_priorities_start() {
    for (auto i = 1; i < MAX_THREAD_NUM; ++i) {
        ids_priorities.push(i);
    }
}

/**
 * The function return the next not blocked thread from the ready queue.
 * @return thread id upon success , otherwise -1.
 */
int get_next_ready_thread() {
    while (!ready_queue.empty()) {
        int new_thread = ready_queue.front();
        ready_queue.pop();
        return new_thread;
    }

    return 0;
}

/**
 * This function goes over all the threads in the slepping list ebery quantom time.
 * Decreasing for each thread the sleeping time, an if it equals to 0 take it out the sleeping list.
 */
void sleeping_list_handler() {
    int flag = 1;
    while (flag == 1) {
        for (auto it = sleeping_threads.begin();; it++) {

            if (it == sleeping_threads.end()) {
                flag = 0;
                break;
            }


            T cur_thread = thread_map[(*it)];
            cur_thread->sleeping_timer--;

            if (cur_thread->sleeping_timer == 0) { // awake if no blocked
                sleeping_threads.erase(it); //take out the thread from the sleeping list

                if (cur_thread->state == READY)
                    ready_queue.push(*it); // enter the thread to the ready queue.

                break;
            }
        }
    }
}

/**
 * With this function we get the next minimal number for the thread tid.
 */
int new_id() {
    int ret_val = ids_priorities.top(); //Getting the minimum number from the min heep.
    ids_priorities.pop(); //Popping out the minimum number from the min heep.
    return ret_val;
}

/**
 *
 * @param tid Thread id to be checked if valid
 * @return 0 on success -1 on failure
 */
int tid_check(int tid) {
    if (0 > tid || tid > MAX_THREAD_NUM || thread_map.find(tid) == thread_map.end()) {
        function_failure_printer(INVALID_ID);
        return FAILURE;
    }

    return SUCCESS;
}

// void init_block_set(sigset_t *set) {
//     sigemptyset(set);
//     sigaddset(set, SIGVTALRM);
//
// }

/**
 * @brief Handles termination of the currently running thread.
 *
 * @param tid The thread ID to terminate.
 * @param cur_thread The thread object to terminate.
 */
void handle_running_thread_termination(int tid, T cur_thread) {
    ids_priorities.push(tid);
    thread_map.erase(tid);
    if (cur_thread != nullptr) {
        delete cur_thread;
    }
    switch_threads(TERMINATE);
}

/**
 * @brief Handles termination of a non-running thread.
 *
 * @param tid The thread ID to terminate.
 * @param cur_thread The thread object to terminate.
 */
void handle_non_running_thread_termination(int tid, T cur_thread) {
    if (cur_thread->state == READY) {
        remove_from_queue(tid);
    }

    if (cur_thread->sleeping_timer > 0) {
        sleeping_threads.remove(tid);
    }

    ids_priorities.push(tid);
    thread_map.erase(tid);
    delete cur_thread;
}


/**
 * @brief Blocks the current running thread.
 *
 * @param cur_thread Pointer to the current thread structure.
 */
void block_current_running_thread(T cur_thread) {
    cur_thread->state = BLOCKED;
    switch_threads(N_TERMINATE);
}

void block_ready_sleep_thread(T cur_thread, int tid) {
    if (cur_thread->sleeping_timer == 0) { // if the thread is not sleep - its mean it was ready
        remove_from_queue(tid);
    }
    cur_thread->state = BLOCKED;
}


// ----- LIBRARY FUNCTIONS --------

/**
 * @brief initializes the thread library.
 *
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs) {
    //Argument Check - make sure quantum time is positive
    if (quantum_usecs <= 0) {
        function_failure_printer(INVALID_QUANTUM);
        return FAILURE;
    }

    // initialize the block set
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGVTALRM);

    //Initialize quantum counters
    quantum_time = quantum_usecs;
    quantum_counter = 1;

    //Initial the main thread
    thread_map[0] = new Thread(0, nullptr); //Initialize the dummy process.
    thread_map[0]->state = RUNNING;
    thread_map[0]->quantum_counter++;
    current_running_thread = 0;

    ids_priorities_start();

    //Initial the signal clock
    timer_init();
    return SUCCESS;
}


void timer_init() {
    sigemptyset(&sa.sa_mask); // Clear all signals from the sa_mask set (no signals will be blocked during the signal handling)
    sa.sa_handler = &switch_threads; // Set the handler function to switch_threads
    if (sigaction(SIGVTALRM, &sa, nullptr) < 0) { //  Define SIGVTALRM signal to use the specified handler
        system_call_failure_printer(INVALID_SIGACTION);
        exit(1);
    }
    // Configure the timer to expire after quantum_time sec... */
    timer.it_value.tv_sec = quantum_time / MILL;        // first time interval, seconds part
    timer.it_value.tv_usec = quantum_time % MILL;        // first time interval, microseconds part

    // configure the timer to expire every 3 sec after that.
    timer.it_interval.tv_sec = quantum_time / MILL;     // following time intervals, seconds part
    timer.it_interval.tv_usec = quantum_time % MILL;      // following time intervals, microseconds part

    // Start a virtual timer. It counts down whenever this process is executing.
    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr)) {
        system_call_failure_printer(INVALID_SETTIMER);
        exit(1);
    }
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point) {
    signal_handler(BLOCK);
    if (ids_priorities.empty()) {
        function_failure_printer(INVALID_EXCEED_THREAD_AMOUNT);
        signal_handler(UNBLOCK);
        return FAILURE;
    }
    if (entry_point == nullptr) {
        function_failure_printer(INVALID_ENTRY_POINT);
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    //Creates new Thread
    int id = new_id();
    Thread *new_thread = new Thread(id, entry_point);
    thread_map[id] = new_thread;
    ready_queue.push(id); // pushing the new thread to the queue.
    signal_handler(UNBLOCK);
    return id;
}


/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid) {
    signal_handler(BLOCK);
    //Argument check - sees if the tid is a legal number.
    if (tid_check(tid) == FAILURE) {
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    // if tid to terminate is the main thread- we will terminate the entire process
    if (tid == 0) {
        for (pair<int, T> thread: thread_map) {
            T cur_thread = thread.second;
            delete cur_thread;
        }

        signal_handler(UNBLOCK);
        exit(0);
    }

    // terminate the thread
    T cur_thread = thread_map[tid];
    if (tid == current_running_thread) {
        handle_running_thread_termination(tid, cur_thread);

    } else { //
        handle_non_running_thread_termination(tid, cur_thread);
    }

    signal_handler(UNBLOCK);
    return SUCCESS;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
    signal_handler(BLOCK);
    if (tid_check(tid) == FAILURE) {
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    if (tid == 0) {
        function_failure_printer(INVALID_ID);
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    T cur_thread = thread_map[tid];

    // If the thread is already blocked, return success
    if (cur_thread->state == BLOCKED) {
        signal_handler(UNBLOCK);
        return SUCCESS;
    }

    if (tid == current_running_thread) { //The current running thread blocked itself.
        block_current_running_thread(cur_thread);

    } else { // A thread from the ready queue got block
        // treat sleep and ready cases
        block_ready_sleep_thread(cur_thread, tid);
    }

    signal_handler(UNBLOCK);
    return SUCCESS;
}


/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
    signal_handler(BLOCK);
    if (tid_check(tid) == FAILURE) {
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    T cur_thread = thread_map[tid];
    if (cur_thread->state == BLOCKED) {
        cur_thread->state = READY; //Changing the thread state

        if (cur_thread->sleeping_timer == 0) { //If the thread is not in sleeping mode
            ready_queue.push(tid); //Pushing the thread ID to the queue
        }
    }

    signal_handler(UNBLOCK);
    return SUCCESS;
}


/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY threads list.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid==0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums) {
    signal_handler(BLOCK);
    if (current_running_thread == 0) { //The main thread can't go to sleep
        function_failure_printer(INVALID_SLEEP_THREAD1);
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    if (num_quantums <= 0) {
        function_failure_printer(INVALID_SLEEP_THREAD2);
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    T cur_thread = thread_map[current_running_thread];
    cur_thread->sleeping_timer = num_quantums;
    switch_threads(N_TERMINATE);
    signal_handler(UNBLOCK);
    return SUCCESS;
}

/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
    return current_running_thread;
}


/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums() {
    return quantum_counter;
}


/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid) {
    signal_handler(BLOCK);
    if (tid_check(tid) == FAILURE) {
        signal_handler(UNBLOCK);
        return FAILURE;
    }

    int result = thread_map[tid]->quantum_counter;
    signal_handler(UNBLOCK);
    return result;
}