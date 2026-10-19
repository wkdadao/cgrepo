// C++17 - Order State Machine, 2-hour OA template
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wswitch-enum
//        -Werror=switch-enum oa_order_state_machine_full.cpp -o order_sm
// Run:   ./order_sm
//
// Scope: one order, one writer, in-memory commit.
// No FIX transport, DB transaction, durable inbox/outbox or deferred queue.
// Action == command intent; the code below NEVER actually transmits FIX.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

enum class State {
    PendingNew,
    Working,
    PartiallyFilled,
    PendingCancel,
    Filled,
    Cancelled,
    Rejected
};

enum class Decision { Applied, Duplicate, Ignored, Invalid, Conflict };
enum class CancelOutcome { Acked, Rejected };

const char* name(State s) {
    switch (s) {
    case State::PendingNew:      return "PendingNew";
    case State::Working:         return "Working";
    case State::PartiallyFilled: return "PartiallyFilled";
    case State::PendingCancel:   return "PendingCancel";
    case State::Filled:          return "Filled";
    case State::Cancelled:       return "Cancelled";
    case State::Rejected:        return "Rejected";
    }
    return "UnknownState";
}

const char* name(Decision d) {
    switch (d) {
    case Decision::Applied:   return "Applied";
    case Decision::Duplicate: return "Duplicate";
    case Decision::Ignored:   return "Ignored";
    case Decision::Invalid:   return "Invalid";
    case Decision::Conflict:  return "Conflict";
    }
    return "UnknownDecision";
}

struct Snapshot {
    std::string order_id;
    std::string cl_ord_id;
    State state = State::PendingNew;
    int order_qty = 0;
    int cum_qty = 0;
    std::uint64_t version = 0;

    // For the OA, keep deduplication and request facts inside the Snapshot.
    // Real execution systems should also support bust/correct and ExecID scope.
    std::unordered_map<std::string, int> executions;  // exec_id -> fill qty
    std::string pending_cancel_id;                    // may remain after full fill
    std::unordered_map<std::string, CancelOutcome> cancel_history;
    bool reconcile_requested = false;

    int leaves() const { return order_qty - cum_qty; }
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

// Domain events: FIX messages should be normalized into these first.
struct NewAck {};
struct NewReject {};
struct Fill { std::string exec_id; int qty; };
struct RequestCancel { std::string cancel_id; };
struct CancelAck { std::string cancel_id; };
struct CancelReject { std::string cancel_id; };
struct RequestTimeout { std::string cancel_id; };
using Event = std::variant<NewAck, NewReject, Fill, RequestCancel,
                           CancelAck, CancelReject, RequestTimeout>;

// Effects are data, not I/O calls. Their payloads are frozen at transition time.
struct SendFixCancel {
    std::string order_id;
    std::string orig_cl_ord_id;
    std::string cancel_cl_ord_id;
    int leaves_qty;
};
struct PublishOrderUpdate {
    std::string order_id;
    State state;
    int cum_qty;
    int leaves_qty;
    std::uint64_t version;
};
struct RequestReconciliation {
    std::string order_id;
    std::string cancel_id;
};
using Action = std::variant<SendFixCancel, PublishOrderUpdate,
                            RequestReconciliation>;

struct ProcessResult {
    Decision decision;
    SnapshotPtr before;
    SnapshotPtr after;
    std::vector<Action> actions;
    std::string reason;
};

struct Outcome {
    Decision decision;
    std::string reason;
    std::vector<Action> actions;
};

Outcome applied(std::string reason, std::vector<Action> actions = {}) {
    return {Decision::Applied, std::move(reason), std::move(actions)};
}
Outcome deny(Decision d, std::string reason) {
    return {d, std::move(reason), {}};
}

bool valid(const Snapshot& s) {
    if (s.order_id.empty() || s.order_qty <= 0 ||
        s.cum_qty < 0 || s.cum_qty > s.order_qty) return false;
    if (s.state == State::Filled && s.cum_qty != s.order_qty) return false;
    if (s.state != State::Filled && s.cum_qty == s.order_qty) return false;
    if (s.state == State::Rejected && s.cum_qty != 0) return false;
    if (s.state == State::PendingCancel && s.pending_cancel_id.empty())
        return false;
    return true;
}

class OrderMachine {
public:
    explicit OrderMachine(std::string order_id, std::string cl_ord_id,
                          int order_qty) {
        if (order_id.empty() || cl_ord_id.empty() || order_qty <= 0)
            throw std::invalid_argument("invalid order");
        current_ = std::make_shared<const Snapshot>(Snapshot{
            std::move(order_id), std::move(cl_ord_id),
            State::PendingNew, order_qty, 0, 0, {}, "", {}, false
        });
    }

    const Snapshot& snapshot() const { return *current_; }

    [[nodiscard]] ProcessResult process(const Event& event) {
        SnapshotPtr before = current_;
        Snapshot candidate = *before; // small-OA model; no mutation until accepted

        Outcome out = std::visit([&](const auto& e) {
            return on(candidate, e);
        }, event);

        if (out.decision == Decision::Applied) {
            candidate.version = before->version + 1;
            if (!valid(candidate)) {
                return {Decision::Conflict, before, before, {},
                        "candidate violates snapshot invariant"};
            }
            if (candidate.state != before->state ||
                candidate.cum_qty != before->cum_qty) {
                out.actions.emplace_back(PublishOrderUpdate{
                    candidate.order_id, candidate.state, candidate.cum_qty,
                    candidate.leaves(), candidate.version
                });
            }
            // OA: immediately commit to memory. Production: durable commit first.
            current_ = std::make_shared<const Snapshot>(std::move(candidate));
        }

        return {out.decision, std::move(before), current_,
                std::move(out.actions), std::move(out.reason)};
    }

private:
    SnapshotPtr current_;

    static Outcome on(Snapshot& s, const NewAck&) {
        switch (s.state) {
        case State::PendingNew:
            s.state = State::Working;
            return applied("new order acknowledged");
        case State::Working:
        case State::PartiallyFilled:
        case State::PendingCancel:
        case State::Filled:
        case State::Cancelled:
            return deny(Decision::Duplicate, "new ack already reflected");
        case State::Rejected:
            return deny(Decision::Conflict, "new ack contradicts reject");
        }
        return deny(Decision::Invalid, "unknown state");
    }

    static Outcome on(Snapshot& s, const NewReject&) {
        switch (s.state) {
        case State::PendingNew:
            s.state = State::Rejected;
            return applied("new order rejected");
        case State::Rejected:
            return deny(Decision::Duplicate, "already rejected");
        case State::Working:
        case State::PartiallyFilled:
        case State::PendingCancel:
        case State::Filled:
        case State::Cancelled:
            return deny(Decision::Conflict, "new reject contradicts accepted order");
        }
        return deny(Decision::Invalid, "unknown state");
    }

    static Outcome on(Snapshot& s, const Fill& e) {
        // Global checks run before state-specific policy.
        if (e.exec_id.empty() || e.qty <= 0)
            return deny(Decision::Invalid, "bad execution id or quantity");
        auto it = s.executions.find(e.exec_id);
        if (it != s.executions.end()) {
            return it->second == e.qty
                ? deny(Decision::Duplicate, "same execution already applied")
                : deny(Decision::Conflict, "same execution id has different qty");
        }

        switch (s.state) {
        case State::Working:
        case State::PartiallyFilled:
        case State::PendingCancel: {
            if (e.qty > s.leaves())
                return deny(Decision::Conflict, "overfill");
            const bool cancelling = (s.state == State::PendingCancel);
            s.executions.emplace(e.exec_id, e.qty);
            s.cum_qty += e.qty; // safe: qty <= leaves(), so no signed overflow
            if (s.leaves() == 0)
                s.state = State::Filled;
            else if (!cancelling)
                s.state = State::PartiallyFilled;
            return applied("fill accepted");
        }
        case State::PendingNew:
            // A production handler may defer this if the protocol permits.
            return deny(Decision::Conflict, "fill before new ack needs recovery policy");
        case State::Filled:
        case State::Cancelled:
        case State::Rejected:
            // New facts after a terminal state must not be silently dropped.
            return deny(Decision::Conflict, "new fill after terminal state");
        }
        return deny(Decision::Invalid, "unknown state");
    }

    static Outcome on(Snapshot& s, const RequestCancel& e) {
        if (e.cancel_id.empty())
            return deny(Decision::Invalid, "empty cancel id");
        if (e.cancel_id == s.pending_cancel_id ||
            s.cancel_history.count(e.cancel_id))
            return deny(Decision::Duplicate, "cancel request id already known");

        switch (s.state) {
        case State::Working:
        case State::PartiallyFilled:
            s.state = State::PendingCancel;
            s.pending_cancel_id = e.cancel_id;
            s.reconcile_requested = false;
            return applied("cancel queued", {SendFixCancel{
                s.order_id, s.cl_ord_id, e.cancel_id, s.leaves()
            }});
        case State::PendingCancel:
            return deny(Decision::Conflict, "different cancel already pending");
        case State::PendingNew:
            return deny(Decision::Invalid, "new order not acknowledged");
        case State::Filled:
        case State::Cancelled:
        case State::Rejected:
            return deny(Decision::Ignored, "cancel request after terminal state");
        }
        return deny(Decision::Invalid, "unknown state");
    }

    static Outcome on(Snapshot& s, const CancelAck& e) {
        if (e.cancel_id.empty())
            return deny(Decision::Invalid, "empty cancel id");
        auto it = s.cancel_history.find(e.cancel_id);
        if (it != s.cancel_history.end()) {
            return it->second == CancelOutcome::Acked
                ? deny(Decision::Duplicate, "duplicate cancel ack")
                : deny(Decision::Conflict, "ack contradicts prior cancel reject");
        }
        if (e.cancel_id != s.pending_cancel_id)
            return deny(Decision::Conflict, "unknown cancel ack id");

        switch (s.state) {
        case State::PendingCancel:
            s.state = State::Cancelled;
            s.cancel_history.emplace(e.cancel_id, CancelOutcome::Acked);
            s.pending_cancel_id.clear();
            s.reconcile_requested = false;
            return applied("cancel acknowledged");
        case State::Filled:
            // Full fill occurred while cancel was pending. Keep the fill fact.
            s.cancel_history.emplace(e.cancel_id, CancelOutcome::Acked);
            s.pending_cancel_id.clear();
            s.reconcile_requested = false;
            return applied("late cancel ack recorded; state stays Filled");
        case State::PendingNew:
        case State::Working:
        case State::PartiallyFilled:
        case State::Cancelled:
        case State::Rejected:
            return deny(Decision::Conflict, "cancel ack in incompatible state");
        }
        return deny(Decision::Invalid, "unknown state");
    }

    static Outcome on(Snapshot& s, const CancelReject& e) {
        if (e.cancel_id.empty())
            return deny(Decision::Invalid, "empty cancel id");
        auto it = s.cancel_history.find(e.cancel_id);
        if (it != s.cancel_history.end()) {
            return it->second == CancelOutcome::Rejected
                ? deny(Decision::Duplicate, "duplicate cancel reject")
                : deny(Decision::Conflict, "reject contradicts prior cancel ack");
        }
        if (e.cancel_id != s.pending_cancel_id)
            return deny(Decision::Conflict, "unknown cancel reject id");

        switch (s.state) {
        case State::PendingCancel:
            // Recover original working status from authoritative cum_qty.
            s.state = s.cum_qty == 0 ? State::Working : State::PartiallyFilled;
            s.cancel_history.emplace(e.cancel_id, CancelOutcome::Rejected);
            s.pending_cancel_id.clear();
            s.reconcile_requested = false;
            return applied("cancel rejected");
        case State::Filled:
            s.cancel_history.emplace(e.cancel_id, CancelOutcome::Rejected);
            s.pending_cancel_id.clear();
            s.reconcile_requested = false;
            return applied("late cancel reject recorded; state stays Filled");
        case State::PendingNew:
        case State::Working:
        case State::PartiallyFilled:
        case State::Cancelled:
        case State::Rejected:
            return deny(Decision::Conflict, "cancel reject in incompatible state");
        }
        return deny(Decision::Invalid, "unknown state");
    }

    static Outcome on(Snapshot& s, const RequestTimeout& e) {
        if (e.cancel_id.empty())
            return deny(Decision::Invalid, "empty timeout request id");
        if (e.cancel_id != s.pending_cancel_id)
            return deny(Decision::Ignored, "stale/unrelated timeout");
        if (s.reconcile_requested)
            return deny(Decision::Duplicate, "reconciliation already requested");

        switch (s.state) {
        case State::PendingCancel:
        case State::Filled:
            // Timeout != cancel reject. Outcome is unknown.
            s.reconcile_requested = true;
            return applied("request outcome unknown; reconcile", {
                RequestReconciliation{s.order_id, e.cancel_id}
            });
        case State::PendingNew:
        case State::Working:
        case State::PartiallyFilled:
        case State::Cancelled:
        case State::Rejected:
            return deny(Decision::Ignored, "timeout not applicable");
        }
        return deny(Decision::Invalid, "unknown state");
    }
};

void print_result(const ProcessResult& r) {
    std::cout << name(r.decision) << "  " << name(r.before->state)
              << " -> " << name(r.after->state)
              << "  cum=" << r.after->cum_qty
              << " version=" << r.after->version
              << "  reason=" << r.reason << '\n';
    for (const Action& action : r.actions) {
        std::visit([](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, SendFixCancel>) {
                std::cout << "  Action: SendFixCancel(" << a.cancel_cl_ord_id
                          << ", leaves=" << a.leaves_qty << ")\n";
            } else if constexpr (std::is_same_v<T, PublishOrderUpdate>) {
                std::cout << "  Action: PublishOrderUpdate(" << name(a.state)
                          << ", version=" << a.version << ")\n";
            } else if constexpr (std::is_same_v<T, RequestReconciliation>) {
                std::cout << "  Action: RequestReconciliation(" << a.cancel_id
                          << ")\n";
            }
        }, action);
    }
}

void expect(Decision d, const ProcessResult& r) {
    assert(r.decision == d);
    if (d != Decision::Applied) {
        assert(r.before == r.after);     // no mutation on non-applied events
        assert(r.actions.empty());       // no effects for ignored/invalid inputs
    }
}

int main() {
    {
        std::cout << "=== fill races with cancel ===\n";
        OrderMachine sm("O1", "N1", 100);
        auto run = [&](const Event& e, Decision d) {
            auto r = sm.process(e);
            print_result(r);
            expect(d, r);
            return r;
        };
        run(NewAck{}, Decision::Applied);
        run(Fill{"E1", 40}, Decision::Applied);
        run(Fill{"E1", 40}, Decision::Duplicate);
        run(Fill{"E1", 41}, Decision::Conflict);
        auto cancel = run(RequestCancel{"C1"}, Decision::Applied);
        assert(cancel.actions.size() == 2); // SendFixCancel + PublishUpdate
        run(Fill{"E2", 30}, Decision::Applied);
        assert(sm.snapshot().state == State::PendingCancel);
        run(RequestTimeout{"C1"}, Decision::Applied);
        run(RequestTimeout{"C1"}, Decision::Duplicate);
        run(Fill{"E3", 30}, Decision::Applied);
        assert(sm.snapshot().state == State::Filled);
        run(CancelReject{"C1"}, Decision::Applied); // cannot un-fill
        run(CancelReject{"C1"}, Decision::Duplicate);
        run(CancelAck{"C1"}, Decision::Conflict);
        run(Fill{"E4", 1}, Decision::Conflict);
    }
    {
        std::cout << "=== cancel ack and late fill ===\n";
        OrderMachine sm("O2", "N2", 100);
        expect(Decision::Applied, sm.process(NewAck{}));
        expect(Decision::Applied, sm.process(RequestCancel{"C2"}));
        expect(Decision::Applied, sm.process(CancelAck{"C2"}));
        assert(sm.snapshot().state == State::Cancelled);
        expect(Decision::Duplicate, sm.process(CancelAck{"C2"}));
        expect(Decision::Conflict, sm.process(CancelReject{"C2"}));
        expect(Decision::Conflict, sm.process(Fill{"LATE", 5}));
    }
    {
        std::cout << "=== cancel rejected after partial fill ===\n";
        OrderMachine sm("O3", "N3", 100);
        expect(Decision::Applied, sm.process(NewAck{}));
        expect(Decision::Applied, sm.process(Fill{"E1", 20}));
        expect(Decision::Applied, sm.process(RequestCancel{"C3"}));
        expect(Decision::Applied, sm.process(CancelReject{"C3"}));
        assert(sm.snapshot().state == State::PartiallyFilled);
        assert(sm.snapshot().cum_qty == 20);
    }
    {
        std::cout << "=== new reject and contradiction ===\n";
        OrderMachine sm("O4", "N4", 100);
        expect(Decision::Applied, sm.process(NewReject{}));
        expect(Decision::Duplicate, sm.process(NewReject{}));
        expect(Decision::Conflict, sm.process(NewAck{}));
    }
    std::cout << "All assertions passed.\n";
}
