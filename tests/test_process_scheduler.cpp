/**
 * @file test_process_scheduler.cpp
 * @brief Tests for Process<> and ProcessScheduler<>.
 *
 * Tests cover:
 *  1.  Process starts in Uninitialized state
 *  2.  init() called on first tick
 *  3.  update() called every tick while running
 *  4.  succeed() transitions to Succeeded and removes from scheduler
 *  5.  fail() transitions to Failed and removes from scheduler
 *  6.  succeeded() hook called on success
 *  7.  failed() hook called on failure
 *  8.  abort() transitions to Aborted
 *  9.  aborted() hook called on abort
 * 10.  abortAll() aborts all live processes
 * 11.  Successor runs after successful completion
 * 12.  Successor does NOT run after failure
 * 13.  Successor does NOT run after abort
 * 14.  Chained successors run in order
 * 15.  scheduler.size() and empty() reflect live count
 * 16.  Multiple independent top-level processes tick independently
 * 17.  Process receiving delta value correctly
 * 18.  Process with shared Data context
 * 19.  Process that never succeeds runs indefinitely
 * 20.  attach().then() chain: handle points to latest successor
 */

#include <fatp_ecs/FatpEcs.h>

#include <cassert>
#include <cstdio>
#include <vector>
#include <string>

using namespace fatp_ecs;

// =============================================================================
// Test harness
// =============================================================================

static int sTestsPassed = 0;
static int sTestsFailed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
            ++sTestsFailed;                                                 \
            return;                                                         \
        }                                                                   \
    } while (0)

#define RUN_TEST(fn)                                    \
    do                                                  \
    {                                                   \
        std::printf("  Running: %s\n", #fn);            \
        fn();                                           \
        ++sTestsPassed;                                 \
    } while (0)

// =============================================================================
// Test process types
// =============================================================================

// Records calls to each lifecycle hook
struct TraceProcess : Process<TraceProcess, float>
{
    std::vector<std::string>* log{nullptr};
    int ticksUntilSucceed{1};

    explicit TraceProcess(std::vector<std::string>& l, int ticks = 1)
        : log(&l), ticksUntilSucceed(ticks) {}

    void onInit()      { log->push_back("init"); }
    void onUpdate(float, void*&)
    {
        log->push_back("update");
        --ticksUntilSucceed;
        if (ticksUntilSucceed <= 0) succeed();
    }
    void onSucceeded() { log->push_back("succeeded"); }
    void onFailed()    { log->push_back("failed"); }
    void onAborted()   { log->push_back("aborted"); }
};

struct FailProcess : Process<FailProcess, float>
{
    std::vector<std::string>* log{nullptr};
    explicit FailProcess(std::vector<std::string>& l) : log(&l) {}
    void onUpdate(float, void*&) { fail(); }
    void onFailed() { log->push_back("failed"); }
};

struct CountProcess : Process<CountProcess, float>
{
    int* count{nullptr};
    int limit{0};
    CountProcess(int& c, int lim) : count(&c), limit(lim) {}
    void onUpdate(float, void*&)
    {
        ++(*count);
        if (*count >= limit) succeed();
    }
};

// Process with a typed Data context
struct DataProcess : Process<DataProcess, float, int>
{
    int* out{nullptr};
    explicit DataProcess(int& o) : out(&o) {}
    void onUpdate(float, int& data)
    {
        *out = data;
        succeed();
    }
};

// =============================================================================
// Tests
// =============================================================================

static void test_initial_state()
{
    std::vector<std::string> log;
    TraceProcess p(log);
    TEST_ASSERT(p.state() == ProcessState::Uninitialized, "starts Uninitialized");
    TEST_ASSERT(!p.alive(),     "not alive initially");
    TEST_ASSERT(!p.succeeded(), "not succeeded initially");
    TEST_ASSERT(!p.failed(),    "not failed initially");
    TEST_ASSERT(!p.aborted(),   "not aborted initially");
}

static void test_init_called_on_first_tick()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 99); // won't succeed for 99 ticks

    sched.update(0.f);
    TEST_ASSERT(log.size() >= 1 && log[0] == "init", "init called first tick");
}

static void test_update_called_every_tick()
{
    int count = 0;
    ProcessScheduler<float> sched;
    sched.attach<CountProcess>(count, 5);

    for (int i = 0; i < 4; ++i) sched.update(1.f);
    TEST_ASSERT(count == 4, "update called 4 times before succeed");
    sched.update(1.f); // 5th tick: succeed
    TEST_ASSERT(count == 5, "update called 5th time");
    TEST_ASSERT(sched.empty(), "process removed after success");
}

static void test_succeed_removes_process()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 1);

    TEST_ASSERT(sched.size() == 1, "one process");
    sched.update(0.f); // init + update + succeed on same tick
    TEST_ASSERT(sched.empty(), "process removed after succeed");
}

static void test_fail_removes_process()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<FailProcess>(log);

    sched.update(0.f);
    TEST_ASSERT(sched.empty(), "process removed after fail");
}

static void test_succeeded_hook_called()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 1);
    sched.update(0.f);

    bool found = false;
    for (const auto& s : log) if (s == "succeeded") { found = true; break; }
    TEST_ASSERT(found, "succeeded hook called");
}

static void test_failed_hook_called()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<FailProcess>(log);
    sched.update(0.f);
    TEST_ASSERT(log[0] == "failed", "failed hook called");
}

static void test_abort()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 99);
    sched.update(0.f); // start it running

    // Manually abort all
    sched.abortAll();
    TEST_ASSERT(sched.empty(), "process removed after abortAll");
}

static void test_aborted_hook_called()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 99);
    sched.update(0.f);
    log.clear();

    sched.abortAll();
    bool found = false;
    for (const auto& s : log) if (s == "aborted") { found = true; break; }
    TEST_ASSERT(found, "aborted hook called");
}

static void test_abort_all()
{
    int c1 = 0, c2 = 0;
    ProcessScheduler<float> sched;
    sched.attach<CountProcess>(c1, 100);
    sched.attach<CountProcess>(c2, 100);

    sched.update(1.f);
    sched.abortAll();
    TEST_ASSERT(sched.empty(), "both processes removed");
}

static void test_successor_runs_after_success()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 1)
         .then<TraceProcess>(log, 1);

    sched.update(0.f); // first process: init+update+succeed
    TEST_ASSERT(sched.size() == 1, "successor queued after first completes");

    sched.update(0.f); // successor: init+update+succeed
    TEST_ASSERT(sched.empty(), "successor also completed");

    // log should contain two "init" calls
    int initCount = 0;
    for (const auto& s : log) if (s == "init") ++initCount;
    TEST_ASSERT(initCount == 2, "both processes ran init");
}

static void test_successor_not_run_after_fail()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<FailProcess>(log)
         .then<TraceProcess>(log, 1);

    sched.update(0.f); // fail first process
    TEST_ASSERT(sched.empty(), "no successor after failure");

    bool hadInit = false;
    for (const auto& s : log) if (s == "init") hadInit = true;
    TEST_ASSERT(!hadInit, "successor init never called");
}

static void test_successor_not_run_after_abort()
{
    std::vector<std::string> log;
    ProcessScheduler<float> sched;
    sched.attach<TraceProcess>(log, 99)
         .then<TraceProcess>(log, 1);

    sched.update(0.f); // start running
    sched.abortAll();
    TEST_ASSERT(sched.empty(), "no successor after abort");
}

static void test_chained_successors_run_in_order()
{
    std::vector<std::string> events;
    struct TaggedProcess : Process<TaggedProcess, float>
    {
        std::vector<std::string>* log;
        std::string tag;
        TaggedProcess(std::vector<std::string>& l, std::string t)
            : log(&l), tag(std::move(t)) {}
        void onUpdate(float, void*&)
        {
            log->push_back(tag);
            succeed();
        }
    };

    ProcessScheduler<float> sched;
    sched.attach<TaggedProcess>(events, "A")
         .then<TaggedProcess>(events, "B")
         .then<TaggedProcess>(events, "C");

    sched.update(0.f); // A runs
    sched.update(0.f); // B runs
    sched.update(0.f); // C runs

    TEST_ASSERT(events.size() == 3,          "three updates");
    TEST_ASSERT(events[0] == "A", "A first");
    TEST_ASSERT(events[1] == "B", "B second");
    TEST_ASSERT(events[2] == "C", "C third");
}

static void test_size_and_empty()
{
    int c = 0;
    ProcessScheduler<float> sched;
    TEST_ASSERT(sched.empty(),   "empty initially");
    TEST_ASSERT(sched.size()==0, "size 0 initially");

    sched.attach<CountProcess>(c, 10);
    TEST_ASSERT(sched.size() == 1, "size 1 after attach");
    TEST_ASSERT(!sched.empty(),    "not empty after attach");

    for (int i = 0; i < 10; ++i) sched.update(1.f);
    TEST_ASSERT(sched.empty(), "empty after process completes");
}

static void test_multiple_independent_processes()
{
    int c1 = 0, c2 = 0, c3 = 0;
    ProcessScheduler<float> sched;
    sched.attach<CountProcess>(c1, 3);
    sched.attach<CountProcess>(c2, 5);
    sched.attach<CountProcess>(c3, 2);

    TEST_ASSERT(sched.size() == 3, "three live processes");
    sched.update(1.f); // c1=1, c2=1, c3=1 (c3 doesn't succeed yet, 2 ticks needed)
    sched.update(1.f); // c1=2, c2=2, c3=2 -> c3 succeeds
    TEST_ASSERT(sched.size() == 2, "c3 finished at tick 2");
    sched.update(1.f); // c1=3 -> c1 succeeds, c2=3
    TEST_ASSERT(sched.size() == 1, "c1 finished at tick 3");
}

static void test_delta_passed_correctly()
{
    struct DeltaCapture : Process<DeltaCapture, float>
    {
        float captured{0.f};
        void onUpdate(float dt, void*&) { captured = dt; succeed(); }
    };

    ProcessScheduler<float> sched;
    sched.attach<DeltaCapture>();
    // Can't easily inspect after removal — check via flag before succeed
    // Use a raw instance to verify tick behavior directly
    DeltaCapture proc;
    void* dummy = nullptr;
    proc.tick(3.14f, dummy);
    TEST_ASSERT(proc.captured == 3.14f, "delta passed to update");
}

static void test_typed_data_context()
{
    int result = 0;
    ProcessScheduler<float, int> sched;
    sched.attach<DataProcess>(result);

    sched.update(1.f, 42);
    TEST_ASSERT(result == 42, "typed data context received in update");
    TEST_ASSERT(sched.empty(), "process completed");
}

static void test_perpetual_process()
{
    int count = 0;
    ProcessScheduler<float> sched;
    // 1000-tick process — never terminates in this test
    sched.attach<CountProcess>(count, 1000);

    for (int i = 0; i < 50; ++i) sched.update(1.f);
    TEST_ASSERT(count == 50, "process ticked 50 times without completing");
    TEST_ASSERT(sched.size() == 1, "still live");
}

static void test_then_chain_handle()
{
    // attach().then().then() — verify third process in chain eventually runs
    int order = 0;
    struct SeqProcess : Process<SeqProcess, float>
    {
        int* order;
        int step;
        SeqProcess(int& o, int s) : order(&o), step(s) {}
        void onUpdate(float, void*&) { *order = step; succeed(); }
    };

    ProcessScheduler<float> sched;
    auto h = sched.attach<SeqProcess>(order, 1);
    h.then<SeqProcess>(order, 2).then<SeqProcess>(order, 3);

    sched.update(0.f); TEST_ASSERT(order == 1, "step 1");
    sched.update(0.f); TEST_ASSERT(order == 2, "step 2");
    sched.update(0.f); TEST_ASSERT(order == 3, "step 3");
    TEST_ASSERT(sched.empty(), "all done");
}

// =============================================================================
// main
// =============================================================================

int main()
{
    std::printf("=== test_process_scheduler ===\n");

    RUN_TEST(test_initial_state);
    RUN_TEST(test_init_called_on_first_tick);
    RUN_TEST(test_update_called_every_tick);
    RUN_TEST(test_succeed_removes_process);
    RUN_TEST(test_fail_removes_process);
    RUN_TEST(test_succeeded_hook_called);
    RUN_TEST(test_failed_hook_called);
    RUN_TEST(test_abort);
    RUN_TEST(test_aborted_hook_called);
    RUN_TEST(test_abort_all);
    RUN_TEST(test_successor_runs_after_success);
    RUN_TEST(test_successor_not_run_after_fail);
    RUN_TEST(test_successor_not_run_after_abort);
    RUN_TEST(test_chained_successors_run_in_order);
    RUN_TEST(test_size_and_empty);
    RUN_TEST(test_multiple_independent_processes);
    RUN_TEST(test_delta_passed_correctly);
    RUN_TEST(test_typed_data_context);
    RUN_TEST(test_perpetual_process);
    RUN_TEST(test_then_chain_handle);

    std::printf("\n%d/%d tests passed\n", sTestsPassed, sTestsPassed + sTestsFailed);
    return sTestsFailed == 0 ? 0 : 1;
}
