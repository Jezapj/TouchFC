# Core benchmarks

Release build, MSVC 19.50. Machine reported by Google Benchmark: 4 x 2695 MHz, L1 data 32 KiB x4, L2 512 KiB x4, L3 4 MiB. Raw console log: [core_bench.txt](core_bench.txt). JSON: [results.json](results.json).

Times are the mean reported by Google Benchmark.

| Benchmark | Time | What it is |
|---|---|---|
| SPSC push/pop | 8.38 ns | One producer, one consumer, 32-bit slots |
| Mutex queue | 122 ns | Same traffic, `std::mutex` + `std::queue` |
| Seqlock load | 685 ns | Reader copies `Config` while a writer stores on every iteration |
| Mutex config load | 64.9 ns | Same hostile writer, under a mutex |
| Triple-buffer read | 5.48 ns | `try_read` against a writer that publishes every iteration |
| malloc 64 x256 | 17.9 us | 256 malloc/free pairs |
| Arena 64 x256 | 1.42 us | 256 bump allocations, one reset per iteration |
| Wake, 0 spin | 1751 ns | Round trip that sleeps immediately |
| Wake, 64 spin | 175 ns | Spin, then sleep if still idle |
| Wake, pure spin | 127 ns | Spin budget larger than the round trip |
| Ping-pong, same line | 11.3 ms / 100k | Two atomics with no padding |
| Ping-pong, 64 B | 14.6 ms / 100k | Each atomic on its own line |
| Ping-pong, 128 B | 15.1 ms / 100k | Padded past an adjacent-line prefetch |

```mermaid
xychart-beta
  title "Nanoseconds per operation (lower is better)"
  x-axis ["SPSC", "mutex queue", "triple buffer", "mutex config", "seqlock", "wake spin", "wake sleep"]
  y-axis "ns" 0 --> 1800
  bar [8.38, 122, 5.48, 64.9, 685, 127, 1751]
```

## Preprocess (M2)

1280x720 BGRA letterboxed to 640x640 CHW float32, one frame per iteration.

| Variant | Time per frame | Throughput |
|---|---|---|
| Scalar | 1.76 ms | 1.95 GiB/s |
| AVX2 (gather + lane blend) | 1.02 ms | 3.36 GiB/s |

The AVX2 path is 1.7x faster. The rest of the frame budget at 50 fps is 20 ms, so neither path is a bottleneck on this 4-core machine. The win is real but bounded: both paths write the same 4.7 MiB of destination floats, which is cache miss traffic the SIMD cannot avoid. Full output: [preprocess.txt](preprocess.txt).

## How to read this

The SPSC ring is about 15 times faster than the mutex queue on this machine. That is the comparison the pipeline actually depends on.

The seqlock is slower than the mutex **in this benchmark** because the writer never pauses, so the reader retries. A config writer in the app stores a few times a second, not once per load. The seqlock is there so readers do not take a lock and do not write a shared cache line. It is the wrong tool if the writer is as busy as the reader. The triple-buffer number is a single atomic load plus, sometimes, a pointer swap. It does not copy `Config`. It is not the same work as the seqlock load.

The arena is about 12 times faster than malloc for 64-byte blocks, and it does not free until `reset`. That is the frame-scratch pattern. It is not a general allocator.

Sleeping in `WakeSeq` costs about 1.7 us on this box. A short spin (64 pauses) is close to a pure spin. The pipeline spins a little, then sleeps, so an idle capture thread does not sit on a core.

False sharing did **not** show the textbook result here. Separating the two atomics made the handoff slower, not faster. 100 000 round trips at 11–15 ms is about 110–150 ns each, which is the cache-to-cache cost on this CPU, and one line bouncing was cheaper than two. The padding is still in the ring, because other CPUs (and the adjacent-line prefetch the 128-byte column is there to show) do not behave like this one. The number is reported rather than adjusted.

Profile-guided optimisation, SIMD preprocess and the tracker sweep are later milestones. They are not in this table.

## Image GMC (M9)

One 1920x1080 BGRA frame reduced to a 160x90 grass-and-line image, then the coarse (5 scales x 15x15 shifts at 80x45) and fine (5 scales x 5x5 at 160x90) AVX2 SAD search. Runs on T3 every frame.

| Benchmark | Time per frame |
|---|---|
| `BM_ImageGmc` | 0.86 ms |

Under 5% of the 20 ms budget at 50 fps. If T3 p99 needs it, the reduce (strided reads from the 8 MiB frame) and the float bilinear zoom are the parts to look at first.

