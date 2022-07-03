# kaleidoscope
- https://llvm.org/docs/tutorial/index.html

```shell
# Compile
clang++ -g -O3 main.cpp -o out/main `llvm-config --cxxflags --ldflags --system-libs --libs core orcjit native`

# Run
./out/main

# Output CFG
cd ir
opt <FILE_NAME> -dot-cfg > /dev/null
```
