import builder;
import analyzer;
import parser;
import view;

#include <iostream>
#include <string_view>
#include <vector>

auto main(int argc, char* argv[]) -> int
{
    std::vector<std::string_view> input;
    input.reserve(argc - 1);

    for (int i = 1; i < argc; i++)
    {
        input.emplace_back(argv[i]);
    }

    const auto& parse_result = parse(input);
    if (!parse_result.success)
    {

        // print help??
    }

    // build the tree
    auto output = build_tree("/home");

    // we need to know which options we even parsed ??
    // what if path is not a path ?? i think normalizd handles that
    // do we need a funtion that takes the tree and the parse results then iterates to
    // find which options were passed, ehhe then how do we do the ouput ?? we have to pass the
    // functons to the format output

    auto buds = compute_summary(output);
    print_summary(std::cout, buds);
    // auto bud = compute_largest_N_Files(output, 2);

    // std::cout << output.files[bud[0]].path.filename().string();
    // std::cout << output.files[bud[1]].path.filename().string();
}
// Todo
// input
// output format
//