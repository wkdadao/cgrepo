#include <algorithm>
#include <iostream>
#include <bits/stdc++.h>
#include <istream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <vector>
using namespace std;

bool enableLog = false;
#define LOG enableLog && std::cout << "[LOG " << __LINE__ << "]: "

#define CHECK(x) if (!(x)) std::cerr << "[CHECK error " << __LINE__ << "]: " << #x << std::endl;

/*
19:20

*/

using Shape = std::array<std::array<int, 2>, 4>;

Shape Q_shape()
{
    Shape shape{};
    shape[0] = {0, 0};
    shape[1] = {0, 1};
    shape[2] = {1, 0};
    shape[3] = {1, 1};
    return shape;
}

Shape Z_shape()
{
    Shape shape{};
    shape[0] = {0, 0};
    shape[1] = {0, 1};
    shape[2] = {1, 1};
    shape[3] = {1, 2};
    return shape;
}

Shape S_shape()
{
    Shape shape{};
    shape[0] = {0, 1};
    shape[1] = {0, 2};
    shape[2] = {1, 0};
    shape[3] = {1, 1};
    return shape;
}

Shape T_shape()
{    
    Shape shape{};
    shape[0] = {0, 0};
    shape[1] = {0, 1};
    shape[2] = {0, 2};
    shape[3] = {1, 1};
    return shape;
}

Shape I_shape()
{
    Shape shape{};
    shape[0] = {0, 0};
    shape[1] = {0, 1};
    shape[2] = {0, 2};
    shape[3] = {0, 3};
    return shape;    
}

Shape L_shape()
{    
    Shape shape{};
    shape[0] = {0, 0};
    shape[1] = {1, 0};
    shape[2] = {2, 0};
    shape[3] = {2, 1};
    return shape;    
}

Shape J_shape()
{    
    Shape shape{};
    shape[0] = {0, 1};
    shape[1] = {1, 1};
    shape[2] = {2, 0};
    shape[3] = {2, 1};
    return shape;
}

using Board = std::array<std::array<int, 10>, 10>;

std::string toStr(const Shape& shape)
{
    std::string result;
    std::set<std::pair<int, int>> set;
    for (const auto& p : shape)
    {
        set.emplace(p[0], p[1]);
    }
    for (int r = 0; r < 4; ++r)
    {
        result += "\n";
        for (int c = 0; c < 4; ++c)
        {
            result += (set.contains(std::pair(r, c)) ? "*" : " ");
        }
    }
    return result;
}

std::string toStr(const Board& board)
{
    std::string result;
    for (const auto& r : board)
    {
        result += "\n";
        for (const auto& c : r)
        {
            result += (c == 1 ? "*" : " ");
        }
    }
    return result;
}

Shape moveCol(const Shape& shape, int c)
{
    auto result = shape;
    for (auto& p : result)
    {
        p[1] += c;
    }
    return result;
}

Shape moveRow(const Shape& shape, int r)
{
    auto result = shape;
    for (auto& p : result)
    {
        p[0] += r;
    }
    return result;
}

bool canMoveTo(const Shape& shape, const Board& board)
{
    int n = int(board.size());
    for (auto& p : shape)
    {
        if (p[0] < 0 || p[1] < 0 || p[0] >= n || p[1] >= n)
        {
            return false;
        }

        if (board[p[0]][p[1]] == 1)
        {
            return false;
        }
    }
    return true;   
}

void applyShape(const Shape& shape, Board& board)
{
    for (auto& p : shape)
    {
        board[p[0]][p[1]] = 1;
    }
}

void reduce(Board& board)
{
    int n = int(board.size());
    for (int r = n - 1; r >= 0;)
    {
        int fillCnt = 0;
        for (int c = 0; c < n; ++c)
        {
            if (board[r][c] == 1)
                ++fillCnt;
        }

        if (fillCnt == n)
        {
            for (int r2 = r; r2 > 0; --r2)
            {
                board[r2] = board[r2 - 1];
            }
            board[0] = {};            
        }
        else
        {
            --r;
        }
    }
}

/*
Q0,I2,I6,I0,I6,I6,Q2,Q4
*/

std::vector<Shape> parse(const std::string& input)
{
    std::vector<Shape> result;

    std::stringstream is(input);
    std::string line;
    while (std::getline(is, line, ','))
    {
        char type = line[0];
        int pos = std::stoi(line.substr(1, line.size() - 1));
        Shape shape;
        if (type == 'Q')
        {
            shape = Q_shape();
        }
        else if (type == 'Z')
        {
            shape = Z_shape();
        }
        else if (type == 'S')
        {
            shape = S_shape();
        }
        else if (type == 'T')
        {
            shape = T_shape();
        }
        else if (type == 'I')
        {
            shape = I_shape();
        }
        else if (type == 'L')
        {
            shape = L_shape();
        }
        else if (type == 'J')
        {
            shape = J_shape();
        }
        shape = moveCol(shape, pos);
        result.push_back(shape);
    }
    return result;
}

class Teris
{
public:
    bool process(const Shape& shape)
    {
        LOG << "process: shape" << toStr(shape) << std::endl;

        if (!canMoveTo(shape, board_))
            return false;

        int n = int(board_.size());
        Shape newShape = shape;
        for (int r = 1; r < n; ++r)
        {
            auto tmp = moveRow(newShape, 1);
            if (!canMoveTo(tmp, board_))
                break;
            LOG << "can move to r=" << r << std::endl;
            newShape = tmp;
        }

        LOG << "before apply: board_" << toStr(board_) << std::endl;

        applyShape(newShape, board_);
        
        LOG << "before reduce: board_" << toStr(board_) << std::endl;

        reduce(board_);

        LOG << "after reduce: board_" << toStr(board_) << std::endl;

        return true;
    }

    int getHeight() const
    {
        int n = int(board_.size());
        for (int r = 0; r < n; ++r)
        {
            for (int c = 0; c < n; ++c)
            {
                if (board_[r][c])
                {
                    return n - r;
                }
            }
        }
        return 0;
    }

private:
    Board board_;
};

int main()
{
    LOG << toStr(Q_shape()) << std::endl;
    LOG << toStr(moveCol(Q_shape(), 1)) << std::endl;
    LOG << toStr(Z_shape()) << std::endl;
    LOG << toStr(S_shape()) << std::endl;
    LOG << toStr(T_shape()) << std::endl;
    LOG << toStr(I_shape()) << std::endl;
    LOG << toStr(L_shape()) << std::endl;
    LOG << toStr(J_shape()) << std::endl;
    LOG << toStr(moveRow(J_shape(), 1)) << std::endl;
    LOG << "done" << std::endl;

    {
        auto shapes = parse("Q0");
        Teris teris{};
        for (const auto& shape : shapes)
        {
            teris.process(shape);
        }
        std::cout << "============>" << teris.getHeight() << std::endl;
    }

    {
        auto shapes = parse("I0,I4,Q8");
        Teris teris{};
        for (const auto& shape : shapes)
        {
            teris.process(shape);
        }
        std::cout << "============>" << teris.getHeight() << std::endl;
    }

    {
        auto shapes = parse("T1,Z3,I4");
        Teris teris{};
        for (const auto& shape : shapes)
        {
            teris.process(shape);
        }
        std::cout << "============>" << teris.getHeight() << std::endl;
    }

    {
        auto shapes = parse("Q0,I2,I6,I0,I6,I6,Q2,Q4");
        Teris teris{};
        for (const auto& shape : shapes)
        {
            teris.process(shape);
        }
        std::cout << "============>" << teris.getHeight() << std::endl;
    }

    {
        auto shapes = parse("");
        Teris teris{};
        for (const auto& shape : shapes)
        {
            teris.process(shape);
        }
        std::cout << "============>" << teris.getHeight() << std::endl;
    }

    {
        auto shapes = parse("Q0,Q2,Q4,Q6,Q8");
        Teris teris{};
        for (const auto& shape : shapes)
        {
            teris.process(shape);
        }
        std::cout << "============>" << teris.getHeight() << std::endl;
    }

    return 0;
}
