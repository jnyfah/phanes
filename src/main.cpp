import builder;

#include <iostream>

auto main() -> int
{
    auto output = build_tree("/home");

    std::cout << output.directories[0].path;
}

// int main(int argc, char** argv) {
//     Config cfg = parse_cli(argc, argv);
//     DirectoryNode tree = build_tree(cfg.root);
//     run_requested_reports(tree, cfg);
//     format_and_print_results(...);
// }
