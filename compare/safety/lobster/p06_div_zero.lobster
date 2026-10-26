// Integer division by a zero only known at run time.
let n = command_line_arguments().length + 1  // argc, as in the Rust and Goose probes
print 10 / (n - 1)
