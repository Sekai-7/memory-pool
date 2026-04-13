## v4 Build Modes

`v4` 现在支持一个统一的 CMake 参数：`MEMORY_POOL_BUILD_MODE`。

- `PERF`
  - 用于 perf 分析、火焰图、热点定位和单元测试
  - 自动启用：
    - `CMAKE_BUILD_TYPE=RelWithDebInfo`
    - `BUILD_TESTING=ON`
    - `MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS=ON`
    - `-fno-omit-frame-pointer`

- `BENCHMARK`
  - 用于 benchmark 跑分
  - 自动启用：
    - `CMAKE_BUILD_TYPE=Release`
    - `BUILD_TESTING=OFF`
    - `MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS=OFF`

### 编译 PERF 版本

```bash
cmake -S v4 -B v4/build -DMEMORY_POOL_BUILD_MODE=PERF
cmake --build v4/build --target memory_pool_test
./v4/build/test/memory_pool_test
```

### 编译 BENCHMARK 版本

```bash
cmake -S v4 -B v4/build -DMEMORY_POOL_BUILD_MODE=BENCHMARK
cmake --build v4/build --target memory_pool_benchmark
./v4/build/benchmark/memory_pool_benchmark
```

切换模式时，直接在同一个 `v4/build` 目录重新执行 `cmake -S v4 -B v4/build -DMEMORY_POOL_BUILD_MODE=...` 即可，不需要维护两个 build 目录。