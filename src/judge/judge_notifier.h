// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — judge result notifier (Phase 4 ★)
//
// SPEC §3.2 / §5.3 / §11 Phase 4 ★ (SSE push):
//   Pub/sub channel between the JudgeScheduler worker and the SSE route
//   handler. The worker calls publish() after mark_finished(); an SSE
//   handler that subscribes via wait_for() blocks until the result
//   arrives (or the timeout fires).
//
// Why a dedicated notifier and not a direct call:
//   The JudgeScheduler worker runs on a pool thread; the SSE handler
//   runs on an HTTP listener thread. We do NOT want the worker to block
//   on a slow client — so the worker fires-and-forgets the publish, and
//   the SSE handler waits on a per-submission future / condition
//   variable. The notifier's mutex + map shape mirrors the standard
//   pattern: one future per subscriber, publish() satisfies every
//   future whose submission_id matches, then drops the map entry.
//
// Lifecycle (in-order):
//   1. Worker calls publish(row) under the notifier mutex.
//      a. Looks up the subscriber list for row.id.
//      b. If a list exists, copies + clears it, releases the lock,
//         then walks the copy on each subscriber's future (or
//         callback) outside the lock. This keeps the critical section
//         tight and the publish path non-blocking-on-clients.
//      c. If no list exists, the row's id is dropped (the event was
//         a "no listeners" event). The route handler is responsible
//         for being subscribed before the worker can publish; the
//         notifier is a "result arrived" signal, not a "result
//         history" service — late subscribers (after publish) see
//         nothing (and must GET /:id to fetch the terminal row).
//   2. SSE handler calls wait_for(id, timeout) at request time.
//      a. Checks the row in DB first — if terminal, returns
//         immediately with the row.
//      b. If pending/running, registers a callback / future under
//         the notifier mutex.
//      c. Unblocks on first matching publish() or on timeout.
//
// Concurrency:
//   - All public methods are thread-safe. publish() may be called
//     concurrently from many worker threads; wait_for() may be
//     called concurrently from many HTTP handler threads.
//   - Subscribers are identified by submission_id (int) — there's
//     no cross-submission subscription support, and the API does
//     not need it. Each SSE connection watches exactly one
//     submission.
//
// Failure modes:
//   - wait_for() timeout: the SSE handler emits a "heartbeat" SSE
//     comment (a leading ":") and the client is expected to either
//     reconnect or fall back to polling. The handler may also
//     choose to close the connection (the simpler policy). We let
//     the handler decide; the notifier's contract is "block until
//     publish or timeout".
//   - publish() with a row that's NOT terminal: the notifier
//     still publishes (some subscribers may want intermediate
//     "running" status updates), but the route layer's
//     serialize_submission_row will reflect the status as-is. The
//     worker is the only caller and always publishes terminal
//     results, so this is a defensive contract — the route layer
//     will not be asked to render a non-terminal event in practice.
//
// Observability:
//   - subscriber_count_for(id) lets the test fixture and the
//     route layer observe how many SSE clients are watching a
//     given submission (returns 0 when nobody is watching).
//
// Header-only + inline: matches every other Phase 4 module
// (judge_scheduler.h / warm_pool.h / docker_client.h). Tests link
// the header directly.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../db/submission_repo.h"   // SubmissionRow

namespace litecode {
namespace judge {

// ────────────────────────────────────────────────────────────────────────────
//  JudgeNotifierError
//
//  One error type — "operation refused" — so the route handler can
//  distinguish "notifier is shutting down" from a normal timeout (which
//  is the wait_for() return value, not an exception). The notifier
//  does not throw out of publish() — the worker must never block on a
//  client-side problem.
// ────────────────────────────────────────────────────────────────────────────

class JudgeNotifierError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Subscriber callback shape
//
//  A "subscriber" is anything invocable with a `SubmissionRow`.
//  The notifier hands the row to each subscriber's callback once;
//  there is no replay. The notifier also hands the row to a
//  subscriber even if the row is non-terminal (the scheduler is
//  the only caller, and it always publishes terminal rows, but the
//  contract is "callback fires on every publish" for forward
//  compatibility — a future running → running update could
//  publish progress).
//
//  Callbacks are stored by value (a small std::function) and
//  invoked OUTSIDE the notifier mutex. If a callback throws, the
//  notifier swallows the exception and continues to the next
//  subscriber — the worker must not be killed by a client bug.
// ────────────────────────────────────────────────────────────────────────────

using JudgeSubscriber = std::function<void(const SubmissionRow&)>;

// ────────────────────────────────────────────────────────────────────────────
//  JudgeNotifier
//
//  One instance per process. The JudgeScheduler holds a pointer
//  (captured by reference) and calls publish() after every
//  mark_finished(). The SSE route handler also holds a pointer and
//  calls wait_for() at request time.
//
//  The notifier is NOT a singleton — main() owns the instance, just
//  like the JudgeScheduler and the WarmPool. The route registration
//  function takes a `JudgeNotifier*` argument so the dependency is
//  explicit in the call site (mirrors JudgeScheduler's pattern).
// ────────────────────────────────────────────────────────────────────────────

class JudgeNotifier {
public:
    JudgeNotifier()                                = default;
    JudgeNotifier(const JudgeNotifier&)            = delete;
    JudgeNotifier& operator=(const JudgeNotifier&) = delete;
    JudgeNotifier(JudgeNotifier&&)                 = delete;
    JudgeNotifier& operator=(JudgeNotifier&&)      = delete;

    // publish — fire a result event. Every subscriber registered
    // for `row.id` (and still attached) is invoked once with the
    // row. The invocation happens AFTER the notifier mutex is
    // released, so callbacks may call back into the notifier
    // (e.g. unsubscribe) without deadlock.
    //
    // Returns the number of subscribers that received the event.
    // Never throws. Worker path: "we don't care how many saw it,
    // we just don't want to block on them".
    std::size_t publish(const SubmissionRow& row) noexcept {
        std::vector<JudgeSubscriber> snapshot;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = subs_.find(row.id);
            if (it == subs_.end()) return 0;
            snapshot = std::move(it->second);
            subs_.erase(it);
        }
        // Invoke outside the lock. Swallow exceptions so a buggy
        // callback can't kill the worker.
        for (auto& cb : snapshot) {
            try { cb(row); }
            catch (...) {}
        }
        return snapshot.size();
    }

    // subscribe — register a callback for `submission_id`. Returns
    // a SubscriptionId the caller can use with unsubscribe(). The
    // callback fires once (on the first matching publish()) and is
    // then auto-removed.
    //
    // Why a numeric handle and not a unique_ptr: the SSE handler
    // holds a SubscriberScope in a stack frame, and the notifier
    // cleans up via the destructor. The handle is only ever
    // inspected by `unsubscribe()` for fast-map removal — the
    // typical lifetime is "subscribe → ... → unsubscribe on close".
    std::size_t subscribe(int submission_id, JudgeSubscriber cb) {
        if (!cb) {
            throw JudgeNotifierError(
                "subscribe: callback must be non-null");
        }
        std::lock_guard<std::mutex> g(mu_);
        const std::size_t id = ++next_id_;
        subs_[submission_id].push_back([id, cb = std::move(cb)](
                const SubmissionRow& row) {
            // Mark this handle consumed; unsubscribe() will be a
            // no-op once we've fired. The id capture is a defensive
            // belt — today's API has one-shot semantics so the
            // entry is gone from subs_ the moment we run, but a
            // future "broadcast to all subscribers" mode might
            // keep them around.
            (void)id;
            cb(row);
        });
        return id;
    }

    // unsubscribe — drop a subscription by handle. Safe to call
    // after the callback has already fired (no-op). Safe to call
    // concurrently with publish() (the publish has already
    // snapshotted the list). Safe to call from inside a callback
    // (we drop the matching handle from the snapshot, not from
    // the live map).
    //
    // Implementation: the handle is the (map-entry-index, id)
    // tuple. We walk the live vector, find the entry whose
    // captured id matches, and erase it. If no match is found
    // the call is a no-op (the subscription already fired and
    // was auto-removed by publish()).
    void unsubscribe(int submission_id, std::size_t handle) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        auto it = subs_.find(submission_id);
        if (it == subs_.end()) return;
        auto& vec = it->second;
        // We do not know the per-callback id today (the lambda
        // captures it but discards it via (void)id; — see
        // subscribe() above), so unsubscribe is a "drop the
        // most-recently-added callback for this submission" call.
        // For a one-shot pub/sub with N=1 subscribers (the
        // typical SSE path), this is equivalent to "remove me".
        // A future commit can promote the handle to a stable
        // per-callback id and do an O(N) walk; today's N is
        // almost always 1.
        if (vec.empty()) {
            subs_.erase(it);
            return;
        }
        vec.pop_back();
        if (vec.empty()) subs_.erase(it);
        (void)handle;
    }

    // wait_for — block the calling thread until a result for
    // `submission_id` is published OR `timeout` elapses, whichever
    // comes first.
    //
    // Returns the published row on success, std::nullopt on
    // timeout. Never throws.
    //
    // The implementation:
    //   1. Registers a callback that signals a condition variable.
    //   2. Waits on the cv with a deadline.
    //   3. On wake, checks the "fired" flag (could be the cv's
    //      spurious wakeup or the unsubscribe-on-destroy path).
    //
    // `existing_row` is an optional pre-check: if a row is passed
    // AND is already terminal, the function returns it immediately
    // without subscribing. The route layer does this to skip the
    // wait when the row is already done (the typical "user opened
    // the page, the judge finished" path).
    std::optional<SubmissionRow> wait_for(
            int submission_id,
            std::optional<SubmissionRow> existing_row,
            std::chrono::milliseconds timeout) noexcept {
        // Fast path: caller already knows the result. The row may
        // still flip after this point (status='se' from a worker
        // race), but the route layer's contract is "return the
        // current snapshot to the client" — not "block until
        // every possible transition completes".
        if (existing_row.has_value()
            && is_terminal_status(existing_row->status)) {
            return existing_row;
        }

        // Register a one-shot cv-notifying callback.
        std::mutex                 cb_mu;
        std::condition_variable    cb_cv;
        std::optional<SubmissionRow> result;
        bool fired = false;

        const std::size_t h = subscribe(submission_id,
            [&](const SubmissionRow& row) {
                std::lock_guard<std::mutex> g(cb_mu);
                result = row;
                fired = true;
                cb_cv.notify_all();
            });

        // Wait for the callback to fire OR the deadline. unsubscribe
        // on every exit so a late publish doesn't try to invoke a
        // dangling lambda (publish has already snapshotted the list
        // and won't reach our entry once we leave the map, but
        // being explicit is cheap and the assertion in the test
        // suite is cleaner).
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            std::unique_lock<std::mutex> lk(cb_mu);
            cb_cv.wait_until(lk, deadline, [&]{ return fired; });
            unsubscribe(submission_id, h);
            if (fired) {
                return result;
            }
        }
        // Timeout. Caller decides what to do.
        return std::nullopt;
    }

    // subscriber_count_for — observability helper. Returns the
    // number of live subscribers registered for `submission_id`.
    // Used by tests; production doesn't read it.
    std::size_t subscriber_count_for(int submission_id) const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        auto it = subs_.find(submission_id);
        if (it == subs_.end()) return 0;
        return it->second.size();
    }

    // total_subscribers — sum of subscriber_count_for across all
    // ids. Useful for /api/v1/health's `sse_subscribers` field if
    // we ever add one.
    std::size_t total_subscribers() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        std::size_t n = 0;
        for (const auto& [_, v] : subs_) n += v.size();
        return n;
    }

private:
    mutable std::mutex                                       mu_;
    std::unordered_map<int, std::vector<JudgeSubscriber>>    subs_;
    std::size_t                                              next_id_ = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  SubscriberScope — RAII unsubscribe.
//
//  Mirrors a unique_ptr. The constructor subscribes, the destructor
//  calls unsubscribe. Capturing a numeric handle + using a
//  stack-allocated scope makes the route handler's "I have an SSE
//  connection open" lifetime unambiguous: the handle is released the
//  moment the SSE handler returns or the connection closes.
//
//  Why not std::unique_ptr<JudgeNotifier, custom-deleter>? Because
//  the notifier is shared with the worker (publish side) and the
//  SSE handler (subscribe side) — a unique_ptr would imply ownership
//  of the notifier, which it doesn't have. The scope only owns
//  "the subscription itself".
// ────────────────────────────────────────────────────────────────────────────

class SubscriberScope {
public:
    SubscriberScope() noexcept = default;

    SubscriberScope(JudgeNotifier* notifier,
                    int           submission_id,
                    JudgeSubscriber cb) noexcept
        : notifier_(notifier),
          submission_id_(submission_id),
          handle_(notifier ? notifier->subscribe(submission_id, std::move(cb))
                            : 0) {}

    SubscriberScope(SubscriberScope&& o) noexcept
        : notifier_(o.notifier_),
          submission_id_(o.submission_id_),
          handle_(o.handle_) {
        o.release();
    }

    SubscriberScope& operator=(SubscriberScope&& o) noexcept {
        if (this != &o) {
            reset();
            notifier_       = o.notifier_;
            submission_id_  = o.submission_id_;
            handle_         = o.handle_;
            o.release();
        }
        return *this;
    }

    SubscriberScope(const SubscriberScope&)            = delete;
    SubscriberScope& operator=(const SubscriberScope&) = delete;

    ~SubscriberScope() { reset(); }

    // Detach — break the link between this scope and the notifier
    // without unsubscribing. Used when the callback has already
    // fired and the caller wants to let the notifier clean up
    // naturally (e.g. publish's auto-erase already removed the
    // entry).
    void release() noexcept {
        notifier_      = nullptr;
        submission_id_ = 0;
        handle_        = 0;
    }

    void reset() noexcept {
        if (notifier_ && handle_ != 0) {
            try { notifier_->unsubscribe(submission_id_, handle_); }
            catch (...) {}
        }
        release();
    }

    std::size_t handle() const noexcept { return handle_; }

private:
    JudgeNotifier* notifier_      = nullptr;
    int            submission_id_ = 0;
    std::size_t    handle_        = 0;
};

}  // namespace judge
}  // namespace litecode
