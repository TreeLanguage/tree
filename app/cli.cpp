#include "cli.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"
#include "resolver.h"
#include "token.h"
#include "tree/config.h"

#include <CLI/CLI.hpp>

namespace {

using Command = int (*)(CLI::App&);

struct CommandEntry {
    std::string_view name;
};

class TreeFormatter : public CLI::Formatter {
public:
    TreeFormatter() = default;

    std::string make_help(const CLI::App* app,
                          std::string /*unused*/,
                          CLI::AppFormatMode /*mode*/) const override {
        std::ostringstream out;

        if (app->get_parent() != nullptr) {
            out << "usage: " << app->get_parent()->get_name() << ' ' << app->get_name() << '\n';

            if (!app->get_description().empty()) {
                out << '\n' << app->get_description() << '\n';
            }

            return out.str();
        }

        out << "usage: " << app->get_name() << " [option] ... [ file ] [arg] ...\n";
        out << "Options:\n"
            << "  help       show this help message\n"
            << "  version    show version information\n"
            << "  <file>     run a Tree source file\n\n\n";
        out << "Arguments:\n"
            << "  file   : program read from script file\n"
            << "  arg ...: arguments passed to program in argv[1:]\n";

        return out.str();
    }
};

std::string read_file(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error("unable to open '" + path.string() + "'");
    }

    std::ostringstream stream;
    stream << file.rdbuf();

    return stream.str();
}

int run_file(const std::filesystem::path& path) {
    const std::string source = read_file(path);
    tree::DiagnosticEngine diagnostics(path.string(), source);

    const auto tokens = tree::lexer(source, diagnostics);
    const auto ast = tree::parse(tokens, diagnostics);
    [[maybe_unused]] const auto resolved = tree::resolve(ast, diagnostics);

    if (diagnostics.has_errors()) {
        diagnostics.print_all(std::cerr);
        return 1;
    }

    return 0;
}

int run_help(CLI::App& app, const std::string& topic) {
    if (topic.empty()) {
        std::cout << app.get_formatter()->make_help(&app,
                                                    app.get_name(),
                                                    CLI::AppFormatMode::Normal);

        return 0;
    }

    if (auto* sub = app.get_subcommand(topic)) {
        std::cout << sub->get_formatter()->make_help(sub,
                                                     sub->get_name(),
                                                     CLI::AppFormatMode::Normal);

        return 0;
    }

    std::cerr << "unknown command: " << topic << '\n';

    return 1;
}

int run_version(CLI::App& /*unused*/) {
    std::cout << "Tree " << tree::config::VERSION << '\n';

    return 0;
}

}  // namespace

namespace tree {

int run(int argc, char** argv) {
    CLI::App app{};
    app.name(std::filesystem::path(argv[0]).filename().string());
    app.formatter(std::make_shared<TreeFormatter>());

    std::string help_topic;
    auto* help_cmd = app.add_subcommand("help", "show this help message");
    help_cmd->add_option("topic", help_topic, "command to show help for");

    int result = 0;
    bool ran_subcommand = false;

    help_cmd->callback([&app, &help_topic, &result, &ran_subcommand] {
        ran_subcommand = true;
        result = run_help(app, help_topic);
    });

    auto* version_cmd = app.add_subcommand("version");
    version_cmd->callback([&app, &result, &ran_subcommand] {
        ran_subcommand = true;
        result = run_version(app);
    });

    std::filesystem::path input;
    app.add_option("file", input, "Tree source file")->check(CLI::ExistingFile);

    CLI11_PARSE(app, argc, argv);

    if (ran_subcommand) {
        return result;
    }

    if (!input.empty()) {
        return run_file(input);
    }

    std::cerr << app.get_formatter()->make_help(&app, app.get_name(), CLI::AppFormatMode::Normal);

    return 1;
}

}  // namespace tree