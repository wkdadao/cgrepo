#include <bits/stdc++.h>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "


struct Account
{
    std::string name;
    int64_t limit{};
    int64_t exposure{};
};

enum class Status
{
    OPEN,
    FILLED,
    CANCELED
};
const char* str(Status status)
{
    switch (status)
    {
    case Status::OPEN: return "OPEN";
    case Status::FILLED: return "FILLED";
    case Status::CANCELED: return "CANCELED";
    }
    return "";
}

struct Order
{
    std::string id{};
    std::string accountName;
    int64_t totalQty{};
    int64_t filledQty{};
    Status status{Status::OPEN};
    int64_t openQty() const { return status == Status::OPEN ?  totalQty - filledQty : 0; }
};

struct AccountEvent
{
    Account account;
};

struct NewEvent
{
    std::string id;
    std::string accountName;
    int64_t qty;
};

struct FillEvent
{
    std::string id;
    int64_t qty;
};

struct AmendEvent
{
    std::string id;
    int64_t newTotal;
};

struct CancelEvent
{
    std::string id;
};

struct QueryEvent
{
    std::string id;
};

struct RiskEvent
{
    std::string accountName;
};

using Event = std::variant<AccountEvent, NewEvent, FillEvent, AmendEvent, CancelEvent, QueryEvent, RiskEvent>;

/*
12
ACCOUNT A 10
NEW O1 A 6
NEW O2 A 5
NEW O2 A 4
FILL O1 2
AMEND O2 7
CANCEL O1
AMEND O2 7
FILL O2 7
QUERY O2
RISK A
CANCEL O2
*/
std::vector<Event> parse(std::istream& is)
{
    std::vector<Event> events;

    std::string line;
    if (!std::getline(is, line)) throw "invalid";

    int cnt;
    std::stringstream ss1(line);
    if (!(ss1 >> cnt))  throw "invalid";

    for (int i = 0; i < cnt; ++i)
    {
        if (!std::getline(is, line)) throw "invalid";
        std::stringstream ss2(line);
        std::string eventType;

        if (!(ss2 >> eventType)) throw "invalid";
        if (eventType == "ACCOUNT")
        {
            AccountEvent event{};
            if (!(ss2 >> event.account.name >> event.account.limit)) throw "invalid";
            events.push_back(event);
        }
        else if (eventType == "NEW")
        {
            NewEvent event{};
            if (!(ss2 >> event.id >> event.accountName >> event.qty)) throw "invalid";
            events.push_back(event);
        }
        else if (eventType == "FILL")
        {
            FillEvent event{};
            if (!(ss2 >> event.id >> event.qty)) throw "invalid";
            events.push_back(event);
        }
        else if (eventType == "AMEND")
        {
            AmendEvent event{};
            if (!(ss2 >> event.id >> event.newTotal)) throw "invalid";
            events.push_back(event);
        }
        else if (eventType == "CANCEL")
        {
            CancelEvent event{};
            if (!(ss2 >> event.id)) throw "invalid";
            events.push_back(event);
        }
        else if (eventType == "QUERY")
        {
            QueryEvent event{};
            if (!(ss2 >> event.id)) throw "invalid";
            events.push_back(event);
        }
        else if (eventType == "RISK")
        {
            RiskEvent event{};
            if (!(ss2 >> event.accountName)) throw "invalid";
            events.push_back(event);
        }
    }

    INFO << "events size=" << cnt << std::endl;
    return events;
}

class Engine
{
public:
    void run(std::istream& is, std::ostream& os)
    {
        auto events = parse(is);
        for (const auto& event : events)
        {
            std::visit([&](auto&& e) { apply(e, os); }, event);
        }
    }

private:
    void apply(const AccountEvent& e, std::ostream& os)
    {
        if (!(e.account.limit > 0))
        {
            INFO << "not limit > 0。" << std::endl;
            os << "ERROR\n";
            return;
        }

        if (accounts_.contains(e.account.name))
        {
            INFO << "not 账户名此前不存在。" << std::endl;
            os << "ERROR\n";
        }
        else 
        {
            accounts_.emplace(e.account.name, e.account);
            os << "OK\n";
        }
    }

    void apply(const NewEvent& e, std::ostream& os)
    {
        if (orders_.contains(e.id) || !(e.qty > 0))
        {
            INFO << "订单存在, not qty > 0" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto it = accounts_.find(e.accountName);
        if (it == accounts_.end())
        {
            INFO << "not account 已存在" << std::endl;
            os << "ERROR\n";
            return;
        }
        // 新订单所需的风险额度不会超过账户剩余额度。
        else if (!(e.qty + it->second.exposure <= it->second.limit))
        {
            INFO << "not 新订单所需的风险额度不会超过账户剩余额度。" << std::endl;
            os << "ERROR\n";
            return;
        }

        it->second.exposure += e.qty;
        auto order = Order{.id=e.id, .accountName=e.accountName, .totalQty=e.qty, .filledQty=0, .status=Status::OPEN };
        orders_.emplace(e.id, order);
        os << "OK\n";

        CHECK(check(order));

        INFO << "after new, exposure=" << it->second.exposure << std::endl;
    }

    void apply(const FillEvent& e, std::ostream& os)
    {
        auto it = orders_.find(e.id); 
        if (it == orders_.end())
        {
            INFO << "not 订单存在" << std::endl;
            os << "ERROR\n";
            return;
        }
        else if (!(it->second.status == Status::OPEN && e.qty > 0 && e.qty <= it->second.openQty()))
        {
            INFO << "not 订单存在，且状态为 OPEN, qty > 0, qty <= totalQty - filledQty" << std::endl;
            os << "ERROR\n";
            return;
        }
        
        auto& account = accounts_[it->second.accountName];
        account.exposure -= e.qty;

        it->second.filledQty += e.qty;
        if (it->second.filledQty == it->second.totalQty)
        {
            it->second.status = Status::FILLED;
        }
        os << "OK\n";

        CHECK(check(it->second));
        INFO << "after fill, exposure=" << account.exposure << std::endl;
    }

    void apply(const AmendEvent& e, std::ostream& os)
    {
        auto it = orders_.find(e.id); 
        if (it == orders_.end())
        {
            INFO << "not 订单存在" << std::endl;
            os << "ERROR\n";
            return;
        }
        else if (!(it->second.status == Status::OPEN && e.newTotal > it->second.filledQty))
        {
            INFO << "not: 订单存在，且状态为 OPEN. newTotal > filledQty" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto& account = accounts_[it->second.accountName];
        if (!(account.exposure - it->second.totalQty + e.newTotal <= account.limit))
        {
            INFO << "not: 修改后的账户风险占用不能超过 limit" << std::endl;
            os << "ERROR\n";
            return;
        }

        account.exposure += e.newTotal - it->second.totalQty;
        it->second.totalQty = e.newTotal;        
        os << "OK\n";

        CHECK(check(it->second));
        INFO << "after amend, exposure=" << account.exposure << std::endl;
    }

    void apply(const CancelEvent& e, std::ostream& os)
    {
        auto it = orders_.find(e.id); 
        if (it == orders_.end())
        {
            INFO << "not 订单存在" << std::endl;
            os << "ERROR\n";
            return;
        }
        else if (!(it->second.status == Status::OPEN))
        {
            INFO << "not 订单状态为 OPEN" << std::endl;
            os << "ERROR\n";
            return;
        }
        
        // 释放该订单剩余的全部风险占用
        auto& account = accounts_[it->second.accountName];
        account.exposure -= it->second.openQty();

        it->second.status = Status::CANCELED;
        os << "OK\n";

        CHECK(check(it->second));
        INFO << "after cancel, exposure=" << account.exposure << std::endl;
    }

    void apply(const QueryEvent& e, std::ostream& os)
    {
        auto it = orders_.find(e.id);
        if (it == orders_.end())
        {
            INFO << "not 订单存在" << std::endl;
            os << "NOT_FOUND\n";
            return;
        }

        // ORDER id status totalQty filledQty openQty
        os << "ORDER " << it->second.id << " " << str(it->second.status) << " " << it->second.totalQty << " " << it->second.filledQty << " " << it->second.openQty() << "\n";

        CHECK(check(it->second));
    }

    void apply(const RiskEvent& e, std::ostream& os)
    {
        auto it = accounts_.find(e.accountName);
        if (it == accounts_.end())
        {
            INFO << "账户不存在" << std::endl;
            os << "NOT_FOUND\n";
            return;
        }

        // ACCOUNT account exposure limit
        os << "ACCOUNT " << it->second.name << " " << it->second.exposure << " " << it->second.limit << "\n";
    }

    /*
    totalQty > 0。
    0 <= filledQty <= totalQty。
    若 status == OPEN，则 filledQty < totalQty，且 openQty = totalQty - filledQty > 0。
    若 status == FILLED，则 filledQty == totalQty，且 openQty = 0。
    若 status == CANCELED，则 openQty = 0；允许 filledQty < totalQty。
    每个订单引用的账户必须存在。
    */

    bool check(const Order& order)
    {
        if (!(order.totalQty > 0)) return false;
        if (!(order.filledQty >= 0 && order.filledQty <= order.totalQty)) return false;
        if (order.status == Status::OPEN)
        {
            if (!(order.filledQty < order.totalQty && order.openQty() > 0 && order.openQty() == order.totalQty - order.filledQty)) return false;
        }
        else if (order.status == Status::FILLED)
        {
            if (!(order.filledQty == order.totalQty && order.openQty()== 0)) return false;
        }
        else if (order.status == Status::CANCELED)
        {
            if (!(order.openQty() == 0)) return false;
        }

        if (!accounts_.contains(order.accountName)) return false;

        return true;
    }

    std::unordered_map<std::string, Account> accounts_;
    std::unordered_map<std::string, Order> orders_;
};

void test()
{
    {
        std::string input = R"(12
ACCOUNT A 10
NEW O1 A 6
NEW O2 A 5
NEW O2 A 4
FILL O1 2
AMEND O2 7
CANCEL O1
AMEND O2 7
FILL O2 7
QUERY O2
RISK A
CANCEL O2
)";
        std::string output = R"(OK
OK
ERROR
OK
OK
ERROR
OK
OK
OK
ORDER O2 FILLED 7 7 0
ACCOUNT A 0 10
ERROR
)";
        std::stringstream is(input);
        std::stringstream os;
        Engine engine;
        engine.run(is, os);
        CHECK(os.str() == output);
        INFO << "output=\n" << os.str() << std::endl;
    }

     {
        std::string input = R"(13
ACCOUNT ALPHA 12
NEW A ALPHA 9
FILL A 3
QUERY A
AMEND A 8
RISK ALPHA
NEW B ALPHA 7
AMEND A 10
CANCEL B
FILL A 5
RISK ALPHA
QUERY B
FILL B 1
)";
        std::string output = R"(OK
OK
OK
ORDER A OPEN 9 3 6
OK
ACCOUNT ALPHA 5 12
OK
ERROR
OK
OK
ACCOUNT ALPHA 0 12
ORDER B CANCELED 7 0 0
ERROR
)";
        std::stringstream is(input);
        std::stringstream os;
        Engine engine;
        engine.run(is, os);
        CHECK(os.str() == output);
        INFO << "output=\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(11
ACCOUNT X 3
NEW O1 X 2
NEW O1 X 1
FILL O1 3
CANCEL O1
NEW O1 X 1
QUERY O1
QUERY O9
RISK Y
FILL O9 1
RISK X
)";
        std::string output = R"(OK
OK
ERROR
ERROR
OK
ERROR
ORDER O1 CANCELED 2 0 0
NOT_FOUND
NOT_FOUND
ERROR
ACCOUNT X 0 3
)";
        std::stringstream is(input);
        std::stringstream os;
        Engine engine;
        engine.run(is, os);
        CHECK(os.str() == output);
        INFO << "output=\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(5
NEW O1 X 2
ACCOUNT X 3
NEW O1 X 2
FILL O1 0
QUERY O1
)";
        std::string output = R"(ERROR
OK
OK
ERROR
ORDER O1 OPEN 2 0 2
)";
        std::stringstream is(input);
        std::stringstream os;
        Engine engine;
        engine.run(is, os);
        CHECK(os.str() == output);
        INFO << "output=\n" << os.str() << std::endl;
    }
}

int main()
{
    // test();
    Engine engine;
    engine.run(std::cin, std::cout);
    return 0;
}