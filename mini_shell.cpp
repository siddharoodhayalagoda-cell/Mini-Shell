// mini_shell.cpp
// -----------------------------------------------------------------------------
// A Unix-like command-line shell written in C++.
//
// Features implemented:
//   1. Interactive REPL (read-eval-print loop)
//   2. External command execution via fork() + execvp()
//   3. Built-in commands: cd, pwd, export, unset, history, jobs, help, exit
//   4. I/O redirection: <, >, >>
//   5. Pipes: cmd1 | cmd2 | cmd3 ...
//   6. Background execution: cmd &
//   7. Signal handling: Ctrl+C (SIGINT) doesn't kill the shell itself
//   8. Environment variable expansion: $VAR
//   9. Persistent command history saved to ~/.mini_shell_history
//
// Build:  make
// Run:    ./mini_shell
// -----------------------------------------------------------------------------

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <pwd.h>
#include <climits>
#include <fstream>

using namespace std;

// ============================= Global state =================================
// g_history        : all commands entered this session (and loaded from disk)
// g_backgroundJobs  : pids of currently running background (`&`) jobs
// g_fgPid          : pid of the process currently running in the foreground,
//                    used so the SIGINT handler knows who to forward Ctrl+C to
vector<string> g_history;
vector<pid_t> g_backgroundJobs;
volatile sig_atomic_t g_fgPid = -1;

// ============================ History file path ==============================
string getHistoryFilePath() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return string(home) + "/.mini_shell_history";
}

// ============================= Signal handling ================================
// By default, Ctrl+C sends SIGINT to every process in the terminal's
// foreground process group -- including our shell. We don't want that:
// a real shell survives Ctrl+C and just kills whatever it's currently running.
//
// So we install our own handler that:
//   - forwards SIGINT to the foreground child (g_fgPid) if one is running
//   - otherwise just prints a fresh prompt line and keeps the shell alive
void sigintHandler(int signo) {
    if (g_fgPid > 0) {
        kill(g_fgPid, SIGINT);
    } else {
        cout << "\n";
    }
}

// ========================= Command data structure =============================
// One "Command" represents a single program + arguments in a pipeline,
// plus any redirection that applies to it.
struct Command {
    vector<string> args;
    string inputFile;
    string outputFile;
    bool appendOutput = false;
};

// =============================== Tokenizer ====================================
// Turns a raw input line into a list of tokens. Handles:
//   - whitespace separation
//   - double-quoted strings ("like this")
//   - special operator tokens: | < > >> &
vector<string> tokenize(const string& input) {
    vector<string> tokens;
    string current;
    bool inQuotes = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];

        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }

        if (!inQuotes && isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        if (!inQuotes && (c == '|' || c == '<' || c == '>' || c == '&')) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            if (c == '>' && i + 1 < input.size() && input[i + 1] == '>') {
                tokens.push_back(">>");
                i++;
            } else {
                tokens.push_back(string(1, c));
            }
            continue;
        }

        current += c;
    }

    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// ===================== Environment variable expansion =========================
// If a token starts with '$', treat the rest as an environment variable name
// and substitute its value (e.g. "$HOME" -> "/home/user").
string expandVariables(const string& token) {
    if (token.empty() || token[0] != '$') return token;
    string varName = token.substr(1);
    const char* val = getenv(varName.c_str());
    return val ? string(val) : string("");
}

// ========================= Parse tokens into a pipeline ========================
// Splits the token stream on '|' into individual Commands, and pulls out
// redirection targets (<, >, >>) and the trailing '&' background flag.
vector<Command> parsePipeline(vector<string>& tokens, bool& background) {
    vector<Command> commands;
    Command current;
    background = false;

    for (size_t i = 0; i < tokens.size(); i++) {
        const string& tok = tokens[i];

        if (tok == "|") {
            commands.push_back(current);
            current = Command();
        } else if (tok == "<") {
            if (i + 1 < tokens.size()) current.inputFile = tokens[++i];
        } else if (tok == ">") {
            if (i + 1 < tokens.size()) { current.outputFile = tokens[++i]; current.appendOutput = false; }
        } else if (tok == ">>") {
            if (i + 1 < tokens.size()) { current.outputFile = tokens[++i]; current.appendOutput = true; }
        } else if (tok == "&") {
            background = true;
        } else {
            current.args.push_back(expandVariables(tok));
        }
    }

    commands.push_back(current);
    return commands;
}

// ============================ Built-in commands ================================
// Built-ins run *inside* the shell process itself (no fork). This matters for
// commands like `cd` -- if you forked and ran cd in a child, the parent
// shell's working directory would never actually change.
bool runBuiltin(const Command& cmd, bool& shouldExit) {
    if (cmd.args.empty()) return true;
    const string& name = cmd.args[0];

    if (name == "exit") {
        shouldExit = true;
        return true;
    }

    if (name == "cd") {
        string target = cmd.args.size() > 1 ? cmd.args[1] : (getenv("HOME") ? getenv("HOME") : "/");
        if (chdir(target.c_str()) != 0) perror("cd");
        return true;
    }

    if (name == "pwd") {
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf))) cout << buf << "\n";
        return true;
    }

    if (name == "export") {
        // usage: export VAR=value
        if (cmd.args.size() > 1) {
            size_t eq = cmd.args[1].find('=');
            if (eq != string::npos) {
                string key = cmd.args[1].substr(0, eq);
                string val = cmd.args[1].substr(eq + 1);
                setenv(key.c_str(), val.c_str(), 1);
            }
        }
        return true;
    }

    if (name == "unset") {
        if (cmd.args.size() > 1) unsetenv(cmd.args[1].c_str());
        return true;
    }

    if (name == "history") {
        for (size_t i = 0; i < g_history.size(); i++)
            cout << "  " << i + 1 << "  " << g_history[i] << "\n";
        return true;
    }

    if (name == "jobs") {
        cout << g_backgroundJobs.size() << " background job(s) running\n";
        for (pid_t pid : g_backgroundJobs) cout << "  [pid " << pid << "]\n";
        return true;
    }

    if (name == "help") {
        cout << "mini-shell built-ins: cd, pwd, export, unset, history, jobs, exit, help\n";
        cout << "supports: pipes (|), redirection (<, >, >>), background (&), $VAR expansion\n";
        return true;
    }

    return false; // not a built-in -- caller should exec it externally
}

// ===================== Reap finished background jobs ============================
// Called each loop iteration so the shell doesn't accumulate zombie processes.
// WNOHANG makes waitpid non-blocking: it returns immediately if the child
// hasn't finished yet.
void reapBackgroundJobs() {
    for (auto it = g_backgroundJobs.begin(); it != g_backgroundJobs.end(); ) {
        int status;
        pid_t result = waitpid(*it, &status, WNOHANG);
        if (result > 0) {
            cout << "[background job " << *it << " finished]\n";
            it = g_backgroundJobs.erase(it);
        } else {
            ++it;
        }
    }
}

// ========================= Execute a pipeline of commands =======================
// This is the core of the shell. For N commands connected by pipes, we need
// N-1 pipe() pairs wiring each command's stdout to the next command's stdin.
void executePipeline(vector<Command>& commands, bool background) {
    int numCommands = static_cast<int>(commands.size());
    vector<pid_t> pids;
    vector<int> pipes((numCommands - 1) * 2);

    for (int i = 0; i < numCommands - 1; i++) {
        if (pipe(&pipes[i * 2]) < 0) {
            perror("pipe");
            return;
        }
    }

    for (int i = 0; i < numCommands; i++) {
        Command& cmd = commands[i];
        if (cmd.args.empty()) continue;

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return; }

        if (pid == 0) {
            // ---------------- Child process ----------------

            // Foreground children should die on Ctrl+C like normal programs;
            // only the shell process itself ignores/redirects SIGINT.
            signal(SIGINT, SIG_DFL);

            if (i > 0) dup2(pipes[(i - 1) * 2], STDIN_FILENO);
            if (i < numCommands - 1) dup2(pipes[i * 2 + 1], STDOUT_FILENO);

            for (int fd : pipes) close(fd); // duped fds already; originals unneeded

            if (!cmd.inputFile.empty()) {
                int fd = open(cmd.inputFile.c_str(), O_RDONLY);
                if (fd < 0) { perror("open input"); exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (!cmd.outputFile.empty()) {
                int flags = O_WRONLY | O_CREAT | (cmd.appendOutput ? O_APPEND : O_TRUNC);
                int fd = open(cmd.outputFile.c_str(), flags, 0644);
                if (fd < 0) { perror("open output"); exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            vector<char*> argv;
            for (auto& a : cmd.args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());

            // execvp only returns if it failed
            cerr << "mini-shell: command not found: " << cmd.args[0] << "\n";
            exit(127);
        }

        pids.push_back(pid);
    }

    // ---------------- Parent process ----------------
    for (int fd : pipes) close(fd);

    if (background) {
        for (pid_t pid : pids) g_backgroundJobs.push_back(pid);
        cout << "[running in background, pid " << pids.back() << "]\n";
    } else {
        for (pid_t pid : pids) {
            g_fgPid = pid;
            int status;
            waitpid(pid, &status, 0);
        }
        g_fgPid = -1;
    }
}

// ================================ Prompt ========================================
string buildPrompt() {
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    struct passwd* pw = getpwuid(getuid());
    string user = pw ? pw->pw_name : "user";
    return user + ":" + string(cwd) + "$ ";
}

// ============================ History persistence ================================
void loadHistory() {
    ifstream in(getHistoryFilePath());
    string line;
    while (getline(in, line)) if (!line.empty()) g_history.push_back(line);
}

void appendHistory(const string& line) {
    ofstream out(getHistoryFilePath(), ios::app);
    out << line << "\n";
}

// ================================ Main REPL =======================================
int main() {
    signal(SIGINT, sigintHandler);
    loadHistory();

    cout << "mini-shell (type 'help' for built-ins, 'exit' to quit)\n";

    string line;
    bool shouldExit = false;

    while (!shouldExit) {
        reapBackgroundJobs();

        cout << buildPrompt();
        if (!getline(cin, line)) break; // EOF (Ctrl+D)
        if (line.empty()) continue;

        g_history.push_back(line);
        appendHistory(line);

        vector<string> tokens = tokenize(line);
        if (tokens.empty()) continue;

        bool background;
        vector<Command> pipeline = parsePipeline(tokens, background);

        if (pipeline.size() == 1 && runBuiltin(pipeline[0], shouldExit)) {
            continue;
        }

        executePipeline(pipeline, background);
    }

    cout << "exit\n";
    return 0;
}
