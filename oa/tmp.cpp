#include <cctype>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <bits/stdc++.h>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <variant>
using namespace std;

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = true;
#define INFO enableInfo && std::cout << "[INFO " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR " << __LINE__ << "]: "

// To execute C++, please define "int main()"
// 15 PM

enum class State
{
    UP,
    DOWN,
    DEL
};

struct SourceEvent
{
    std::string source;
    int64_t ttl{};
};

struct TimeEvent
{
    int64_t now{};
};

struct SetSourceEvent
{
    std::string source;
    State state;
};

struct BBOEvent
{
    std::string symbol;    
};

struct UpdateEvent
{
    std::string source;
    std::string symbol;
    int64_t seq{};
    int64_t bidPrice{};
    int64_t bidQty{};
    int64_t askPrice{};
    int64_t askQty{};
};

struct DelEvent
{
    std::string source;
    std::string symbol;
    int64_t seq{};
};

using AggEvent = std::variant<SourceEvent, TimeEvent, SetSourceEvent, BBOEvent, UpdateEvent, DelEvent>;

struct MktData
{
    int64_t arriveAt{};
    int64_t ttl{};
    State state{State::UP};
    UpdateEvent updateEvent;
};

struct Level
{
    int64_t qty{};
    std::unordered_set<const MktData*> mktDatas;
};

class Clock
{
public:
    int64_t get() const
    {
        return ts_;
    }

    void set(int64_t ts)
    {
        ts_ = ts;
    }

private:
    int64_t ts_;
};

class TtlProvider
{
public:
    bool getTtl(const std::string& source, int64_t& ttl) const
    {
        if (auto it = ttls_.find(source); it != ttls_.end())
        {
            ttl = it->second;
            return true;
        }
        else
        {
            return false;
        }
    }

    bool setTtl(const std::string& source, int64_t ttl)
    {
        return ttls_.emplace(source, ttl).second;
    }

private:
    std::unordered_map<std::string, int64_t> ttls_;
};

class OrderBook
{
public:
    OrderBook(const std::string& symbol, const Clock& clock, const TtlProvider& ttlProvider)
    : symbol_(symbol)
    , clock_(clock)
    , ttlProvider_(ttlProvider)
    {
    }

    void update(const UpdateEvent& event)
    {
        int64_t ttl;
        if (!ttlProvider_.getTtl(event.source, ttl))
        {
            ERROR << "no ttl for source " << event.source << std::endl;
            return;
        }

        MktData* mktData{};
        if (auto it = map_.find(event.source); it != map_.end())
        {
            if (it->second.updateEvent.seq <= event.seq)
            {
                INFO << "skip mktData.seq=" << event.seq << std::endl;
            }
            else
            {
                // remove old
                if (it->second.state == State::UP && it->second.updateEvent.bidPrice > 0)
                {
                    auto oldBidIt = bids_.find(it->second.updateEvent.bidPrice);
                    oldBidIt->second.qty -= it->second.updateEvent.bidQty;
                    oldBidIt->second.mktDatas.erase(&it->second);
                    if (oldBidIt->second.mktDatas.empty())
                    {
                        bids_.erase(oldBidIt);
                    }
                }
                if (it->second.state == State::UP && it->second.updateEvent.askPrice > 0)
                {
                    auto oldAskIt = bids_.find(it->second.updateEvent.askPrice);
                    oldAskIt->second.qty -= it->second.updateEvent.askQty;
                    oldAskIt->second.mktDatas.erase(&it->second);
                    if (oldAskIt->second.mktDatas.empty())
                    {
                        bids_.erase(oldAskIt);
                    }
                }

                mktData = &it->second;
            }
        }
        else
        {
            // add new
            mktData = &map_[event.source];
            mktData->ttl = ttl;
        }

        mktData->arriveAt = clock_.get();
        mktData->updateEvent = event;

        if (mktData->state == State::UP && mktData->updateEvent.bidPrice > 0)
        {
            auto& bidLevel = bids_[mktData->updateEvent.bidPrice];
            bidLevel.qty += mktData->updateEvent.bidQty;
            bidLevel.mktDatas.insert(mktData);
        }
        if (mktData->state == State::UP && mktData->updateEvent.askPrice > 0)
        {
            auto& askLevel = asks_[mktData->updateEvent.askPrice];
            askLevel.qty += mktData->updateEvent.askQty;
            askLevel.mktDatas.insert(mktData);
        }                    
    }

    void del(const DelEvent& event)
    {
        if (auto it = map_.find(event.source); it != map_.end())
        {
            if (it->second.updateEvent.seq <= event.seq)
            {
                INFO << "skip mktData.seq=" << event.seq << std::endl;
            }
            else
            {
                // remove old
                if (it->second.state == State::UP && it->second.updateEvent.bidPrice > 0)
                {
                    auto oldBidIt = bids_.find(it->second.updateEvent.bidPrice);
                    oldBidIt->second.qty -= it->second.updateEvent.bidQty;
                    oldBidIt->second.mktDatas.erase(&it->second);
                    if (oldBidIt->second.mktDatas.empty())
                    {
                        bids_.erase(oldBidIt);
                    }
                }
                if (it->second.state == State::UP && it->second.updateEvent.askPrice > 0)
                {
                    auto oldAskIt = bids_.find(it->second.updateEvent.askPrice);
                    oldAskIt->second.qty -= it->second.updateEvent.askQty;
                    oldAskIt->second.mktDatas.erase(&it->second);
                    if (oldAskIt->second.mktDatas.empty())
                    {
                        bids_.erase(oldAskIt);
                    }
                }

                it->second.state = State::DEL;
            }
        }
    }

    void setSource(const std::string& source, State state)
    {
        if (auto it = map_.find(source); it != map_.end())
        {
            auto& mktData = it->second;
            if (state == State::UP)
            { 
                if (mktData.state == State::DOWN)
                {
                    // up it
                    if (mktData.updateEvent.bidPrice > 0)
                    {
                        auto& bidLevel = bids_[mktData.updateEvent.bidPrice];
                        bidLevel.qty += mktData.updateEvent.bidQty;
                        bidLevel.mktDatas.insert(&mktData);
                    }
                    if (mktData.updateEvent.askPrice > 0)
                    {
                        auto& askLevel = asks_[mktData.updateEvent.askPrice];
                        askLevel.qty += mktData.updateEvent.askQty;
                        askLevel.mktDatas.insert(&mktData);
                    }  

                    mktData.state = State::UP;
                }
                
            }
            else
            {
                if (mktData.state == State::UP)
                {
                    // down it
                    if (mktData.updateEvent.bidPrice > 0)
                    {
                        auto oldBidIt = bids_.find(mktData.updateEvent.bidPrice);
                        oldBidIt->second.qty -= mktData.updateEvent.bidQty;
                        oldBidIt->second.mktDatas.erase(&mktData);
                        if (oldBidIt->second.mktDatas.empty())
                        {
                            bids_.erase(oldBidIt);
                        }
                    }
                    if (mktData.updateEvent.askPrice > 0)
                    {
                        auto oldAskIt = bids_.find(mktData.updateEvent.askPrice);
                        oldAskIt->second.qty -= mktData.updateEvent.askQty;
                        oldAskIt->second.mktDatas.erase(&mktData);
                        if (oldAskIt->second.mktDatas.empty())
                        {
                            bids_.erase(oldAskIt);
                        }
                    }

                    mktData.state = State::DOWN;
                }
            }
        }
        else
        {
            ERROR << "failed to find for source " << source << std::endl;
        }
    }

    void bbo(std::ostream& os)
    {
        // BBO symbol bidPrice bidQty bidSources askPrice askQty askSources state
        int64_t bidPrice{};
        int64_t bidQty{};
        std::string bidSources;
        if (!bids_.empty())
        {
            bidPrice = bids_.begin()->first;
            bidQty = bids_.begin()->second.qty;
            for (const auto& mktData : bids_.begin()->second.mktDatas)
            {
                if (!bidSources.empty())
                    bidSources += ",";
                bidSources += mktData->updateEvent.source;
            }
        }
        int64_t askPrice{};
        int64_t askQty{};
        std::string askSources{};
        if (!asks_.empty())
        {
            askPrice = asks_.begin()->first;
            askQty = asks_.begin()->second.qty;
            for (const auto& mktData : asks_.begin()->second.mktDatas)
            {
                if (!askSources.empty())
                    askSources += ",";
                askSources += mktData->updateEvent.source;
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
        os << "BBO"
            << " " << symbol_
            << " " << (bidPrice == 0 ? "-" : std::to_string(bidPrice))
            << " " << bidQty
            << " " << (bidSources.empty() ? "-" : bidSources)
            << " " << (askPrice == 0 ? "-" : std::to_string(askPrice))
            << " " << askQty
            << " " << (askSources.empty() ? "-" : askSources)
            << " " << state;
    }

private:
    const std::string& symbol_;
    const Clock& clock_;
    const TtlProvider& ttlProvider_;
    std::map<int64_t, Level, std::greater<>> bids_; // dec
    std::map<int64_t, Level> asks_; // asc

    // <source, mktData>
    std::unordered_map<std::string, MktData> map_;
};

class MktDatatAgg
{
public:
    void process(const AggEvent& event)
    {
        std::visit([this](const auto& e) { processImpl(e); }, event);
    }

    void processImpl(const SourceEvent& event)
    {
        if (!ttlProvider_.setTtl(event.source, event.ttl))
        {
            ERROR << "failed to set ttl for source " << event.source << std::endl;
        }
    }

    void processImpl(const SetSourceEvent& event)
    {
        for (auto& [_, ob] : orderBooks_)
        {
            ob->setSource(event.source, event.state);
        }
    }

    void processImpl(const BBOEvent& event)
    {
        getOrderBook(event.symbol).bbo(std::cout);
    }

    void processImpl(const UpdateEvent& event)
    {
        getOrderBook(event.symbol).update(event);
    }

    void processImpl(const DelEvent& event)
    {
        getOrderBook(event.symbol).del(event);
    }

    void processImpl(const TimeEvent& event)
    {
        clock_.set(event.now);
    }

private:
    OrderBook& getOrderBook(const std::string& symbol)
    {
        if (auto it = orderBooks_.find(symbol); it != orderBooks_.end())
        {
            return *it->second;
        }
        else
        {
            auto ob = std::make_unique<OrderBook>(symbol, clock_, ttlProvider_);
            return *orderBooks_.emplace(symbol, std::move(ob)).first->second;
        }
    };

    Clock clock_{};
    TtlProvider ttlProvider_{};
    // <symbol, ob>
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> orderBooks_;
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

bool parse(std::string_view line, AggEvent& event)
{
    size_t pos{};
    auto evenType = nextToken(line, pos);
    if (evenType == "SOURCE")
    {
        // SOURCE source ttl
        auto source = nextToken(line, pos); 
        if (!source) return false;

        int64_t ttl;
        if (!nextNumber(line, pos, ttl)) return false;

        return !nextToken(line, pos);
    }
    else if (evenType == "TIME")
    {
        // TIME now
        int64_t now;
        if (!nextNumber(line, pos, now)) return false;

        return !nextToken(line, pos);
    }
    else if (evenType == "SETSOURCE")
    {
        // SETSOURCE source state
        auto source = nextToken(line, pos);   
        if (!source) return false;

        auto state = nextToken(line, pos);   
        if (!(state == "UP" || state == "DOWN")) return false;

        return !nextToken(line, pos);
    }
    else if (evenType == "BBO")
    {
        // BBO symbol
        auto symbol = nextToken(line, pos);
        if (!symbol) return false;

        return !nextToken(line, pos);
    }
    else if (evenType == "UPDATE")
    {
        // UPDATE source symbol seq bidPrice bidQty askPrice askQty
        auto source = nextToken(line, pos); 
        if (!source) return false;

        auto symbol = nextToken(line, pos);
        if (!symbol) return false;

        int64_t seq;
        if (!nextNumber(line, pos, seq)) return false;

        int64_t bidPrice;
        if (!nextNumber(line, pos, bidPrice)) return false;

        int64_t bidQty;
        if (!nextNumber(line, pos, bidQty)) return false;

        int64_t askPrice;
        if (!nextNumber(line, pos, askPrice)) return false;

        int64_t askQty;
        if (!nextNumber(line, pos, askQty)) return false;

        return !nextToken(line, pos);
    }
    else if (evenType == "DELETE")
    {
        // DELETE source symbol seq
        auto source = nextToken(line, pos); 
        if (!source) return false;

        auto symbol = nextToken(line, pos);
        if (!symbol) return false;

        int64_t seq;
        if (!nextNumber(line, pos, seq)) return false;

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
    while (std::getline(iss, line))
    {
        AggEvent event;
        if (parse(line, event))
        {
            mktDatatAgg.process(event);
        }
        else
        {
            ERROR << "failed to parse " << line << std::endl;
        }
    }
}

int main()
{
    INFO << "done" << std::endl;
    return 0;
}
