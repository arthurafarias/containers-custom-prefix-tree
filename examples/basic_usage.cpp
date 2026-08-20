#include <containers/custom/prefix_tree.hpp>

#include <array>
#include <iostream>
#include <memory_resource>

int main() {
    // Default construction uses std::pmr::get_default_resource().
    containers::custom::prefix_tree<int> word_count;
    for (std::string_view w : {"the", "quick", "brown", "fox", "the", "fox"}) {
        ++word_count[std::string(w)];
    }

    std::cout << "-- all entries, in lexicographic order --\n";
    for (const auto& entry : word_count) {
        std::cout << entry.first << ": " << entry.second << '\n';
    }

    std::cout << "\n-- autocomplete for prefix \"f\" --\n";
    for (auto [it, end] = word_count.prefix_range("f"); it != end; ++it) {
        std::cout << it->first << '\n';
    }

    // A fixed-size stack buffer backs every node allocation for this prefix_tree --
    // no heap traffic at all as long as the buffer isn't exhausted.
    std::array<std::byte, 4096> buffer{};
    std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
    containers::custom::prefix_tree<int> arena_trie{&arena};
    arena_trie.insert("stack-allocated", 1);
    arena_trie.insert("stack-allocated-too", 2);

    std::cout << "\n-- arena-backed prefix_tree --\n";
    for (const auto& entry : arena_trie) {
        std::cout << entry.first << ": " << entry.second << '\n';
    }
}
