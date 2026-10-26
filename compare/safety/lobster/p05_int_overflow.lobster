// Signed overflow at run time.
let n = command_line_arguments().length + 1  // argc, as in the Rust and Goose probes
let x = 9223372036854775807 + n
print x
