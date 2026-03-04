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

        // Todo: print help function and errors
        return 0;
    }

    // get path here before passing to parse?
    auto output = build_tree("/home");

    // if (parse_result.options.summary)
    // {
    //     const auto& summary = compute_summary(output);
    //     print_summary(std::cout, summary);
    // }
    // if (parse_result.options.empty_dirs)
    // {
    //     const auto& empty = compute_empty_directories(output);
    //     print_empty_directories(std::cout, empty, output);
    // }
    // if (parse_result.options.extensions)
    // {
    //     const auto& extensions = compute_extension_stats(output);
    //     print_extension_stats(std::cout, extensions);
    // }
    // if (parse_result.options.symlinks)
    // {
    //     const auto& symlink = compute_symlinks(output);
    //     print_symlinks(std::cout, symlink, output);
    // }
    // if (parse_result.options.largest_dirs)
    // {
    //     const auto& largest_dir = compute_largest_N_Directories(output, parse_result.options.largest_dirs.value());
    //     print_largest_directories(std::cout, largest_dir, output);
    // }
    // if (parse_result.options.largest_files)
    // {
    //     const auto& largest_files = compute_largest_N_Files(output, parse_result.options.largest_files.value());
    //     print_largest_files(std::cout, largest_files, output);
    // }
    // if (parse_result.options.recent)
    // {
    //     const auto& recent = compute_recent_files(output, parse_result.options.recent.value());
    //     print_recent_files(std::cout, recent, parse_result.options.recent.value(), output);
    // }

    return 0;
}
