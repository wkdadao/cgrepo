#include <bits/stdc++.h>

#define CHECK(x) if (!(x)) std::cout << "[CHECK ERROR " << __LINE__ << "]: " << #x << std::endl;

bool enableInfo = false;
#define INFO enableInfo && std::cout << "[INFO trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

bool enableError = true;
#define ERROR enableError && std::cout << "[ERROR trd_" << std::this_thread::get_id() << " " << __LINE__ << "]: "

struct Board
{
    int rSize{};
    int cSize{};
    std::vector<std::vector<int>> cells;
};

std::ostream& operator<< (std::ostream& os, const Board& board)
{
    for (int r = 0; r < board.rSize; ++r)
    {
        for (int c = 0; c < board.cSize; ++c)
        {
            if (c > 0) os << " ";
            os << board.cells[r][c];
        }
        os << "\n";
    }
    return os;
}

/*
5 5
3 1 1 3 2
2 1 2 2 1
2 2 4 4 4
1 2 4 3 1
1 4 3 1 3
*/

Board parse(std::istream& is)
{
    Board board{};

    std::string line;
    if (!std::getline(is, line)) throw "invalid";
    std::stringstream ss1(line);
    if (!(ss1 >> board.rSize >> board.cSize)) throw "invalid";

    for (int r = 0; r < board.rSize; ++r)
    {
        if (!std::getline(is, line)) throw "invalid";
        std::stringstream ss2(line);
        std::vector<int> row;
        for (int c = 0; c < board.cSize; ++c)
        {
            int cell{};
            if (!(ss2 >> cell))  throw "invalid";
            row.push_back(cell);
        }
        board.cells.push_back(std::move(row));
    }

    INFO << "rSize=" << board.rSize << ", cSize=" << board.cSize << "\n" << board << std::endl;
    return board;
}

struct Cell
{
    int r;
    int c;    
};

void run(std::istream& is, std::ostream& os)
{
    auto board = parse(is);

    int rounds = 0;
    while (board.rSize > 0 && board.cSize > 0)
    {
        INFO << "rounds=" << rounds << "\n" << board << std::endl;

        std::vector<Cell> matched;

        // rows
        for (int r = 0 ; r < board.rSize; ++r)
        {
            int lastVal = board.cells[r][0];
            int sameCnt = 1;
            for (int c = 1; c < board.cSize; ++c)
            {
                if (board.cells[r][c] != 0 && board.cells[r][c] == lastVal)
                {
                    ++sameCnt;
                }
                else
                {
                    if (sameCnt >= 3)
                    {
                        for (int i = c - sameCnt; i < c; ++i)
                        {
                            matched.push_back(Cell{r, i});
                            INFO << "push r=" << r << ", c=" << i << std::endl;
                        }
                    }

                    lastVal = board.cells[r][c];
                    sameCnt = 1;
                }
            }

            if (sameCnt >= 3)
            {
                for (int i = board.cSize - sameCnt; i < board.cSize; ++i)
                {
                    matched.push_back(Cell{r, i});
                    INFO << "push r=" << r << ", c=" << i << std::endl;
                }
            }
        }

        // col
        for (int c = 0 ; c < board.cSize; ++c)
        {
            int lastVal = board.cells[0][c];
            int sameCnt = 1;
            for (int r = 1; r < board.rSize; ++r)
            {
                if (board.cells[r][c] != 0 && board.cells[r][c] == lastVal)
                {
                    ++sameCnt;
                }
                else
                {
                    if (sameCnt >= 3)
                    {
                        for (int i = r - sameCnt; i < r; ++i)
                        {
                            matched.push_back(Cell{i, c});
                            INFO << "push r=" << i << ", c=" << c << std::endl;
                        }
                    }

                    lastVal = board.cells[r][c];
                    sameCnt = 1;
                }
            }

            if (sameCnt >= 3)
            {
                for (int i = board.rSize - sameCnt; i < board.rSize; ++i)
                {
                    matched.push_back(Cell{i, c});
                    INFO << "push r=" << i << ", c=" << c << std::endl;
                }
            }
        }

        if (matched.empty()) break;

        for (const auto& cell : matched)
        {
            board.cells[cell.r][cell.c] = 0;
        }
        INFO << "after setting\n" << board << std::endl;

        // reduce
        /*
        for (int r = board.rSize - 2; r >= 0; --r)
        {
            for (int c = 0 ; c < board.cSize; ++c)
            {
                if (board.cells[r + 1][c] == 0)
                {
                    auto rr = r + 1;
                    while (rr < board.rSize && board.cells[rr][c] == 0) ++rr;
                    board.cells[rr - 1][c] = board.cells[r][c];
                    board.cells[r][c] = 0;
                }
            }
        }
        */
        for (int c = 0 ; c < board.cSize; ++c)
        {
            //for (int r = board.rSize - 2; r >= 0; --r)
            auto down = board.rSize - 1;
            for (auto up = board.rSize - 1; up >= 0; --up)
            {
                if (board.cells[up][c] != 0)
                {
                    board.cells[down][c] = board.cells[up][c];
                    --down;
                }
            }

            for (; down >= 0; --down)
            {
                board.cells[down][c] = 0;
            }
        }

        INFO << "after reduce\n" << board << std::endl;

        ++rounds;
    }

    os << "ROUNDS " << rounds << "\n";
    os << board;
}

void test()
{
    {
        std::string input = R"(5 5
3 1 1 3 2
2 1 2 2 1
2 2 4 4 4
1 2 4 3 1
1 4 3 1 3)";
        std::string output = R"(ROUNDS 2
0 0 0 0 0
3 1 0 0 2
2 1 1 3 1
1 2 4 3 1
1 4 3 1 3
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(3 3
1 2 3
2 2 2
3 2 1)";
        std::string output = R"(ROUNDS 1
0 0 0
1 0 3
3 0 1
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(3 4
1 2 3 1
2 3 1 2
3 1 2 3)";
        std::string output = R"(ROUNDS 0
1 2 3 1
2 3 1 2
3 1 2 3
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(3 3
0 0 0
0 0 0
0 0 0)";
        std::string output = R"(ROUNDS 0
0 0 0
0 0 0
0 0 0
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(3 5
0 0 0 0 0
1 2 3 4 5
7 7 7 7 7)";
        std::string output = R"(ROUNDS 1
0 0 0 0 0
0 0 0 0 0
1 2 3 4 5
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(0 0
)";
        std::string output = R"(ROUNDS 0
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(2 3
1 1 1
1 1 1
)";
        std::string output = R"(ROUNDS 1
0 0 0
0 0 0
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(2 3
1 0 1
0 1 0
)";
        std::string output = R"(ROUNDS 0
1 0 1
0 1 0
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }


    {
        std::string input = R"(4 3
1 1 0
0 1 0
0 1 0
0 1 0
)";
        std::string output = R"(ROUNDS 1
0 0 0
0 0 0
0 0 0
1 0 0
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }

    {
        std::string input = R"(2 3
1 0 1
1 1 1
)";
        std::string output = R"(ROUNDS 1
0 0 0
1 0 1
)";
        std::stringstream is(input);
        std::stringstream os;
        run(is, os);
        CHECK(os.str() == output);
    }
}

int main()
{
    //run(std::cin, std::cout);
    test();
    return 0;
}
