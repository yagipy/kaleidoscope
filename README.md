# kaleidoscope
- https://llvm.org/docs/tutorial/index.html

```shell
# Compile
clang++ -g -O3 main.cpp -o out/main `llvm-config --cxxflags --ldflags --system-libs --libs core orcjit native`

# Output object file
clang++ -g -O3 main.cpp -o out/main `llvm-config --cxxflags --ldflags --system-libs --libs all`

./out/main

ready> def average(x y) (x + y) * 0.5;
^D
Wrote output.o

clang++ test.cpp output.o -o main

./main

average of 3.0 and 4.0: 3.5

# Run
./out/main

# Output CFG
cd ir
opt <FILE_NAME> -dot-cfg > /dev/null
```
