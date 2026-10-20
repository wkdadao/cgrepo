#include <iostream>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <optional>
#include <sstream>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR " << __LINE__ << "]: "

namespace agg
{

enum class State
{
    UP = 0,
    DOWN = 1
};

struct DelEvent
{
    std::string source;
    std::string symbol;
    int64_t seq;
    inline auto operator<=> (const DelEvent&) const = default;
};

struct UpdateEvent: DelEvent
{
    int64_t bidPrice{};
    int64_t bidQty{};
    int64_t askPrice{};
    int64_t askQty{};
    inline auto operator<=> (const UpdateEvent&) const = default;
};

using ExpireSet = std::set<std::tuple<int64_t, std::string, std::string>>;

enum class EventType
{
    UPDATE,
    DELETE
};
struct MktData;
struct MktDataCmp
{
    bool operator()(MktData* a, MktData* b) const;
};
struct MktData
{
    EventType eventType{};
    int64_t expireAt{};
    bool bidIndexed{};
    bool askIndexed{};
    std::set<MktData*, MktDataCmp>::iterator bidIndexedIt;
    std::set<MktData*, MktDataCmp>::iterator askIndexedIt;
    ExpireSet::iterator expireIt;
    UpdateEvent event;
};

bool MktDataCmp::operator()(MktData* a, MktData* b) const { return a->event.source < b->event.source; }

std::ostream& operator<< (std::ostream& os, const MktData& mktData)
{
    os << "expireAt=" << mktData.expireAt
        << "/bidIndexed=" << mktData.bidIndexed
        << "/askIndexed=" << mktData.askIndexed
        << "/bidQty=" << mktData.event.bidQty
        << "/askQty=" << mktData.event.askQty
        << "/source=" << mktData.event.source;

    return os;
}

struct Level
{
    std::set<MktData*, MktDataCmp> mktDatas;
};
std::ostream& operator<< (std::ostream& os, const Level& level)
{
    os << "level: ";
    for (const auto& mktData : level.mktDatas)
    {
        os << *mktData << " ";
    }
    os << "\n";
    return os;
}

struct AscCmp
{
    bool asc{};
    inline bool operator()(auto&& a, auto&& b) const { return asc ? a < b : a > b; }
};

struct OrderBook
{
    std::map<int64_t, Level, AscCmp> bids{AscCmp{false}}; // desc, 5, 4, 3
    std::map<int64_t, Level, AscCmp> asks{AscCmp{true}};  // asc, 6, 7, 8
    std::unordered_map<std::string, MktData> allMktDatas;
};
std::ostream& operator<< (std::ostream& os, const OrderBook& ob)
{
    os << "\nbids:\n";
    for (const auto& bid : ob.bids)
    {
        os << bid.second;
    }
    os << "\nasks:\n";
    for (const auto& ask : ob.asks)
    {
        os << ask.second;
    }
    os << "\n";
    return os;
}

struct SourceEvent
{
    std::string source;
    int64_t ttl;
};

struct SetSourceEvent
{
    std::string source;
    State state;
};

struct TimeEvent
{
    int64_t now;
};

struct BBOEvent
{
    std::string symbol;
};

using AggEvent = std::variant<UpdateEvent, DelEvent, SourceEvent, SetSourceEvent, TimeEvent, BBOEvent>;

class MktDatatAgg
{
public:
    bool process(const AggEvent& aggEvent)
    {
        return std::visit([this](auto&& event) { return on(event); }, aggEvent);
    }

private:
    bool on(const UpdateEvent& event)
    {
        if (!ttls_.contains(event.source)) return false;

        auto& orderBook = orderBooks_[event.symbol];
        auto& mktData = orderBook.allMktDatas[event.source];

        if (mktData.event.seq != 0) // old one
        {
            if (mktData.event.seq > event.seq)
            {
                INFO << "ignore small seq: " << event.seq << std::endl;
                return true;
            }
            if (mktData.event.seq == event.seq)
            {
                if (mktData.event == event && mktData.eventType == EventType::UPDATE)
                {
                    INFO << "ignore same seq: " << event.seq << std::endl;
                    return true;
                }
                else
                {
                    INFO << "got wrong same seq: " << event.seq << std::endl;
                    return false;
                }
            }

            removeFromIndex(orderBook, mktData);

            expires_.erase(std::tuple(mktData.expireAt, event.source, event.symbol));
        }

        mktData.expireAt = clock_ + ttls_[event.source];
        mktData.expireIt = expires_.emplace(mktData.expireAt, event.source, event.symbol).first;
        mktData.eventType = EventType::UPDATE;
        mktData.event = event;
        if (states_[event.source] == State::UP)
        {
            addToIndex(orderBook, mktData);
            ups_[event.source].insert(event.symbol);
        }
        else
        {
            downs_[event.source].insert(event.symbol);
        }

        INFO << "after update: symbol=" << event.symbol << ", source=" << event.source << " " << orderBook << std::endl;

        return true;
    }

    bool on(const DelEvent& event)
    {
        if (!ttls_.contains(event.source)) return false;

        auto& orderBook = orderBooks_[event.symbol];
        auto& mktData = orderBook.allMktDatas[event.source];

        if (mktData.event.seq != 0) // old one
        {
            if (mktData.event.seq > event.seq)
            {
                INFO << "ignore small seq: " << event.seq << std::endl;
                return true;
            }
            if (mktData.event.seq == event.seq)
            {
                if (mktData.event == event && mktData.eventType == EventType::DELETE)
                {
                    INFO << "ignore same seq: " << event.seq << std::endl;
                    return true;
                }
                else
                {
                    INFO << "got wrong same seq: " << event.seq << std::endl;
                    return false;
                }
            }

            expires_.erase(std::tuple(mktData.expireAt, event.source, event.symbol));
        }

        mktData.eventType = EventType::DELETE;
        DelEvent& DelEvent = mktData.event;
        DelEvent = event;
        removeFromIndex(orderBook, mktData);

        ups_[event.source].erase(event.symbol);
        downs_[event.source].erase(event.symbol);

        INFO << "after delete: symbol=" << event.symbol << ", source=" << event.source << " " << orderBook << std::endl;

        return true;
    }

    bool on(const SourceEvent& event)
    {
        if (ttls_.contains(event.source)) return false;

        ttls_[event.source] = event.ttl;
        return true;
    }

    bool on(const SetSourceEvent& event)
    {
        if (!ttls_.contains(event.source)) return false;

        applyState(event.source, states_[event.source], event.state);
        states_[event.source] = event.state;
        return true;
    }

    bool on(const TimeEvent& event)
    {
        if (clock_ > event.now)
        {
            INFO << "now is old: now=" << event.now << ", clock=" << clock_ << std::endl;
            return false;
        }

        clock_ = event.now;
        applyExpire();
        return true;
    }

    bool on(const BBOEvent& event)
    {
        const auto& orderBook = orderBooks_[event.symbol];

        // BBO symbol bidPrice bidQty bidSources askPrice askQty askSources state
        int64_t bidPrice{};
        int64_t bidQty{};
        std::string bidSources;
        if (!orderBook.bids.empty())
        {
            bidPrice = orderBook.bids.begin()->first;
            for (const auto& mktData : orderBook.bids.begin()->second.mktDatas)
            {
                bidQty += mktData->event.bidQty;
                if (!bidSources.empty())
                    bidSources += ",";
                bidSources += mktData->event.source;
            }
        }
        int64_t askPrice{};
        int64_t askQty{};
        std::string askSources{};
        if (!orderBook.asks.empty())
        {
            askPrice = orderBook.asks.begin()->first;
            for (const auto& mktData : orderBook.asks.begin()->second.mktDatas)
            {
                askQty += mktData->event.askQty;
                if (!askSources.empty())
                    askSources += ",";
                askSources += mktData->event.source;
            }
        }

        const char* state = "";
        
        // 两侧都不存在	EMPTY
        if (bidPrice == 0 && askPrice == 0)
        {
            state = "EMPTY";
        }
        // 
        else if (bidPrice != 0 && askPrice == 0)
        {
            state = "BID_ONLY";
        }
        else if (bidPrice == 0 && askPrice != 0)
        {
            state = "ASK_ONLY";
        }
        else if (bidPrice < askPrice)
        {
            state = "NORMAL";
        }
        else if (bidPrice == askPrice)
        {
            state = "LOCKED";
        }
        else if (bidPrice > askPrice)
        {
            state = "CROSSED";
        }

        // BBO symbol bidPrice bidQty bidSources askPrice askQty askSources state
        std::cout << "BBO"
            << " " << event.symbol 
            << " " << (bidPrice == 0 ? "-" : std::to_string(bidPrice))
            << " " << bidQty
            << " " << (bidSources.empty() ? "-" : bidSources)
            << " " << (askPrice == 0 ? "-" : std::to_string(askPrice))
            << " " << askQty
            << " " << (askSources.empty() ? "-" : askSources)
            << " " << state << std::endl;

        return true;
    }

private:
    void applyExpire()
    {
        // Empty in the screenshots; expiration behavior not available.
        while (!expires_.empty() && std::get<0>(*expires_.begin()) <= clock_)
        {
            const auto& [_, source, symbol] = *expires_.begin();
            auto& orderBook = orderBooks_[symbol];
            auto& mktData = orderBook.allMktDatas[source];

            removeFromIndex(orderBook, mktData);
            ups_[source].erase(symbol);
            downs_[source].erase(symbol);
            expires_.erase(expires_.begin());
        }
    }

    void applyState(const std::string& source, State oldState, State newState)
    {
        if (oldState == newState) return;
        applyState(source, newState);
    }

    void applyState(const std::string& source, State newState)
    {
        if (newState == State::UP)
        {
            for (const auto& symbol : downs_[source])
            {
                auto& orderBook = orderBooks_[symbol];
                addToIndex(orderBook, orderBook.allMktDatas[source]);
                ups_[source].insert(symbol);
            }
        }
        else
        {
            for (const auto& symbol : ups_[source])
            {
                auto& orderBook = orderBooks_[symbol];
                removeFromIndex(orderBook, orderBook.allMktDatas[source]);
                downs_[source].insert(symbol);
            }
        }
    }

    void addToIndex(OrderBook& orderBook, MktData& mktData)
    {
        if (mktData.event.bidQty > 0)
        {
            auto& mktDatas = orderBook.bids[mktData.event.bidPrice].mktDatas;
            mktData.bidIndexedIt = mktDatas.insert(&mktData).first;
            mktData.bidIndexed = true;
        }
        if (mktData.event.askQty > 0)
        {
            auto& mktDatas = orderBook.asks[mktData.event.askPrice].mktDatas;
            mktData.askIndexedIt = mktDatas.insert(&mktData).first;
            mktData.askIndexed = true;
        }
    }

    void removeFromIndex(OrderBook& orderBook, MktData& mktData)
    {
        INFO << "before removeFromIndex: mktData=" << mktData << std::endl;
        if (mktData.bidIndexed)
        {
            if (auto it = orderBook.bids.find(mktData.event.bidPrice); it != orderBook.bids.end())
            {
                it->second.mktDatas.erase(mktData.bidIndexedIt);
                if (it->second.mktDatas.empty())
                {
                    orderBook.bids.erase(it);
                }
            }
            mktData.bidIndexed = false;
        }

        if (mktData.askIndexed)
        {
            if (auto it = orderBook.asks.find(mktData.event.askPrice); it != orderBook.asks.end())
            {
                it->second.mktDatas.erase(mktData.askIndexedIt);
                if (it->second.mktDatas.empty())
                {
                    orderBook.asks.erase(it);
                }
            }
            mktData.askIndexed = false;
        }
        INFO << "after removeFromIndex: mktData=" << mktData << std::endl;
    }

    std::unordered_map<std::string, OrderBook> orderBooks_;
    int64_t clock_{};

    // <expireAt, symbol, source> (as labeled in the screenshot)
    std::set<std::tuple<int64_t, std::string, std::string>> expires_;

    // <source, state>
    std::unordered_map<std::string, State> states_;
    std::unordered_map<std::string, std::unordered_set<std::string>> ups_;
    std::unordered_map<std::string, std::unordered_set<std::string>> downs_;

    // <source, ttl>
    std::unordered_map<std::string, int64_t> ttls_;
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
    
    if (token == "-")
    {
        number = 0;
        return true;
    }

    auto [ptr, ec] = std::from_chars(token->data(), token->data() + token->size(), number);
    return ptr == token->data() + token->size() && ec == std::errc();
}

/*
source	行情来源，长度 1～16，仅含 ASCII 字母、数字或下划线，区分大小写
symbol	品种标识，格式与 source 相同
ttl	来源报价的有效时长，整数，范围 1～10^9
now	全局逻辑时间，整数，范围 0～10^12
seq	消息序号，整数，范围 1～10^12
price	价格，整数，范围 1～10^9
qty	数量，整数，范围 1～10^9
*/

bool badId(std::string_view id)
{
    for (const char c : id)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return id.size() < 1 || id.size() > 16;
}

bool badTtl(int64_t ttl)
{
    return ttl < 1 || ttl > int64_t{1000000000};
}

bool badPrice(int64_t price)
{
    return price < 0 || price > int64_t{1000000000};
}

bool badQty(int64_t qty)
{
    return qty < 0 || qty > int64_t{1000000000};
}

bool badNow(int64_t now)
{
    return now < 1 || now > int64_t{1000000000000};
}

bool badSeq(int64_t seq)
{
    return seq < 1 || seq > int64_t{1000000000000};
}

bool parse(std::string_view line, AggEvent& event)
{
    size_t pos{};
    auto evenType = nextToken(line, pos);
    if (evenType == "SOURCE")
    {
        // SOURCE source ttl
        auto source = nextToken(line, pos); 
        if (!source || badId(*source)) return false;

        int64_t ttl;
        if (!nextNumber(line, pos, ttl)) return false;
        if (badTtl(ttl)) return false;

        event = SourceEvent{.source=std::string(*source), .ttl=ttl};
        return !nextToken(line, pos);
    }
    else if (evenType == "TIME")
    {
        // TIME now
        int64_t now;
        if (!nextNumber(line, pos, now)) return false;
        if (badNow(now)) return false;

        event = TimeEvent{.now=now};
        return !nextToken(line, pos);
    }
    else if (evenType == "SETSOURCE")
    {
        // SETSOURCE source state
        auto source = nextToken(line, pos);   
        if (!source || badId(*source)) return false;

        auto state = nextToken(line, pos);   
        if (!(state == "UP" || state == "DOWN")) return false;

        event = SetSourceEvent{.source=std::string(*source), .state=(state == "UP" ? State::UP : State::DOWN)};
        return !nextToken(line, pos);
    }
    else if (evenType == "BBO")
    {
        // BBO symbol
        auto symbol = nextToken(line, pos);
        if (!symbol || badId(*symbol)) return false;

        event = BBOEvent{.symbol=std::string(*symbol)};
        return !nextToken(line, pos);
    }
    else if (evenType == "UPDATE")
    {
        // UPDATE source symbol seq bidPrice bidQty askPrice askQty
        auto source = nextToken(line, pos); 
        if (!source || badId(*source)) return false;

        auto symbol = nextToken(line, pos);
        if (!symbol || badId(*symbol)) return false;

        int64_t seq;
        if (!nextNumber(line, pos, seq)) return false;
        if (badSeq(seq)) return false;

        int64_t bidPrice;
        if (!nextNumber(line, pos, bidPrice)) return false;
        if (badPrice(bidPrice)) return false;

        int64_t bidQty;
        if (!nextNumber(line, pos, bidQty)) return false;
        if (badQty(bidQty)) return false;

        int64_t askPrice;
        if (!nextNumber(line, pos, askPrice)) return false;
        if (badPrice(askPrice)) return false;

        int64_t askQty;
        if (!nextNumber(line, pos, askQty)) return false;
        if (badQty(askQty)) return false;

        if (bidPrice == 0 && askPrice == 0) return false;
        if (bidPrice > 0 && askPrice > 0 && bidPrice > askPrice) return false;

        UpdateEvent updateEvent{};
        updateEvent.source = std::string(*source);
        updateEvent.symbol = std::string(*symbol);
        updateEvent.seq=seq;
        updateEvent.bidPrice=bidPrice;
        updateEvent.bidQty=bidQty;
        updateEvent.askPrice=askPrice;
        updateEvent.askQty=askQty;
        event = updateEvent;
        return !nextToken(line, pos);
    }
    else if (evenType == "DELETE")
    {
        // DELETE source symbol seq
        auto source = nextToken(line, pos); 
        if (!source) return false;
        if (!source || badId(*source)) return false;

        auto symbol = nextToken(line, pos);
        if (!symbol) return false;
        if (!symbol || badId(*symbol)) return false;

        int64_t seq;
        if (!nextNumber(line, pos, seq)) return false;
        if (badSeq(seq)) return false;

        event = DelEvent{
            .source = std::string(*source),
            .symbol = std::string(*symbol),
            .seq=seq};
        return !nextToken(line, pos);
    }
    else
    {
        return false;
    }
}

void run(const std::string& inputs)
{
    MktDatatAgg mktDatatAgg;

    std::stringstream iss(inputs);
    std::string line;
    int64_t lineNo{};
    while (std::getline(iss, line))
    {
        ++lineNo;
        AggEvent event;
        INFO << "processing[" << lineNo << "]: " << line << std::endl;
        if (parse(line, event))
        {
            if (!mktDatatAgg.process(event))
            {
                std::cout << "ERROR " << lineNo << std::endl;       
            }
        }
        else
        {
            INFO << "failed to parse: " << line << std::endl;
            std::cout << "ERROR " << lineNo << std::endl;
        }
    }
}

} // namespace agg

int main()
{
/*
    {
        std::string inputs =
R"(SOURCE V2 50
SOURCE V1 50
SOURCE V3 50
UPDATE V2 ABC 1 100 7 103 8
UPDATE V1 ABC 1 100 5 102 6
UPDATE V3 ABC 1 99 20 102 4
BBO ABC
UPDATE V3 ABC 2 101 9 - -
BBO ABC
UPDATE V2 ABC 2 - - 101 3
BBO ABC
UPDATE V1 ABC 2 102 2 104 1
BBO ABC
DELETE V1 ABC 3
DELETE V3 ABC 3
BBO ABC
DELETE V2 ABC 3
BBO ABC)";
        agg::run(inputs);
    }
    {
        std::string inputs =
R"(SOURCE X 10
TIME 100
UPDATE X EUR 5 200 2 202 3
TIME 109
UPDATE X EUR 5 200 2 202 3
BBO EUR
TIME 110
BBO EUR
UPDATE X EUR 4 199 1 203 1
UPDATE X EUR 5 200 2 202 3
BBO EUR
UPDATE X EUR 5 201 2 202 3
UPDATE X EUR 6 200 2 202 3
BBO EUR
DELETE X EUR 7
UPDATE X EUR 6 201 5 203 5
BBO EUR
DELETE X EUR 7
UPDATE X EUR 7 200 2 202 3
UPDATE X EUR 8 - - 205 4
BBO EUR
TIME 120
BBO EUR)";
        agg::run(inputs);
    }
    {
        std::string inputs =
R"(SOURCE A 5
SOURCE B 20
UPDATE A XYZ 1 100 3 104 4
UPDATE B XYZ 1 99 5 105 6
BBO XYZ
SETSOURCE A DOWN
BBO XYZ
TIME 4
UPDATE A XYZ 2 101 7 103 8
BBO XYZ
SETSOURCE A UP
BBO XYZ
TIME 9
BBO XYZ
SETSOURCE B DOWN
BBO XYZ
SETSOURCE A UP
BBO XYZ
SETSOURCE B UP
BBO XYZ
TIME 20
BBO XYZ)";
        agg::run(inputs);
    }
*/
 {
        std::string inputs =
R"(SOURCE A 10
SOURCE B 10
UPDATE A AAA 5 100 2 103 3
UPDATE A BBB 1 200 1 203 1
UPDATE B AAA 1 101 4 104 5
BBO AAA
UPDATE A AAA 0 100 2 103 3
UPDATE A AAA 5 102 2 103 3
UPDATE A AAA 4 105 2 103 3
TIME 8
TIME 7
UPDATE B AAA 2 - - - -
UPDATE A AAA 6 - 4 103 3
SOURCE A 10
SETSOURCE C DOWN
BBO AAA
DELETE B CCC 9
UPDATE B CCC 8 100 1 102 1
BBO CCC
UPDATE B CCC 10 102 1 - -
BBO CCC
TIME 10
BBO AAA
BBO BBB
BBO CCC)";
        agg::run(inputs);
    }

    INFO << "done" << std::endl;
    return 0;
}