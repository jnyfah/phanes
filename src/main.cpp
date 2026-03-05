import builder;
import parser;
import executor;

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

    // we are not printing errors yet why ??
    const auto& result = parse(input);
    if (!result.success)
    {
        // Todo: print help function and errors
        return 0;
    }

    const auto tree = build_tree(result.path);

    const Executor executor{tree, std::cout};

    for (const auto& action : result.actions)
    {
        std::visit(executor, action);
    }

    return 0;
}
