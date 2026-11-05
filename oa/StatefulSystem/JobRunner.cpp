#include <bits/stdc++.h>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "


enum class State
{
    PENDING, // 尚有至少一个直接依赖未成功完成，且没有失败的依赖祖先
    READY, // 所有直接依赖均为 SUCCESS，可以运行
    RUNNING, // 已运行，尚未被报告完成或失败
    SUCCESS, // 已成功完成，终态
    FAILED, // 任务自身执行失败，终态
    BLOCKED, // 某个直接或间接依赖任务已失败，无法运行，终态
};

const char* str(State state)
{
    switch (state)
    {
    case State::PENDING: return "PENDING";
    case State::READY: return "READY";
    case State::RUNNING: return "RUNNING";
    case State::SUCCESS: return "SUCCESS";
    case State::FAILED: return "FAILED";
    case State::BLOCKED: return "BLOCKED";
    }
    return "";
}

enum class Stage
{
    BUILD,
    ACTIVE
};

struct Add
{
    std::string id;
    std::vector<std::string> deps;
};
struct Start {};
struct Ready { };
struct Run { std::string id; };
struct Complete { std::string id; };
struct Fail { std::string id; };
struct Status { std::string id; };

using Event = std::variant<Add, Start, Ready, Run, Complete, Fail, Status>;

/*
N
ADD id k [dep1 dep2 ... depk]
START
READY
RUN id
COMPLETE id
FAIL id
STATUS id
*/

std::vector<Event> parse(std::istream& is)
{
    std::vector<Event> events;

    std::string line;

    if (!std::getline(is, line)) throw "invalid";
    std::stringstream ss1(line);
    int cnt;
    if (!(ss1 >> cnt))  throw "invalid";

    for (int i = 0; i < cnt; ++i)
    {
        if (!std::getline(is, line)) throw "invalid";
        std::stringstream ss2(line);

        std::string op;
        if (!(ss2 >> op)) throw "invalid";

        if (op == "ADD")
        {
            std::string id;
            int depCnt;
            if (!(ss2 >> id >> depCnt)) throw "invalid";

            std::vector<std::string> deps;
            for (int j = 0; j < depCnt; ++j)
            {
                std::string dep;
                if (!(ss2 >> dep)) throw "invalid";
                deps.push_back(dep);
            }
            
            events.push_back(Add{.id=id, .deps=std::move(deps)});
        }
        else if (op == "START")
        {
            events.push_back(Start{});
        }
        else if (op == "READY")
        {
            events.push_back(Ready{});
        }
        else if (op == "RUN")
        {
            std::string id;
            if (!(ss2 >> id)) throw "invalid";
            events.push_back(Run{.id=id});
        }
        else if (op == "COMPLETE")
        {
            std::string id;
            if (!(ss2 >> id)) throw "invalid";
            events.push_back(Complete{.id=id});
        }
        else if (op == "FAIL")
        {
            std::string id;
            if (!(ss2 >> id)) throw "invalid";
            events.push_back(Fail{.id=id});
        }
        else if (op == "STATUS")
        {
            std::string id;
            if (!(ss2 >> id)) throw "invalid";
            events.push_back(Status{.id=id});
        }
        else
        {
            throw "invalid";
        }
    }

    return events;
}

class Runner
{
private:
    struct Job
    {
        std::string id;
        State state;
        std::unordered_set<std::string> deps;
        std::unordered_set<std::string> nexts;
    };

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
    void apply(const Add& e, std::ostream& os)
    {
        INFO << "add: " << e.id << std::endl;

        if (jobs_.contains(e.id))
        {
            INFO << "Job ID 全局唯一。已成功添加的 Job 不可重复添加。" << std::endl;
            os << "ERROR\n";
            return;
        }
        
        std::unordered_set<std::string> deps(e.deps.begin(), e.deps.end());
        if (deps.contains(e.id))
        {
            INFO << "不允许 Job 直接依赖自身。" << std::endl;
            os << "ERROR\n";
            return;
        }

        if (deps.size() != e.deps.size())
        {
            INFO << "依赖列表内不得出现重复 ID。" << std::endl;
            os << "ERROR\n";
            return;
        }

        if (stage_ == Stage::ACTIVE)
        {
            INFO << "在 ACTIVE 阶段执行 ADD 一律失败。" << std::endl;
            os << "ERROR\n";
            return;
        }

        jobs_[e.id] = Job{.id=e.id, .state=State::PENDING, .deps=std::move(deps), .nexts={}};
        os << "OK\n";
    }

    void apply(const Start&, std::ostream& os)
    {
        INFO << "start" << std::endl;

        if (stage_ != Stage::BUILD)
        {
            INFO << "仅在 BUILD 阶段允许执行。" << std::endl;
            os << "ERROR\n";
            return;
        }

        if (jobs_.empty())
        {
            INFO << "至少有一个成功添加的 Job。" << std::endl;
            os << "ERROR\n";
            return;
        }

        for (const auto& [_, job] : jobs_)
        {
            for (const auto& dep : job.deps)
            {
                if (!jobs_.contains(dep))
                {
                    INFO << "每个被引用的依赖 ID 都存在。" << std::endl;
                    os << "ERROR\n";
                    return; 
                }
            }
        }

        // build nexts
        for (const auto& [_, job] : jobs_)
        {
            for (const auto& dep : job.deps)
            {
                jobs_[dep].nexts.insert(job.id);
            }
        }

        std::unordered_map<std::string, int> ins;
        for (const auto& [id, _] : jobs_)
        {
            ins[id] = 0;
        }

        for (const auto& [_, job] : jobs_)
        {
            for (const auto& next : job.nexts)
            {
                ++ins[next];
            }
        }

        std::deque<std::string> queue;
        for (const auto& [id, cnt] : ins)
        {
            if (cnt == 0)
            {
                queue.push_back(id);
            }
        }

        size_t popCnt = 0;
        while (!queue.empty())
        {
            auto top = queue.front();
            queue.pop_front();
            ++popCnt;

            for (const auto& next : jobs_[top].nexts)
            {
                auto& in = ins[next];
                --in;
                if (in == 0)
                {
                    queue.push_back(next);
                }
            }
        }

        if (popCnt != jobs_.size())
        {
            INFO << "整个图无有向环。popCnt=" << popCnt << ", jobs_.size()=" << jobs_.size() << std::endl;
            os << "ERROR\n";
            return; 
        }

        for (const auto& [id, _] : jobs_)
        {
            ins_[id] = 0;
        }

        for (const auto& [_, job] : jobs_)
        {
            for (const auto& next : job.nexts)
            {
                ++ins_[next];
            }
        }

        for (const auto& [id, cnt] : ins_)
        {
            INFO << "ins: id=" << id << ", cnt=" << cnt << std::endl;

            if (cnt == 0)
            {
                readyQueue_.insert(id);
                jobs_[id].state = State::READY;
            }
        }

        stage_ = Stage::ACTIVE;
        os << "OK\n";
    }

    void apply(const Ready&, std::ostream& os)
    {
        INFO << "ready: " << std::endl;

        if (stage_ != Stage::ACTIVE)
        {
            INFO << "在 BUILD 阶段使用该命令输出 ERROR" << std::endl;
            os << "ERROR\n";
            return;
        }

        std::vector<const Job*> readyJobs;
        for (const auto& [_, job] : jobs_)
        {
            if (job.state == State::READY)
            {
                readyJobs.push_back(&job);
            }
        }

        std::sort(readyJobs.begin(), readyJobs.end(), [](auto&& a, auto&& b){ return a->id < b->id; });

        // READY k id1 id2 ... idk
        os << "READY " << readyJobs.size();
        for (const auto& job : readyJobs)
        {
            os << " " << job->id;
        }
        os << "\n";
    }

    void apply(const Run& e, std::ostream& os)
    {
        INFO << "run: " << e.id << std::endl;

        if (stage_ != Stage::ACTIVE)
        {
            INFO << "在 ACTIVE 阶段启动一个 Job。" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto it = readyQueue_.find(e.id);
        if (it == readyQueue_.end())
        {
            INFO << "只有当前处于 READY 的 Job 才能执行该命令" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto& job = jobs_[*it];
        CHECK(job.state == State::READY);
        job.state = State::RUNNING;

        runningQueue_.insert(*it);
        readyQueue_.erase(it);

        os << "OK\n";
    }

    void apply(const Complete& e, std::ostream& os)
    {
        INFO << "complete: " << e.id << std::endl;

        if (stage_ != Stage::ACTIVE)
        {
            INFO << "在 ACTIVE 阶段正在运行的 Job 标记为成功完成" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto it = runningQueue_.find(e.id);
        if (it == runningQueue_.end())
        {
            INFO << "对不存在、未运行或已终止任务执行 COMPLETE，输出 ERROR，不产生部分更新" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto& job = jobs_[*it];
        CHECK(job.state == State::RUNNING);
        job.state = State::SUCCESS;

        for (const auto& next : job.nexts)
        {
            auto& in = ins_[next];
            --in;
            if (in == 0)
            {
                jobs_[next].state = State::READY;
                readyQueue_.insert(next);
                INFO << "move to ready: " << next << std::endl;
            }
            CHECK(in >= 0);
        }

        INFO << "completed: " << *it << std::endl;
        completeQueue_.insert(*it);
        runningQueue_.erase(it);

        os << "OK\n";
    }

    void apply(const Fail& e, std::ostream& os)
    {
        INFO << "fail: " << e.id << std::endl;

        if (stage_ != Stage::ACTIVE)
        {
            INFO << "报告一项 RUNNING Job 执行失败" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto it = runningQueue_.find(e.id);
        if (it == runningQueue_.end())
        {
            INFO << "不存在或非 RUNNING Job 执行 FAIL 一律返回 ERROR，且不得影响任何下游 Job" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto& job = jobs_[*it];
        CHECK(job.state == State::RUNNING);
        job.state = State::FAILED;

        std::deque<std::string> queue{job.nexts.begin(), job.nexts.end()};
        while (!queue.empty())
        {
            auto top = queue.front();
            queue.pop_front();

            auto& j = jobs_[top];
            if (j.state == State::PENDING)
            {
                j.state = State::BLOCKED;
                for (const auto& n : j.nexts)
                {
                    queue.push_back(n);
                }
            }
        }

        INFO << "blocked: " << *it << std::endl;
        blockedQueue_.insert(*it);
        runningQueue_.erase(it);

        os << "OK\n";
    }

    void apply(const Status& e, std::ostream& os)
    {
        INFO << "status: " << e.id << std::endl;

        if (stage_ == Stage::BUILD)
        {
            INFO << "BUILD 阶段调用 STATUS，无论 ID 是否已经添加，都输出 ERROR" << std::endl;
            os << "ERROR\n";
            return;
        }

        auto it = jobs_.find(e.id);
        if (it == jobs_.end())
        {
            INFO << "ACTIVE 阶段查询不存在的 ID 输出 NOT_FOUND" << std::endl;
            os << "NOT_FOUND\n";
            return;
        }

        // JOB id state
        os << "JOB " << it->second.id << " " << str(it->second.state) << "\n";
    }

    std::unordered_map<std::string, Job> jobs_;
    std::unordered_map<std::string, int> ins_;
    std::unordered_set<std::string> readyQueue_;
    std::unordered_set<std::string> runningQueue_;
    std::unordered_set<std::string> completeQueue_;
    std::unordered_set<std::string> blockedQueue_;

    Stage stage_{Stage::BUILD};
};

void test()
{
    {
        std::string input = R"(24
ADD parse 0
ADD build 1 parse
ADD test 1 build
ADD docs 1 parse
ADD deploy 2 test docs
START
READY
RUN parse
COMPLETE parse
READY
RUN build
RUN docs
COMPLETE build
READY
COMPLETE docs
READY
RUN test
COMPLETE test
READY
STATUS deploy
RUN deploy
COMPLETE deploy
READY
STATUS deploy)";
        std::string output = R"(OK
OK
OK
OK
OK
OK
READY 1 parse
OK
OK
READY 2 build docs
OK
OK
OK
READY 1 test
OK
READY 1 test
OK
OK
READY 1 deploy
JOB deploy READY
OK
OK
READY 0
JOB deploy SUCCESS
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(22
ADD A 0
ADD B 0
ADD C 1 A
ADD D 2 C B
ADD E 1 D
ADD F 1 B
START
READY
RUN A
FAIL A
STATUS C
STATUS D
STATUS E
READY
RUN B
COMPLETE B
READY
RUN F
COMPLETE F
STATUS F
RUN C
STATUS A)";
        std::string output = R"(OK
OK
OK
OK
OK
OK
OK
READY 2 A B
OK
OK
JOB C BLOCKED
JOB D BLOCKED
JOB E BLOCKED
READY 1 B
OK
OK
READY 1 F
OK
OK
JOB F SUCCESS
ERROR
JOB A FAILED
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(12
ADD high 1 low
START
ADD low 0
START
READY
RUN high
RUN low
COMPLETE low
READY
STATUS high
ADD extra 0
START)";
        std::string output = R"(OK
ERROR
OK
OK
READY 1 low
ERROR
OK
OK
READY 1 high
JOB high READY
ERROR
ERROR
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(22
ADD A 0
ADD B 0
ADD C 1 A
ADD D 2 C B
ADD E 1 D
ADD F 1 B
START
READY
RUN A
FAIL A
STATUS C
STATUS D
STATUS E
READY
RUN B
COMPLETE B
READY
RUN F
COMPLETE F
STATUS F
RUN C
STATUS A)";
        std::string output = R"(OK
OK
OK
OK
OK
OK
OK
READY 2 A B
OK
OK
JOB C BLOCKED
JOB D BLOCKED
JOB E BLOCKED
READY 1 B
OK
OK
READY 1 F
OK
OK
JOB F SUCCESS
ERROR
JOB A FAILED
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(6
ADD X 1 Y
ADD Y 1 Z
ADD Z 1 X
START
READY
STATUS X)";
        std::string output = R"(OK
OK
OK
ERROR
ERROR
ERROR
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(13
ADD A 1 A
ADD B 2 A A
ADD A 0
ADD B 1 A
START
READY
RUN A
RUN A
COMPLETE B
COMPLETE A
FAIL A
READY
STATUS B)";
        std::string output = R"(ERROR
ERROR
OK
OK
OK
READY 1 A
OK
ERROR
ERROR
OK
ERROR
READY 1 B
JOB B READY
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }

    {
        std::string input = R"(11
ADD A 1 A
ADD B 2 A A
ADD A 0
ADD B 1 A
READY
RUN A
RUN A
COMPLETE B
COMPLETE A
FAIL A
READY
STATUS B)";
        std::string output = R"(ERROR
ERROR
OK
OK
ERROR
ERROR
ERROR
ERROR
ERROR
ERROR
ERROR
)";
        std::stringstream is(input);
        std::stringstream os;
        Runner runner;
        runner.run(is, os);
        CHECK(os.str() == output);
        INFO << "output:\n" << os.str() << std::endl;
    }
}

int main()
{
    //test();
    Runner runner;
    runner.run(std::cin, std::cout);
    return 0;
}