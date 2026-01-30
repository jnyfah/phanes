Core Data Structures (FIRST — non-negotiable)

Why first?
Everything depends on them.
CLI parsing is useless without something to configure.

You define:

FileNode

DirectoryNode

ErrorRecord

No filesystem code yet.
No CLI yet.
Just types.

If these are wrong, everything else will be painful.

2️⃣ Traversal & Model Construction (SECOND)

Now you answer:

“Given a path, how do I build the tree model?”

Implement:
DirectoryNode build_tree(const std::filesystem::path&, ErrorCollector&);


Rules:

no printing

no analysis

no formatting

no flags

Just:

collect data

record errors

return a value

This gives you a solid foundation.

3️⃣ Analysis Layer (THIRD)

Now you write pure functions:

SummaryReport summarize(const DirectoryNode&);
TypeReport by_type(const DirectoryNode&);
TopFilesReport top_files(const DirectoryNode&, size_t N);


Rules:

no filesystem

no CLI

no output

deterministic input → output

This layer is where:

ranges shine

algorithms live

correctness is tested

4️⃣ CLI Parsing (FOURTH — intentionally late)

Now that you know:

what reports exist

what parameters they need

You can sensibly parse CLI flags into:

struct Config {
    path root;
    bool summary;
    bool by_type;
    std::optional<size_t> top_n;
    std::optional<Duration> recent;
    ...
};


Parsing earlier is a trap — you’ll rewrite it.

5️⃣ Orchestration (FIFTH)

This is the glue, not the logic.

int main(int argc, char** argv) {
    Config cfg = parse_cli(argc, argv);
    DirectoryNode tree = build_tree(cfg.root);
    run_requested_reports(tree, cfg);
    format_and_print_results(...);
}


This layer:

decides what runs

decides parallelism

handles cancellation

handles progress

6️⃣ Output & Formatting (LAST)

Only now do you touch:

std::format

text vs JSON

output files

Why last?