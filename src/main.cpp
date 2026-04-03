import builder;
import parser;
import executor;

#include <iostream>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

auto main(int argc, char* argv[]) -> int
{
    std::ios::sync_with_stdio(false);
    std::cout.tie(nullptr);

    const std::span args(argv + 1, static_cast<std::size_t>(argc - 1));

    if (args.empty())
    {
        print_help(std::cout);
        return 0;
    }

    std::vector<std::string_view> input(args.begin(), args.end());
    const auto result = parse(input);

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
