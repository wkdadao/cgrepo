#include <bits/stdc++.h>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "


struct Request
{
    int id{};
    int64_t arriveTime{};
    int64_t from{};
    int64_t to{};
    int64_t pickupTime{};
    int64_t dropoffTime{};
    int64_t waitTime{};
};

struct Input
{
    int n{};
    int liftStartPos{};
    int moveTime{};
    int gateTime{};
    std::vector<Request> requests;
};

/*
5 2 1 2
3
0 2 3
5 1 4
5 4 2
*/

Input parse(std::istream& is)
{
    Input input{};

    std::string line;
    if (!std::getline(is, line)) throw "invalid";
    std::stringstream ss1(line);
    if (!(ss1 >> input.n >> input.liftStartPos >> input.moveTime >> input.gateTime)) throw "invalid";

    if (!std::getline(is, line)) throw "invalid";
    std::stringstream ss2(line);
    int cnt;
    if (!(ss2 >> cnt)) throw "invalid";

    for (int i = 0; i < cnt; ++i)
    {
        if (!std::getline(is, line)) throw "invalid";
        std::stringstream ss3(line);
        Request request{};
        request.id = i + 1;
        if (!(ss3 >> request.arriveTime >> request.from >> request.to))  throw "invalid";
        
        input.requests.push_back(std::move(request));
    }
    INFO << "total request=" << cnt << std::endl;

    return input;
}

struct State
{
    int64_t now;
    int liftPos;
};

struct Output
{
    int64_t totalDis{};
    int64_t finishTime{};
    int endFloor{};
};

void run(std::istream& is, std::ostream& os)
{
    auto input = parse(is);
    Output output{};

    std::sort(input.requests.begin(), input.requests.end(), [](auto& a, auto& b){ return std::pair(a.arriveTime, a.id) < std::pair(b.arriveTime, b.id); });

    State state{.now = 0, .liftPos = input.liftStartPos};
    for (auto& request : input.requests)
    {
        state.now = std::max(state.now, request.arriveTime);
        INFO << "begin: id=" << request.id << ", now=" << state.now << ", liftPos=" << state.liftPos << std::endl;

        // wait lift
        state.now += abs(request.from - state.liftPos) * input.moveTime;
        output.totalDis += abs(request.from - state.liftPos);
        state.liftPos = request.from;
        INFO << "waitLift: now=" << state.now << ", liftPos=" << state.liftPos << std::endl;
        request.pickupTime = state.now;

        // get in
        state.now += input.gateTime;
        INFO << "getIn: now=" << state.now << ", liftPos=" << state.liftPos << std::endl;

        // lifting
        state.now += abs(request.to - state.liftPos) * input.moveTime;
        output.totalDis += abs(request.to - state.liftPos);
        state.liftPos = request.to;
        INFO << "lifting: now=" << state.now << ", liftPos=" << state.liftPos << std::endl;
        request.dropoffTime = state.now;

        // get out
        state.now += input.gateTime;
        INFO << "getOut: now=" << state.now << ", liftPos=" << state.liftPos << std::endl;

        request.waitTime = request.pickupTime - request.arriveTime;

        output.finishTime = state.now;
        output.endFloor = state.liftPos;

        INFO << "output, totalDis=" << output.totalDis << ", finishTime=" << output.finishTime << ", endFloor=" << output.endFloor << std::endl;
    }

    std::sort(input.requests.begin(), input.requests.end(), [](auto& a, auto& b){ return a.id < b.id; });

    for (const auto& request : input.requests)
    {
        os << request.pickupTime << " " << request.dropoffTime << " " << request.waitTime << "\n";
    }

    os << "TOTAL_DISTANCE " << output.totalDis << "\n";
    os << "FINISH_TIME " << output.finishTime << "\n";
    os << "END_FLOOR " << output.endFloor << "\n";
}

void test()
{
    // Sample 1 — 忙碌期间请求持续到达
    {
        std::string input = R"(8 3 2 1
4
0 3 6
0 5 2
4 7 8
15 2 4
)";       
        std::string output = R"(0 7 0
10 17 10
28 31 24
44 49 29
TOTAL_DISTANCE 21
FINISH_TIME 50
END_FLOOR 4
)";

        std::stringstream is(input);
        std::stringstream os;
        run(is, os);

        CHECK(os.str() == output);
        INFO << "----\n" << os.str() << std::endl;
    }

    // Sample 2 — 请求恰在 Ready 时到达，同时间请求按 ID 排序
    {
        std::string input = R"(5 2 1 2
3
0 2 3
5 1 4
5 4 2
)";       
        std::string output = R"(0 3 0
7 12 2
14 18 9
TOTAL_DISTANCE 8
FINISH_TIME 20
END_FLOOR 2
)";

        std::stringstream is(input);
        std::stringstream os;
        run(is, os);

        CHECK(os.str() == output);
        INFO << "----\n" << os.str() << std::endl;
    }

    // Sample 3 — 电梯长时间空闲
    {
        std::string input = R"(6 6 3 2
2
10 1 3
50 3 2
)";       
        std::string output = R"(25 33 15
50 55 0
TOTAL_DISTANCE 8
FINISH_TIME 57
END_FLOOR 2
)";

        std::stringstream is(input);
        std::stringstream os;
        run(is, os);

        CHECK(os.str() == output);
        INFO << "----\n" << os.str() << std::endl;
    }
    
    // Sample 4 — 输入并未按到达时间排序
    {
        std::string input = R"(5 1 1 1
3
10 5 4
0 1 2
0 2 3
)";       
        std::string output = R"(12 14 2
0 2 0
3 5 3
TOTAL_DISTANCE 5
FINISH_TIME 15
END_FLOOR 4
)";

        std::stringstream is(input);
        std::stringstream os;
        run(is, os);

        CHECK(os.str() == output);
        INFO << "----\n" << os.str() << std::endl;
    }
}

int main()
{
    // test();
    run(std::cin, std::cout);
    return 0;
}
