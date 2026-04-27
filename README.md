## v4 Build Modes

完整设计说明见：[v4/docs/design.md](/home/ubuntu/memory-pool/v4/docs/design.md)

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

### Benchmark 说明

`memory_pool_benchmark` 现在分成四类结果，含义不一样：

- `BM_*AllocateFree` / `BM_*WarmAllocateFree`
  - 偏向 thread-local cache 命中的热路径
  - 用来观察 allocator 的上限性能
  - 不适合代表共享竞争和慢路径

- `BM_*SharedCentralContention` / `BM_*CrossThreadFree`
  - 用来打共享竞争点
  - 前者主要看同一 size class 下的 `CentralCache` 竞争
  - 后者主要看 `thread1 alloc / thread2 free` 的跨线程返还成本

- `BM_*RssLifecycle` / `BM_*SingleSizeMetrics` / `BM_*MixedSizeMetrics`
  - 用来观察内存利用率、碎片和 RSS 生命周期
  - `RssLifecycle` 会经历扩张、部分保留、全部释放三个阶段
  - 比短批次的指标 case 更适合看 RSS 变化

- `BM_*HotTailLatency` / `BM_*SlowPathTailLatency`
  - 用来观察尾延迟
  - `HotTailLatency` 是热路径单次 `allocate/free`
  - `SlowPathTailLatency` 是批量触发 refill/flush 的慢路径

内存指标和尾延迟 case 会通过 `google benchmark` 的 `UserCounters` 输出。

内存类 counters 主要包括：

- `memory_utilization`
  - 有效请求字节 / `max(RSS增长增量, 估算保留字节)`
  - 越大越好

- `internal_fragmentation`
  - `(估算保留字节 - 请求字节) / 估算保留字节`
  - 主要反映对齐和 size class 带来的浪费
  - 越小越好

- `external_fragmentation_estimate`
  - 基于 RSS 增量的黑盒近似指标，不读取 allocator 内部状态
  - 表示“当前保留在 RSS 中，但不属于存活负载”的比例
  - 越小越好

- `rss_grow_delta_kb`
  - 扩张阶段 RSS 相对开始时的增量

- `rss_retained_delta_kb`
  - 只保留存活集后，RSS 相对开始时的增量

- `rss_after_release_delta_kb`
  - 全部释放后，RSS 相对开始时的增量

- `rss_reclaim_ratio`
  - `(RSS增长增量 - 释放后RSS增量) / RSS增长增量`
  - 越大说明释放后的 RSS 回落越明显

- `vm_hwm_kb`
  - 进程历史峰值 RSS（来自 `/proc/self/status` 的 `VmHWM`）

尾延迟类 counters 主要包括：

- `avg_ns`
- `p50_ns`
- `p90_ns`
- `p99_ns`
- `p99_9_ns`
- `max_ns`

说明：

- 这些内存指标采用**黑盒口径**，不暴露 allocator 内部统计接口。
- 对 `malloc` 的 `reserved_bytes_est` 只能做近似估算，因此更适合和 memory pool 做相对比较，不适合当成精确内存账本。
- 目前 metrics case 里的大尺寸选择的是 allocator 现有 size class 语义下的安全代表值，避免把 allocator 语义问题和 benchmark 指标混在一起。

如果只想看共享竞争和尾延迟：

```bash
./v4/build/benchmark/memory_pool_benchmark \
  --benchmark_filter='BM_(Malloc|MemoryPool)(SharedCentralContention|CrossThreadFree|HotTailLatency|SlowPathTailLatency).*'
```

如果只想看内存和 RSS 指标：

```bash
./v4/build/benchmark/memory_pool_benchmark \
  --benchmark_filter='BM_(Malloc|MemoryPool)(RssLifecycle|SingleSizeMetrics|MixedSizeMetrics).*'
```

切换模式时，直接在同一个 `v4/build` 目录重新执行 `cmake -S v4 -B v4/build -DMEMORY_POOL_BUILD_MODE=...` 即可，不需要维护两个 build 目录。
