# Mooncake HA 复现：单客户端的多个内存段共享 endpoint

2026-11-24 归档于个人 fork，不计划向上游提交 PR。[完整设计与实现边界](HA_SHARED_ENDPOINT_FIX.md)

## 复现条件

同一客户端挂载两个 16 MiB 的 Store 内存段 A、B。两者 UUID 不同、地址范围相邻、不重叠，但逻辑名字和 Transfer Engine endpoint 相同。这是 `MC_STORE_HCA_NUMA_SEGMENTS=1` 支持的布局；这里用显式挂载模拟，不依赖 NUMA 或 RDMA 硬件。它不同于[两个客户端使用同名段的场景](https://github.com/kvcache-ai/Mooncake/issues/5738)。

先挂 A，写入一个 9 MiB 对象，再挂 B，写入另一个 9 MiB 对象。此时 A 只有 7 MiB 剩余空间，第二个对象只能落在 B。测试使用内存 KV 后端，但经过实际 batch OpLog writer、reader 和 Master 晋升入口。它验证的是控制面恢复出的副本描述符，不是跨机器读取 payload。

## 同一用例，修复前后

| 版本 | Linux 实测结果 |
|---|---|
| [复现提交 `d607b0f7`](https://github.com/Kanata369/Mooncake/commit/d607b0f7) | 两条 mount 和两条对象记录均已回放。standby 按 endpoint 保存段，B 覆盖 A，收到 18 MiB 的活副本却只剩 16 MiB 容量。晋升返回 `INVALID_PARAMS`，日志为 `memory descriptors exceed segment capacity`，生命周期转入 `Failed`。 |
| [修复提交 `afac54e1`](https://github.com/Kanata369/Mooncake/commit/afac54e1) | 日志为 `Restored from standby: 2 objects, 2 segments, initial_seq_id=4, invalid_endpoints=2, rejected_objects=0`，随后转入 `Ready`。只 remount A 被拒，两个对象均返回 `REPLICA_IS_NOT_READY`；以相反顺序完整 remount `{B, A}` 后，`GetReplicaList` 分别返回原 endpoint、地址和长度。 |

修复版另跑了 Linux `master_service_ha_test` 全量：**149/149 通过**，`standby_segment_test` 也通过。macOS 上用同一提交的 registry 和区间匹配实现、项目锁定的 struct_pack 编译 `standby_segment_test`，**3/3 通过**，ASan/UBSan 未报错。新旧布局互读测试不是混部或回滚验证。

## 如何重跑

在装好项目依赖的 Linux checkout 中，选择上面一个提交，在各自的构建目录执行。测试使用 CPU/TCP；无需运行 etcd 服务，但需要项目的原生构建依赖。验证机有 14 GiB 内存，使用 `-j1` 避免 C++ 编译时 OOM。

```bash
cmake -S . -B build-ha -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DUSE_CUDA=OFF -DWITH_EP=OFF -DWITH_PG=OFF -DWITH_STORE_RUST=OFF \
  -DBUILD_UNIT_TESTS=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF
cmake --build build-ha --target master_service_ha_test -j1
binary=$(find build-ha -type f -name master_service_ha_test -print -quit)
GLOG_logtostderr=1 "$binary" \
  --gtest_filter=MasterServiceHATest.BatchPromotionRestoresSharedEndpointSegments
```

复现提交的该命令**预期失败**，修复提交预期通过。在修复提交上重跑另外两组检查：

```bash
cmake --build build-ha --target standby_segment_test -j1
ctest --test-dir build-ha -R '^standby_segment_test$' --output-on-failure
"$binary"
```
