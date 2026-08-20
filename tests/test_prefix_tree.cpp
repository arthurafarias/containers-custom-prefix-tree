#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include <containers/custom/prefix_tree.hpp>

using containers::custom::prefix_tree;
using namespace std::string_view_literals;

namespace {

std::vector<std::string> collect(const prefix_tree<char>& tree) {
    std::vector<std::string> result;
    for (const auto& entry : tree) {
        result.emplace_back(entry.begin(), entry.end());
    }
    return result;
}

} // namespace

TEST_CASE("default-constructed tree is empty") {
    prefix_tree<char> tree;
    CHECK(tree.empty());
    CHECK(tree.size() == 0);
    CHECK(tree.begin() == tree.end());
}

TEST_CASE("insert reports whether the sequence was newly added") {
    prefix_tree<char> tree;
    CHECK(tree.insert("fox"sv).second);
    CHECK(tree.size() == 1);
    CHECK_FALSE(tree.insert("fox"sv).second);
    CHECK(tree.size() == 1);
    CHECK(tree.insert("dog"sv).second);
    CHECK(tree.size() == 2);
}

TEST_CASE("iteration yields every stored sequence in lexicographic order") {
    prefix_tree<char> tree{"fox"sv, "dog"sv, "fo"sv, "food"sv};
    CHECK(collect(tree) == std::vector<std::string>{"dog", "fo", "food", "fox"});
}

TEST_CASE("find/contains/count") {
    prefix_tree<char> tree{"arthur"sv, "arthur farias"sv};
    CHECK(tree.contains("arthur"sv));
    CHECK(tree.contains("arthur farias"sv));
    CHECK_FALSE(tree.contains("art"sv));
    CHECK(tree.count("arthur"sv) == 1);
    CHECK(tree.count("nope"sv) == 0);
    CHECK(tree.find("nope"sv) == tree.end());

    auto it = tree.find("arthur"sv);
    REQUIRE(it != tree.end());
    CHECK(std::ranges::equal(*it, "arthur"sv));
}

TEST_CASE("starts_with answers prefix-shaped questions without a full scan") {
    prefix_tree<char> tree{"fox"sv, "food"sv, "dog"sv};
    CHECK(tree.starts_with("fo"sv));
    CHECK(tree.starts_with(""sv));
    CHECK_FALSE(tree.starts_with("cat"sv));
}

TEST_CASE("prefix_range yields exactly the keys beginning with prefix, in order") {
    prefix_tree<char> tree{"fox"sv, "food"sv, "foo"sv, "dog"sv};
    auto [first, last] = tree.prefix_range("foo"sv);
    std::vector<std::string> matches;
    for (; first != last; ++first) {
        matches.emplace_back((*first).begin(), (*first).end());
    }
    CHECK(matches == std::vector<std::string>{"foo", "food"});

    auto [none_first, none_last] = tree.prefix_range("zzz"sv);
    CHECK(none_first == none_last);
}

TEST_CASE("erase removes exactly one sequence and prunes dead nodes") {
    prefix_tree<char> tree{"fox"sv, "food"sv};
    CHECK(tree.erase("fox"sv) == 1);
    CHECK(tree.size() == 1);
    CHECK_FALSE(tree.contains("fox"sv));
    CHECK(tree.contains("food"sv));
    CHECK(tree.erase("fox"sv) == 0);

    // "fo" was only ever an intermediate node, never inserted itself.
    CHECK_FALSE(tree.contains("fo"sv));
    CHECK(tree.starts_with("fo"sv));
}

TEST_CASE("erase(iterator) removes at a position and returns the next iterator") {
    prefix_tree<char> tree{"dog"sv, "fox"sv, "fox2"sv};
    auto it = tree.find("fox"sv);
    REQUIRE(it != tree.end());
    it = tree.erase(it);
    CHECK(std::ranges::equal(*it, "fox2"sv));
    CHECK(tree.size() == 2);
    CHECK_FALSE(tree.contains("fox"sv));
}

TEST_CASE("erase_prefix removes every sequence beginning with prefix") {
    prefix_tree<char> tree{"fox"sv, "food"sv, "foo"sv, "dog"sv};
    CHECK(tree.erase_prefix("foo"sv) == 2);
    CHECK(tree.size() == 2);
    CHECK(collect(tree) == std::vector<std::string>{"dog", "fox"});

    CHECK(tree.erase_prefix(""sv) == 2);
    CHECK(tree.empty());
}

TEST_CASE("clear empties the tree") {
    prefix_tree<char> tree{"fox"sv, "dog"sv};
    tree.clear();
    CHECK(tree.empty());
    CHECK(tree.begin() == tree.end());
}

TEST_CASE("copy construction deep-copies, independent of the original") {
    prefix_tree<char> original{"fox"sv, "dog"sv};
    prefix_tree<char> copy(original);
    copy.insert("cat"sv);
    CHECK(copy.size() == 3);
    CHECK(original.size() == 2);
    CHECK_FALSE(original.contains("cat"sv));
}

TEST_CASE("move construction leaves the source empty") {
    prefix_tree<char> original{"fox"sv, "dog"sv};
    prefix_tree<char> moved(std::move(original));
    CHECK(moved.size() == 2);
    CHECK(original.empty());
}

TEST_CASE("operator== compares contents, not identity") {
    prefix_tree<char> a{"fox"sv, "dog"sv};
    prefix_tree<char> b{"dog"sv, "fox"sv};
    prefix_tree<char> c{"fox"sv};
    CHECK(a == b);
    CHECK_FALSE(a == c);
}

TEST_CASE("swap exchanges contents") {
    prefix_tree<char> a{"fox"sv};
    prefix_tree<char> b{"dog"sv, "cat"sv};
    a.swap(b);
    CHECK(a.size() == 2);
    CHECK(a.contains("dog"sv));
    CHECK(b.size() == 1);
    CHECK(b.contains("fox"sv));
}

TEST_CASE("element_view streams and compares like the sequence it reconstructs") {
    prefix_tree<char> tree{"arthur"sv};
    auto it = tree.begin();
    REQUIRE(it != tree.end());
    CHECK(*it == "arthur"sv);
    std::ostringstream os;
    os << *it;
    CHECK(os.str() == "arthur");
}
