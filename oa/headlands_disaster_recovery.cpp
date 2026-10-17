#include <algorithm>
#include <iostream>
#include <bits/stdc++.h>
#include <istream>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

bool enableLog = false;
#define LOG enableLog && std::cout << "[LOG " << __LINE__ << "]: "

#define CHECK(x) if (!(x)) std::cout << "[CHECK error " << __LINE__ << "]: " << #x << std::endl;

/*
14:40
non-negative integral commit id
on-negative integral timestamp
described by its path and an opaque identifier
*/

// To execute C++, please define "int main()"

struct FileInfo
{
    std::string path;
    std::string opaqueId; 
    inline auto operator<=> (const FileInfo&) const = default;
};

std::ostream& operator<< (std::ostream& os, const FileInfo& fi)
{
    os << "path=" << fi.path << "/opaqueId=" << fi.opaqueId;
    return os;
}

struct Commit
{
    uint64_t id;
    uint64_t ts;
    std::vector<FileInfo> fileInfos;
};

struct Query
{
    uint64_t startTs;
    uint64_t endTs; // inclusive
    FileInfo fileInfo;
};

struct Pathkey
{
    int groupId;
    std::string str;
    inline auto operator<=> (const Pathkey&) const = default;
};

struct Idkey
{
    int groupId;
    std::string str;
    inline auto operator<=> (const Idkey&) const = default;
};

class DSU
{
public:
    DSU(int n)
    : parents_(n)
    , ranks_(n)
    {
        for (int i = 0; i < n; ++i)
        {
            parents_[i] = i;
        }
    }

    int find(int a)
    {
        auto p = parents_[a];
        if (p != a)
        {
            parents_[a] = find(p);
        }
        return parents_[a];
    }

    bool unit(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b) return false;

        if (ranks_[a] < ranks_[b])
        {
            parents_[a] = b;
        }
        else if (ranks_[a] > ranks_[b])
        {
            parents_[b] = a;
        }
        else
        {
            parents_[b] = a;
            ++ranks_[a];
        }

        return true;
    }

private:
    std::vector<int> parents_;
    std::vector<int> ranks_;
};

std::optional<std::string_view> nextToken(std::string_view msg, size_t& pos)
{
    if (pos > msg.size()) return {};
    auto begin = pos;
    while (pos < msg.size() && !std::isspace(msg[pos])) ++pos;
    auto end = pos;
    ++pos;
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
id 0 timestamp 23534 include/foo.h da39a3 include/bar.h e615fe
id 2 timestamp 23534 include/baz.h 249e5d
id 1 timestamp 23606 include/bar.h e615fe src/bar.cpp 89bff2
*/
bool parseCommit(std::string_view line, Commit& commit)
{
    size_t pos{0};

    if (nextToken(line, pos) != "id") return false;
    if (!nextNumber(line, pos, commit.id)) return false;
    if (nextToken(line, pos) != "timestamp") return false;
    if (!nextNumber(line, pos, commit.ts)) return false;

    int fiCnt = 0;
    FileInfo fi;
    while (true)
    {
        auto path = nextToken(line, pos);
        if (!path) break;
        if (path == "") return false;

        auto opaqueId = nextToken(line, pos);
        if (!opaqueId || opaqueId == "") return false;;

        commit.fileInfos.push_back({std::string(*path), std::string(*opaqueId)});
        ++fiCnt;
    }

    return fiCnt > 0;
}

/*
0 10000 quicksort.cpp 839ad0
0 500 vector.h fcc2af
*/
bool parseQuery(std::string_view line, Query& query)
{
    size_t pos{0};
    if (!nextNumber(line, pos, query.startTs)) return false;
    if (!nextNumber(line, pos, query.endTs)) return false;

    auto path = nextToken(line, pos);
    if (!path || path == "") return false;

    auto opaqueId = nextToken(line, pos);
    if (!opaqueId || opaqueId == "") return false;;
    
    query.fileInfo.path = std::string(*path);
    query.fileInfo.opaqueId = std::string(*opaqueId);
    return !nextToken(line, pos).has_value() && query.endTs >= query.startTs;
}

bool parseNumber(std::string_view line, uint64_t& number)
{
    size_t pos{0};
    if (!nextNumber(line, pos, number)) return false;
    return !nextToken(line, pos).has_value();
}


void testParse()
{
    Commit commit;
    CHECK(parseCommit("id 0 timestamp 23534 include/foo.h da39a3 include/bar.h e615fe", commit));
    CHECK(!parseCommit("id 0 timestamp 23534 include/foo.h da39a3 include/bar.h", commit));
    CHECK(!parseCommit(" id 0 timestamp 23534 include/foo.h da39a3 include/bar.h e615fe", commit));
    CHECK(!parseCommit("id 0 timestamp 23534 include/foo.h da39a3 include/bar.h e615fe ", commit));
    CHECK(!parseCommit("id 0 timestamp 23534 include/foo.h da39a3 include/bar.h  e615fe", commit));
    CHECK(!parseCommit("id 0 timestamp 23534", commit));
    CHECK(!parseCommit("id -1 timestamp 23534 include/foo.h da39a3 include/bar.h e615fe", commit));

    Query query;
    CHECK(parseQuery("0 500 vector.h fcc2af", query));
    CHECK(parseQuery("500 500 vector.h fcc2af", query));
    CHECK(!parseQuery("501 500 vector.h fcc2af", query));
    CHECK(!parseQuery("0 500 vector.h fcc2af ", query));
    CHECK(!parseQuery("0 500 vector.h fcc2af vector.h fcc2af", query));
}

class Recovery
{
public:
    Recovery(const std::vector<Commit>& commits)
    : commits_(commits)
    , dsu_(int(commits_.size()))
    {
        int n = int(commits.size());
        for (int i = 0; i < n; ++i)
        {
            for (const auto& fi : commits[i].fileInfos)
            {
                if (auto it = fileInfo2CommitIdx_.find(fi); it != fileInfo2CommitIdx_.end())
                {
                    dsu_.unit(it->second, i);
                }
                else
                {
                    fileInfo2CommitIdx_[fi] = i;
                    // LOG << "fileInfo2CommitIdx_: add fi=" << fi << ", i=" << i << std::endl;
                }
            }
        }

        for (const auto& [fi, commitIdx] : fileInfo2CommitIdx_)
        {
            auto groupId = dsu_.find(commitIdx);
            fileInfo2GroupId_[fi] = groupId;
        }

        for (int i = 0; i < n; ++i)
        {
            auto groupId = dsu_.find(i);
            groupId2CommitIdxs_[groupId].push_back(i);
        }

        // check Ambiguous
        std::map<Pathkey, Idkey> path2OpaqueId;
        std::map<Idkey, Pathkey> opaque2PathId;
        for (const auto& [fileInfo, groupId] : fileInfo2GroupId_)
        {
            Pathkey pathKey{.groupId=groupId, .str=fileInfo.path};
            Idkey idKey{.groupId=groupId, .str=fileInfo.opaqueId};

            if (auto it = path2OpaqueId.find(pathKey); it != path2OpaqueId.end())
            {
                if (it->second != idKey)
                {
                    gotAmbiguous_ = true;
                    break;
                }
            }
            else
            {
                path2OpaqueId[pathKey] = idKey;
            }

            if (auto it = opaque2PathId.find(idKey); it != opaque2PathId.end())
            {
                if (it->second != pathKey)
                {
                    gotAmbiguous_ = true;
                    break;
                }
            }
            else
            {
                opaque2PathId[idKey] = pathKey;
            }
        }

        // sort
        if (!gotAmbiguous_)
        {
            for (auto& [_, commitIdxs] : groupId2CommitIdxs_)
            {
                std::sort(commitIdxs.begin(), commitIdxs.end(), [this](auto&& a, auto&& b) {
                    return std::tie(commits_[a].ts, commits_[a].id) < std::tie(commits_[b].ts, commits_[b].id);
                });
            }
        }
    }

    std::string query(const std::vector<Query>& queries)
    {
        if (gotAmbiguous_) 
            return "AMBIGUOUS INPUT!";

        std::string result;
        for (const auto& q: queries)
        {
            result += query(q);
            result += "\n";
        }
        return result;
    }

private:
    std::string query(const Query& query)
    {
        LOG << "query: fileInfo2GroupId_ size=" << fileInfo2GroupId_.size()
            << " fi" << query.fileInfo
            << std::endl;
        std::string result;
        if (auto it1 = fileInfo2GroupId_.find(query.fileInfo); it1 != fileInfo2GroupId_.end())
        {
            const auto& commitIdxs = groupId2CommitIdxs_[it1->second];
            auto it2 = std::lower_bound(commitIdxs.begin(), commitIdxs.end(), query.startTs, 
            [this](auto&& e, auto& target) {
                return commits_[e].ts < target;
            });

            for (; it2 != commitIdxs.end() && commits_[*it2].ts <= query.endTs; ++it2)
            {
                if (!result.empty())
                {
                    result += " ";
                }
                result += std::to_string(commits_[*it2].id);
            }

            LOG << "commitIdxs=" << commitIdxs.size()
                << " result=" << result
                << std::endl;
            return result;
        }
        else
        {
            return result;
        }
    }    

private:
    const std::vector<Commit>& commits_;
    DSU dsu_;
    std::map<FileInfo, int> fileInfo2CommitIdx_;
    std::map<FileInfo, int> fileInfo2GroupId_;
    std::map<int, std::vector<FileInfo>> groupId2FileInfos_;
    std::map<int, std::vector<int>> groupId2CommitIdxs_;
    bool gotAmbiguous_{false};
};

std::string run(const std::string& input)
{
    std::stringstream ss(input);
    std::string line;

    uint64_t commitNumber{};
    if (!std::getline(ss, line)) return "no commitNumber";
    if (!parseNumber(line, commitNumber)) return "wrong commitNumber: " + line;

    LOG << "commitNumber=" << commitNumber << std::endl;

    std::vector<Commit> commits;
    for (size_t i = 0; i < commitNumber; ++i)
    {
        if (!std::getline(ss, line)) return "no commit line";
        Commit commit;
        if (parseCommit(line, commit))
            commits.push_back(std::move(commit));
        else
        {
            LOG << "failed to parse: " << line << std::endl;
        }
    }

    uint64_t queryNumber{};
    if (!std::getline(ss, line)) return "no queryNumber";
    if (!parseNumber(line, queryNumber)) return "wrong queryNumber: " + line;

    LOG << "queryNumber=" << queryNumber << std::endl;

    std::vector<Query> queries;
    for (size_t i = 0; i < queryNumber; ++i)
    {
        if (!std::getline(ss, line)) return "no query line";
        Query query;
        if (parseQuery(line, query))
            queries.push_back(std::move(query));
        else
        {
            LOG << "failed to parse: " << line << std::endl;
        }
    }

    if (std::getline(ss, line)) return "got more line: " + line;

    LOG << "commits size = " << commits.size() << std::endl;
    LOG << "queries size = " << queries.size() << std::endl;

    Recovery qecovery(commits);
    return qecovery.query(queries);
}

int main()
{
    testParse();

    {
    std::string input =
R"(6
id 8 timestamp 200 quicksort.cpp 839ad0 mergesort.cpp 0cdde1 bubblesort.cpp 248dd1
id 0 timestamp 500 array.h 163111 sequence.h 294d3f
id 6 timestamp 200 mergesort.cpp 0cdde1 bogosort.cpp 4213ff
id 4 timestamp 1000 array.h 163111 vector.h fcc2af
id 2 timestamp 300 bubblesort.cpp 248dd1 bogosort.cpp 4213ff
id 3 timestamp 300 bubblesort.cpp eaf88a bogosort.cpp 4f11aa
4
0 10000 quicksort.cpp 839ad0
0 500 vector.h fcc2af
0 100000 no_found.h empty_response
100 200 bogosort.cpp 4213ff)";

    auto result = run(input);
    std::cout << result << std::endl;
    }

    {
    std::string input =
R"(5
id 38024 timestamp 74820 foo.py ac819f bar.py 0d82b9
id 49283 timestamp 19837 bar.py 0d82b9 baz.py f28dc2
id 20391 timestamp 23488 baz.py f28dc2 foo.py f918ca
id 2938 timestamp 101 qux.h d139af qux.cpp 718be3
id 2939 timestamp 102 qux.h d139af
2
0 100000 bar.py 0d82b9
0 100000 qux.h d139af)";

    auto result = run(input);
    std::cout << result << std::endl;
    }

    std::cout << "done" << std::endl;
    return 0;
}
