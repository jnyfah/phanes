#include <iostream>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

import builder;
import parser;
import executor;

auto main(int argc, char* argv[]) -> int
{
    std::ios::sync_with_stdio(false);
    std::cout.tie(nullptr);

    // view of CLI argument, skipping the program name
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty())
    {
        print_help(std::cout);
        return 0;
    }

    const auto result = parse(std::span{args});

    if (!result)
    {
        for (const auto& err : result.error())
        {
            std::cerr << "warning: " << err << '\n';
            std::cerr << '\n';
        }
    }

    if (result->actions.empty())
    {
        print_help(std::cout);
        return 0;
    }

    const auto tree = build_tree(result->path);

    const Executor executor{tree, std::cout};

    executor.run(result->actions);

    return 0;
}
