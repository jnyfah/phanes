import builder;
import analysis;

#include <iostream>

auto main() -> int
{
    auto output = build_tree("/home");
    auto bud = compute_largest_N_Directories(output, 2);

    std::cout << output.directories[bud[0]].path.filename();
    std::cout << output.directories[bud[1]].path.filename();
}

// int main(int argc, char** argv) {
//     Config cfg = parse_cli(argc, argv);
//     DirectoryNode tree = build_tree(cfg.root);
//     run_requested_reports(tree, cfg);
//     format_and_print_results(...);
// }
