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
#include <bits/stdc++.h>
#include <semaphore>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "


struct Board
{
    std::vector<std::string> cells;
    int r_size;
    int c_size;
};

struct Player
{
    int r;
    int c;
    int dir;
    char dirStr() const
    {
        if (dir == 0) return 'N';
        else if (dir == 1) return 'E';
        else if (dir == 2) return 'S';
        else return 'W';
    }
};

struct Move
{
    bool forward;
    int steps;
};

struct ChangeDir
{
    bool right;
};

struct Transfer
{
    int dir;
};

using Event = std::variant<Move, ChangeDir, Transfer>;

struct Input
{
    Board board;
    Player player;
    std::vector<Event> commands;
};

/*
5 7
#######
#S..>.#
#.#...#
#.....#
#######
E
4
F 2
F 1
R
F 1
*/

Input parse(std::istream& is)
{
    Input result;

    std::string line;
    
    if (!std::getline(is, line)) throw "invalid input";
    std::stringstream is2(line);
    int r_size;
    int c_size;
    is2 >> r_size;
    is2 >> c_size;

    result.board.r_size = r_size;
    result.board.c_size = c_size;
    INFO << "r_size=" << r_size << ", c_size=" << c_size << std::endl;

    for (int r = 0; r < r_size; ++r)
    {
        if (!std::getline(is, line)) throw "invalid input";
        result.board.cells.push_back(line);
        int c_size = int(line.size());
        for (int c = 0; c < c_size; ++c)
        {
            if (line[c] == 'S')
            {
                result.player.r = r;
                result.player.c = c;
                break;
            }
        }
    }
    result.board.cells[result.player.r][result.player.c] = ' ';
    INFO << "result.player.r=" << result.player.r << ", result.player.c=" << result.player.c << std::endl;

    int dir;
    if (!std::getline(is, line)) throw "invalid input";
    if (line == "N")
    {
        dir = 0;
    }
    else if (line == "E")
    {
        dir = 1;
    }
    else if (line == "S")
    {
        dir = 2;
    }
    else if (line == "W")
    {
        dir = 3;
    }
    else
    {
        throw "invalid input";
    }
    result.player.dir = dir;
    INFO << "dir=" << dir << std::endl;

    if (!std::getline(is, line)) throw "invalid input";
    int commandCnt = std::stoi(line);
    INFO << "commandCnt=" << commandCnt << std::endl;

    for (int i = 0; i < commandCnt; ++i)
    {
        if (!std::getline(is, line)) throw "invalid input";
        INFO << "line=" << line << std::endl;

        if (line[0] == 'L')
        {
            result.commands.push_back(ChangeDir(false));
        }
        else if (line[0] == 'R')
        {
            result.commands.push_back(ChangeDir(true));
        }
        else
        {
            if (line.size() < 3)  throw "invalid input";
            int steps = std::stoi(line.substr(2, line.size() - 2));
            result.commands.push_back(Move(line[0] == 'F' ? true : false, steps));
        }
        INFO << "added command" << std::endl;
    }

    return result;
}

struct State
{
    const Board* board;
    Player player;
    std::set<std::pair<int, int>> inStack;
};

std::ostream& operator<< (std::ostream& os, const State& state)
{
    for (int r = 0; r < state.board->r_size; ++r)
    {
        for (int c = 0; c < state.board->c_size; ++c)
        {
            if (r == state.player.r && c == state.player.c)
            {
                os << "S";
            }
            else
            {
                os << state.board->cells[r][c];
            }
        }
        os << "\n";
    }
    return os;
}

bool isValidPos(const State& state)
{
    if (state.player.r < 0 || state.player.c < 0 || state.player.r >= state.board->r_size || state.player.c >= state.board->c_size ) return false;
    if (state.board->cells[state.player.r][state.player.c] == '#') return false;
    return true;
}

bool apply(State& state, const Transfer& event, auto& queue);
bool apply(State& state, const Move& event, auto& queue)
{
    INFO << "move: steps=" << event.steps << std::endl;

    if (state.player.dir == 0)
    {
        state.player.r -= event.forward ? 1 : -1;
    }
    else if (state.player.dir == 1)
    {
        state.player.c += event.forward ? 1 : -1;
    }
    else if (state.player.dir == 2)
    {
        state.player.r += event.forward ? 1 : -1;
    }
    else if (state.player.dir == 3)
    {
        state.player.c -= event.forward ? 1 : -1;
    }

    if (!isValidPos(state))
    {
        INFO << "failed to move" << std::endl;
        return false;
    }

    if (state.board->cells[state.player.r][state.player.c] == '^')
    {
        if (!apply(state, Transfer{0}, queue))
        {
            return false;
        }
    }
    else if (state.board->cells[state.player.r][state.player.c] == '>')
    {
        if (!apply(state, Transfer{1}, queue))
        {
            return false;
        }
    }
    else if (state.board->cells[state.player.r][state.player.c] == 'v')
    {
        if (!apply(state, Transfer{2}, queue))
        {
            return false;
        }
    }
    else if (state.board->cells[state.player.r][state.player.c] == '<')
    {
        if (!apply(state, Transfer{3}, queue))
        {
            return false;
        }
    }

    if (event.steps > 1)
    {
        queue.push(Move(event.forward, event.steps - 1));
    }

    INFO << "after move:\n" << state;

    return true;
}

bool apply(State& state, const ChangeDir& event, auto&)
{
    INFO << "changeDir: right=" << event.right << std::endl;
    if (event.right)
    {
        state.player.dir = (state.player.dir + 1) % 4;
    }
    else
    {
        state.player.dir = (state.player.dir + 3) % 4;
    }
    INFO << "dir is changed to " << state.player.dir << std::endl;

    INFO << "after changeDir:\n" << state;

    return true;
}

bool apply(State& state, const Transfer& event, auto& queue)
{
    INFO << "transfer: " << std::endl;

    if (event.dir == 0)
    {
        --state.player.r;
    }
    else if (event.dir == 1)
    {
        ++state.player.c;
    }
    else if (event.dir == 2)
    {
        ++state.player.r;
    }
    else if (event.dir == 3)
    {
        --state.player.c;
    }

    if (!isValidPos(state))
    {
        INFO << "failed to transfer" << std::endl;
        return false;
    }

    const auto key = std::pair(state.player.r, state.player.c);
    if (state.inStack.contains(key)) return false;
    state.inStack.insert(key);

    if (state.board->cells[state.player.r][state.player.c] == '^')
    {
        if (!apply(state, Transfer{0}, queue))
        {
            return false;
        }
    }
    else if (state.board->cells[state.player.r][state.player.c] == '>')
    {
        if (!apply(state, Transfer{1}, queue))
        {
            return false;
        }
    }
    else if (state.board->cells[state.player.r][state.player.c] == 'v')
    {
        if (!apply(state, Transfer{2}, queue))
        {
            return false;
        }
    }
    else if (state.board->cells[state.player.r][state.player.c] == '<')
    {
        if (!apply(state, Transfer{3}, queue))
        {
            return false;
        }
    }

    INFO << "after transfer:\n" << state;

    state.inStack.erase(key);

    return true;
}

bool run(std::istream& is)
{
    const auto input = parse(is);

    State state;
    state.board = &input.board;
    state.player = input.player;

    int cmdCnt = 0;
    for (const auto& command : input.commands)
    {
        ++cmdCnt;
        auto stateCpy = state;

        std::queue<Event> queue;
        queue.push(command);

        while (!queue.empty())
        {
            auto event = queue.front();
            queue.pop();
            if (!std::visit([&](auto&& e){ return apply(stateCpy, e, queue); }, event))
            {
                std::cout << "INVALID " << cmdCnt << std::endl;
                return false;
            }
        }

        state = std::move(stateCpy);
    }

    std::cout << "VALID " << (state.player.r + 1) << " " << (state.player.c + 1) << " " << state.player.dirStr() << std::endl;
    return true;
}

void test()
{
    /*
    {
    std::string input=R"(5 7
#######
#S..>.#
#.#...#
#.....#
#######
E
4
F 2
F 1
R
F 1
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(4 7
#######
#S.>#.#
#.....#
#######
E
2
F 2
R
)";
    std::stringstream is(input);
    run(is);
    }

*/
    {
    std::string input=R"(5 7
#######
#.....#
#..S..#
#.....#
#######
N
4
B 1
L
F 2
R
)";
    std::stringstream is(input);
    run(is);
    }

}

int main()
{
    //test();
    //std::cout << "done" << std::endl;
    run(std::cin);
    return 0;
} 