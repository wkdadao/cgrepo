#include <algorithm>
#include <bits/ranges_algo.h>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <bits/stdc++.h>
#include <memory>
#include <ranges>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <unordered_map>
using namespace std;

// start 20:30

bool enableInfo = true;
#define INFO enableInfo && std::cout << "[INFO line " << __LINE__ << "] : "

bool enableError = true;
#define ERROR enableError && std::cerr << "[ERROR line " << __LINE__ << "] : "

#define CHECK(x) if (!(x)) ERROR << " check error: " << #x << std::endl;

struct Order
{
    int64_t orderId{};
    double price{};
    int64_t vol{};
    bool buy{};
    inline auto operator<=> (const Order&) const = default;
};

std::ostream& operator<< (std::ostream& os, const Order& order)
{
    os << "orderId=" << order.orderId << "/price=" << order.price << "/vol=" << order.vol << "/side=" << (order.buy ? "buy" : "sell");
    return os;
}

struct OrderNode
{
    Order order{};
    OrderNode* prev{};
    OrderNode* next{};
    inline auto operator<=> (const OrderNode&) const = default;
};

struct Level
{
    double price{};
    int64_t vol{};
    OrderNode* head{};
    OrderNode* tail{};
    void append(OrderNode* node)
    {
        if (head == nullptr)
        {
            head = tail = node;
        }
        else
        {
            node->prev = tail;
            tail->next = node;
            tail = node;
        }
    }

    void erase(OrderNode* node)
    {
        // 1
        if (head == tail && node == head)
        {
            head = tail = nullptr;
        }
        else if (node == head)
        {
            head->next->prev = nullptr;
            head = head->next;
        }
        else if (node == tail)
        {
            tail->prev->next = nullptr;
            tail = tail->prev;
        }
        else
        {
            auto prev = node->prev;
            auto next = node->next;
            prev->next = next;
            next->prev = prev;
        }
    }

    inline auto operator<=> (const Level&) const = default;
};

std::ostream& operator<< (std::ostream& os, const Level& level)
{
    os << "price:" << level.price << ", vol=" << level.vol << ", orders:";
    for (auto p = level.head; p; p = p->next)
    {
        os << " " << p->order;
    }
    return os;
}

struct OrderBook
{
    std::vector<Level> buys; // asc
    std::vector<Level> sells; // des
};

std::ostream& operator<< (std::ostream& os, const OrderBook& ob)
{
    os << "\nbuys:\n";
    for (const auto& level : ob.buys)
    {
        os << level << std::endl;
    }

    os << "\nsells:\n";
    for (auto it = ob.sells.rbegin(); it != ob.sells.rend(); ++it)
    {
        os << *it << std::endl;
    }
    return os;
}

struct Add
{
    Order order;
};

struct Del
{
    int64_t orderId{};
};

struct Modify
{
    Order order;
};

std::optional<std::string_view> nextToken(std::string_view msg, size_t& pos)
{
    while (pos < msg.size() && std::isspace(msg[pos])) ++pos;
    if (pos == msg.size()) return {};
    auto begin = pos;
    while (pos < msg.size() && !std::isspace(msg[pos])) ++pos;
    auto end = pos;
    while (pos < msg.size() && std::isspace(msg[pos])) ++pos;
    return msg.substr(begin, end - begin);
}

bool nextNumber(std::string_view msg, size_t& pos, auto& number)
{
    auto token = nextToken(msg, pos);
    if (!token.has_value() || token == "") return false;

    auto [ptr, ec] = std::from_chars(token->data(), token->data() + token->size(), number);
    return (ptr == token->data() + token->size()) && (ec == std::errc());
}

/*
A 1001 1.1 100 B
M 1001 1.1 100 S
D 1001
*/

using Instruction = std::variant<Add, Del, Modify>;
bool parse(std::string_view msg, Instruction& instruction)
{
    size_t pos{0};
    auto op = nextToken(msg, pos);
    if (!(op == "A" || op == "D" || op == "M")) return false;
    
    //INFO << "op=" << *op << std::endl;

    int64_t orderId{};
    if (!nextNumber(msg, pos, orderId) || !(orderId > 0)) return false;

    //INFO << "orderId=" << orderId << std::endl;

    if (op == "A" || op == "M")
    {
        double price{};
        if (!nextNumber(msg, pos, price) || !std::isfinite(price) || !(price > 0.0)) return false;

        //INFO << "price=" << price << std::endl;

        int64_t vol{};
        if (!nextNumber(msg, pos, vol) || !(vol > 0)) return false;

        //INFO << "vol=" << vol << std::endl;

        auto side = nextToken(msg, pos);
        if (!(side == "B" || side == "S"))
        {
            return false;
        }

        //INFO << "side=" << *side << std::endl;

        if (nextToken(msg, pos).has_value()) return false;
        if (op == "A")
        {
            instruction = Add{.order=Order{.orderId=orderId, .price=price, .vol=vol, .buy=(side == "B")}};
            //INFO << "add instr" << std::endl;
        }
        else
        {
            instruction = Modify{.order=Order{.orderId=orderId, .price=price, .vol=vol, .buy=(side == "B")}};
        }
        return true;
    }
    else
    {
        if (nextToken(msg, pos).has_value()) return false;
        instruction = Del{.orderId=orderId};
        return true;
    }
}

class MatchEngine
{
public:
    MatchEngine(size_t poolSize)
    : nodePool_(poolSize) 
    , freeNodes_(poolSize)
    {
        for (size_t i = 0; i < poolSize; ++i)
        {
            freeNodes_[i] = &nodePool_[i];
        }
    }
    bool apply(const Add& add)
    {
        INFO << "adding: " << add.order << std::endl;

        if (orders_.contains(add.order.orderId))
            return false;
    
        auto newAdd = add;
        auto buy = newAdd.order.buy;

        // matching
        auto& oppLevels = buy ? orderBook_.sells : orderBook_.buys;
        while (newAdd.order.vol > 0 && !oppLevels.empty() && ( buy ? oppLevels.back().head->order.price <= newAdd.order.price : oppLevels.back().head->order.price >= newAdd.order.price))
        {
            auto& backOrder = oppLevels.back().head->order;
            if (newAdd.order.vol >= backOrder.vol)
            {
                const auto tradeVol = backOrder.vol;
                INFO << "trading with new=" << newAdd.order.orderId << ", opp=" << backOrder.orderId << ", price=" << backOrder.price << ", tradeVol=" << tradeVol << std::endl;

                orders_.erase(backOrder.orderId);
                newAdd.order.vol -= tradeVol;
                oppLevels.back().vol -= tradeVol;
                auto node = oppLevels.back().head;
                oppLevels.back().erase(node);
                if (oppLevels.back().vol == 0)
                {
                    oppLevels.pop_back();
                }

                destroy(node);
            }
            else
            {
                const auto tradeVol = newAdd.order.vol;
                INFO << "trading with new=" << newAdd.order.orderId << ", opp=" << backOrder.orderId << ", price=" << backOrder.price << ", tradeVol=" << tradeVol << std::endl;

                newAdd.order.vol -= tradeVol;
                backOrder.vol -= tradeVol;
                oppLevels.back().vol -= tradeVol;
            }
        }

        // add remaing to book
        if (newAdd.order.vol > 0)
        {
            auto node = allocate(newAdd.order);
            if (!node) return false;

            auto& levels = buy ? orderBook_.buys : orderBook_.sells;
            auto cmp = [buy](auto&& val, auto&& target) { return buy ? val.price < target : val.price > target; };
            if (auto it = std::lower_bound(levels.begin(), levels.end(), newAdd.order.price, cmp); it != levels.end() && it->price == newAdd.order.price)
            {
                if (newAdd.order.vol > std::numeric_limits<int64_t>::max() - it->vol)
                {
                    destroy(node);
                    return false;
                }

                it->vol += newAdd.order.vol;
                it->append(node);
            }
            else
            {
                Level level{.price=newAdd.order.price, .vol=newAdd.order.vol};
                level.append(node);
                levels.insert(it, level);
            }

            orders_[add.order.orderId] = node;
        }

        INFO << "added: " << orderBook_ << std::endl;

        if (!orderBook_.buys.empty() && !orderBook_.sells.empty())
        { 
            CHECK(orderBook_.buys.back().price < orderBook_.sells.back().price);
        }

        return true;
    }

    bool apply(const Del& del)
    {
        INFO << "delelting: " << del.orderId << std::endl;
        if (auto it = orders_.find(del.orderId); it != orders_.end())
        {
            auto node = it->second;
            auto buy = node->order.buy;
            auto& levels = buy ? orderBook_.buys : orderBook_.sells;
            auto cmp = [buy](auto&& val, auto&& target) { return buy ? val.price < target : val.price > target; };
            if (auto it2 = std::lower_bound(levels.begin(), levels.end(), node->order.price, cmp); it2 != levels.end() && it2->price == node->order.price)
            {
                it2->vol -= node->order.vol;
                it2->erase(node);
                destroy(node);

                if (it2->vol == 0)
                {
                    levels.erase(it2);
                }

                orders_.erase(it);

                INFO << "delelted: " << orderBook_ << std::endl;
                return true;
            }
            else
            {
                ERROR << "can not find " << del.orderId << std::endl;
                return false;
            }
        }
        else
        {
            ERROR << "can not find " << del.orderId << std::endl;
            return false;
        }
    }

    bool apply(const Modify& modify)
    {
        INFO << "modifying: " << modify.order << std::endl;
        Del del{.orderId=modify.order.orderId};
        if (!apply(del)) return false;

        Add add{.order=modify.order};
        return apply(add);
    }

    const auto& getOrderBook() const
    {
        return orderBook_;
    }

private:
    OrderNode* allocate(const Order& order)
    {
        if (freeNodes_.empty())
            return nullptr;

        auto node = freeNodes_.back();
        freeNodes_.pop_back();
        *node = {};
        node->order = order;
        return node;
    }

    void destroy(OrderNode* node)
    {
        freeNodes_.push_back(node);
    }

    OrderBook orderBook_;
    std::unordered_map<int64_t, OrderNode*> orders_;
    std::vector<OrderNode> nodePool_;
    std::vector<OrderNode*> freeNodes_;
};

class App
{
public:
    App(size_t poolSize)
    : matchEngine_(poolSize)
    {
    }

    bool process(std::string_view msg)
    {
        Instruction instruction;
        if (!parse(msg, instruction))
        {
            ERROR << "parsing error: msg=" << msg << std::endl;
            return false;   
        }

        return std::visit([this](auto&& arg) { return matchEngine_.apply(arg); }, instruction);
    }

private:
    MatchEngine matchEngine_;
};


void testApp()
{
    App app(1024 * 1024);
    CHECK(!app.process(""));

    CHECK(app.process("A 10001 1.1 100 B"));
    CHECK(app.process("A 10003 1.3 100 B"));
    CHECK(app.process("A 10002 1.2 100 B"));
    CHECK(app.process("A 12003 1.3 100 B"));
    CHECK(app.process("A 13003 1.3 100 B"));
    CHECK(app.process("D 12003"));
    CHECK(app.process("M 10003 1.31 100 B"));
    CHECK(app.process("A 14003 1.3 100 B"));

    CHECK(app.process("A 20001 2.1 100 S"));
    CHECK(app.process("A 20003 2.3 100 S"));
    CHECK(app.process("A 20002 2.2 100 S"));
    CHECK(app.process("A 22003 2.3 100 S"));
    CHECK(app.process("A 23003 2.3 100 S"));
    CHECK(app.process("D 22003"));
    CHECK(app.process("M 20003 2.31 100 S"));

    CHECK(app.process("A 14004 2.2 50 B"));

    CHECK(app.process("A 20004 1.3 250 S"));
};

// To execute C++, please define "int main()"
int main() {

    testApp();
    CHECK(1 == 2);
    return 0;
}
