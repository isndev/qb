#!/usr/bin/env python3
"""mk_cv.py <qb tree> --variant E|G [--mutant NAME]... [--loop-door] [--trace] [--no-tests]

SCRATCH prototype of the CONVERGED coroutine-teardown design on qb 9053b0c7 (train 7, QB-1246 follow-up). Never committed.

Variant E   the watchers of a destroyed loop are detached AT THE LOOP (qev: ev_loop_destroy), no awaiter guard.
Variant G   the awaiter destructors skip their stop when the thread's loop is gone (detail::stop_unless_loop_gone).

Common to both:
   1  ~CoroutineScheduler destroys what it owns while its thread's loop lives, again until it owns nothing; once the
      loop is gone -- or while one of its own coroutines is executing -- it destroys NOTHING, counts every root it
      owns in abandoned_coroutine_frames_total() and says why on one line.
   2  the per-thread record (unborn / alive / gone), read at both doors: detail::bind_thread_scheduler() and
      listener::coro_scheduler(). A wake, a completed frame's hand-over and a cleanup are dropped after the loop; a
      request for the scheduler stops the process with one named line.
   4  exit() from a coroutine body: a scheduler never destroys frames while its own drain is on the stack.
   6  listener::release(): one entry point for ~base, the QUIC endpoint and the tcp connector.
   8  the lane guard: in AddressSanitizer builds the thread's listener is poisoned once its destructor has completed.
(3 and 7 are the modules': mk_cv_mod.py.)

--mutant NAME plants ONE defect (see MUTANTS); --loop-door adds the third door (listener::loop()); --trace adds a
scratch-only line naming the backend each listener came up on (CV_BACKEND_TRACE=1).
"""
import argparse
import sys

MUTANTS = {
    "M1": "the destructor no longer destroys (alive branch)",
    "M2": "the gate removed: destroy even when the thread's loop is gone",
    "M3": "the tally counts the suspended set again",
    "M4": "schedule_via_current binds when the loop is gone",
    "M5": "bind_thread_scheduler reaches the listener when it is gone",
    "M6timer": "G: ~timer_awaiter's guard removed",
    "M6io": "G: ~socket_awaiter's guard removed",
    "M6recv": "G: the timed recv awaiter's guard removed",
    "M6send": "G: the timed send awaiter's guard removed",
    "M6timeout": "G: the timeout awaiter's guard removed",
    "E5all": "E: ev_loop_destroy no longer detaches",
    "E5timer": "E: the timers are not detached",
    "E5io": "E: the io watchers are not detached",
    "E5pending": "E: the pending entries are not cleared",
    "M7": "the phase never becomes gone",
    "M8": "a temporary listener enters the record",
    "M9": "listener::release() reaches the listener (the detached branch removed)",
    "M10": "the poisoning removed",
    "N1": "the repeat-until-empty loop removed (F4)",
    "N2": "the gone branch destroys the ready queue and the completed frames (F2)",
    "N3": "listener::coro_scheduler() unguarded (F5)",
    "N5": "the executing-frame exclusion removed (F6)",
    "N6": "the bound on the teardown passes removed",
    "N7": "a cleanup back on coro_scheduler() (the timeout awaiter's)",
    "N8": "defer_frame_destruction asks for the scheduler when the loop is gone",
    "N9": "the timeout awaiter stops on listener::current.loop() again",
    "N10": "the foreign-thread refusal removed",
}

ap = argparse.ArgumentParser()
ap.add_argument("root")
ap.add_argument("--variant", required=True, choices=["E", "G"])
ap.add_argument("--mutant", action="append", default=[])
ap.add_argument("--loop-door", action="store_true")
ap.add_argument("--loop-record", action="store_true",
                help="ALTERNATIVE: the record names the loop; sleep()/wait_*()/recv_for/send_for/timeouts read it there (implies --loop-door)")
ap.add_argument("--inline-marker", action="store_true",
                help="EXTENSION: a coroutine resumed outside a drain (an ask reply, inline) marks the thread for its duration")
ap.add_argument("--door2", default="first", choices=["first", "cold", "record"],
                help="where listener::coro_scheduler() reads the record: first (before any member, the brief), cold (in the "
                     "branch that creates the scheduler), record (first, and the free coro_scheduler() reads the scheduler from the record)")
ap.add_argument("--trace", action="store_true")
ap.add_argument("--no-tests", action="store_true")
ap.add_argument("--f6-by-handle", action="store_true", help="ALTERNATIVE (refuted): exclude only the handle run_ready() resumed")
args = ap.parse_args()
root, V, M = args.root, args.variant, set(args.mutant)
if args.loop_record:
    assert V == "E", "--loop-record is prototyped on variant E"
    args.loop_door = True
for m in M:
    assert m in MUTANTS, m


class File:
    def __init__(self, rel):
        self.rel = rel
        self.path = root + "/" + rel
        self.s = open(self.path, newline="", encoding="utf-8").read()
        assert "\r" not in self.s, rel

    def sub(self, old, new, count=1):
        assert self.s.count(old) == count, (self.rel, old[:90], self.s.count(old))
        self.s = self.s.replace(old, new)

    def save(self):
        open(self.path, "w", newline="", encoding="utf-8").write(self.s)
        print("patched", self.rel)


# ======================================================================================================================
# coroutine/task.h -- the report's signature
# ======================================================================================================================
task = File("src/qb/io/async/coroutine/task.h")
task.sub(
    "// Defined OUT OF LINE in qb/io/logger.cpp too, for the same reasons. Called from\n"
    "// `~CoroutineScheduler` when it tears down with `count` frames still SUSPENDED -- parked on a\n"
    "// watcher or a wait list -- which it deliberately abandons rather than destroys (Finding 2.B.8:\n"
    "// their watchers still reference them, and destroying a frame under a live loop is worse than\n"
    "// leaking it). Until 3.2 that was a `fprintf` behind `#ifndef NDEBUG`: in every release build a\n"
    "// service that recreated listeners accumulated frames with no message and no number (Huly QB-84).\n"
    "// It adds `count` to the process-wide tally below and prints ONE line on `qb::io::cerr`, in every\n"
    "// build. Reaching it is a lifecycle misuse -- stop the loop before destroying its scheduler --\n"
    "// so it is never on a hot path.\n"
    "void report_abandoned_coroutine_frames(std::size_t count, void const *scheduler) noexcept;\n"
    "\n"
    "/// The number of suspended coroutine frames abandoned at scheduler teardown since the process\n"
    "/// started, every thread included: what an operator or a test reads to know a lifecycle went\n"
    "/// wrong somewhere, since the frames themselves are gone (Huly QB-84).\n",
    "/// Why a `CoroutineScheduler` being destroyed left the coroutines it owns undestroyed (the orders in which\n"
    "/// destroying them would run code that cannot run): see `~CoroutineScheduler`.\n"
    "enum class coroutine_abandon_reason : unsigned char {\n"
    "    loop_gone,      ///< destroyed after its thread's `listener::current`: the loop its coroutines waited on is gone\n"
    "    draining,       ///< destroyed from inside one of its own coroutines (`exit()` in a coroutine body): one is executing\n"
    "    foreign_thread, ///< destroyed on another thread than the one that built it: its coroutines wait on that thread's loop\n"
    "    respawning,     ///< its coroutines' destructors kept spawning on it, teardown pass after teardown pass\n"
    "};\n"
    "\n"
    "// Defined OUT OF LINE in qb/io/logger.cpp too, for the same reasons. Called from\n"
    "// `~CoroutineScheduler` when it leaves `count` coroutines it OWNS undestroyed -- the spawned roots,\n"
    "// each with its chain -- for the reason `why` names. A scheduler destroys what it owns; these are\n"
    "// the orders in which it cannot (Finding 2.B.8 abandoned in every order until 3.4). It adds `count`\n"
    "// to the process-wide tally below and prints ONE line on `qb::io::cerr`, in every build (Huly QB-84).\n"
    "// Reaching it is a lifecycle misuse -- destroy a scheduler before its thread's listener, and not\n"
    "// from one of its own coroutines -- so it is never on a hot path.\n"
    "void report_abandoned_coroutine_frames(std::size_t count, void const *scheduler, coroutine_abandon_reason why) noexcept;\n"
    "\n"
    "/// The number of coroutines a scheduler owned and could not destroy when it was destroyed, since the\n"
    "/// process started, every thread included: what an operator or a test reads to know a lifecycle went\n"
    "/// wrong somewhere, since the frames themselves are unreachable (Huly QB-84). A coroutine, not a frame:\n"
    "/// a root is counted once, whatever chain it awaits.\n",
)
task.save()

# ======================================================================================================================
# logger.cpp -- the report says which order it was
# ======================================================================================================================
lg = File("src/qb/io/logger.cpp")
lg.sub(
    "qb::io::async::report_abandoned_coroutine_frames(std::size_t const count, void const *const scheduler) noexcept {\n"
    "    g_abandoned_coroutine_frames.fetch_add(count, std::memory_order_relaxed);\n"
    "    try {\n"
    "        qb::io::cerr() << \"WARNING: qb coroutine scheduler \" << scheduler << \" destroyed with \" << count\n"
    "                       << \" suspended coroutine frame(s) still parked on a watcher or a wait list -- ABANDONED, not \"\n"
    "                          \"destroyed (their watchers still reference them). Stop the event loop before destroying its \"\n"
    "                          \"scheduler. Process total so far: \"\n"
    "                       << g_abandoned_coroutine_frames.load(std::memory_order_relaxed) << std::endl;\n",
    "qb::io::async::report_abandoned_coroutine_frames(std::size_t const count, void const *const scheduler,\n"
    "                                                 coroutine_abandon_reason const why) noexcept {\n"
    "    g_abandoned_coroutine_frames.fetch_add(count, std::memory_order_relaxed);\n"
    "    char const *order = \"AFTER its thread's event loop (listener::current)\";\n"
    "    char const *cause = \"the loop they waited on is gone and nothing can run on that thread any more. Destroy a \"\n"
    "                        \"scheduler before its thread's listener\";\n"
    "    if (why == coroutine_abandon_reason::draining) {\n"
    "        order = \"FROM INSIDE one of its own coroutines\";\n"
    "        cause = \"one of them is executing. Leave the coroutine before its scheduler goes (exit() in a coroutine \"\n"
    "                \"body ends its thread's listener from inside it)\";\n"
    "    } else if (why == coroutine_abandon_reason::foreign_thread) {\n"
    "        order = \"ON ANOTHER THREAD than the one that built it\";\n"
    "        cause = \"they wait on that thread's loop, which may be running. A scheduler is destroyed on the thread \"\n"
    "                \"that built it (exit() on another thread destroys the ones with static storage there)\";\n"
    "    } else if (why == coroutine_abandon_reason::respawning) {\n"
    "        order = \"while its coroutines' destructors kept spawning on it\";\n"
    "        cause = \"each teardown pass left it new ones. A coroutine local's destructor must not spawn on, and \"\n"
    "                \"pump, a scheduler that is being destroyed\";\n"
    "    }\n"
    "    try {\n"
    "        qb::io::cerr() << \"WARNING: qb coroutine scheduler \" << scheduler << \" destroyed \" << order << \" with \" << count\n"
    "                       << \" coroutine(s) it owns -- ABANDONED, not destroyed: \" << cause << \". Process total so far: \"\n"
    "                       << g_abandoned_coroutine_frames.load(std::memory_order_relaxed) << std::endl;\n",
)
lg.save()

# ======================================================================================================================
# coroutine/scheduler.h
# ======================================================================================================================
sched = File("src/qb/io/async/coroutine/scheduler.h")

record = (
    "CoroutineScheduler &bind_thread_scheduler();\n"
    "\n"
    "/// Where this thread's `listener::current` -- the loop every awaiter of qb arms its watcher on, and the owner of the\n"
    "/// thread's scheduler -- stands.\n"
    "enum class thread_loop_phase : unsigned char { unborn, alive, gone };\n"
    "/// Trivially destructible on purpose, like `live_schedulers_t`: it is read after every thread_local destructor, and by\n"
    "/// the destructors of objects with static storage, which run after the main thread's thread-locals.\n"
    "struct thread_loop_t {\n"
    "    void const       *owner; ///< the thread's `listener::current`; once it is gone, only COMPARED, never followed\n"
)
if V == "G":
    record += "    struct ev_loop   *raw;   ///< its loop; once it is gone, only COMPARED, never followed\n"
if args.loop_record:
    record += "    struct ev_loop   *raw;   ///< ALTERNATIVE: the thread's loop while its listener lives, null before and after\n"
if args.door2 == "record":
    record += "    CoroutineScheduler *scheduler; ///< ALTERNATIVE: the thread's listener's scheduler while it has one\n"
if args.inline_marker:
    record += "    unsigned          inline_resumes; ///< EXTENSION: coroutines of this thread being resumed outside a run_ready() drain\n"
record += (
    "    thread_loop_phase phase; ///< alive from the end of its constructor to the end of its destructor's body\n"
    "};\n"
    "QB_ABI_ANCHOR inline thread_loop_t &\n"
    "thread_loop() noexcept {\n"
    "    thread_local thread_loop_t state{};\n"
    "    return state;\n"
    "}\n"
    "/// The thread's loop was destroyed: nothing will pump this thread again.\n"
    "[[nodiscard]] inline bool\n"
    "thread_loop_gone() noexcept {\n"
    "    return thread_loop().phase == thread_loop_phase::gone;\n"
    "}\n"
    "/// `listener` is this thread's `listener::current` and its destructor has run.\n"
    "[[nodiscard]] inline bool\n"
    "is_gone_thread_listener(void const *listener) noexcept {\n"
    "    thread_loop_t const &state = thread_loop();\n"
    "    return state.phase == thread_loop_phase::gone && state.owner == listener;\n"
    "}\n"
    "/// A request that cannot be served once the thread's loop is gone -- the thread's scheduler, to register a wait or\n"
    "/// to spawn on it -- stops the process with one line that names `asked`; defined in `listener.cpp`. A wake, the\n"
    "/// hand-over of a completed frame and a cleanup never get here: they are dropped (`schedule_via_current()`,\n"
    "/// `defer_frame_destruction()`, `detail::cleanup_scheduler()`).\n"
    "[[noreturn]] void refuse_after_thread_loop(char const *asked) noexcept;\n"
)
if args.inline_marker:
    record += (
        "/// EXTENSION. For its lifetime, a coroutine of this thread is being resumed OUTSIDE a `run_ready()` drain -- inline,\n"
        "/// from the handler that routed an `ask` reply (qb-core). A teardown reached from that coroutine (`exit()` in its\n"
        "/// body ends the thread's listener) then finds a frame executing, exactly as under a drain, and destroys nothing.\n"
        "class inline_resume_scope {\n"
        "public:\n"
        "    inline_resume_scope() noexcept {\n"
        "        ++thread_loop().inline_resumes;\n"
        "    }\n"
        "    ~inline_resume_scope() {\n"
        "        --thread_loop().inline_resumes;\n"
        "    }\n"
        "    inline_resume_scope(inline_resume_scope const &)            = delete;\n"
        "    inline_resume_scope &operator=(inline_resume_scope const &) = delete;\n"
        "};\n"
    )
if V == "G":
    record += (
        "/// `loop` is this thread's loop and it was destroyed: a watcher armed on it can neither fire nor be stopped.\n"
        "[[nodiscard]] inline bool\n"
        "loop_is_gone(struct ev_loop const *loop) noexcept {\n"
        "    thread_loop_t const &state = thread_loop();\n"
        "    return state.phase == thread_loop_phase::gone && state.raw == loop;\n"
        "}\n"
        "/// The stop of an awaiter's DESTRUCTOR: a frame destroyed after the loop its watcher was armed on -- a task with\n"
        "/// static storage, after the main thread's listener -- ignores the watcher (libev's contract after\n"
        "/// ev_loop_destroy) instead of stopping it on freed memory. The stops of `await_resume()` stay raw: a coroutine\n"
        "/// that resumes has a live loop. LIMIT: only the thread's own loop is known; a watcher armed on another\n"
        "/// listener's loop (a temporary listener) is stopped, whatever became of that loop.\n"
        "inline void\n"
        "stop_unless_loop_gone(struct ev_loop *loop, ev_timer *watcher) noexcept {\n"
        "    if (!loop_is_gone(loop)) [[likely]]\n"
        "        ev_timer_stop(loop, watcher);\n"
        "}\n"
        "inline void\n"
        "stop_unless_loop_gone(struct ev_loop *loop, ev_io *watcher) noexcept {\n"
        "    if (!loop_is_gone(loop)) [[likely]]\n"
        "        ev_io_stop(loop, watcher);\n"
        "}\n"
    )
sched.sub("CoroutineScheduler &bind_thread_scheduler();\n", record)

# ---- the destructor ---------------------------------------------------------------------------------------------------
sched.sub(
    "     * Drains and destroys all coroutines in the ready queue only. Coroutines\n"
    "     * that are suspended (waiting on I/O or timers) are NOT destroyed, because\n"
    "     * their libev watchers are still active; destroying those handles would\n"
    "     * run awaiter destructors that call `ev_*_stop()`, which can cause\n"
    "     * use-after-free or double-free if the loop is being torn down or if\n"
    "     * callbacks are in flight.\n"
    "     *\n"
    "     * Finding 2.B.8 — decision log:\n"
    "     *   Destroying suspended frames here would be *worse* than leaking\n"
    "     *   them because:\n"
    "     *     1. libev watchers registered in the awaiter still reference the\n"
    "     *        frame via `ev_watcher::data`. If the loop iterates once more\n"
    "     *        (e.g. another listener on this thread), it would dispatch to\n"
    "     *        freed memory.\n"
    "     *     2. The frame's awaiter may hold non-trivial RAII (cancellation\n"
    "     *        tokens, shared_ptrs) whose destruction order is not the one\n"
    "     *        intended by the user when they wrote their coroutine.\n"
    "     *   The correct lifecycle is therefore: **stop the event loop first**,\n"
    "     *   then destroy the scheduler. The listener does this on its own\n"
    "     *   destruction. Suspended frames at process exit are reclaimed by\n"
    "     *   the OS (they are by definition not going to make progress).\n"
    "     *\n"
    "     *   Suspended frames left at teardown are REPORTED, in every build: one\n"
    "     *   line on `qb::io::cerr` naming the count, and the count added to\n"
    "     *   `abandoned_coroutine_frames_total()` (Huly QB-84). It used to be a\n"
    "     *   debug-only `fprintf`, so a release service that recreated listeners\n"
    "     *   accumulated frames with no message and no number.\n"
    "     */\n",
    "     * Destroys every coroutine it owns -- the spawned roots, each with its chain down to the awaiter parked on a\n"
    "     * watcher, whose destructor stops it -- then the frames queued and the frames completed, as `recycle()` does,\n"
    "     * and again while a destructor it ran left it a coroutine (one that spawned on it and pumped it). A frame it\n"
    "     * does not own (a task driven by hand) is its task's.\n"
    "     *\n"
    "     * Three orders are refused, and then it destroys NOTHING -- parked, queued and completed frames alike: the roots\n"
    "     * it owns are counted in `abandoned_coroutine_frames_total()` and one line on `qb::io::cerr` says which order\n"
    "     * it was (Huly QB-84):\n"
    "     *   - destroyed AFTER its thread's `listener::current` (static storage, a thread_local built before the\n"
    "     *     listener): the loop its coroutines waited on is gone, and no destructor of a coroutine local is run\n"
    "     *     against a thread that can run nothing any more. Destroy a scheduler before its thread's listener.\n"
    "     *   - destroyed while its own `run_ready()` is on the stack -- from inside one of its coroutines, which is\n"
    "     *     what `exit()` in a coroutine body does to the thread's listener: one of them is executing, and which\n"
    "     *     root it belongs to is not known here (the handle a drain resumes is a leaf, not its root).\n"
    "     *   - destroyed on ANOTHER THREAD than the one that built it (static storage, and `exit()` called on a thread\n"
    "     *     that is not the scheduler's): its coroutines wait on their own thread's loop, which may be running.\n"
    "     * A fourth report exists for a scheduler whose coroutines' destructors spawn on it again at every pass.\n"
    "     *\n"
    "     * (Finding 2.B.8 abandoned in every order until 3.4. Its reason was the watchers, which a frame's destruction\n"
    "     * stops; what it protected against was the first order above alone.)\n"
    "     */\n",
)

dtor_body = "        QB_SCHED_TRACE(\"~CoroutineScheduler() begin this=%p\", (void *) this);\n"
if "M3" in M:
    dtor_body += (
        "        if (const std::size_t suspended = suspended_coroutines_.size(); suspended != 0) // MUTANT M3\n"
        "            report_abandoned_coroutine_frames(suspended, this, coroutine_abandon_reason::loop_gone);\n"
    )
if "M1" in M:
    dtor_body += (
        "        coroutine_abandon_reason why = coroutine_abandon_reason::respawning;\n"
        "        if (must_abandon(why)) [[unlikely]]\n"
        "            abandon_everything(why);\n"
        "        else {\n"
        "            destroy_ready_and_deferred(); // MUTANT M1: the parked roots are not destroyed\n"
        "            owned_frames_.clear();\n"
        "        }\n"
    )
else:
    dtor_body += (
        "        coroutine_abandon_reason why = coroutine_abandon_reason::respawning; // what is left after the last pass\n"
        "        if (must_abandon(why) || !destroy_everything_owned()) [[unlikely]]\n"
        "            abandon_everything(why);\n"
    )
dtor_body += "        detail::track_live_scheduler(this, false); // out of the live ones, and unbound if current (Huly QB-1220)\n"
sched.sub(
    "        QB_SCHED_TRACE(\"~CoroutineScheduler() begin this=%p\", (void *) this);\n"
    "        drain_for_teardown();\n"
    "        detail::track_live_scheduler(this, false); // out of the live ones, and unbound if current (Huly QB-1220)\n",
    dtor_body,
)

# ---- recycle() and the teardown helpers --------------------------------------------------------------------------------
gone_test = "false /* MUTANT M2 */" if "M2" in M else "detail::thread_loop_gone()"
drain_test = "false /* MUTANT N5 */" if "N5" in M else "in_run_ready_"
if args.inline_marker and "N5" not in M:
    drain_test = "in_run_ready_ || detail::thread_loop().inline_resumes != 0"
foreign_test = "false /* MUTANT N10 */" if "N10" in M else "!detail::built_on_this_thread(this)"
if "N1" in M:
    passes_loop = (
        "        destroy_all_suspended(); // MUTANT N1: one pass\n"
        "        destroy_ready_and_deferred();\n"
        "        return owned_frames_.empty();\n"
    )
elif "N6" in M:
    passes_loop = (
        "        for (;;) { // MUTANT N6: unbounded\n"
        "            destroy_all_suspended();\n"
        "            destroy_ready_and_deferred();\n"
        "            if (owned_frames_.empty())\n"
        "                return true;\n"
        "        }\n"
    )
else:
    passes_loop = (
        "        for (unsigned pass = 0; pass < max_teardown_passes; ++pass) {\n"
        "            destroy_all_suspended();\n"
        "            destroy_ready_and_deferred();\n"
        "            if (owned_frames_.empty()) [[likely]]\n"
        "                return true;\n"
        "        }\n"
        "        return false;\n"
    )
if "N2" in M:
    abandon_queues = (
        "        destroy_ready_and_deferred(); // MUTANT N2: the queued and the completed frames are destroyed all the same\n"
    )
else:
    abandon_queues = (
        "        ready_queue_.clear();       // queued, not resumed: dropped, never destroyed\n"
        "        frames_to_destroy_.clear(); // completed, at their final suspension: their parameters are not destroyed either\n"
    )
abandon_count = "owned_frames_.size()"

sched.sub(
    "    void\n"
    "    recycle() {\n"
    "        destroy_all_suspended();\n"
    "        drain_for_teardown();\n"
    "    }\n"
    "\n"
    "private:\n"
    "    // The destructor's drains: owned ready frames and deferred-destroy frames are destroyed, a\n"
    "    // suspended frame is reported (Huly QB-84), the bookkeeping is cleared.\n"
    "    void\n"
    "    drain_for_teardown() {\n",
    "    void\n"
    "    recycle() {\n"
    "        coroutine_abandon_reason why{};\n"
    "        if (!must_abandon(why)) [[likely]]\n"
    "            (void) destroy_everything_owned(); // what pass after pass of respawning destructors leaves stays owned\n"
    "    }\n"
    "\n"
    "private:\n"
    "    // Sixteen passes: one is the rule, two is a destructor that spawned on the dying scheduler and pumped it, and a\n"
    "    // destructor that does it at every pass would otherwise keep this one running for ever.\n"
    "    static constexpr unsigned max_teardown_passes = 16;\n"
    "\n"
    "    // The two orders in which this scheduler must not destroy the coroutines it owns (see the destructor); `why`\n"
    "    // is written only when it answers true.\n"
    "    [[nodiscard]] bool\n"
    "    must_abandon(coroutine_abandon_reason &why) const noexcept {\n"
    "        if (" + foreign_test + ") [[unlikely]] { // not this thread's: nothing of it is read, its frames are another loop's\n"
    "            why = coroutine_abandon_reason::foreign_thread;\n"
    "            return true;\n"
    "        }\n"
    "        if (" + drain_test + ") [[unlikely]] { // one of its own coroutines is executing, under this drain\n"
    "            why = coroutine_abandon_reason::draining;\n"
    "            return true;\n"
    "        }\n"
    "        if (" + gone_test + ") [[unlikely]] {\n"
    "            why = coroutine_abandon_reason::loop_gone;\n"
    "            return true;\n"
    "        }\n"
    "        return false;\n"
    "    }\n"
    "\n"
    "    // Destroy every coroutine it owns: the parked roots, then the queued and the completed ones -- and again while\n"
    "    // a destructor those passes ran left it one (it spawned on this scheduler and pumped it, so the new coroutine\n"
    "    // parked: outside the snapshot of the pass that was running). False when it still owns some after the last pass.\n"
    "    [[nodiscard]] bool\n"
    "    destroy_everything_owned() {\n" + passes_loop + "    }\n"
    "\n"
    "    // Drop everything it holds and destroy nothing: the refused orders of the destructor. What it owned is counted\n"
    "    // and reported, a root once whatever it awaits; a frame it does not own (registered by a task driven by hand)\n"
    "    // is its task's and is not.\n"
    "    void\n"
    "    abandon_everything(coroutine_abandon_reason why) noexcept {\n"
    "        const std::size_t abandoned = " + abandon_count + ";\n" + abandon_queues + "        suspended_coroutines_.clear();\n"
    "        in_flight_.clear();\n"
    "        owned_frames_.clear();\n"
    "        if (abandoned != 0)\n"
    "            report_abandoned_coroutine_frames(abandoned, this, why);\n"
    "    }\n"
    "\n"
    "    // The queued and the completed frames it owns are destroyed, the bookkeeping of the others is cleared.\n"
    "    void\n"
    "    destroy_ready_and_deferred() {\n",
)
sched.sub(
    "        if (const std::size_t abandoned = suspended_coroutines_.size(); abandoned != 0)\n"
    "            report_abandoned_coroutine_frames(abandoned, this);\n"
    "        QB_SCHED_TRACE(\"  clearing suspended_coroutines_ (count=%zu), NOT destroying handles\", suspended_coroutines_.size());\n"
    "        suspended_coroutines_.clear();\n"
    "        in_flight_.clear();\n"
    "        // Any owned frames still here are suspended (not in the ready queue) and are intentionally leaked at\n"
    "        // teardown per Finding 2.B.8 — just drop the bookkeeping; the OS reclaims them at process/thread exit.\n"
    "        owned_frames_.clear();\n"
    "        detail::destroying_scheduler() = was;\n",
    "        // A frame still registered as suspended is not this scheduler's (destroy_all_suspended() ran first): its task's.\n"
    "        suspended_coroutines_.clear();\n"
    "        in_flight_.clear();\n"
    "        detail::destroying_scheduler() = was;\n",
)

# ---- destroy_all_suspended(): never under this scheduler's own drain --------------------------------------------------
if args.f6_by_handle:
    # ALTERNATIVE, refuted by cv_w exit_in_nested: exclude the handle run_ready() resumed, and only it.
    sched.sub(
        "                QB_SCHED_TRACE(\"run_ready resume handle=%p owned=%d\", (void *) handle.address(), (int) item.owned);\n"
        "                handle.resume();\n",
        "                QB_SCHED_TRACE(\"run_ready resume handle=%p owned=%d\", (void *) handle.address(), (int) item.owned);\n"
        "                running_ = handle.address();\n"
        "                handle.resume();\n"
        "                running_ = nullptr;\n",
    )
    sched.sub(
        "            auto handle = std::coroutine_handle<>::from_address(addr);\n"
        "            if (handle && (deferred || !handle.done())) {\n",
        "            auto handle = std::coroutine_handle<>::from_address(addr);\n"
        "            if (addr == running_)\n"
        "                continue; // ALTERNATIVE: the frame run_ready() resumed is executing\n"
        "            if (handle && (deferred || !handle.done())) {\n",
    )
    sched.sub("    bool in_run_ready_{false};\n", "    bool in_run_ready_{false};\n    void *running_ = nullptr;\n")
    sched.sub("        if (" + drain_test + ") [[unlikely]] { // one of its own coroutines is executing, under this drain\n",
              "        if (false) [[unlikely]] { // ALTERNATIVE: no scheduler-wide exclusion\n")
else:
    sched.sub(
        "        if (owned_frames_.empty() && suspended_coroutines_.empty())\n"
        "            return;\n"
        "        std::vector<void *> owned_roots;\n",
        "        if (owned_frames_.empty() && suspended_coroutines_.empty())\n"
        "            return;\n"
        "        // Never under this scheduler's own drain: one of its coroutines is executing -- `exit()` in a coroutine body\n"
        "        // ends the thread's listener from inside it -- and destroying that one runs its awaiters' destructors a\n"
        "        // second time. Which root the executing frame belongs to is not known here (a drain resumes the leaf a\n"
        "        // waker queued, never its root), so nothing is destroyed; the destructor then reports what is left.\n"
        "        if (" + drain_test + " || " + foreign_test + ") [[unlikely]]\n"
        "            return;\n"
        "        std::vector<void *> owned_roots;\n",
    )
sched.sub(
    "     *        spawned) is left to its task<T>, which destroys it once (Huly QB-1220). Call this before reset/destroy\n"
    "     *        when the event loop is still valid (e.g. test TearDown). Must not be used in ~CoroutineScheduler()\n"
    "     *        at process exit (loop may be tearing down); use only when explicitly resetting the scheduler.\n",
    "     *        spawned) is left to its task<T>, which destroys it once (Huly QB-1220). The destructor runs it while\n"
    "     *        the thread's loop lives. A no-op while this scheduler is resuming a coroutine (`is_draining_ready()`).\n",
)

# ---- built on this thread? the live table answers, exactly ------------------------------------------------------------
sched.sub(
    "QB_ABI_ANCHOR inline bool                 is_live_scheduler(CoroutineScheduler const *scheduler) noexcept;\n",
    "QB_ABI_ANCHOR inline bool                 is_live_scheduler(CoroutineScheduler const *scheduler) noexcept;\n"
    "QB_ABI_ANCHOR inline bool                 built_on_this_thread(CoroutineScheduler const *scheduler) noexcept;\n",
)
sched.sub(
    "struct live_schedulers_t {\n"
    "    CoroutineScheduler *slot[16];\n"
    "    unsigned            count;\n"
    "};\n",
    "struct live_schedulers_t {\n"
    "    CoroutineScheduler *slot[16];\n"
    "    unsigned            count;\n"
    "    bool                overflowed; ///< a scheduler built here found no slot: `built_on_this_thread()` cannot say no\n"
    "};\n",
)
sched.sub(
    "    if (alive && live.count < std::size(live.slot))\n"
    "        live.slot[live.count++] = scheduler;\n"
    "}\n",
    "    if (alive && live.count < std::size(live.slot))\n"
    "        live.slot[live.count++] = scheduler;\n"
    "    else if (alive)\n"
    "        live.overflowed = true;\n"
    "}\n"
    "\n"
    "// Whether `scheduler` was built on the calling thread: it is in this thread's table from its constructor to the end\n"
    "// of its destructor. A scheduler destroyed on another thread -- static storage, and `exit()` on a thread that is not\n"
    "// its own -- is not, and its coroutines are that other thread's: their watchers are on its loop, which may be\n"
    "// running. A thread that ever had more schedulers than slots answers yes for all of them (the table cannot tell).\n"
    "QB_ABI_ANCHOR inline bool\n"
    "built_on_this_thread(CoroutineScheduler const *scheduler) noexcept {\n"
    "    live_schedulers_t const &live = live_schedulers();\n"
    "    if (live.overflowed)\n"
    "        return true;\n"
    "    for (unsigned i = 0; i < live.count; ++i)\n"
    "        if (live.slot[i] == scheduler)\n"
    "            return true;\n"
    "    return false;\n"
    "}\n",
)

# ---- the wake, and a completed frame's hand-over, with nowhere to go ---------------------------------------------------
if "M4" not in M:
    sched.sub(
        "    if (!sched) [[unlikely]]\n"
        "        sched = &detail::bind_thread_scheduler(); // first coroutine on this thread: the loop's scheduler\n"
        "    sched->schedule_resume(handle);\n",
        "    if (!sched) [[unlikely]] {\n"
        "        if (detail::thread_loop_gone())\n"
        "            return; // the thread's loop is destroyed and nothing will ever resume this frame: the wake is dropped\n"
        "        sched = &detail::bind_thread_scheduler(); // first coroutine on this thread: the loop's scheduler\n"
        "    }\n"
        "    sched->schedule_resume(handle);\n",
    )
# ---- the completion door of the modules: the same wake, not noexcept ------------------------------------------------
sched.sub(
    "inline void\n"
    "enqueue_for_later_via_current(std::coroutine_handle<> handle) noexcept {\n",
    "/**\n"
    " * @brief A completion's wake, delivered from a reply handler (the qbm modules'): `handle` is queued on the current\n"
    " *        scheduler, the thread's being bound if none is, and dropped once the thread's loop is gone --\n"
    " *        `schedule_via_current()`'s rule, read at the same record.\n"
    " * @details Not `noexcept`, on purpose: queuing can allocate, and a reply handler is not a destructor -- an\n"
    " *          allocation failure propagates to the caller's reply path, as it did when the modules called\n"
    " *          `coro_scheduler().schedule_resume()`. (Measured too: with a `noexcept` wake in its lambda, g++ 14 at -O3\n"
    " *          reports -Wmaybe-uninitialized in qbm-redis's `TReply::fail` for `hscan<std::map>`, an error under -Werror.)\n"
    " */\n"
    "inline void\n"
    "resume_via_current(std::coroutine_handle<> handle) {\n"
    "    auto *sched = CoroutineScheduler::current_ptr();\n"
    "    if (!sched) [[unlikely]] {\n"
    "        if (detail::thread_loop_gone())\n"
    "            return; // delivered after the thread's loop: nothing will ever resume this frame\n"
    "        sched = &detail::bind_thread_scheduler();\n"
    "    }\n"
    "    sched->schedule_resume(handle);\n"
    "}\n"
    "\n"
    "inline void\n"
    "enqueue_for_later_via_current(std::coroutine_handle<> handle) noexcept {\n",
)

if "N8" not in M:
    sched.sub(
        "inline void\n"
        "defer_frame_destruction(std::coroutine_handle<> handle) noexcept {\n"
        "    CoroutineScheduler::current().defer_destroy(handle);\n"
        "}\n",
        "inline void\n"
        "defer_frame_destruction(std::coroutine_handle<> handle) noexcept {\n"
        "    auto *sched = CoroutineScheduler::current_ptr();\n"
        "    if (!sched) [[unlikely]] {\n"
        "        if (detail::thread_loop_gone())\n"
        "            return; // finished after the thread's loop: the frame stays with the scheduler that owns it, which counts it\n"
        "        sched = &detail::bind_thread_scheduler();\n"
        "    }\n"
        "    sched->defer_destroy(handle);\n"
        "}\n",
    )
sched.save()

# ======================================================================================================================
# listener.cpp -- the first door, and the one line
# ======================================================================================================================
lcpp = File("src/qb/io/async/listener.cpp")
bind_guard = (
    "    // Never reach a listener whose destructor has run. A wake and a completed frame's hand-over are dropped before\n"
    "    // they get here; what is left is coroutine code RUNNING on a thread whose loop is destroyed, asking to suspend.\n"
    "    if (detail::thread_loop_gone()) [[unlikely]]\n"
    "        detail::refuse_after_thread_loop(\"its thread's scheduler\");\n"
)
lcpp.sub(
    "CoroutineScheduler &\n"
    "detail::bind_thread_scheduler() {\n",
    "void\n"
    "detail::refuse_after_thread_loop(char const *const asked) noexcept {\n"
    "    // stderr, not qb::io::cerr(): this runs among the destructors of objects with static storage.\n"
    "    std::fputs(\"qb: a coroutine asked for \", stderr);\n"
    "    std::fputs(asked, stderr);\n"
    "    std::fputs(\" after that thread's event loop (listener::current) was destroyed -- no coroutine can run there any \"\n"
    "               \"more\\n\",\n"
    "               stderr);\n"
    "    std::fflush(stderr);\n"
    "    std::terminate();\n"
    "}\n"
    "\n"
    "CoroutineScheduler &\n"
    "detail::bind_thread_scheduler() {\n" + ("" if "M5" in M else bind_guard),
)
lcpp.sub("#include <qb/io/async/listener.h>\n", "#include <cstdio>\n#include <exception>\n#include <qb/io/async/listener.h>\n")
lcpp.save()

# ======================================================================================================================
# listener.h
# ======================================================================================================================
lis = File("src/qb/io/async/listener.h")

# ---- the lane guard's includes ----------------------------------------------------------------------------------------
lis.sub(
    "#include \"event/base.h\"\n"
    "#include \"coroutine/scheduler.h\"\n"
    "#include <cstdio>\n"
    "#include <cstdlib>\n"
    "#include <cstring>\n",
    "#include \"event/base.h\"\n"
    "#include \"coroutine/scheduler.h\"\n"
    "#include <cstdio>\n"
    "#include <cstdlib>\n"
    "#include <cstring>\n"
    "\n"
    "// The sanitizer's poison call, in an AddressSanitizer build only -- detected as thread_arena.h does it.\n"
    "#if defined(__SANITIZE_ADDRESS__)\n"
    "#define QB_LISTENER_ASAN 1\n"
    "#elif defined(__has_feature)\n"
    "#if __has_feature(address_sanitizer)\n"
    "#define QB_LISTENER_ASAN 1\n"
    "#endif\n"
    "#endif\n"
    "#ifdef QB_LISTENER_ASAN\n"
    "#include <sanitizer/asan_interface.h>\n"
    "#endif\n",
)

# ---- the tagged constructor -------------------------------------------------------------------------------------------
enter = (
    "        detail::thread_loop_t &state = detail::thread_loop();\n"
    "        state.owner                  = this;\n"
)
if V == "G" or args.loop_record:
    enter += "        state.raw                    = static_cast<struct ev_loop *>(_loop);\n"
enter += "        state.phase                  = detail::thread_loop_phase::alive;\n"
tag_ctor = (
    "private:\n"
    "    // The constructor of `listener::current`, and of nothing else: it enters the thread's record. A temporary\n"
    "    // listener never does -- the record is about the loop the thread's coroutines wait on.\n"
    "    struct thread_current_tag {};\n"
    "    explicit listener(thread_current_tag)\n"
    "        : listener() {\n" + enter + "    }\n"
    "\n"
)
lis.sub(
    "public:\n"
    "    /**\n"
    "     * @brief Constructor\n"
    "     *\n"
    "     * Creates a new listener with a dynamic event loop.",
    tag_ctor + "public:\n"
    "    /**\n"
    "     * @brief Constructor\n"
    "     *\n"
    "     * Creates a new listener with a dynamic event loop.",
)

# ---- the destructor leaves the record ---------------------------------------------------------------------------------
leave = (
    "        // From here the thread's loop is gone: the frames this listener owned were destroyed above, while it lived;\n"
    "        // `_loop` goes after this body, with the members. `owner` stays, to be compared.\n"
    "        if (detail::thread_loop_t &state = detail::thread_loop(); state.owner == this)\n"
    + ("            {\n                state.raw   = nullptr;\n                state.phase = detail::thread_loop_phase::gone;\n            }\n" if args.loop_record else
       "            state.phase = detail::thread_loop_phase::gone;\n")
)
lis.sub(
    "        QB_LISTENER_TRACE(\"~listener() about to reset _coro_scheduler\");\n"
    "        reset_coro_scheduler();\n"
    "    }\n",
    "        QB_LISTENER_TRACE(\"~listener() about to reset _coro_scheduler\");\n"
    "        reset_coro_scheduler();\n" + ("" if "M7" in M else leave) + "    }\n",
)

# ---- listener::release() ----------------------------------------------------------------------------------------------
release_body = (
    "        if (kevent->_detached_by_clear) // on no listener's list any more: freed as unregisterEvent() frees it,\n"
    "            kevent->_detached_by_clear = false; // and no listener is reached -- its own may be gone\n"
    "        else if (!current._unlink(kevent))\n"
    "            return; // not registered here (a second release)\n"
    "        delete kevent;\n"
)
if "M9" in M:
    release_body = "        current.unregisterEvent(kevent); // MUTANT M9\n"
lis.sub(
    "        if (_unlink(kevent) || kevent->_detached_by_clear) {\n"
    "            kevent->_detached_by_clear = false;\n"
    "            delete kevent;\n"
    "        }\n"
    "    }\n",
    "        if (_unlink(kevent) || kevent->_detached_by_clear) {\n"
    "            kevent->_detached_by_clear = false;\n"
    "            delete kevent;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    /**\n"
    "     * @brief Release the registration an io object holds: the one entry point of every owner's teardown.\n"
    "     * @details A wrapper `clear()` detached is freed WITHOUT reaching a listener: its owner may be destroyed after\n"
    "     *          the listener that held it (an io object with static storage, after the main thread's\n"
    "     *          `listener::current`), and `clear()` already took it off the list. Any other is unregistered from the\n"
    "     *          thread's listener, as `listener::current.unregisterEvent()` does.\n"
    "     */\n"
    "    static void\n"
    "    release(IRegisteredKernelEvent *kevent) {\n"
    "        if (!kevent)\n"
    "            return;\n" + release_body +
    "    }\n",
)

# ---- the cleanup accessor: no door, no creation ------------------------------------------------------------------------
lis.sub(
    "    has_coro_scheduler() const {\n"
    "        return _coro_scheduler != nullptr;\n"
    "    }\n",
    "    has_coro_scheduler() const {\n"
    "        return _coro_scheduler != nullptr;\n"
    "    }\n"
    "\n"
    "    /**\n"
    "     * @brief The thread's scheduler for a CLEANUP -- reclaiming a helper frame it owns, scrubbing a handle from its\n"
    "     *        queues -- or null when there is nothing to clean: the thread's loop is gone (its listener destroyed or\n"
    "     *        abandoned every frame it owned, and is not reached), or the listener never had a scheduler.\n"
    "     * @details What a destructor and a cancellation hook use instead of `coro_scheduler()`, which creates a scheduler\n"
    "     *          and stops the process once the loop is gone: a cleanup after the loop is dropped, never refused.\n"
    "     */\n"
    "    [[nodiscard]] static CoroutineScheduler *\n"
    "    cleanup_scheduler() noexcept {\n"
    "        if (detail::thread_loop_gone()) [[unlikely]]\n"
    "            return nullptr;\n"
    "        return current._coro_scheduler.get();\n"
    "    }\n",
)

# ---- the second door --------------------------------------------------------------------------------------------------
door2 = (
    "        // The second door to the thread's scheduler (the first is detail::bind_thread_scheduler()): never on a\n"
    "        // listener whose destructor has run. Read before any member: the record is all that may be read then.\n"
    "        if (detail::thread_loop_gone()) [[unlikely]]\n"
    "            if (detail::is_gone_thread_listener(this))\n"
    "                detail::refuse_after_thread_loop(\"its thread's scheduler (coro_scheduler())\");\n"
)
if args.door2 == "cold":
    lis.sub(
        "    coro_scheduler() {\n"
        "        if (!_coro_scheduler) {\n",
        "    coro_scheduler() {\n"
        "        if (!_coro_scheduler) {\n"
        "            // ALTERNATIVE (--door2 cold): the record is read only here, where a scheduler would be created. After its\n"
        "            // destructor the listener's `_coro_scheduler` is null (reset_coro_scheduler() ran), so a late call lands here.\n"
        "            if (detail::is_gone_thread_listener(this)) [[unlikely]]\n"
        "                detail::refuse_after_thread_loop(\"its thread's scheduler (coro_scheduler())\");\n",
    )
else:
    lis.sub(
        "    coro_scheduler() {\n"
        "        if (!_coro_scheduler) {\n",
        "    coro_scheduler() {\n" + ("" if "N3" in M else door2) + "        if (!_coro_scheduler) {\n",
    )
if args.door2 == "record":
    # ALTERNATIVE (--door2 record): the record also names the thread's scheduler, and the free coro_scheduler() reads it there
    lis.sub(
        "            _coro_scheduler = std::make_unique<CoroutineScheduler>();\n",
        "            _coro_scheduler = std::make_unique<CoroutineScheduler>();\n"
        "            if (detail::thread_loop_t &state = detail::thread_loop(); state.owner == this)\n"
        "                state.scheduler = _coro_scheduler.get();\n",
    )
    lis.sub(
        "        if (CoroutineScheduler::current_ptr() == _coro_scheduler.get())\n"
        "            CoroutineScheduler::set_current(nullptr);\n"
        "        _coro_scheduler.reset();\n",
        "        if (CoroutineScheduler::current_ptr() == _coro_scheduler.get())\n"
        "            CoroutineScheduler::set_current(nullptr);\n"
        "        if (detail::thread_loop_t &state = detail::thread_loop(); state.owner == this)\n"
        "            state.scheduler = nullptr;\n"
        "        _coro_scheduler.reset();\n",
    )
if args.loop_door:
    lis.sub(
        "    loop() const {\n"
        "        return _loop;\n",
        "    loop() const {\n"
        "        // The third door: a request for the thread's loop, to arm a watcher on it.\n"
        "        if (detail::thread_loop_gone()) [[unlikely]]\n"
        "            if (detail::is_gone_thread_listener(this))\n"
        "                detail::refuse_after_thread_loop(\"its thread's event loop (listener::current.loop())\");\n"
        "        return _loop;\n",
    )
if args.trace:
    lis.sub(
        "    explicit listener(thread_current_tag)\n"
        "        : listener() {\n",
        "    explicit listener(thread_current_tag)\n"
        "        : listener() {\n"
        "        if (std::getenv(\"CV_BACKEND_TRACE\")) // SCRATCH instrumentation, not part of the design\n"
        "            std::fprintf(stderr, \"[cv] listener::current came up on backend %s\\n\", backend_name(backend()));\n",
    )

# ---- the definition, and the poisoner ----------------------------------------------------------------------------------
poison_call = "(void) 0; /* MUTANT M10 */" if "M10" in M else "__asan_poison_memory_region(&listener::current, sizeof(listener));"
lis.sub(
    "QB_ABI_ANCHOR inline thread_local listener listener::current = {};\n",
    "namespace detail {\n"
    "#ifdef QB_LISTENER_ASAN\n"
    "/// The sanitize lane's guard: once its destructor has completed, the thread's `listener::current` is POISONED, so a\n"
    "/// destructor that reaches it afterwards -- an object with static storage, a thread_local built before it -- is a\n"
    "/// use-after-poison instead of a read of a destroyed object nothing reports. No data member (the layout of\n"
    "/// `listener` is the same in a sanitized and an unsanitized translation unit): a function-local thread_local built\n"
    "/// BEFORE the listener, in the initialiser of `listener::current`, so that it is destroyed AFTER it.\n"
    "struct listener_poisoner {\n"
    "    listener_poisoner() noexcept {}\n"
    "    ~listener_poisoner() noexcept;\n"
    "};\n"
    "inline void\n"
    "arm_listener_poisoner() noexcept {\n"
    "    thread_local listener_poisoner poisoner;\n"
    "    (void) poisoner;\n"
    "}\n"
    "#else\n"
    "inline void\n"
    "arm_listener_poisoner() noexcept {}\n"
    "#endif\n"
    "} // namespace detail\n"
    "\n"
    "QB_ABI_ANCHOR inline thread_local listener listener::current{(detail::arm_listener_poisoner(), listener::thread_current_tag{})};\n"
    "\n"
    "#ifdef QB_LISTENER_ASAN\n"
    "inline detail::listener_poisoner::~listener_poisoner() noexcept {\n"
    "    " + poison_call + "\n"
    "}\n"
    "#endif\n",
)
if "M8" in M:
    # MUTANT M8: every listener enters the record -- the public constructor does what the tagged one does
    lis.sub(
        "        _park_cap.set<listener, &listener::_on_park_cap>(this);\n"
        "    }\n",
        "        _park_cap.set<listener, &listener::_on_park_cap>(this);\n" + enter + "    }\n",
    )
lis.save()

# ======================================================================================================================
# coroutine/utils.h -- the cleanup accessor
# ======================================================================================================================
ut = File("src/qb/io/async/coroutine/utils.h")
ut.sub(
    "inline CoroutineScheduler &\n"
    "coro_scheduler() {\n"
    "    // Always use the listener's scheduler to ensure consistency\n"
    "    // This creates the scheduler on first access and sets it as current\n"
    "    return listener::current.coro_scheduler();\n"
    "}\n",
    "inline CoroutineScheduler &\n"
    "coro_scheduler() {\n"
    "    // Always use the listener's scheduler to ensure consistency\n"
    "    // This creates the scheduler on first access and sets it as current\n"
    "    return listener::current.coro_scheduler();\n"
    "}\n"
    "\n"
    "namespace detail {\n"
    "/// The thread's scheduler for a CLEANUP, or null (`listener::cleanup_scheduler()`): what a destructor and a\n"
    "/// cancellation hook use instead of `coro_scheduler()`.\n"
    "[[nodiscard]] inline CoroutineScheduler *\n"
    "cleanup_scheduler() noexcept {\n"
    "    return listener::cleanup_scheduler();\n"
    "}\n"
    "/// CLEANUP: destroy a helper frame the thread's scheduler owns (`cancel_spawned()`); dropped after the loop.\n"
    "inline void\n"
    "cancel_spawned_helper(std::coroutine_handle<> handle) noexcept {\n"
    "    if (CoroutineScheduler *const sched = cleanup_scheduler())\n"
    "        sched->cancel_spawned(handle);\n"
    "}\n"
    "/// CLEANUP: scrub a frame its task is about to destroy from the thread's scheduler (`forget()`); dropped after the\n"
    "/// loop (the task's destructor scrubs every live scheduler by itself).\n"
    "inline void\n"
    "forget_on_thread_scheduler(std::coroutine_handle<> handle) noexcept {\n"
    "    if (CoroutineScheduler *const sched = cleanup_scheduler())\n"
    "        sched->forget(handle);\n"
    "}\n"
    "} // namespace detail\n",
)
if args.door2 == "record":
    ut.sub(
        "    // Always use the listener's scheduler to ensure consistency\n"
        "    // This creates the scheduler on first access and sets it as current\n"
        "    return listener::current.coro_scheduler();\n",
        "    // ALTERNATIVE (--door2 record): the thread's record names the listener's scheduler while it has one; the\n"
        "    // listener is asked only to create it -- or to refuse, once the thread's loop is gone.\n"
        "    if (CoroutineScheduler *const sched = detail::thread_loop().scheduler) [[likely]]\n"
        "        return *sched;\n"
        "    return listener::current.coro_scheduler();\n",
    )
if args.loop_record:
    ut.sub(
        "inline timer_awaiter\n"
        "sleep(qb::duration duration) {\n",
        "namespace detail {\n"
        "/// ALTERNATIVE (--loop-record). The thread's loop, to arm a watcher on: read from the thread's record while its\n"
        "/// listener lives -- no thread_local with a destructor on the way; otherwise the listener is asked, which builds\n"
        "/// it on a thread that has none yet, and stops the process by name once it is gone (`listener::loop()`).\n"
        "[[nodiscard]] inline ev::loop_ref\n"
        "thread_listener_loop() {\n"
        "    if (struct ev_loop *const raw = thread_loop().raw) [[likely]]\n"
        "        return ev::loop_ref(raw);\n"
        "    return listener::current.loop();\n"
        "}\n"
        "} // namespace detail\n"
        "\n"
        "inline timer_awaiter\n"
        "sleep(qb::duration duration) {\n",
    )
    ut.sub("listener::current.loop()};", "detail::thread_listener_loop()};", 7)
    ch2 = File("src/qb/io/async/coroutine/channel.h")
    ch2.sub(", loop(listener::current.loop())", ", loop(::qb::io::async::detail::thread_listener_loop())", 2)
    ch2.save()
    ac = File("src/qb/core/Actor.cpp")
    ac.sub(
        "    return qb::io::async::listener::current.loop();\n",
        "    return qb::io::async::detail::thread_listener_loop(); // the record, not the thread_local: see coroutine/utils.h\n",
    )
    ac.save()
    co2 = File("src/qb/io/async/coroutine/combinators.h")
    co2.sub("= listener::current.loop();", "= detail::thread_listener_loop();", 2)
    co2.save()
ut.sub(
    "        if (auto h = std::exchange(*_slot, {}); h && listener::current.has_coro_scheduler())\n"
    "            coro_scheduler().cancel_spawned(h);\n",
    "        if (auto h = std::exchange(*_slot, {}))\n"
    "            detail::cancel_spawned_helper(h);\n",
)
ut.save()

# ======================================================================================================================
# the cleanup sites: cancellation.h, combinators.h, scope.h
# ======================================================================================================================
ca = File("src/qb/io/async/coroutine/cancellation.h")
ca.sub("coro_scheduler().cancel_spawned(", "detail::cancel_spawned_helper(", 8)
ca.sub("coro_scheduler().forget(", "detail::forget_on_thread_scheduler(", 4)
ca.save()

sc = File("src/qb/io/async/coroutine/scope.h")
sc.sub("coro_scheduler().cancel_spawned(", "detail::cancel_spawned_helper(", 1)
sc.save()

co = File("src/qb/io/async/coroutine/combinators.h")
co.sub(
    "        auto &sched = coro_scheduler();\n"
    "        sched.cancel_spawned(std::exchange(st.branch_handles[I], {}));\n"
    "        if (auto inner = std::get<I>(st.tasks).handle())\n"
    "            sched.forget(inner);\n",
    "        const auto branch = std::exchange(st.branch_handles[I], {});\n"
    "        if (CoroutineScheduler *const sched = detail::cleanup_scheduler()) { // null after the thread's loop: dropped\n"
    "            sched->cancel_spawned(branch);\n"
    "            if (auto inner = std::get<I>(st.tasks).handle())\n"
    "                sched->forget(inner);\n"
    "        }\n",
    2,
)
co.sub(
    "        auto &sched = coro_scheduler();\n"
    "        sched.cancel_spawned(std::exchange(st.branch_handles[j], {}));\n"
    "        if (auto inner = st.tasks[j].handle())\n"
    "            sched.forget(inner);\n",
    "        const auto branch = std::exchange(st.branch_handles[j], {});\n"
    "        if (CoroutineScheduler *const sched = detail::cleanup_scheduler()) { // null after the thread's loop: dropped\n"
    "            sched->cancel_spawned(branch);\n"
    "            if (auto inner = st.tasks[j].handle())\n"
    "                sched->forget(inner);\n"
    "        }\n",
    2,
)
if "N7" not in M:
    co.sub(
        "            if (_state->runner_handle)\n"
        "                coro_scheduler().cancel_spawned(std::exchange(_state->runner_handle, {}));\n"
        "            if (auto ih = _state->inner_task.handle())\n"
        "                coro_scheduler().forget(ih);\n",
        "            const auto runner = std::exchange(_state->runner_handle, {});\n"
        "            if (CoroutineScheduler *const sched = detail::cleanup_scheduler()) { // null after the thread's loop: dropped\n"
        "                if (runner)\n"
        "                    sched->cancel_spawned(runner);\n"
        "                if (auto ih = _state->inner_task.handle())\n"
        "                    sched->forget(ih);\n"
        "            }\n",
        2,
    )
# the timeout awaiter stops its timer on the loop it armed it on: no listener in a destructor
if "N9" not in M:
    co.sub(
        "        bool     timer_started{false};\n",
        "        bool     timer_started{false};\n"
        "        struct ev_loop *timer_loop{nullptr}; ///< the loop the timer was armed on: the stop never asks a listener for it\n",
        2,
    )
    co.sub(
        "            ev_timer_stop(static_cast<struct ev_loop *>(listener::current.loop()), &_state->timer);\n",
        "            ev_timer_stop(_state->timer_loop, &_state->timer);\n",
        2,
    )
    co.sub(
        "        ev_timer_start(loop, &_state->timer);\n        _state->timer_started = true;\n",
        "        ev_timer_start(loop, &_state->timer);\n        _state->timer_started = true;\n"
        "        _state->timer_loop    = static_cast<struct ev_loop *>(loop);\n",
        2,
    )
    if V == "G" and "M6timeout" not in M:
        # the destructor's stop, guarded; await_resume()'s finish() stays raw
        co.sub(
            "            _state->inner_task = task<T>{};\n"
            "        }\n"
            "        finish();\n"
            "    }\n",
            "            _state->inner_task = task<T>{};\n"
            "        }\n"
            "        if (_state && _state->timer_started) {\n"
            "            detail::stop_unless_loop_gone(_state->timer_loop, &_state->timer);\n"
            "            _state->timer_started = false;\n"
            "        }\n"
            "    }\n",
        )
        co.sub(
            "            _state->inner_task = task<void>{};\n"
            "        }\n"
            "        finish();\n"
            "    }\n",
            "            _state->inner_task = task<void>{};\n"
            "        }\n"
            "        if (_state && _state->timer_started) {\n"
            "            detail::stop_unless_loop_gone(_state->timer_loop, &_state->timer);\n"
            "            _state->timer_started = false;\n"
            "        }\n"
            "    }\n",
        )
co.save()

# ======================================================================================================================
# variant G: the awaiter destructors' guards
# ======================================================================================================================
if V == "G":
    aw = File("src/qb/io/async/coroutine/awaiter.h")
    if "M6timer" not in M:
        aw.sub(
            "        if (started_) {\n"
            "            ev_timer_stop(loop_, &watcher_);\n"
            "            started_ = false;\n"
            "        }\n"
            "    }\n"
            "\n"
            "    /**\n"
            "     * @brief libev callback when timer fires\n",
            "        // Destroyed after the loop it was armed on (a frame kept past its thread's listener): the watcher is\n"
            "        // ignored, never stopped. Cold: `started_` is false after any resume.\n"
            "        if (started_) {\n"
            "            detail::stop_unless_loop_gone(loop_, &watcher_);\n"
            "            started_ = false;\n"
            "        }\n"
            "    }\n"
            "\n"
            "    /**\n"
            "     * @brief libev callback when timer fires\n",
        )
    if "M6io" not in M:
        aw.sub(
            "        if (started_) {\n"
            "            ev_io_stop(loop_, &watcher_);\n"
            "            started_ = false;\n"
            "        }\n"
            "    }\n"
            "\n"
            "    /**\n"
            "     * @brief libev callback when I/O is ready\n",
            "        if (started_) {\n"
            "            detail::stop_unless_loop_gone(loop_, &watcher_); // see ~timer_awaiter\n"
            "            started_ = false;\n"
            "        }\n"
            "    }\n"
            "\n"
            "    /**\n"
            "     * @brief libev callback when I/O is ready\n",
        )
    aw.save()
    ch = File("src/qb/io/async/coroutine/channel.h")
    if "M6recv" not in M:
        ch.sub(
            "                if (_scheduler)\n"
            "                    forget_frame_from(_scheduler, _handle); // never through `_scheduler` alone: it may be gone (QB-1220)\n"
            "                disarm();\n",
            "                if (_scheduler)\n"
            "                    forget_frame_from(_scheduler, _handle); // never through `_scheduler` alone: it may be gone (QB-1220)\n"
            "                if (_armed) { // the destructor's stop: see detail::stop_unless_loop_gone()\n"
            "                    ::qb::io::async::detail::stop_unless_loop_gone(loop, &watchdog);\n"
            "                    _armed = false;\n"
            "                }\n",
        )
    if "M6send" not in M:
        ch.sub(
            "                if (_scheduler)\n"
            "                    forget_frame_from(_scheduler, _handle); // timer_awaiter's unschedule(): see ~timed_recv_awaiter\n"
            "                disarm();\n",
            "                if (_scheduler)\n"
            "                    forget_frame_from(_scheduler, _handle); // timer_awaiter's unschedule(): see ~timed_recv_awaiter\n"
            "                if (_armed) { // the destructor's stop: see detail::stop_unless_loop_gone()\n"
            "                    ::qb::io::async::detail::stop_unless_loop_gone(loop, &watchdog);\n"
            "                    _armed = false;\n"
            "                }\n",
        )
    ch.save()

# ======================================================================================================================
# the io objects: one entry point
# ======================================================================================================================
io = File("src/qb/io/async/io.h")
io.sub(
    "        _async_event.stop();\n"
    "        listener::current.unregisterEvent(_async_event._interface);\n",
    "        _async_event.stop();\n"
    "        listener::release(_async_event._interface); // frees a wrapper clear() detached without reaching a listener\n",
)
io.sub(
    "     *          Specifically, it calls `_async_event.stop()` and then `listener::current.unregisterEvent(_async_event._interface)`.\n",
    "     *          Specifically, it calls `_async_event.stop()` and then `listener::release(_async_event._interface)`.\n",
)
io.save()
qe = File("src/qb/io/async/quic/endpoint.h")
qe.sub("listener::current.unregisterEvent(", "listener::release(", 2)
qe.save()
cn = File("src/qb/io/async/tcp/connector.h")
cn.sub("listener::current.unregisterEvent(", "listener::release(", 10)
cn.save()

if args.inline_marker:
    act = File("src/qb/core/Actor.h")
    act.sub(
        "        if (const auto h = me->cont; h) // bound in await_suspend, before any reply can be routed\n"
        "            h.resume();                 // `me` may be gone when this returns: see above\n",
        "        if (const auto h = me->cont; h) { // bound in await_suspend, before any reply can be routed\n"
        "            qb::io::async::detail::inline_resume_scope resuming; // no drain is on the stack: say a frame is executing\n"
        "            h.resume();                                          // `me` may be gone when this returns: see above\n"
        "        }\n",
    )
    act.save()

# ======================================================================================================================
# variant E: qev detaches the watchers of a loop it destroys
# ======================================================================================================================
if V == "E" and "E5all" not in M:
    ev = File("src/qb/ev/ev.c")
    ev.sub(
        "static void once_reclaim(EV_P);\n",
        "static void once_reclaim(EV_P);\nstatic void watchers_detach(EV_P);\n",
    )
    ev.sub(
        "    /* the ev_once objects still waiting: freed, never invoked (Huly QB-736) */\n    once_reclaim(EV_A);\n",
        "    /* no watcher stays \"active\" or \"pending\" in a loop that is gone (Huly QB-1246) */\n"
        "    watchers_detach(EV_A);\n"
        "\n"
        "    /* the ev_once objects still waiting: freed, never invoked (Huly QB-736) */\n    once_reclaim(EV_A);\n",
    )
    pend = "" if "E5pending" in M else (
        "    for (pri = NUMPRI; pri--;)\n"
        "        for (i = pendingcnt[pri]; i--;)\n"
        "            if (!EV_DETACH_OWN(pendings[pri][i].w))\n"
        "                pendings[pri][i].w->pending = 0;\n"
        "\n"
    )
    ios = "" if "E5io" in M else (
        "    for (i = 0; i < anfdmax; ++i)\n"
        "        for (wl = anfds[i].head; wl; wl = wl->next)\n"
        "            if (!EV_DETACH_OWN(wl)) {\n"
        "                ((W) wl)->active = 0;\n"
        "#if defined(_WIN32)\n"
        "                /* what ev_io_stop does last: the watcher's reference on the process-wide fd registry */\n"
        "                if (((ev_io *) wl)->handle) {\n"
        "                    ev_win32_fd_release(((ev_io *) wl)->fd, 0);\n"
        "                    ((ev_io *) wl)->fd = -1;\n"
        "                }\n"
        "#endif\n"
        "            }\n"
        "\n"
    )
    tim = "" if "E5timer" in M else (
        "    for (i = timercnt + HEAP0; i-- > HEAP0;) {\n"
        "        W w = (W) ANHE_w(timers[i]);\n"
        "\n"
        "        if (EV_DETACH_OWN(w))\n"
        "            continue;\n"
        "#if EV_STAT_ENABLE\n"
        "        if (ev_cb((ev_timer *) w) == stat_timer_cb) /* an ev_stat polling through its timer */\n"
        "            ((W) EV_CONTAINER_OF((ev_timer *) w, ev_stat, timer))->active = 0;\n"
        "#endif\n"
        "        ev_at(w) -= mn_now; /* relative again, as ev_timer_stop leaves it */\n"
        "        w->active = 0;\n"
        "    }\n"
        "\n"
    )
    ev.sub(
        "/* See ev.h: no-op when cb is NULL, or when fd < 0 and timeout < 0. (nothing could fire). */\n",
        "/* qev (Huly QB-1246): a loop being destroyed DETACHES every watcher still started on it or pending in it. Each is\n"
        " * left inactive and not pending -- the state ev_TYPE_stop leaves -- so a stop that comes AFTER the loop (the\n"
        " * watcher's owner outlived it: static storage, a thread_local destroyed later, a coroutine frame freed late)\n"
        " * returns at its ev_is_active test, before it reads the loop. Until then such a stop read and wrote the freed\n"
        " * loop: its timer heap, its fd lists, its counters. Nothing is invoked, as for ev_once (once_reclaim, which runs\n"
        " * next and frees the ev_once objects this walk passes). A watcher that lives INSIDE the loop is the loop's own\n"
        " * (pipe_w, sigfd_w, timerfd_w, the backends'): ev_loop_destroy still tests those, and they are freed with it.\n"
        " * A signal or a child watcher sits on a process-wide list that names its loop: those are stopped for real,\n"
        " * which takes the loop out of that list too. */\n"
        "#if !EV_MULTIPLICITY\n"
        "/* without EV_MULTIPLICITY the loop is a set of globals: its own watchers are named */\n"
        "inline_size int\n"
        "watcher_is_loop_own(void *w) {\n"
        "    if (w == (void *) &pending_w || w == (void *) &pipe_w)\n"
        "        return 1;\n"
        "#if EV_USE_SIGNALFD\n"
        "    if (w == (void *) &sigfd_w)\n"
        "        return 1;\n"
        "#endif\n"
        "#if EV_USE_TIMERFD\n"
        "    if (w == (void *) &timerfd_w)\n"
        "        return 1;\n"
        "#endif\n"
        "#if EV_USE_INOTIFY\n"
        "    if (w == (void *) &fs_w)\n"
        "        return 1;\n"
        "#endif\n"
        "#if EV_USE_IOURING\n"
        "    if (w == (void *) &iouring_tfd_w)\n"
        "        return 1;\n"
        "#endif\n"
        "#if EV_USE_LINUXAIO\n"
        "    if (w == (void *) &linuxaio_epoll_w)\n"
        "        return 1;\n"
        "#endif\n"
        "    return 0;\n"
        "}\n"
        "#endif\n"
        "\n"
        "ecb_noinline ecb_cold static void\n"
        "watchers_detach(EV_P) {\n"
        "    int i, pri;\n"
        "    WL  wl;\n"
        "\n"
        "#if EV_MULTIPLICITY\n"
        "#define EV_DETACH_OWN(w) ((char *) (w) >= (char *) EV_A && (char *) (w) < (char *) EV_A + sizeof(struct ev_loop))\n"
        "#else\n"
        "#define EV_DETACH_OWN(w) watcher_is_loop_own((void *) (w))\n"
        "#endif\n"
        "\n"
        "#if EV_SIGNAL_ENABLE\n"
        "    for (i = 0; i < EV_NSIG - 1; ++i)\n"
        "#if EV_MULTIPLICITY\n"
        "        if (signals[i].loop == EV_A)\n"
        "#endif\n"
        "            while ((wl = signals[i].head))\n"
        "                ev_signal_stop(EV_A_(ev_signal *) wl);\n"
        "#endif\n"
        "#if EV_CHILD_ENABLE\n"
        "    if (ev_is_default_loop(EV_A))\n"
        "        for (i = (EV_PID_HASHSIZE); i--;)\n"
        "            while ((wl = childs[i]))\n"
        "                ev_child_stop(EV_A_(ev_child *) wl);\n"
        "#endif\n"
        "\n" + pend + ios + tim +
        "#if EV_PERIODIC_ENABLE\n"
        "    for (i = periodiccnt + HEAP0; i-- > HEAP0;)\n"
        "        ((W) ANHE_w(periodics[i]))->active = 0;\n"
        "#endif\n"
        "#if EV_STAT_ENABLE && EV_USE_INOTIFY\n"
        "    for (i = 0; i < (EV_INOTIFY_HASHSIZE); ++i) /* the ev_stat watchers inotify serves */\n"
        "        for (wl = fs_hash[i].head; wl; wl = wl->next)\n"
        "            ((W) wl)->active = 0;\n"
        "#endif\n"
        "#if EV_IDLE_ENABLE\n"
        "    for (pri = NUMPRI; pri--;)\n"
        "        for (i = idlecnt[pri]; i--;)\n"
        "            ((W) idles[pri][i])->active = 0;\n"
        "#endif\n"
        "#if EV_PREPARE_ENABLE\n"
        "    for (i = preparecnt; i--;) {\n"
        "#if EV_EMBED_ENABLE\n"
        "        if (ev_cb(prepares[i]) == embed_prepare_cb) /* an ev_embed: its io and its fork are on the lists walked here */\n"
        "            ((W) EV_CONTAINER_OF(prepares[i], ev_embed, prepare))->active = 0;\n"
        "#endif\n"
        "        ((W) prepares[i])->active = 0;\n"
        "    }\n"
        "#endif\n"
        "#if EV_CHECK_ENABLE\n"
        "    for (i = checkcnt; i--;)\n"
        "        ((W) checks[i])->active = 0;\n"
        "#endif\n"
        "#if EV_FORK_ENABLE\n"
        "    for (i = forkcnt; i--;)\n"
        "        ((W) forks[i])->active = 0;\n"
        "#endif\n"
        "#if EV_CLEANUP_ENABLE\n"
        "    for (i = cleanupcnt; i--;)\n"
        "        ((W) cleanups[i])->active = 0;\n"
        "#endif\n"
        "#if EV_ASYNC_ENABLE\n"
        "    for (i = asynccnt; i--;)\n"
        "        ((W) asyncs[i])->active = 0;\n"
        "#endif\n"
        "#undef EV_DETACH_OWN\n"
        "}\n"
        "\n"
        "/* See ev.h: no-op when cb is NULL, or when fd < 0 and timeout < 0. (nothing could fire). */\n",
    )
    ev.save()

# ======================================================================================================================
# the two QB-84 cases that pin the old tally: the dossier's T1c and T2
# ======================================================================================================================
if not args.no_tests:
    t = File("tests/io/unit/coroutine/scheduler-abandoned-frames.cpp")
    a = t.s.index("/**\n * @test One frame parked at teardown of a directly-owned scheduler: the tally moves by exactly\n")
    b = t.s.index("/**\n * @test A clean teardown -- every frame completed -- reports nothing.\n")
    t.s = (
        t.s[:a]
        + "/**\n"
        " * @test T1c. A frame a task driven by hand owns, registered as suspended with a directly-owned scheduler, is NOT\n"
        " *       reported when that scheduler goes: it is its task's, which destroys it afterwards -- once, with no leak.\n"
        " *       (Until 3.4 the tally counted it: it counted the suspended set, not what the scheduler owned.)\n"
        " */\n"
        "TEST_F(SchedulerAbandonedFrames, AFrameItsTaskOwnsIsNotReported) {\n"
        "    const std::size_t before = abandoned_coroutine_frames_total();\n"
        "    const long        live   = detail::CoroutineFrameAllocator::live_frames;\n"
        "    {\n"
        "        task<void> parked = park_for_ever();\n"
        "        {\n"
        "            CoroutineScheduler sched; // owned here, not by the listener\n"
        "            CoroutineScheduler::set_current(&sched);\n"
        "            parked.handle().resume(); // runs to the park: registered as suspended with `sched`\n"
        "            EXPECT_EQ(sched.active_count(), 1u) << \"the parked frame must be counted as suspended\";\n"
        "        } // ~CoroutineScheduler: it owns nothing\n"
        "        CoroutineScheduler::set_current(nullptr);\n"
        "        EXPECT_EQ(abandoned_coroutine_frames_total(), before) << \"a frame its task owns is not the scheduler's to report\";\n"
        "        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, live + 1) << \"and not the scheduler's to destroy\";\n"
        "    }\n"
        "    EXPECT_EQ(abandoned_coroutine_frames_total(), before);\n"
        "    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, live) << \"its task destroyed it\";\n"
        "}\n"
        "\n"
        "/**\n"
        " * @test A scheduler owned directly DESTROYS the coroutines it owns when it goes, while its thread's loop lives: a\n"
        " *       root parked on an awaiter that registers, and one parked on an awaiter that registers nothing.\n"
        " */\n"
        "TEST_F(SchedulerAbandonedFrames, AnOwnedRootIsDestroyedWithItsScheduler) {\n"
        "    const std::size_t before = abandoned_coroutine_frames_total();\n"
        "    const long        live   = detail::CoroutineFrameAllocator::live_frames;\n"
        "    channel<int>      ch;\n"
        "    {\n"
        "        CoroutineScheduler sched;\n"
        "        CoroutineScheduler::set_current(&sched);\n"
        "        sched.spawn(park_for_ever());\n"
        "        sched.spawn([&ch]() -> task<void> { (void) co_await ch.recv(); });\n"
        "        sched.run_ready();\n"
        "        EXPECT_GE(detail::CoroutineFrameAllocator::live_frames, live + 2);\n"
        "    }\n"
        "    CoroutineScheduler::set_current(nullptr);\n"
        "    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, live) << \"every frame the scheduler owned is destroyed\";\n"
        "    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << \"and nothing is reported\";\n"
        "}\n"
        "\n"
        + t.s[b:]
    )
    a = t.s.index("/**\n * @test The tally is cumulative across schedulers: two teardowns, one frame each, add two.\n")
    b = t.s.index("/**\n * @test The listener's teardown DESTROYS a parked frame it owns")
    t.s = (
        t.s[:a]
        + "/**\n"
        " * @test T2. A scheduler destroyed AFTER its thread's loop abandons the coroutines it owns and reports them, a root\n"
        " *       once: static storage, in a child process whose last-destroyed static is the judge. Two schedulers, one\n"
        " *       root parked on `recv()` (it registers nothing: the old tally missed it) and one on a registering awaiter,\n"
        " *       add two; neither frame's locals are destroyed.\n"
        " */\n"
        "namespace scheduler_abandoned_frames_test {\n"
        "int g_late_locals_destroyed = 0;\n"
        "struct counted_local {\n"
        "    ~counted_local() {\n"
        "        ++g_late_locals_destroyed;\n"
        "    }\n"
        "};\n"
        "task<void>\n"
        "recv_for_ever(channel<int> &ch) {\n"
        "    counted_local local;\n"
        "    (void) co_await ch.recv();\n"
        "}\n"
        "task<void>\n"
        "park_for_ever_counted() {\n"
        "    counted_local local;\n"
        "    co_await park_registered{};\n"
        "}\n"
        "std::size_t g_tally_before = 0;\n"
        "// The judge: an atexit handler registered BEFORE the statics below are built, so it runs after their destructors --\n"
        "// after the two schedulers, which went after the main thread's listener. It ends the child with the verdict and\n"
        "// without the exit-time leak check: the abandoned frames are the point. (Not a static's destructor: a destructor\n"
        "// that never returns is MSVC's C4722.)\n"
        "void\n"
        "judge_at_exit() {\n"
        "    const std::size_t moved = abandoned_coroutine_frames_total() - g_tally_before;\n"
        "    std::fprintf(stderr, \"judge: tally moved by %zu, locals destroyed %d\\n\", moved, g_late_locals_destroyed);\n"
        "    std::fflush(stderr);\n"
        "    std::_Exit(moved == 2 && g_late_locals_destroyed == 0 ? 0 : 1);\n"
        "}\n"
        "[[noreturn]] void\n"
        "schedulers_outliving_the_loop() {\n"
        "    g_tally_before = abandoned_coroutine_frames_total();\n"
        "    std::atexit(judge_at_exit);\n"
        "    static channel<int>       ch;\n"
        "    static CoroutineScheduler first;\n"
        "    static CoroutineScheduler second;\n"
        "    listener::current.clear(); // the main thread's loop exists\n"
        "    CoroutineScheduler::set_current(&first);\n"
        "    first.spawn(recv_for_ever(ch));\n"
        "    first.run_ready();\n"
        "    CoroutineScheduler::set_current(&second);\n"
        "    second.spawn(park_for_ever_counted());\n"
        "    second.run_ready();\n"
        "    CoroutineScheduler::set_current(nullptr);\n"
        "    std::exit(3); // the thread-locals, then the statics: the judge decides the exit code\n"
        "}\n"
        "} // namespace scheduler_abandoned_frames_test\n"
        "\n"
        "TEST(SchedulerAbandonedFramesDeathTest, ASchedulerDestroyedAfterItsLoopAbandonsItsRootsAndCountsThem) {\n"
        "    GTEST_FLAG_SET(death_test_style, \"threadsafe\");\n"
        "    EXPECT_EXIT(schedulers_outliving_the_loop(), ::testing::ExitedWithCode(0), \"judge: tally moved by 2, locals destroyed 0\");\n"
        "}\n"
        "\n"
        + t.s[b:]
    )
    t.sub("#include <cstddef>\n", "#include <cstddef>\n#include <cstdio>\n#include <cstdlib>\n")
    t.save()

print("variant", V, "mutants", sorted(M) or "none", "loop-door" if args.loop_door else "", "trace" if args.trace else "")
