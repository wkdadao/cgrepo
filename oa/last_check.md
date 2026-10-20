# OA Last Check

## C++23 Compile Commands

**1. Normal compile (warnings + debug):**

```bash
g++ -std=c++23 -Wall -Wextra -g main.cpp -o main
```

**2. Memory safety / undefined behavior check (ASan + UBSan):**

```bash
g++ -std=c++23 -g -fsanitize=address,undefined main.cpp -o main
```

Run with `./main`.

> Ensure the onsite compiler supports C++23. If not, use `-std=c++20` or the standard specified by the exercise.
