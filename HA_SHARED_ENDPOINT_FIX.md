# Mooncake Master HA 共享 endpoint 的多段恢复

2026-11-24 归档。原型保留在个人 fork，不计划向上游提交 PR。

**实测范围：** [原型 `afac54e1`](https://github.com/Kanata369/Mooncake/commit/afac54e1) 保留段的 UUID 和地址范围，按段校验容量，修复了同一客户端两个不重叠的段共享 endpoint 时的晋升失败。Linux 回归验证了完整 remount 后公开读接口返回原副本描述符；`master_service_ha_test` 全量 149/149 通过。[修复前后对照与重跑命令](HA_SHARED_ENDPOINT_REPRO.md)

**设计与原型的区别：** 以下各节保留讨论时的完整方案，第 6 节是验收计划，不是已通过测试的清单。原型未持久化每副本的段 UUID，未实现旧段认领标记，也未把读就绪状态全面改为按段管理。不能据此保证旧 standby 安全晋升、跨版本回滚，或已卸载段的地址被新段复用时仍能识别旧副本。

## 1. 故障和修复目标

一个 Store 客户端可以注册多个同名、同 Transfer Engine endpoint 的内存段。每段有自己的 UUID、起始地址和容量。`MC_STORE_HCA_NUMA_SEGMENTS=1` 正是这种布局，而且一个对象必须完整落在其中一段。本文只讨论同一客户端的内存段。跨客户端共享名字的 #5738 单独处理。[Store 设计](docs/source/design/store/mooncake-store.md)

复现中，A、B 各 16 MiB，各有一个 9 MiB 的活副本。`SEGMENT_MOUNT` 未记录 UUID 和 `base`，standby 按 endpoint 写 registry，B 覆盖 A。promotion 最终用 16 MiB 容量核对 18 MiB 副本，返回 `INVALID_PARAMS`。成功的判据是晋升后客户端能重新绑定两段，公开读接口分别返回两个对象原来的地址和长度。[复现记录](HA_SHARED_ENDPOINT_REPRO.md) · [回归用例 `BatchPromotionRestoresSharedEndpointSegments`](mooncake-store/tests/ha/master_service_ha_test.cpp)

## 2. 持久化哪些信息

| 记录 | 新内容 | 用途 |
|---|---|---|
| `SegmentMountOp`、`SegmentUpdateOp`、`StandbySegmentInfo` | 在尾部追加可选的 `{segment_id, base}` 整体 | 一个字段同时表达“有身份和范围”。合法的零地址不会与旧格式混淆。`base` 是该段的起始虚拟地址。 |
| `SegmentMountOp` | 在上述身份信息外记录该事件来自普通 mount 还是 remount；能认领旧记录的 remount 额外标记 `adopts_legacy` | 普通 mount 可能是在已有段旁边增加 B。仅凭 endpoint、名字和容量相同无法证明 B 就是旧段。 |
| `SegmentUnmountOp` | 追加可选 `segment_id`，保留 endpoint | 显式卸载与客户端离线清理都写入被卸载段的 ID。 |
| HA 对象元数据 `MetadataPayload`、`StandbyObjectMetadata` | 追加可选的内存副本归属表，键为 `ReplicaID`，值为段 UUID 或只用于历史数据的 `legacy_endpoint` | 区分同一 endpoint 和地址上先后挂载的 A、B。卸载 A 后，清理对象的日志可能尚未持久化，此时旧副本仍会出现在晋升输入中。 |

追加字段使用 `struct_pack::compatible` 和当前类型的下一个兼容版本，更新 `YLT_REFL`。有无字段由 presence 判断。保留现有 segment oplog 的 `object_key`；身份和回放顺序分别取自 payload 和 `sequence_id`。将 mount/remount 来源和认领信息放在一个有明确取值的字段中，例如 `mount`、`remount`、`remount_adopts_legacy`，避免两个标志组合成矛盾状态。

副本归属只写入 HA 元数据。生成 HA 记录时，从实际分配或重绑定该 buffer 的段取 UUID，在绑定处保留这个值；按副本 ID 对应，不能用当前 endpoint 反查。覆盖 put、对象元数据重写、持久化 repair、快照往返及 `MetadataPayload::ToStandbyMetadata`。新记录中可以同时存在已知归属的副本和从旧历史继承的无归属副本，后者按第 4 节处理。校验重复或悬空的归属项，以及指向非内存副本的项。公开读接口仍返回原有传输描述符。[对象元数据类型](mooncake-store/include/metadata_store.h) · [buffer 绑定](mooncake-store/src/segment_allocator_registration.cpp) · [卸载后异步清理](mooncake-store/src/master_service.cpp)

## 3. Registry 和混合版本回放

内存段按 UUID 存储，沿用 `SegmentManager` 的 `std::unordered_map<UUID, …, boost::hash<UUID>>`。未记录 UUID 的历史段单独按 endpoint 保存。endpoint 查询返回候选集合；保留 `HasSegment(endpoint)` 的精确 endpoint 查询，单值 `GetSegment(endpoint)` 仅在唯一命中时返回。非内存段沿用已有行为。[主节点段索引](mooncake-store/include/segment.h) · [standby registry](mooncake-store/src/metadata_store.cpp)

| 回放输入 | 原子状态变化 |
|---|---|
| 纯旧历史 | mount/update 仍按 endpoint upsert；unmount 删除唯一旧段；重复卸载成功。 |
| 新 mount/remount，UUID 已知 | 相同 UUID、名字、endpoint、base 和容量的重放幂等；同 ID 的冲突 mount 拒绝。显式 update 按 ID 更新，并先验证更新后的范围及其它段。 |
| 新普通 mount B，E 已有 A | B 独立插入。即使 A 是无 UUID 的历史段，B 也不能认领 A。新 update 只能更新明确指向的记录。 |
| 旧基线 + 新 remount A | 新主在整个 remount 请求中核对旧记录、活段与请求。仅当唯一的旧记录可以对应唯一请求段，旧字段吻合且已有活副本全部能由它接管时，才生成认领标记。applier 在同一把锁下将旧记录替换为 UUID A；找不到旧项但已存在完全相同的 A 时幂等成功。多个候选无法区分时拒绝认领。 |
| 新 unmount(A,E) | 优先删除匹配 UUID 且 endpoint 一致的 A，保留 E 的其它已知段。在完整、有序的单旧段历史上，若 A 从未作为新段插入且 E 只剩一个旧候选，按该旧段退休处理。若历史或候选不足以唯一对应，则报告冲突。完全缺失的段按现有幂等卸载处理；同一序号的重投由 applier 先去重。 |
| 新基线 + 旧事件 | 旧 unmount 只在 E 唯一时可应用；多个候选时拒绝。旧 update 可在唯一身份可保留时更新。旧 mount 可能代表替换。唯一新段仍有活副本时，身份连续性无法证明就拒绝；仅在没有待处置副本时允许退回单旧段状态。多候选时拒绝。 |

“唯一”包括历史语义。旧基线若已经把多个段覆盖成一个条目，基线自身无法证明丢失段的 ID。只能按旧单段语义恢复能够验证的数据。检测到容量、候选或 remount 范围矛盾时报告错误；同批升级也不能补回已丢失的历史。认领判定须在 remount 事务持锁预检中完成；保留 oplog 组预留和大小校验，最终 payload 在任何可见修改前确定。

基线装载与事件回放分别校验。快照重复 UUID、重复旧 endpoint、空的内存段别名、零容量、不可表示的范围，以及同一 `(name, endpoint)` 下相交的新段，均为结构错误；相邻区间合法。不同组仍沿用既有别名冲突校验。加载、复制或重启恢复 registry 时先建临时索引，校验成功后一次替换，禁止用 upsert 抹掉快照重复项。原样持久化仍待认领的旧条目。

segment applier 需要把**可解码记录的身份冲突**返回给 `ApplyOpLogEntry`。失败记录不修改 registry，不推进 expected sequence；reader 保留上一个完整 batch 的 durable cursor。当前 `TestApplySegmentMount_InvalidPayload` 明确要求对无法反序列化的历史垃圾 payload 跳过并推进，保留这个独立行为。新增冲突测试使用可解码的 payload。验证实际旧类型编码的单条事件及多元素快照，分别用旧、新 reader 解码；旧 reader 即使能解码新字段，也只能表达旧版的单段语义。[applier](mooncake-store/src/ha/oplog/oplog_applier.cpp) · [batch reader](mooncake-store/src/ha/oplog/oplog_batch_standby_reader.cpp) · [快照 provider](mooncake-store/src/ha/snapshot/batch_oplog/batch_oplog_snapshot_provider.cpp)

## 4. 晋升时如何确定副本归属

先校验 descriptor 的对象长度、非零长度和地址算术，再按下表解析。对普通 TCP/RDMA 地址，令 `offset = address - base`；仅在 `address >= base` 后做减法，要求 `offset < capacity` 且 `length <= capacity - offset`。名称别名和 endpoint 指向同一条记录时只算一次。

| 情形 | 处理 |
|---|---|
| 有副本归属 UUID | 只查该 UUID，再核对别名和范围。已从有序历史退休的 A 即使被 B 复用地址也按 stale 处理。ID 与存活段属性冲突是坏描述符。 |
| 旧副本、唯一旧段 | 采用旧版唯一别名语义及容量校验；remount 时由真实 `base` 校验完整范围。已经在有序历史中绑定到 `legacy_endpoint` 的旧副本可以继续定位到该旧项，即使同 endpoint 后来加入了带 UUID 的段。 |
| 旧副本、已知范围 | 完整落入唯一候选才可归属。已知 endpoint 上跨界、溢出、多个命中或无可证明的归属，都不能静默跳过。若同时有无范围的旧候选与新段，单凭地址命中新段也不足以排除旧段。 |
| CXL | 描述符保存 `change_to_cxl` 转换后的偏移和逻辑名字。单段按 UUID 或唯一旧别名归属，保留原偏移、协议和名字，通过容量及原有描述符检查。多个候选且缺少 UUID 时报告歧义。remount 维持 `UNAVAILABLE_IN_CURRENT_MODE`。 |
| endpoint 已消失，或有证据表明原段已退休 | 走入口原有 stale 规则。batch 丢弃该副本，若无其它副本则跳过对象；legacy 快照沿用对象拒绝、修复日志及计数策略。 |

对新对象在有序回放时校验其归属（例如一个从未挂载的 UUID 是错误）。采用**卸载时清理 standby 镜像**：记录受影响的 `(tenant, key, ReplicaID)`，只移除归属 A 的描述符及其归属项，保留其它段和其它介质的副本；最后一个副本消失时沿用已有 stale 对象的跳过语义。无需持久化永久退休名单。旧副本在认领事件中与旧段一起补全身份；旧段卸载时，在移除旧身份前处置唯一对应的旧副本。普通 mount B 发生时若旧 A 尚待认领，原有旧副本继续归属未认领的 A，新副本可显式归属 B。旧副本在进入镜像时的身份必须随快照保存，可在归属表的值中用带类型的 `legacy_endpoint` 表达；有新旧混合候选而历史已不足以判断时拒绝。独立加载快照时，对明确指向不存在 UUID 的副本报告不一致。

让 registry 与受影响对象共享同一次回放提交边界：先验证所有目标、构造修改，成功后一起提交。快照捕获只能看到事件之前或之后的状态。若用段到副本的反向索引定位对象，键需包含 tenant；在 put 覆盖、remove、repair 和快照重载时同步更新或重建。`PUT_END` 和 `OBJECT_STORAGE_MUTATION` 的内嵌 `metadata_payload` 都能更新对象，必须从这两个入口执行相同的归属校验，并将语义冲突返回 `ApplyOpLogEntry`。mutation 中的上传记录和元数据仍一次提交。这些规则只约束新归属字段。[standby 对象更新入口](mooncake-store/src/ha/standby_metadata_store.cpp)

legacy 容忍路径在对象级处理坏描述符，继续既有重叠处理与 durable repair；batch 路径对存活段上的非法范围返回 `INVALID_PARAMS`。两者复用同一归属判断，保留各自的错误语义。容量、分配区间、临时记账都以段为单位，排除 `REMOVED`/`FAILED` 的容量贡献。保留逐 chunk 遍历，跨 chunk 的占用区间按段累积。若后续 chunk 校验失败，撤销本次调用先前 chunk 发布的对象，保留调用前状态；回滚必须覆盖 chunk 循环内的提前返回。[晋升和 legacy 恢复](mooncake-store/src/master_service.cpp) · [CXL 地址转换](mooncake-store/src/allocator.cpp)

## 5. Remount、读可见性和指标

选择**共享 endpoint 的待恢复段整组提交**。正常客户端在 `mounted_segments_mutex_` 下收集全部挂载段并发送一次 `ReMountSegment`；服务端现有 `NeedsRemount()` 按客户端决定重复请求是否直接成功。允许一个分段请求先提交 session，会影响剩余段重试。整组条件仅用于同一 `(name, endpoint)` 的待恢复成员。[客户端心跳](mooncake-store/src/client_service.cpp) · [服务端 remount](mooncake-store/src/master_service.cpp)

1. 恢复时给每个内存段保留独立 dummy allocator 和待 remount 状态。把归属保留在恢复 buffer 的运行时绑定上，通过 `IsAwaitingRemount` 检查实际绑定。读热路径无需再按地址搜索所有段。判定某段已挂载时核对真实目录中的 UUID、名字、endpoint、base、容量；旧单段走唯一别名及已有行为。A 已挂载时，B 的 dummy buffer 仍不可读。NoF 等其它类型保留各自的保活逻辑。
2. remount 在现有 session 与 master 锁顺序内预检整组，在 `NeedsRemount()` 的成功早退之前拒绝漏报同组待恢复成员、重复 ID 和属性冲突。新格式须逐项核对 `id/name/te_endpoint/base/size`，空段同样检查；旧格式核对唯一候选的 `name/te_endpoint/capacity`。无关 endpoint 没有历史记录的正常 remount 仍走既有入口。对同组已经活跃且完全一致的成员允许幂等出现。
3. 暂存每段的 `BuildRegionLiveAllocations`、allocator 导入、buffer 替换和记账差额。整组验证成功后，在现有排他锁内替换 allocator 和 buffer、绑定 session、解除对应段的等待状态、刷新相关对象租约，再提交 session。预检或导入失败要恢复新挂载项；读状态、分配能力、租约、记账均保留失败前的值。空段也参与组完整性判断。
4. `standby_accounted_memory_bytes_`、dummy allocator 的强引用和退回旧记录的查询都关联到上一步实际匹配的段。保活引用覆盖所有仍由对应 dummy 支撑的存活 buffer，包括增量恢复前已发布的对象；导入另一段、重复快照或回滚均不可提前释放它。处理旧记录认领时迁移同一份状态。
5. 容量校验和 import 扣账逐段进行。导出的 `MasterMetricManager` 指标继续按逻辑名字汇总；同名两段的差额分别累加，只在最后一个贡献者消失后移除该名字的指标。覆盖恢复替换、失败回滚、幂等重试和析构。

## 6. 实施与验收

先实现 wire 类型及旧格式 fixture，然后依次实现 registry/applier、快照装载、副本归属与两条 restore 路径、remount 事务及读可见性。下面每一行是可独立判定的验收条件。

| 场景 | 通过条件 |
|---|---|
| 现有 2 × 16 MiB 复现 | 在 registry 中按 UUID 核对两段，晋升前保存两个 9 MiB 描述符。晋升后两个 key 的 `GetReplicaList` 都返回 `REPLICA_IS_NOT_READY`。同一 `client_id` 只提交 A 时整组失败，两段仍待读；重试 `{B,A}` 后，单读和批读返回原 endpoint、地址和长度。两段各导入 9 MiB，共计 18 MiB，重复 remount 不改变指标。 |
| 旧基线接新日志 | 分别跑旧 mount → 新 remount 认领、旧 mount → 新 unmount、旧 A → 新普通 mount B（同名同容量）→ 卸 B。核对每一阶段的成员、快照重载结果及最终 cursor。再测同一批次中有效事件后接歧义事件，后者不改变状态，cursor 不越过失败批次。 |
| 精确更新与退休 | 新 A/B 共享 endpoint，更新 A 不改变 B；显式或离线卸 A 均保留 B。截在 unmount 已持久化而 REMOVE 尚未到达的前缀，再让 C 复用 A 的地址。旧副本不可从 C 读出，其它副本及 B 正常。新基线遇到无 ID 的旧 unmount 且有两个候选时停止。 |
| 区间与错误语义 | 紧邻边界且恰好放满成功；A 末端跨入 B 一个字节、整数溢出、重叠段、同一段超容而邻段空闲分别有断言。区分合法 stale 和已知存活段上的坏描述符，分别覆盖 legacy 与 batch。设置 chunk 大小为 1，在第二块引入错误后确认第一块未残留。 |
| 对象回放一致性 | 用 `PUT_END` 和 `OBJECT_STORAGE_MUTATION` 分别生成带归属的同名跨租户对象，覆盖与删除后重载快照。卸载 A 只清除实际引用 A 的副本；构造已解码的错误归属时，对象、上传记录、registry 和失败记录的序号都保持原值。 |
| CXL 与旧 codec | 用真实旧类型产生的字节验证单元素及多元素 vector，并测新写旧读。带新版段身份的单段 CXL 快照恢复后内部描述符原样保留；公开读仍为未就绪，remount 保持原错误码。 |
| 可读性与重试 | A 已挂载，只有 B 待读时，A 可读、B 不可读；再覆盖同组一个段为空、同 UUID 改 `base`、改 endpoint、导入失败和整组重试。检查 `GetReplicaList`、批读、租约、保活及分配行为。 |
| 旧行为回归 | 运行 `master_service_ha_test`、restore lifecycle、standby registry、oplog applier、batch codec/provider 和 snapshot handoff 相关测试。远端已有 14 GiB 构建机采用 `-j1` 增量构建。 |

旧格式已经覆盖掉的段信息是数据边界。新版读写互通、已退休副本跨快照的行为和整组 remount 的事务性都需要上述运行测试提供证据。
