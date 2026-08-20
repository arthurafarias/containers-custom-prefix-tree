#include <array>
#include <iostream>
#include <memory_resource>
#include <string_view>

#include <containers/custom/prefix_tree.hpp>

int main() {
    using namespace std::string_view_literals;

    // prefix_tree<char> stores sequences of char -- i.e. it behaves like a
    // set<string>. Dereferencing an iterator does not copy a string out of
    // the tree: it hands back a zero-copy view that reads each character
    // directly out of the tree's own nodes as you iterate it.
    containers::custom::prefix_tree<char> names{"arthur"sv, "arthur farias"sv,
                                                 "farias"sv};

    std::cout << "-- all entries, in lexicographic order --\n";
    for (const auto& entry : names) {
        std::cout << entry << '\n';
    }

    std::cout << "\n-- prefix_range for \"arthur\" --\n";
    auto [first, last] = names.prefix_range("arthur"sv);
    for (; first != last; ++first) {
        std::cout << *first << '\n';
    }

    std::cout << std::boolalpha;
    std::cout << "\nstarts_with(\"far\"): " << names.starts_with("far"sv)
               << '\n';
    std::cout << "contains(\"farias\"): " << names.contains("farias"sv)
               << '\n';

    // A fixed-size stack buffer backs every node allocation for this
    // prefix_tree -- no heap traffic at all as long as the buffer isn't
    // exhausted.
    std::array<std::byte, 4096> buffer{};
    std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
    containers::custom::prefix_tree<char> arena_names(&arena);
    arena_names.insert("stack-allocated"sv);
    arena_names.insert("stack-allocated-too"sv);

    std::cout << "\n-- arena-backed prefix_tree --\n";
    for (const auto& entry : arena_names) {
        std::cout << entry << '\n';
    }

    return 0;
}
