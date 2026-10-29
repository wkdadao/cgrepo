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
#include <iostream>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = true;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

struct Board
{
    std::vector<std::string> cells;
    int r_size;
    int c_size;
};

struct Cell
{
    int r;
    int c;
};

struct MovePlayer
{
    bool right;
};

struct MoveBox
{
    bool right;
    Cell box;
};

struct State
{
    Board board;
    Cell player;
};

std::ostream& operator<< (std::ostream& os, const State& state)
{
    for (int r = 0; r < state.board.r_size; ++r)
    {
        os << state.board.cells[r] << "\n";
    }
    return os;
}

bool isValidMovePos(const State& state, const Cell& cell)
{
    if (cell.r < 0 || cell.c < 0 || cell.r >= state.board.r_size || cell.c >= state.board.c_size ) return false;
    if (state.board.cells[cell.r][cell.c] == '#') return false;
    return true;
}

bool isValidGravityPos(const State& state, const Cell& cell)
{
    if (cell.r < 0 || cell.c < 0 || cell.r >= state.board.r_size || cell.c >= state.board.c_size ) return false;
    return state.board.cells[cell.r][cell.c] == '.';
}

bool apply(State& state, const MoveBox& event);
bool apply(State& state, const MovePlayer& event)
{
    INFO << "MovePlayer: right=" << event.right << std::endl;

    state.board.cells[state.player.r][state.player.c] = '.';

    state.player.c += event.right ? 1 : -1;

    if (!isValidMovePos(state, state.player))
    {
        INFO << "failed to move" << std::endl;
        return false;
    }

    if (state.board.cells[state.player.r][state.player.c] == 'B')
    {
        if (!apply(state, MoveBox{event.right, Cell{state.player.r, state.player.c}}))
        {
            return false;
        }
    }

    state.board.cells[state.player.r][state.player.c] = 'P';

    INFO << "after MovePlayer:\n" << state;

    return true;
}

bool apply(State& state, const MoveBox& event)
{
    INFO << "MoveBox: right=" << event.right << std::endl;

    state.board.cells[event.box.r][event.box.c] = '.';

    auto box = event.box;
    box.c += event.right ? 1 : -1;

    if (!isValidMovePos(state, box))
    {
        INFO << "failed to move" << std::endl;
        return false;
    }

    if (state.board.cells[box.r][box.c] == 'B')
    {
        if (!apply(state, MoveBox{event.right, box}))
        {
            return false;
        }
    }

    state.board.cells[box.r][box.c] = 'B';

    INFO << "after MoveBox:\n" << state;

    return true;
}

void applyGravity(State& state)
{
    // player
    for (int r = state.board.r_size - 1; r >= 0; --r)
    {
        for (int c = 0; c < state.board.c_size; ++c)
        {
            if (state.board.cells[r][c] == 'P' || state.board.cells[r][c] == 'B')
            {
                bool isPlayer = state.board.cells[r][c] == 'P';
                INFO << "checking r=" << r << ", c=" << c << ", cell is " << state.board.cells[r][c] << std::endl;
                Cell pos{r + 1, c};
                while (isValidGravityPos(state, pos)) ++pos.r;
                --pos.r;
                INFO << "after checking pos.r=" << pos.r << std::endl;
                std::swap(state.board.cells[r][c], state.board.cells[pos.r][c]);

                if (isPlayer) state.player = pos;
            }
        }
    }

    INFO << "applyGravity:\n" << state;
}

using Event = std::variant<MovePlayer, MoveBox>;

struct Input
{
    Board board;
    Cell player;
    std::vector<Event> commands;
};

/*
7 9
#########
#.......#
#..PB...#
#..##...#
#.......#
#.......#
#########
3
R
R
L
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
            if (line[c] == 'P')
            {
                result.player.r = r;
                result.player.c = c;
                break;
            }
        }
    }
    INFO << "result.player.r=" << result.player.r << ", result.player.c=" << result.player.c << std::endl;

    if (!std::getline(is, line)) throw "invalid input";
    int commandCnt = std::stoi(line);
    INFO << "commandCnt=" << commandCnt << std::endl;

    for (int i = 0; i < commandCnt; ++i)
    {
        if (!std::getline(is, line)) throw "invalid input";

        if (line[0] == 'L')
        {
            result.commands.push_back(MovePlayer(false));
        }
        else if (line[0] == 'R')
        {
            result.commands.push_back(MovePlayer(true));
        }
        else
        {
            throw "invalid input";
        }
    }

    return result;
}

bool run(std::istream& is)
{
    const auto input = parse(is);

    State state;
    state.board = input.board;
    state.player = input.player;

    int cmdCnt = 0;
    for (const auto& command : input.commands)
    {
        ++cmdCnt;
        auto stateCpy = state;
        
        if (!std::visit([&](auto&& e){ return apply(stateCpy, e); }, command))
        {
            std::cout << "INVALID " << cmdCnt << std::endl;
            return false;
        }

        applyGravity(stateCpy);

        state = std::move(stateCpy);
    }

    std::cout << "VALID" << std::endl;
    return true;
}

void test()
{
    {
    std::string input=R"(7 9
#########
#.......#
#..PB...#
#..##...#
#.......#
#.......#
#########
3
R
R
L
)";
    std::stringstream is(input);
    run(is);
    }


    {
    std::string input=R"(5 9
#########
#.......#
#.......#
#..PBB.##
#########
3
R
R
L
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(7 10
##########
#........#
#........#
#..PBB...#
#..###...#
#........#
##########
3
R
R
R
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(3 4
....
P...
####
1
L
)";
    std::stringstream is(input);
    run(is);
    }

// 玩家面对墙壁，第一条指令就失败。
{
    std::string input=R"(7 10
##########
#........#
#.......#
#........#
#.P###...#
#........#
##########
3
R
R
R
)";
    std::stringstream is(input);
    run(is);
}

// 玩家紧贴左 / 右边界，向边界外移动。
{
    std::string input=R"(7 10
##########
#........#
#P.......#
#........#
#..###...#
#........#
##########
3
L
R
R
)";
    std::stringstream is(input);
    run(is);
}

// 地图没有外围墙壁，但重力底边仍可支撑物体。
{
    std::string input=R"(7 10
##########
#........#
#.......P.
#.........
#..###....
#........#
##########
1
R
)";
    std::stringstream is(input);
    run(is);
}

/*


玩家直接移动到空地，且不产生重力位移。
玩家离开平台后下落多格。
箱子被推出平台后下落多格。
玩家下落后被箱子支撑。
箱子下落后被另一个箱子支撑。
推动一串长度大于 1 的箱子成功。
推动一串箱子时，最远端被墙阻挡而失败。
推动一串箱子时，最远端是地图边界外而失败。
推动后出现多个物体在同一 tick 可下落，检查同步语义。
下方物体在当前 tick 下落，上方物体不能同 tick 跟进。
重力持续多个 tick，必须执行到全图稳定。
某一条后续指令失败，正确输出首次失败的指令编号。
成功处理全部指令后，地图仍恰好包含一个玩家、原有数量的箱子。
*/

};

int main()
{
    // test();
    run(std::cin);
    return 0;
} 