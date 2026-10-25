# subprocess.h

`subprocess.h`, copied unmodified from
[sheredom/subprocess.h](https://github.com/sheredom/subprocess.h) at commit
`8a4715c126433c0f55abe7c10431830864a18e74` (fetched 2026-10-10).

A single-header library for starting a process with a working directory and
environment, reading its output, and waiting for its exit code. It supports
Windows, Linux, and macOS. `src/process.cpp` uses it for the compiler's process
runner, which starts the C compiler, cmake, and git for `goose build` and
packages.

License: public domain (the Unlicense); the full text is at the top of the
file.

To update, copy the file from a newer commit and record that commit here.
