Me at 3am doing some staff with local llm.

> [!TIP]
> Do not use AI for production code, keep your code base clean! ^^

# Wtf is this

A small cli app, that generates commit messages using local llm

# Usage Server

```sh
   ____                          _ _    ____
  / ___|___  _ __ ___  _ __ ___ (_) |_ / ___| ___ _ __
 | |   / _ \| '_ ` _ \| '_ ` _ \| | __| |  _ / _ \ '_ \
 | |__| (_) | | | | | | | | | | | | |_| |_| |  __/ | | |
  \____\___/|_| |_| |_|_| |_| |_|_|\__|\____|\___|_| |_|

  AI-powered commit message generator

USAGE:
  ./build/commitgen-server --start <model_path>   Start the server
  ./build/commitgen-server --stop                 Stop the server
  ./build/commitgen-server --status               Check server status

EXAMPLES:
  # Start with a GGUF model
  ./build/commitgen-server --start ~/models/codellama-7b.Q4_K_M.gguf

  # Start with an Ollama model blob
  ./build/commitgen-server --start ~/.ollama/models/blobs/sha256-abc123
```

# Usage client

```sh
CommitGen - AI-powered commit message generator

USAGE:
  ./build/commitgen [OPTIONS]

OPTIONS:
  -p, --path <dir>      Git repository path (default: current directory)
  -f, --file <file>     Generate commit for specific file only
  -e, --each            Interactive mode: commit each file separately
  -a, --all             Generate single commit for all staged changes
  -u, --unstaged        Use unstaged changes instead of staged
  -l, --list            List changed files
  -s, --status          Check server status
  -y, --yes             Auto-accept all commits (no prompts)
  -h, --help            Show this help message

EXAMPLES:
  # Generate commit for all staged changes
  ./build/commitgen

  # Interactive mode - commit each file separately
  ./build/commitgen --each

  # Generate commit for a specific file
  ./build/commitgen -f src/main.cpp

  # Interactive mode for another repository
  ./build/commitgen --path ~/projects/myapp --each
```
