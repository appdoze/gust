// Recursion that never ends: exhausts the stack.
def f(n:int) -> int:
    if n < 0: return 0
    return f(n + 1) + 1
print f(command_line_arguments().length + 1)
