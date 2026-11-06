#include <bits/stdc++.h>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "


enum class State
{
    SYNCED, // 当前没有等待中的乱序增量；已知数据全部连续应用
    GAPPED, // 存在缺失 Sequence，尚未接受当前缺口的恢复消息
    RECOVERING // 缺口仍存在，但已经接受过当前缺口的有效 Replay，或进行过有效 Snapshot 恢复
};
const char* str(State state)
{
    switch(state)
    {
        case State::SYNCED: return "SYNCED";
        case State::GAPPED: return "GAPPED";
        case State::RECOVERING: return "RECOVERING";
    }
    return "";
}

struct PriceInfo
{
    int64_t seq;
    std::string symbol{};
    int64_t price{};
    int64_t qty{};

    inline auto operator<=> (const PriceInfo&) const = default;
};

// UPDATE seq symbol price qty
struct Update
{
    PriceInfo priceInfo;
};

// REPLAY seq symbol price qty
struct Replay
{
    PriceInfo priceInfo;
};

// SNAPSHOT seq k symbol1 price1 qty1 ... symbolk pricek qtyk
struct Snapshot
{
    int64_t seq{};
    std::vector<PriceInfo> priceInfos;
};

struct Status{};
struct Query{ std::string symbol; };
struct Gaps{};

using Event = std::variant<Update, Replay, Snapshot, Status, Query, Gaps>;

/*
N
UPDATE seq symbol price qty
REPLAY seq symbol price qty
SNAPSHOT seq k [symbol1 price1 qty1 ... symbolk pricek qtyk]
STATUS
QUERY symbol
GAPS

*/
std::vector<Event> parse(std::istream& is)
{
    std::vector<Event> events;

    std::string line;

    if (!std::getline(is, line)) throw "invalid";
    std::stringstream ss1(line);
    int cnt;
    if (!(ss1 >> cnt))  throw "invalid";
    INFO << "cnt=" << cnt << std::endl;

    for (int i = 0; i < cnt; ++i)
    {
        if (!std::getline(is, line)) throw "invalid";
        std::stringstream ss2(line);

        std::string op;
        if (!(ss2 >> op)) throw "invalid";

        INFO << "op=" << op << std::endl;
        if (op == "UPDATE")
        {
            Update update{};
            if (!(ss2 >> update.priceInfo.seq >> update.priceInfo.symbol >> update.priceInfo.price >> update.priceInfo.qty)) throw "invalid";
            events.push_back(update);
        }
        else if (op == "REPLAY")
        {
            Replay replay{};
            if (!(ss2 >> replay.priceInfo.seq >> replay.priceInfo.symbol >> replay.priceInfo.price >> replay.priceInfo.qty)) throw "invalid";
            events.push_back(replay);
        }
        else if (op == "SNAPSHOT")
        {
            Snapshot snapshot;
            int64_t seq{};
            int k{};
            if (!(ss2 >> seq >> k)) throw "invalid";
            INFO << "seq=" << seq << ", k=" << k << std::endl;
            for (int j = 0; j < k; ++j)
            {
                PriceInfo priceInfo{};
                priceInfo.seq = seq;
                if (!(ss2 >> priceInfo.symbol >> priceInfo.price >> priceInfo.qty)) throw "invalid";
                snapshot.priceInfos.push_back(std::move(priceInfo));
            }

            snapshot.seq = seq;
            events.push_back(snapshot);
        }
        else if (op == "STATUS")
        {
            events.push_back(Status{});
        }
        else if (op == "QUERY")
        {
            Query query{};
            if (!(ss2 >> query.symbol)) throw "invalid";
            events.push_back(query);
        }
        else if (op == "GAPS")
        {
            events.push_back(Gaps{});
        }
        else
        {
            throw "invalid";
        }
    }

    return events;
}

class Mgr
{
public:
    void run(std::istream& is, std::ostream& os)
    {
        const auto events = parse(is);
        for (const auto& event : events)
        {
            std::visit([&](auto&& e) { apply(e, os); }, event);
        }
    }

private:
    void apply(const Update& e, std::ostream& os)
    {
        INFO << "Update, seq=" << e.priceInfo.seq << std::endl;

        if (e.priceInfo.seq <= appliedSeq_)
        {
            INFO << "seq <= appliedSeq：这是旧消息或已应用消息，输出 IGNORED，不修改系统。" << std::endl;
            os << "IGNORED\n";
            return;
        }

        auto it = buffer_.find(e.priceInfo.seq);
        if (it != buffer_.end())
        {
            if (it->second == e.priceInfo)
            {
                INFO << "symbol、price、qty 都与已缓存内容相同，输出 IGNORED" << std::endl;
                os << "IGNORED\n";
                return;
            }
            else
            {
                INFO << "同一 Sequence 的消息内容冲突，输出 ERROR，状态完全不变" << std::endl;
                os << "ERROR\n";
                return;
            }
        }

        if (e.priceInfo.seq == appliedSeq_ + 1)
        {
            ++appliedSeq_;
            submitPriceInfo(e.priceInfo);
            tryContineSubmit();
            if (buffer_.empty()) feedState_ = State::SYNCED;
            
            INFO << "seq == appliedSeq + 1, OK" << std::endl;
            os << "OK\n";
        }
        else
        {
            buffer_[e.priceInfo.seq] = e.priceInfo;
            if (feedState_ == State::SYNCED) feedState_ = State::GAPPED;
            
            INFO << "seq > appliedSeq + 1, BUFFERED, highSeq_=" << highSeq_ << std::endl;
            os << "BUFFERED\n";
        }

        highSeq_ = std::max(highSeq_, e.priceInfo.seq);
    }

    void apply(const Replay& e, std::ostream& os)
    {
        INFO << "Replay, seq=" << e.priceInfo.seq << std::endl;

        if (e.priceInfo.seq <= appliedSeq_)
        {
            INFO << "seq <= appliedSeq，输出 IGNORED" << std::endl;
            os << "IGNORED\n";
            return;
        }

        auto it = buffer_.find(e.priceInfo.seq);
        if (it != buffer_.end())
        {
            if (it->second == e.priceInfo)
            {
                INFO << "内容完全相同：IGNORED" << std::endl;
                os << "IGNORED\n";
                return;
            }
            else
            {
                INFO << "内容冲突：ERROR" << std::endl;
                os << "ERROR\n";
                return;
            }
        }

        if (!buffer_.empty() && appliedSeq_ < e.priceInfo.seq && e.priceInfo.seq <= highSeq_)
        {
            if (e.priceInfo.seq == appliedSeq_ + 1)
            {
                ++appliedSeq_;
                submitPriceInfo(e.priceInfo);
                tryContineSubmit();
                if (buffer_.empty())
                {
                    feedState_ = State::SYNCED;
                }
                else
                {
                    feedState_ = State::RECOVERING;
                }

                INFO << "seq == appliedSeq + 1, OK" << std::endl;
                os << "OK\n";
                return;
            }
            else
            {
                buffer_[e.priceInfo.seq] = e.priceInfo;
                feedState_ = State::RECOVERING;

                INFO << "seq > appliedSeq + 1, BUFFERED, highSeq_=" << highSeq_ << std::endl;
                os << "BUFFERED\n";
                return;
            }
        }
        else
        {
            INFO << "不符合这些条件输出 ERROR" << std::endl;
            os << "ERROR\n";
            return;
        }
    }

    void apply(const Snapshot& e, std::ostream& os)
    {
        INFO << "snapshot, seq=" << e.seq << std::endl;

        if (!(e.seq >= appliedSeq_))
        {
            INFO << "不接受回退到已提交进度之前的旧快照。" << std::endl;
            os << "ERROR\n";
            return;
        }

        std::set<std::string> symbols;
        for (const auto& info : e.priceInfos)
        {
            if (info.price <= 0 || info.qty <= 0)
            {
                INFO << "price > 0 且 qty > 0" << std::endl;
                os << "ERROR\n";
                return;
            }
            symbols.insert(info.symbol);
        }
        if (symbols.size() != e.priceInfos.size())
        {
            INFO << "Symbol 名称在本次 Snapshot 内不能重复" << std::endl;
            os << "ERROR\n";
            return;
        }

        priceInfos_.clear();
        for (const auto& info : e.priceInfos)
        {
            submitPriceInfo(info);
        }

        appliedSeq_ = e.seq;

        // 丢弃缓存中所有 seq <= snapshot seq 的消息
        while (!buffer_.empty() && buffer_.begin()->second.seq <= appliedSeq_)
        {
            buffer_.erase(buffer_.begin());
        }

        tryContineSubmit();

        if (buffer_.empty())
        {
            feedState_ = State::SYNCED;
        }
        else
        {
            feedState_ = State::RECOVERING;
        }
        os << "OK\n";

        highSeq_ = std::max(highSeq_, e.seq);
        INFO << "after snapshot: appliedSeq_=" << appliedSeq_ << ", highSeq_=" << highSeq_ << ", buffer size=" << buffer_.size() << std::endl;
    }

    void apply(const Status&, std::ostream& os)
    {
        // STATUS state appliedSeq highSeq bufferedCount
        os << "STATUS " << str(feedState_) << " " << appliedSeq_ << " " << highSeq_ << " " << buffer_.size() << "\n";

    }

    void apply(const Query& e, std::ostream& os)
    {
        auto it = priceInfos_.find(e.symbol);
        if (it == priceInfos_.end())
        {
            INFO << "NOT_FOUND" << std::endl;
            os << "NOT_FOUND\n";
            return;
        }

        // QUOTE symbol price qty freshness
        os << "QUOTE " << e.symbol << " " << it->second.price << " " << it->second.qty << " " << (feedState_ == State::SYNCED ? "LIVE" : "STALE") << "\n";
    }

    void apply(const Gaps&, std::ostream& os)
    {
        if (buffer_.empty())
        {
            os << "GAPS 0\n";
            return;
        }

        std::vector<std::pair<int64_t, int64_t>> gaps;

        auto lastSeq = appliedSeq_;
        for (auto it = buffer_.begin(); it != buffer_.end(); ++it)
        {
            if (it->first != lastSeq + 1)
            {
                gaps.push_back({lastSeq + 1, it->first - 1});
            }
            lastSeq = it->first;
        }

        os << "GAPS " << gaps.size();
        for (const auto& gap : gaps)
        {
            os << " " << gap.first << " " << gap.second;
        }
        os << "\n";
    }

    void tryContineSubmit()
    {
        while (!buffer_.empty() && buffer_.begin()->second.seq == appliedSeq_ + 1 )
        {
            submitPriceInfo(buffer_.begin()->second);
            INFO << "applying, symbol=" << buffer_.begin()->second.symbol << ", qty=" << buffer_.begin()->second.qty << std::endl;
            buffer_.erase(buffer_.begin());
            ++appliedSeq_;
        }

        INFO << "appliedSeq_=" << appliedSeq_ << ", buffer size=" << buffer_.size() << std::endl;
    }

    void submitPriceInfo(const PriceInfo& priceInfo)
    {
        if (priceInfo.qty == 0)
        {
            priceInfos_.erase(priceInfo.symbol);
        }
        else
        {
            priceInfos_[priceInfo.symbol] = priceInfo;
        }
    }

    int64_t appliedSeq_{};
    int64_t highSeq_{};
    State feedState_{State::SYNCED};
    std::map<int64_t, PriceInfo> buffer_;
    std::unordered_map<std::string, PriceInfo> priceInfos_;
};

void test()
{
    {
        std::string input = R"(16
SNAPSHOT 99 1 AAPL 100 10
STATUS
UPDATE 100 AAPL 101 9
UPDATE 101 MSFT 200 5
UPDATE 104 AAPL 104 4
STATUS
QUERY AAPL
GAPS
UPDATE 103 MSFT 203 6
GAPS
REPLAY 102 AAPL 102 7
STATUS
QUERY AAPL
QUERY MSFT
GAPS
REPLAY 103 MSFT 203 6
)";
        std::string output = R"(OK
STATUS SYNCED 99 99 0
OK
OK
BUFFERED
STATUS GAPPED 101 104 1
QUOTE AAPL 101 9 STALE
GAPS 1 102 103
BUFFERED
GAPS 1 102 102
OK
STATUS SYNCED 104 104 0
QUOTE AAPL 104 4 LIVE
QUOTE MSFT 203 6 LIVE
GAPS 0
IGNORED
)";
        std::stringstream is(input);
        std::stringstream os;
        Mgr mgr;
        mgr.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }
    

    {
        std::string input = R"(15
SNAPSHOT 10 0
UPDATE 14 A 140 2
UPDATE 12 A 120 2
STATUS
GAPS
REPLAY 13 A 130 3
STATUS
REPLAY 11 A 110 5
QUERY A
STATUS
REPLAY 15 A 150 1
UPDATE 14 A 140 2
UPDATE 16 A 160 4
REPLAY 15 A 150 3
STATUS
)";
        std::string output = R"(OK
BUFFERED
BUFFERED
STATUS GAPPED 10 14 2
GAPS 2 11 11 13 13
BUFFERED
STATUS RECOVERING 10 14 3
OK
QUOTE A 140 2 LIVE
STATUS SYNCED 14 14 0
ERROR
IGNORED
BUFFERED
OK
STATUS SYNCED 16 16 0
)";
        std::stringstream is(input);
        std::stringstream os;
        Mgr mgr;
        mgr.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(17
SNAPSHOT 50 2 A 100 10 B 200 20
UPDATE 53 A 103 9
UPDATE 55 B 205 0
STATUS
GAPS
SNAPSHOT 52 1 C 300 4
STATUS
QUERY C
QUERY A
QUERY B
GAPS
REPLAY 54 C 302 8
STATUS
QUERY C
QUERY A
QUERY B
GAPS
)";
        std::string output = R"(OK
BUFFERED
BUFFERED
STATUS GAPPED 50 55 2
GAPS 2 51 52 54 54
OK
STATUS RECOVERING 53 55 1
QUOTE C 300 4 STALE
QUOTE A 103 9 STALE
NOT_FOUND
GAPS 1 54 54
OK
STATUS SYNCED 55 55 0
QUOTE C 302 8 LIVE
QUOTE A 103 9 LIVE
NOT_FOUND
GAPS 0
)";
    }

    {
        std::string input = R"(16
SNAPSHOT 5 1 AAA 100 3
UPDATE 8 AAA 108 3
UPDATE 8 AAA 108 3
UPDATE 8 AAA 109 3
SNAPSHOT 7 2 AAA 107 5 AAA 110 1
STATUS
QUERY AAA
SNAPSHOT 7 1 BBB 70 2
QUERY AAA
QUERY BBB
GAPS
SNAPSHOT 4 0
STATUS
UPDATE 9 AAA 108 0
QUERY AAA
STATUS
)";
        std::string output = R"(OK
BUFFERED
IGNORED
ERROR
ERROR
STATUS GAPPED 5 8 1
QUOTE AAA 100 3 STALE
OK
QUOTE AAA 108 3 LIVE
QUOTE BBB 70 2 LIVE
GAPS 0
ERROR
STATUS SYNCED 8 8 0
OK
NOT_FOUND
STATUS SYNCED 9 9 0
)";

        std::stringstream is(input);
        std::stringstream os;
        Mgr mgr;
        mgr.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }
}

int main()
{
    test();
    return 0;
}