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

bool enableInfo = true;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

struct Shape
{
    int r{};
    int c{};
    int size;
    std::array<std::array<int, 2>, 4> cells;    
};

Shape I_shape()
{
    Shape shape{.size = 4};
    shape.cells[0] = {1, 0};
    shape.cells[1] = {1, 1};
    shape.cells[2] = {1, 2};
    shape.cells[3] = {1, 3};
    return shape;    
}

Shape O_shape()
{
    Shape shape{.size = 2};
    shape.cells[0] = {0, 0};
    shape.cells[1] = {0, 1};
    shape.cells[2] = {1, 0};
    shape.cells[3] = {1, 1};
    return shape;
}


Shape T_shape()
{    
    Shape shape{.size = 3};
    shape.cells[0] = {0, 1};
    shape.cells[1] = {1, 0};
    shape.cells[2] = {1, 1};
    shape.cells[3] = {1, 2};
    return shape;
}

Shape L_shape()
{    
    Shape shape{.size = 3};
    shape.cells[0] = {0, 0};
    shape.cells[1] = {1, 0};
    shape.cells[2] = {1, 1};
    shape.cells[3] = {1, 2};
    return shape;    
}

Shape S_shape()
{
    Shape shape{.size = 3};
    shape.cells[0] = {0, 1};
    shape.cells[1] = {0, 2};
    shape.cells[2] = {1, 0};
    shape.cells[3] = {1, 1};
    return shape;
}

// old[i][j] -> rotated[j][N - 1 - i]
Shape rotate(const Shape& shape)
{
    auto res = shape;;
    for (auto& row : res.cells)
    {
        auto r = row[0];
        auto c = row[1];
        row[0] = c;
        row[1] = shape.size - 1 - r;
    }
    return res;
}

std::ostream& operator<< (std::ostream& os, const Shape& shape)
{
    for (int r = 0; r < shape.size; ++r)
    {
        for (int c = 0; c < shape.size; ++c)
        {
            if (std::find(shape.cells.begin(), shape.cells.end(), std::array{r - shape.r, c - shape.c}) != (shape.cells.end()))
                std::cout << "#";
            else
                std::cout << ".";
        }
        std::cout << "\n";
    }
    return os; 
}

struct Spawn
{
    Shape shape;
};

struct Rotate
{
};

struct Move
{
    bool right;
};

struct NA
{
};

struct Hard
{
};

using Event = std::variant<Spawn, Rotate, Move,  NA, Hard>;

struct Board
{
    int r_size;
    int c_size;
    std::vector<std::string> cells;
};

struct Input
{
    Board board;
    std::vector<std::vector<Event>> commandsList;
};

struct State
{
    Board board;
    std::optional<Shape> shape;
    int lines{0};
};

std::ostream& operator<< (std::ostream& os, const State& state)
{
    auto inShape = [&](int r, int c) {
        if (!state.shape) return false;
        return std::find(state.shape->cells.begin(), state.shape->cells.end(), std::array{r - state.shape->r, c - state.shape->c}) != (state.shape->cells.end());
    };

    os << "LINES " << state.lines << "\n";
    for (int r = 0; r < state.board.r_size; ++r)
    {
        for (int c = 0; c < state.board.c_size; ++c)
        {
            if (inShape(r, c))
                std::cout << "*";
            else
                std::cout << state.board.cells[r][c];
        }
        os << "\n";
    }
    return os; 
}

bool isGoodPos(const Board& board, const Shape& shape)
{
    for (const auto& cell : shape.cells)
    {
        if (cell[0] + shape.r < 0 || cell[1] + shape.c < 0 || cell[0] + shape.r >= board.r_size || cell[1] + shape.c >= board.c_size) return false;
        if (board.cells[cell[0] + shape.r][cell[1] + shape.c] != '.') return false;
    }
    return true;
}

void applyOnceGravity(State& state)
{
    Shape newShape = *state.shape;
    ++newShape.r;
    if (!isGoodPos(state.board, newShape))
    {
        return;
    }
    state.shape = newShape;
}

void apply(State& state, const Spawn& event)
{
    INFO << "Spawn:\n" << event.shape << std::endl;
    if (state.shape) throw "duplciated shape";
    if (!isGoodPos(state.board, event.shape))
    {
        throw "game over";
    }
    state.shape = event.shape;
    INFO << "after spawn:\n" << state << std::endl;
}

void apply(State& state, const Rotate&)
{
    INFO << "rotate" << std::endl;
    Shape newShape = rotate(*state.shape);
    if (isGoodPos(state.board, newShape))
    {
        state.shape = newShape;
    }
    applyOnceGravity(state);
    INFO << "after Rotate:\n" << state << std::endl;
}

void apply(State& state, const Move& event)
{
    INFO << "move, right=" << event.right << std::endl;
    Shape newShape = *state.shape;
    newShape.c += event.right ? 1 : -1;
    if (isGoodPos(state.board, newShape))
    {
        state.shape = newShape;
    }
    applyOnceGravity(state);
    INFO << "after move:\n" << state << std::endl;
}

void apply(State& state, const NA&)
{
    INFO << "NA" << std::endl;
    applyOnceGravity(state);
    INFO << "after NA:\n" << state << std::endl;
}

void apply(State& state, const Hard&)
{
    INFO << "Hard gravity" << std::endl;
    Shape newShape = *state.shape;
    while (true)
    {
        ++newShape.r;
        if (isGoodPos(state.board, newShape))
        {
            state.shape = newShape;
        }
        else
        {
            break;
        }
    }
    INFO << "after Hard gravity:\n" << state << std::endl;

    INFO << "Apply to board" << std::endl;
    for (const auto& cell : state.shape->cells)
    {
        state.board.cells[cell[0] + state.shape->r][cell[1] + state.shape->c] = '#';
    }
    state.shape.reset();
    INFO << "Hard reduce" << std::endl;

    for (int r = state.board.r_size - 1; r >= 0;)
    {
        if (std::all_of(state.board.cells[r].begin(), state.board.cells[r].end(), [](auto&& v) { return v == '#'; }))
        {
            ++state.lines;

            for (int r2 = r; r2 > 0; --r2)
            {
                state.board.cells[r2] = std::move(state.board.cells[r2 - 1]);
            }
            state.board.cells[0] = std::string(state.board.c_size, '.');
        }
        else
        {
            --r;
        }
    }
    INFO << "after Hard reduce:\n" << state << std::endl;
}

/*
6 6
......
......
......
......
......
......
1
O 1 H
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

    for (int r = 0; r < r_size; ++r)
    {
        if (!std::getline(is, line)) throw "invalid input";
        result.board.cells.push_back(line);
    }

    if (!std::getline(is, line)) throw "invalid input";
    int commandCnt = std::stoi(line);
    INFO << "commandCnt=" << commandCnt << std::endl;

    for (int i = 0; i < commandCnt; ++i)
    {
        std::vector<Event> commands;

        if (!std::getline(is, line)) throw "invalid input";

        std::stringstream is3(line);

        char shapeType;
        if (!(is3 >> shapeType)) throw "invalid input";
        INFO << "shapeType=" << shapeType << std::endl;

        Shape shape;
        if (shapeType == 'I')
        {
            shape = I_shape();
            // result.commands.push_back(Spawn());
        }
        else if (shapeType == 'O')
        {
            shape = O_shape();
        }
        else if (shapeType == 'T')
        {
            shape = T_shape();
        }
        else if (shapeType == 'L')
        {
            shape = L_shape();
        }
        else if (shapeType == 'S')
        {
            shape = S_shape();
        }
        else
        {
            throw "invalid input";
        }

        int shapePos;
        if (!(is3 >> shapePos)) throw "invalid input";
        shape.c += shapePos;;
        commands.push_back(Spawn(shape));
        INFO << "shapePos=" << shapePos << std::endl;

        char move;
        while (is3 >> move)
        {
            INFO << "move=" << move << std::endl;
            if (move == 'L')
            {
                commands.push_back(Move(false));
            }
            else if (move == 'R')
            {
                commands.push_back(Move(true));
            }
            else if (move == 'C')
            {
                commands.push_back(Rotate{});
            }
            else if (move == 'N')
            {
                commands.push_back(NA{});
            }
            else if (move == 'H')
            {
                commands.push_back(Hard{});
            }
        }

        result.commandsList.push_back(std::move(commands));
    }

    return result;
}

void run(std::istream& is)
{
    const auto input = parse(is);

    State state;
    state.board = input.board;

    int cmdCnt = 0;
    try
    {
        for (const auto& commands : input.commandsList)
        {
            ++cmdCnt;
            for (const auto& command : commands)
            {
                std::visit([&](auto&& e) { apply(state, e); }, command);
            }
        }

        std::cout << state << std::endl;
    }
    catch(...)
    {
        std::cout << "GAMEOVER " << cmdCnt << std::endl;
    }
}

void test()
{
    /*
    INFO << "I_shape=\n" << I_shape() << std::endl;
    INFO << "O_shape=\n" << O_shape() << std::endl;
    INFO << "T_shape=\n" << T_shape() << std::endl;
    INFO << "L_shape=\n" << L_shape() << std::endl;
    INFO << "S_shape=\n" << S_shape() << std::endl;

    INFO << "RI_shape=\n" << rotate(I_shape()) << std::endl;
    INFO << "RO_shape=\n" << rotate(O_shape()) << std::endl;
    INFO << "RT_shape=\n" << rotate(T_shape()) << std::endl;
    INFO << "RL_shape=\n" << rotate(L_shape()) << std::endl;
    INFO << "RS_shape=\n" << rotate(S_shape()) << std::endl;
    */
    {
    std::string input=R"(6 6
......
......
......
......
......
......
1
O 1 H
)";
    std::stringstream is(input);
    run(is);
    }


    {
    std::string input=R"(6 4
....
....
....
....
##..
##..
1
O 2 H
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(7 7
.......
.......
.......
.......
.......
.......
.......
1
T 2 CH
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(6 6
......
......
......
......
......
......
1
O 0 LH
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(6 6
..#...
......
......
......
......
......
1
O 2 H
)";
    std::stringstream is(input);
    run(is);
    }

    {
    std::string input=R"(6 6
......
......
......
......
......
......
2
O 0 H
O 2 H
)";
    std::stringstream is(input);
    run(is);
    }
}

void test1()
{
    std::string input=R"(8 6
......
......
......
......
......
......
......
......
1
L 0 N N N C R L H
)";
    std::stringstream is(input);
    run(is);
}

int main()
{
    // test1();
    run(std::cin);
    return 0;
}