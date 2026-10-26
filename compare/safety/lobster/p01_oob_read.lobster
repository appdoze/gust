// Read past the end of an array, with an index only known at run time.
let n = command_line_arguments().length + 1  // argc, as in the Rust and Goose probes
let v = [1, 2, 3, 4, 5]
print v[n + 9]
