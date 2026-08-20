# Mini Shell (C++)

A Unix-like command-line shell written from scratch in C++, using direct
Linux system calls (no external libraries).

## Features
- Interactive REPL (prompt, read command, execute, repeat)
- External command execution via `fork()` + `execvp()`
- Built-in commands: `cd`, `pwd`, `export`, `unset`, `history`, `jobs`, `help`, `exit`
- I/O redirection: `<` (input), `>` (output, overwrite), `>>` (output, append)
- Pipes: `cmd1 | cmd2 | cmd3`
- Background execution: `cmd &`
- Signal handling: Ctrl+C doesn't kill the shell, only the running foreground command
- Environment variable expansion: `$HOME`, `$MYVAR`, etc.
- Persistent history saved to `~/.mini_shell_history`

## Build & Run
```bash
make
./mini_shell
```
or just:
```bash
g++ -std=c++17 -Wall -o mini_shell mini_shell.cpp
./mini_shell
```

## Try it out
```
pwd
echo hello
echo hi > out.txt
cat out.txt
cat out.txt | wc -l
sleep 5 &
jobs
export NAME=World
echo $NAME
history
exit
```

## How it works (architecture)

**1. Tokenizer (`tokenize`)**
Turns the raw input line into a list of tokens — words, quoted strings,
and operators (`|`, `<`, `>`, `>>`, `&`). This is the same first step a real
compiler/interpreter does (lexical analysis).

**2. Parser (`parsePipeline`)**
Walks the token list and groups tokens into one or more `Command` structs
(one per stage of a pipeline), pulling out redirection targets and the
background flag along the way.

**3. Built-ins (`runBuiltin`)**
Some commands (`cd`, `export`, `exit`, ...) **must** run inside the shell's
own process — if you `fork()` and run `cd` in a child process, only the
child's working directory changes, and the child immediately exits, so the
parent shell would never actually move. Built-ins are the exception to
"everything runs via fork+exec".

**4. Process creation (`executePipeline`)**
For every external command, we:
- `fork()` — create a child process that's a copy of the shell
- In the child: wire up pipes/redirection with `dup2()`, then `execvp()` to
  replace the child's memory image with the target program
- In the parent: either `waitpid()` for the child to finish (foreground),
  or just track its pid and move on (`&` background jobs)

**5. Pipes**
For N commands joined by `|`, we create N-1 pipes with `pipe()`. Each
command's stdout is `dup2`'d into the write end of the pipe feeding the next
command, and the next command's stdin is `dup2`'d from the read end. This is
exactly what your shell (bash/zsh) does under the hood for `cmd1 | cmd2`.

**6. Signals**
By default, Ctrl+C sends `SIGINT` to every process in the foreground
process group, including the shell — which would kill it. We install a
custom handler that forwards `SIGINT` to whatever child is currently in the
foreground, and does nothing (just reprints the prompt) if nothing is
running. This is why real shells survive Ctrl+C but your running command
doesn't.

## Things you could extend next
- Command history navigation with up/down arrows (needs raw terminal mode
  or a library like `readline`/`linenoise`)
- Tab completion
- Job control (`fg`, `bg`, `Ctrl+Z` to suspend)
- Wildcard/glob expansion (`*.txt`)
- Command substitution (`` `cmd` `` or `$(cmd)`)
- Aliases

## Talking about this project in interviews
Be ready to explain, in your own words:
- Why `fork()` + `execvp()` are two separate steps, and what each one does
- Why built-in commands like `cd` can't be implemented via fork+exec
- How `dup2()` redirects a file descriptor, and why file descriptors 0/1/2
  (stdin/stdout/stderr) matter here
- What a zombie process is, and why `waitpid()` (and `WNOHANG` for
  background jobs) prevents them
- Why the default SIGINT behavior would kill your shell, and how the
  custom handler fixes that

