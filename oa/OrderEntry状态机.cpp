#include <cctype>
#include <charconv>
#include <iostream>
#include <bits/stdc++.h>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <variant>
using namespace std;

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR " << __LINE__ << "]: "

enum class State
{
    Inital, //
    PendingNew, //等待新单回报
    PendingCancel, //等待撤单回报，包括期间全部成交但撤单尚未收到回报的情况
    PendingReplace, //等待改单回报，包括当前生效总量已经全部成交的情况
    New, //新单已接受，没有成交，也没有等待中的请求
    PartiallyFilled, //已部分成交，仍有剩余量，没有等待中的请求
    Filled, //当前订单总量已全部成交，没有等待中的请求
    Canceled, //撤单成功
    Rejected, //新单被拒绝
};

auto toStr(State state)
{
    switch(state)
    {
        case State::Inital: return "Inital";
        case State::PendingNew: return "PendingNew";
        case State::PendingCancel: return "PendingCancel";
        case State::PendingReplace: return "PendingReplace";
        case State::New: return "New";
        case State::PartiallyFilled: return "PartiallyFilled";
        case State::Filled: return "Filled";
        case State::Canceled: return "Canceled";
        case State::Rejected: return "Rejected";
    }

    return "";
}

struct New
{
    std::string orderId;
    std::string reqId;
    char side{};
    int64_t price;
    int64_t qty{};
};

struct NewAck
{
    std::string reqId;
    std::string exchangeOrderId;
};

struct NewReject
{
    std::string reqId;
};

struct Fill
{
    std::string exchangeOrderId;
    std::string execId;
    int64_t lastQty{};
};

struct Cancel
{
    std::string orderId;
    std::string reqId;
};

struct CancelAck
{
    std::string reqId;
};

struct CancelReject
{
    std::string reqId;
};

struct Replace
{
    std::string orderId;
    std::string reqId;
    int64_t newPrice;
    int64_t newQty{};
};

struct ReplaceAck
{
    std::string reqId;
};

struct ReplaceReject
{
    std::string reqId;
};

struct Query
{
    std::string orderId;
};

using Event = std::variant<New, NewAck, NewReject, Fill, Cancel, CancelAck, CancelReject, Replace, ReplaceAck, ReplaceReject, Query>;


struct SendD
{
    std::string reqId;
    std::string orderId;
    char side;
    int64_t price;
    int64_t qty;
};

struct SendF
{
    std::string reqId;
    std::string currentClOrdId;
    std::string exchangeOrderId;
};

struct SendG
{
    std::string reqId;
    std::string currentClOrdId;
    std::string exchangeOrderId;
    int64_t newPrice{};
    int64_t newQty{};
};

struct QueryResult
{
    std::string orderId;
    std::string currentClOrdId;
    std::string exchangeOrderId;
    State state{};
    char side{};
    int64_t price{};
    int64_t orderQty{};
    int64_t cumQty{};
    int64_t leavesQty{};
    std::string pendingReqId{};
};

struct Error
{
    std::string reason;
};

using Action = std::variant<SendD, SendF, SendG, QueryResult, Error>;
void doAction(const SendD& action, int64_t lineNo, std::ostream& os)
{
    // SEND D reqId - orderId side price qty
    os << "SEND D " << action.reqId << " - " << action.orderId
        << " " << action.side
        << " " << action.price
        << " " << action.qty
        << "\n";
}
void doAction(const SendF& action, int64_t lineNo, std::ostream& os)
{
    // SEND F C1 N1 V1
    // SEND F reqId currentClOrdId exchangeOrderId 
    os << "SEND F " << action.reqId
        << " " << action.currentClOrdId
        << " " << action.exchangeOrderId
        << "\n";   
}
void doAction(const SendG& action, int64_t lineNo, std::ostream& os)
{
    // SEND G R1 N2 V2 201 6
    // SEND G reqId currentClOrdId exchangeOrderId newPrice newQty
    os << "SEND G " << action.reqId
        << " " << action.currentClOrdId
        << " " << action.exchangeOrderId
        << " " << action.newPrice
        << " " << action.newQty
        << "\n";   
}
void doAction(const QueryResult& action, int64_t lineNo, std::ostream& os)
{
    // ORDER O1 N1 - PendingNew B 100 10 0 10 N1
    // ORDER orderId currentClOrdId exchangeOrderId state side price orderQty cumQty leavesQty pendingReqId
    os << "ORDER " << action.orderId
        << " " << (action.currentClOrdId.empty() ? "-" : action.currentClOrdId)
        << " " << (action.exchangeOrderId.empty() ? "-" : action.exchangeOrderId)
        << " " << toStr(action.state)
        << " " << action.side 
        << " " << action.price
        << " " << action.orderQty
        << " " << action.cumQty
        << " " << action.leavesQty
        << " " << (action.pendingReqId.empty() ? "-" : action.pendingReqId)
        << "\n";   
}
void doAction(const Error& action, int64_t lineNo, std::ostream& os)
{
    os << "ERROR " << lineNo << std::endl;
    INFO << "action.reason=" << action.reason << std::endl;
}

struct Snapshot
{
    std::string orderId{};
    std::string currentClOrdId{};
    std::string exchangeOrderId{};
    State state{};
    char side{}; // B, S
    int64_t price{};
    int64_t orderQty{};
    int64_t cumQty{};
    int64_t leavesQty{};
    std::string pendingReqId{};

    Replace replaceEvent;
    std::set<std::string> execIds;
};

enum class Decision
{
    Applied,
    Denied,
    Ingored
};

struct GetResult
{
    bool good{false};
    Snapshot snapshot;
    std::string reason;
};

struct ProcessResult
{
    Decision decision;
    std::vector<Action> actions;
};

class StateMachine
{
public:
    void procss(const Event& event, int64_t lineNo, std::ostream& os)
    {
        std::visit([this, lineNo, &os](auto&& e){
            if (auto res1 = this->getSnapshot(e); res1.good)
            {
                auto res2 = process(res1.snapshot, e);
                if (res2.decision == Decision::Applied)
                {
                    this->snapshots_[res1.snapshot.orderId] = std::move(res1.snapshot);
                    commit(e);
                }

                for (const auto& action : res2.actions)
                {
                    std::visit([&](auto&& a){ doAction(a, lineNo, os); }, action);
                }
            }
            else
            {
                // ERROR << "failed to get snapshot: " << lineNo << ", res1=" << res1.reason << std::endl;
                doAction(Error{res1.reason}, lineNo, os);
            }
        }, event);
    }

    GetResult getSnapshot(const New& event)
    {
        if (snapshots_.contains(event.orderId))
        {
            return GetResult{false, {}, "duplicate orderId"};
        }
        if (reqId2OrderIds_.contains(event.reqId))
        {
            return GetResult{false, {}, "duplicate eqId"};
        }
        return GetResult{true};
    }
    void commit(const New& event)
    {
        reqId2OrderIds_[event.reqId] = event.orderId;
    }
    // 要求 orderId 尚未创建、reqId 未被使用、price > 0、qty > 0。
    static ProcessResult process(Snapshot& snapshot, const New& event)
    {
        if (!(event.price > 0) || !(event.qty > 0))
        {
            return ProcessResult{Decision::Denied, {Error{"invalid event"}}};
        }

        switch(snapshot.state)
        {
            case State::PendingNew:
            case State::PendingCancel:
            case State::PendingReplace:
            case State::New:
            case State::PartiallyFilled:
            case State::Filled:
            case State::Canceled:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::Inital:
                break;
        }

        snapshot.orderId = event.orderId;
        snapshot.currentClOrdId = event.reqId;
        snapshot.state = State::PendingNew;
        snapshot.side = event.side;
        snapshot.price = event.price;
        snapshot.orderQty = event.qty;
        snapshot.cumQty = 0;
        snapshot.leavesQty = event.qty;
        snapshot.pendingReqId = event.reqId;

        return ProcessResult{Decision::Applied, {SendD{
            .reqId=event.reqId,
            .orderId=event.orderId,
            .side=event.side,
            .price=event.price,
            .qty=event.qty}}};
    }

    GetResult getSnapshot(const NewAck& event)
    {
        if (exchangeOrderId2OrderIds_.contains(event.exchangeOrderId))
        {
            return GetResult{false, {}, "duplicate exchangeOrderId"};
        }

        if (auto it = reqId2OrderIds_.find(event.reqId); it != reqId2OrderIds_.end())
        {
            if (auto it2 = snapshots_.find(it->second); it2 != snapshots_.end())
            {
                return GetResult{.good=true, .snapshot=it2->second};
            }
            else
            {
                return GetResult{false, {}, "no orderId"};
            }
        }
        else
        {
            return GetResult{false, {}, "no reqId"};
        }
    }
    void commit(const NewAck& event)
    {
        exchangeOrderId2OrderIds_[event.exchangeOrderId] = reqId2OrderIds_[event.reqId];
    }
    static ProcessResult process(Snapshot& snapshot, const NewAck& event)
    {
        INFO << "NewAck...." << std::endl;

        if (event.reqId != snapshot.pendingReqId)
        {
            return ProcessResult{Decision::Denied, {Error{"invalid reqId"}}};
        }

        switch(snapshot.state)
        {
            case State::Inital:
            case State::PendingCancel:
            case State::PendingReplace:
            case State::New:
            case State::PartiallyFilled:
            case State::Filled:
            case State::Canceled:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::PendingNew:
                snapshot.pendingReqId = {};
                snapshot.exchangeOrderId = event.exchangeOrderId;
                snapshot.state = State::New;
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    GetResult getSnapshot(const NewReject& event)
    {
        if (auto it = reqId2OrderIds_.find(event.reqId); it != reqId2OrderIds_.end())
        {
            if (auto it2 = snapshots_.find(it->second); it2 != snapshots_.end())
            {
                return GetResult{.good=true, .snapshot=it2->second};
            }
            else
            {
                return GetResult{false, {}, "no orderId"};
            }
        }
        else
        {
            return GetResult{false, {}, "no reqId"};
        }
    }
    void commit(const NewReject& event)
    {
    }
    static ProcessResult process(Snapshot& snapshot, const NewReject& event)
    {
        INFO << "NewReject...." << std::endl;

        switch(snapshot.state)
        {
            case State::Inital:
            case State::PendingCancel:
            case State::PendingReplace:
            case State::New:
            case State::PartiallyFilled:
            case State::Filled:
            case State::Canceled:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::PendingNew:
                snapshot.pendingReqId = {};
                snapshot.state = State::Rejected;
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    GetResult getSnapshot(const Fill& event)
    {
        if (auto it = exchangeOrderId2OrderIds_.find(event.exchangeOrderId); it != exchangeOrderId2OrderIds_.end())
        {
            if (auto it2 = snapshots_.find(it->second); it2 != snapshots_.end())
            {
                return GetResult{true, it2->second};
            }
            else
            {
                return GetResult{false, {}, "no orderId"};
            }
        }
        else
        {
            return GetResult{false, {}, "no exchangeOrderId"};
        }
    }
    void commit(const Fill& event)
    {
    }
    /* 
一笔新成交要求订单已接受、尚未终结，且 0 < lastQty <= 当前未成交数量。
将本次成交计入该订单的累计成交量。
等待撤单或改单回报期间，仍可按当前已生效的订单总量成交。
成交全部完成，不会自动清除尚未收到回报的撤单或改单请求。
重复成交的内容保证与首次回报相同。非法成交不登记为已处理成交。
相同 (exchangeOrderId, execId) 的成功成交回报再次出现时，直接忽略，不输出、不重复计量，即使订单已经终结。
*/
    static ProcessResult process(Snapshot& snapshot, const Fill& event)
    {
        INFO << "Fill...." << std::endl;

        if (snapshot.execIds.contains(event.execId))
        {
            INFO << "ignored: event.execId=" << event.execId << std::endl;
            return ProcessResult{Decision::Ingored};
        }

        if (snapshot.exchangeOrderId != event.exchangeOrderId)
        {
            return ProcessResult{Decision::Denied, {Error{"invalid exchangeOrderId"}}};
        }

        snapshot.cumQty += event.lastQty;
        snapshot.leavesQty -= event.lastQty;
        if (snapshot.leavesQty < 0)
        {
            return ProcessResult{Decision::Denied, {Error{"over fill leavesQty"}}};
        }
        if (snapshot.cumQty > snapshot.orderQty)
        {
            return ProcessResult{Decision::Denied, {Error{"over fill cumQty"}}};
        }
        snapshot.execIds.insert(event.execId);

        switch(snapshot.state)
        {
            case State::Inital:
            case State::PendingNew:
            case State::Filled: // over fill
            case State::Rejected:
                return ProcessResult{Decision::Ingored};
            case State::PendingCancel:
            case State::PendingReplace:
            case State::Canceled:
                return ProcessResult{Decision::Applied};
            case State::New:
            case State::PartiallyFilled:
                if (snapshot.cumQty == snapshot.orderQty)
                {
                    snapshot.state = State::Filled;
                }
                else if (snapshot.cumQty == 0)
                {
                    snapshot.state = State::New;
                }
                else
                {
                    snapshot.state = State::PartiallyFilled;
                }
                INFO << "snapshot.leavesQty=" << snapshot.leavesQty << std::endl;
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    // 订单必须已被交易所接受、仍有未成交数量，且没有等待中的请求。reqId 必须未被使用。
    GetResult getSnapshot(const Cancel& event)
    {
        if (reqId2OrderIds_.contains(event.reqId))
        {
            return GetResult{false, {}, "duplicate reqId"};
        }

        if (auto it = snapshots_.find(event.orderId); it != snapshots_.end())
        {
            return GetResult{true, it->second};
        }
        
        return GetResult{false, {}, "no orderId"};
    }
    void commit(const Cancel& event)
    {
        INFO << "commit cancel" << event.reqId << std::endl;
        reqId2OrderIds_[event.reqId] = event.orderId;
    }
    static ProcessResult process(Snapshot& snapshot, const Cancel& event)
    {
        INFO << "Cancel...." << std::endl;

        if (event.orderId != snapshot.orderId)
            return ProcessResult{Decision::Denied, {Error{"invalid orderId"}}};

        if (snapshot.leavesQty <= 0)
            return ProcessResult{Decision::Denied, {Error{"no leavesQty"}}};

        snapshot.pendingReqId = event.reqId;

        switch(snapshot.state)
        {
            case State::PendingNew:
            case State::Inital:
            case State::Filled:
            case State::Canceled:
            case State::PendingCancel:
            case State::PendingReplace:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::New:
            case State::PartiallyFilled:
                snapshot.state = State::PendingCancel;
                break;
        }

        return ProcessResult{Decision::Applied, {SendF{
            .reqId=event.reqId,
            .currentClOrdId=snapshot.currentClOrdId,
            .exchangeOrderId=snapshot.exchangeOrderId}}};
    }
    GetResult getSnapshot(const CancelAck& event)
    {
        if (auto it = reqId2OrderIds_.find(event.reqId); it != reqId2OrderIds_.end())
        {
            return GetResult{true, snapshots_[it->second]};
        }
        
        return GetResult{false, {}, "no reqId"};
    }
    void commit(const CancelAck& event)
    {
    }
    /*
    成功时取消未成交部分，订单进入 Canceled；累计成交量保留，剩余量变为零。
    如果订单已经全部成交，撤单成功回报属于非法操作，必须由撤单拒绝回报结束该请求。
    */
    static ProcessResult process(Snapshot& snapshot, const CancelAck& event)
    {
        INFO << "Cancel...." << std::endl;

        if (snapshot.pendingReqId != event.reqId)
            return ProcessResult{Decision::Denied, {Error{"invalid reqId"}}};

        if (snapshot.leavesQty <= 0)
        {
            return ProcessResult{Decision::Denied, {Error{"invalid leavesQty"}}};
        }

        switch(snapshot.state)
        {
            case State::PendingNew:
            case State::Inital:
            case State::Filled:
            case State::PendingReplace:
            case State::New:
            case State::PartiallyFilled:
            case State::Canceled:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::PendingCancel:
                snapshot.leavesQty = 0;
                snapshot.pendingReqId = {};
                snapshot.state = State::Canceled;
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    GetResult getSnapshot(const CancelReject& event)
    {
        if (auto it = reqId2OrderIds_.find(event.reqId); it != reqId2OrderIds_.end())
        {
            return GetResult{true, snapshots_[it->second]};
        }
        
        return GetResult{false, {}, "no reqId"};
    }
    void commit(const CancelReject& event)
    {
    }
    /*
    拒绝时清除等待中的撤单请求，订单保留期间发生的全部成交。
    两种结果均不改变 currentClOrdId。
    */
    static ProcessResult process(Snapshot& snapshot, const CancelReject& event)
    {
        INFO << "CancelReject...." << std::endl;

        if (snapshot.pendingReqId != event.reqId)
            return ProcessResult{Decision::Denied, {Error{"invalid reqId"}}};

        switch(snapshot.state)
        {
            case State::PendingNew:
            case State::Inital:
            case State::Filled:
            case State::PendingReplace:
            case State::New:
            case State::PartiallyFilled:
            case State::Canceled:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::PendingCancel:
                snapshot.pendingReqId = {};
                if (snapshot.cumQty == snapshot.orderQty)
                {
                    snapshot.state = State::Filled;
                }
                else if (snapshot.cumQty == 0)
                {
                    snapshot.state = State::New;
                }
                else
                {
                    snapshot.state = State::PartiallyFilled;
                }
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    // 订单必须已被交易所接受、仍有未成交数量，且没有等待中的请求。reqId 必须未被使用。
    GetResult getSnapshot(const Replace& event)
    {
        if (reqId2OrderIds_.contains(event.reqId))
        {
            return GetResult{false, {}, "duplicate reqId"};
        }

        if (auto it = snapshots_.find(event.orderId); it != snapshots_.end())
        {
            return GetResult{true, it->second};
        }
        else
        {
            return GetResult{false, {}, "no orderId"};
        }
    }
    void commit(const Replace& event)
    {
        reqId2OrderIds_[event.reqId] = event.orderId;
    }
    /*
    订单必须已被交易所接受、仍有未成交数量，且没有等待中的请求。reqId 必须未被使用。
    要求 newPrice > 0、newQty > 0，并且 newQty >= 当前累计成交量。newQty 表示修改后的订单总量，不是新增量或剩余量。
    */
    static ProcessResult process(Snapshot& snapshot, const Replace& event)
    {
        INFO << "Replace...." << std::endl;

        if (!(event.newPrice > 0)) return ProcessResult{Decision::Denied, {Error{"invalid newPrice"}}};
        if (!(event.newQty > 0)) return ProcessResult{Decision::Denied, {Error{"invalid newQty"}}};
        if (event.orderId != snapshot.orderId) return ProcessResult{Decision::Denied, {Error{"invalid orderId"}}};
        if (!(event.newQty >= snapshot.cumQty)) return ProcessResult{Decision::Denied, {Error{"invalid newQty"}}};

        switch(snapshot.state)
        {
            case State::PendingNew:
            case State::Inital:
            case State::Filled:
            case State::PendingCancel:
            case State::PendingReplace:
            case State::Canceled:
            case State::Rejected:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::New:
            case State::PartiallyFilled:
                snapshot.replaceEvent = event;
                snapshot.pendingReqId = event.reqId;
                snapshot.state = State::PendingReplace;
                break;                
        }

        return ProcessResult{Decision::Applied, {SendG{
            .reqId=event.reqId,
            .currentClOrdId=snapshot.currentClOrdId,
            .exchangeOrderId=snapshot.exchangeOrderId,
            .newPrice=event.newPrice,
            .newQty=event.newQty}}};
    }
    GetResult getSnapshot(const ReplaceAck& event)
    {
        if (auto it = reqId2OrderIds_.find(event.reqId); it != reqId2OrderIds_.end())
        {
            return GetResult{true, snapshots_[it->second]};
        }
        else
        {
            return GetResult{false, {}, "no reqId"};
        }
    }
    void commit(const ReplaceAck& event)
    {
    }
    /*
    成功时，价格变为请求中的 newPrice，currentClOrdId 变为该次 reqId。
    成功时实际订单总量为“请求的 newQty”与“确认时累计成交量”中的较大者。期间成交可能使累计成交量超过原先申请的总量。
    */
    static ProcessResult process(Snapshot& snapshot, const ReplaceAck& event)
    {
        INFO << "ReplaceAck...." << std::endl;

        if (snapshot.pendingReqId != event.reqId)
            return ProcessResult{Decision::Denied, {Error{"invalid reqId"}}};

        switch(snapshot.state)
        {
            case State::Inital:
            case State::PendingNew:
            case State::New:
            case State::PartiallyFilled:
            case State::Canceled:
            case State::Rejected:
            case State::PendingCancel:
            case State::Filled:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::PendingReplace:
                snapshot.currentClOrdId = event.reqId;
                snapshot.price = snapshot.replaceEvent.newPrice;
                snapshot.orderQty = std::max(snapshot.replaceEvent.newQty, snapshot.cumQty);
                snapshot.leavesQty = snapshot.orderQty - snapshot.cumQty;
                snapshot.pendingReqId = {};
                if (snapshot.cumQty == snapshot.orderQty)
                {
                    snapshot.state = State::Filled;
                }
                else
                {
                    snapshot.state = State::PartiallyFilled;
                }
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    GetResult getSnapshot(const ReplaceReject& event)
    {
        if (auto it = reqId2OrderIds_.find(event.reqId); it != reqId2OrderIds_.end())
        {
            return GetResult{true, snapshots_[it->second]};
        }
        else
        {
            return GetResult{false, {}, "no reqId"};
        }
    }
    void commit(const ReplaceReject& event)
    {
    }
    /*
    拒绝时，原价格、原订单总量、原 currentClOrdId 保持不变，期间成交仍然有效。
    两种结果均清除等待中的改单请求。
    */
    static ProcessResult process(Snapshot& snapshot, const ReplaceReject& event)
    {
        INFO << "ReplaceReject...." << std::endl;

        if (snapshot.pendingReqId != event.reqId)
            return ProcessResult{Decision::Denied, {Error{"invalid reqId"}}};

        switch(snapshot.state)
        {
            case State::Inital:
            case State::PendingNew:
            case State::New:
            case State::PartiallyFilled:
            case State::Canceled:
            case State::Rejected:
            case State::PendingCancel:
            case State::Filled:
                return ProcessResult{Decision::Denied, {Error{"invalid state"}}};
            case State::PendingReplace:
                snapshot.pendingReqId = {};
                if (snapshot.cumQty == snapshot.orderQty)
                {
                    snapshot.state = State::Filled;
                }
                else
                {
                    snapshot.state = State::PartiallyFilled;
                }
                break;
        }

        return ProcessResult{Decision::Applied};
    }
    GetResult getSnapshot(const Query& event)
    {
        if (auto it = snapshots_.find(event.orderId); it != snapshots_.end())
        {
            return GetResult{true, it->second};
        }
        else
        {
            return GetResult{false, {}, "no orderId"};
        }
    }
    void commit(const Query& event)
    {
    }
    // ORDER orderId currentClOrdId exchangeOrderId state side price orderQty cumQty leavesQty pendingReqId
    static ProcessResult process(Snapshot& snapshot, const Query& event)
    {
        INFO << "query...." << std::endl;
        return ProcessResult{Decision::Ingored, {QueryResult{
            .orderId=snapshot.orderId,
            .currentClOrdId=snapshot.currentClOrdId,
            .exchangeOrderId=snapshot.exchangeOrderId,
            .state=snapshot.state,
            .side=snapshot.side,
            .price=snapshot.price,
            .orderQty=snapshot.orderQty,
            .cumQty=snapshot.cumQty,
            .leavesQty=snapshot.leavesQty,
            .pendingReqId=snapshot.pendingReqId,
        }}};
    }

private:
    std::unordered_map<std::string, Snapshot> snapshots_;
    std::unordered_map<std::string, std::string> reqId2OrderIds_;
    std::unordered_map<std::string, std::string> exchangeOrderId2OrderIds_;
};

std::optional<std::string_view> nextToken(std::string_view line, size_t& pos)
{
    while (pos < line.size() && std::isspace(int(line[pos]))) ++pos;

    if (pos >= line.size()) return {};

    auto begin = pos;
    while (pos < line.size() && !std::isspace(int(line[pos]))) ++pos;
    auto end = pos;

    return line.substr(begin, end - begin);

    while (pos < line.size() && std::isspace(int(line[pos]))) ++pos;
}

bool nextNumber(std::string_view line, size_t& pos, auto& number)
{
    auto token = nextToken(line, pos); 
    if (!token.has_value()) return false;
    
    auto [ptr, ec] = std::from_chars(token->data(), token->data() + token->size(), number);
    return ptr == token->data() + token->size() && ec == std::errc();
}

bool parse(std::string_view line, Event& event, int64_t lineNo)
{
    size_t pos{0};
    auto evntType = nextToken(line, pos);
    if (!evntType) return false;

    if (evntType == "NEW")
    {
        // NEW orderId reqId side price qty
        auto orderId = nextToken(line, pos);
        if (!orderId) return false;

        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        auto side = nextToken(line, pos);
        if (!side) return false;
        if (!(side == "B" || side == "S")) return false;

        int64_t price;
        if (!nextNumber(line, pos, price)) return false;

        int64_t qty;
        if (!nextNumber(line, pos, qty)) return false;

        if (nextToken(line, pos))  return false;

        event = New{std::string(*orderId), std::string(*reqId), side == "B" ? 'B' : 'S', price, qty};
    }
    else if (evntType == "CANCEL")
    {
        // CANCEL orderId reqId
        auto orderId = nextToken(line, pos);
        if (!orderId) return false;

        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        if (nextToken(line, pos)) return false;

        event = Cancel{std::string(*orderId), std::string(*reqId)};
    }
    else if (evntType == "REPLACE")
    {
        // REPLACE orderId reqId newPrice newQty
        auto orderId = nextToken(line, pos);
        if (!orderId) return false;

        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        int64_t newPrice;
        if (!nextNumber(line, pos, newPrice)) return false;

        int64_t newQty;
        if (!nextNumber(line, pos, newQty)) return false;

        if (nextToken(line, pos)) return false;

        event = Replace{std::string(*orderId), std::string(*reqId), newPrice, newQty};

    }
    else if (evntType == "QUERY")
    {
        // QUERY orderId
        auto orderId = nextToken(line, pos);
        if (!orderId) return false;

        if (nextToken(line, pos)) return false;

        event = Query{std::string(*orderId)};
    }
    else if (evntType == "NEW_ACK")
    {
        // NEW_ACK reqId exchangeOrderId
        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        auto exchangeOrderId = nextToken(line, pos);
        if (!exchangeOrderId) return false;

        if (nextToken(line, pos)) return false;

        event = NewAck{.reqId=std::string(*reqId), .exchangeOrderId=std::string(*exchangeOrderId)};

    }
    else if (evntType == "NEW_REJECT")
    {
        // NEW_REJECT reqId
        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        if (nextToken(line, pos)) return false;

        event = NewReject{.reqId=std::string(*reqId)};
    }
    else if (evntType == "FILL")
    {
        // FILL exchangeOrderId execId lastQty
        auto exchangeOrderId = nextToken(line, pos);
        if (!exchangeOrderId) return false;

        auto execId = nextToken(line, pos);
        if (!execId) return false;

        int64_t lastQty;
        if (!nextNumber(line, pos, lastQty)) return false;
        if (!(lastQty > 0)) return false;

        if (nextToken(line, pos)) return false;

        event = Fill{.exchangeOrderId=std::string(*exchangeOrderId), .execId=std::string(*execId), .lastQty=lastQty};
    }
    else if (evntType == "CANCEL_ACK")
    {
        // CANCEL_ACK reqId
        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        if (nextToken(line, pos)) return false;

        event = CancelAck{.reqId=std::string(*reqId)};
    }
    else if (evntType == "CANCEL_REJECT")
    {
        // CANCEL_REJECT reqId
        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        if (nextToken(line, pos)) return false;

        event = CancelReject{.reqId=std::string(*reqId)};
    }
    else if (evntType == "REPLACE_ACK")
    {
        // REPLACE_ACK reqId
        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        if (nextToken(line, pos)) return false;

        event = ReplaceAck{.reqId=std::string(*reqId)};
    }
    else if (evntType == "REPLACE_REJECT")
    {
        // REPLACE_REJECT reqId

        auto reqId = nextToken(line, pos);
        if (!reqId) return false;

        if (nextToken(line, pos)) return false;

        event = ReplaceReject{.reqId=std::string(*reqId)};
    }
    else
    {
        ERROR << "unknown evntType=" << *evntType << std::endl;
        return false;
    }

    return true;
}

void run(std::string& inputs)
{
    StateMachine stateMachine;

    std::stringstream is(inputs);
    std::string line;
    int64_t lineNo = 0;
    while (std::getline(is, line))
    {
        ++lineNo;
        Event event;
        if (parse(line, event, lineNo))
        {
            INFO << "procssing lineNo=" << lineNo << ": " << line << std::endl;
            stateMachine.procss(event, lineNo, std::cout);
        }
        else
        {
            ERROR << "faile to parse: " << line << std::endl;
        }
    }
}

// To execute C++, please define "int main()"
int main()
{
    {
        std::string inputs= 
R"(NEW O1 N1 B 100 10
QUERY O1
NEW_ACK N1 V1
FILL V1 E1 3
CANCEL O1 C1
FILL V1 E2 2
QUERY O1
CANCEL_REJECT C1
FILL V1 E2 2
QUERY O1
CANCEL O1 C2
CANCEL_ACK C2
QUERY O1)";
        run(inputs);
    }

std::cout << "-------------------" << std::endl;

    {
        std::string inputs= 
R"(NEW O2 N2 S 200 10
NEW_ACK N2 V2
FILL V2 E3 4
REPLACE O2 R1 201 6
FILL V2 E4 3
QUERY O2
REPLACE_ACK R1
QUERY O2
CANCEL O2 C3)";
        run(inputs);
    }

    INFO << "done" << std::endl;
    return 0;
}
