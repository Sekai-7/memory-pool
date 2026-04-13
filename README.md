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

### Benchmark 指标说明

`memory_pool_benchmark` 现在除了原有的吞吐/时延 case，还新增了几组指标型 case：

- `BM_*WarmAllocateFree`
  - 先做一轮 warmup，再测单次 `allocate/free`
  - 用来看 steady-state 下的热路径性能

- `BM_*SingleSizeMetrics`
  - 测单一尺寸下的内存指标
  - 当前选取了一组代表性尺寸：`13 / 63 / 127 / 255 / 4096 / 65536`

- `BM_*MixedSizeMetrics`
  - 测混合尺寸分配下的内存指标
  - 更接近真实服务里的混合负载

这些指标 case 会通过 `google benchmark` 的 `UserCounters` 输出：

- `memory_utilization`
  - 有效请求字节 / `max(RSS峰值增量, 估算保留字节)`
  - 越大越好

- `internal_fragmentation`
  - `(估算保留字节 - 请求字节) / 估算保留字节`
  - 主要反映对齐和 size class 带来的浪费
  - 越小越好

- `external_fragmentation_estimate`
  - 基于 RSS 增量的黑盒近似指标，不读取 allocator 内部状态
  - 表示“当前保留在 RSS 中，但不属于存活负载”的比例
  - 越小越好

- `rss_peak_delta_kb`
  - workload 期间 RSS 相对开始时的峰值增量

- `rss_after_release_delta_kb`
  - 大部分对象释放后，RSS 相对开始时的增量
  - 可用于观察回收效果

- `rss_reclaim_ratio`
  - `(峰值RSS增量 - 释放后RSS增量) / 峰值RSS增量`
  - 越大说明释放后的 RSS 回落越明显

- `vm_hwm_kb`
  - 进程历史峰值 RSS（来自 `/proc/self/status` 的 `VmHWM`）

说明：

- 这些内存指标采用**黑盒口径**，不暴露 allocator 内部统计接口。
- 对 `malloc` 的 `reserved_bytes_est` 只能做近似估算，因此更适合和 memory pool 做相对比较，不适合当成精确内存账本。
- 目前 metrics case 里的大尺寸选择的是 allocator 现有 size class 语义下的安全代表值，避免把 allocator 语义问题和 benchmark 指标混在一起。

如果只想看新增指标 case，可以这样跑：

```bash
./v4/build/benchmark/memory_pool_benchmark \
  --benchmark_filter='BM_(Malloc|MemoryPool)(WarmAllocateFree|SingleSizeMetrics|MixedSizeMetrics).*'
```

切换模式时，直接在同一个 `v4/build` 目录重新执行 `cmake -S v4 -B v4/build -DMEMORY_POOL_BUILD_MODE=...` 即可，不需要维护两个 build 目录。
