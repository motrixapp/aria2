# 上游 Issue 回移审查

[English](README.md)

本目录记录了哪些上游 aria2 缺陷修复被回移到 `aria2_motrix`、这些修复来自
哪里，以及最重要的一点：每项修复是否确实在本分支的**生产调用链**上生效，
而不只是“回归测试通过”。

## 修复来源

代码回移自 `aria2-next` 分支的提交 `882585d7`
（`docs(maintenance): add open issue review document and summary of findings`）。
该提交包含 44 个上游未关闭 Issue 的已验证修复，以及一个部分验证的修复
（#1407）。它早于 aria2-next 的 CMake、spdlog 和 C++17 迁移，因此仍使用原有
的 `A2_LOG_*` 宏与 `Makefile.am`，可以干净地应用到 aria2_motrix 的
autotools + CppUnit 代码树。

`aria2-next` 自身的审查矩阵将 `state=fixed-verified` 定义为“该 Issue 的新回归
测试通过”。这个标准并不充分：回归测试可能只覆盖一个孤立的底层单元，而生产
调用链根本不会到达修复代码。本次审查沿真实调用链重新核验了每项修复，发现绿色
的 `fixed-verified` 背后隐藏着 6 项无效回移、2 项危险回移和 3 项部分生效的
回移。最终对抗性复审已移除两项危险实现，并修复剩余可由远程触发的资源耗尽和
范围完整性缺口。

## `matrix.csv` 结构

在 `aria2-next` 原有 7 列的基础上，本目录新增了 3 列：

| 列 | 含义 |
|----|------|
| `number` | 上游 aria2 Issue 编号 |
| `priority` | 对 Motrix 的影响等级：P0/P1/P2/P3 |
| `module` | 子系统分组，继承自 aria2-next |
| `title` | 上游 Issue 标题 |
| `upstream_state` | **aria2-next** 的原始结论（`fixed-verified`、`fixed-partially-verified`） |
| `local_state` | **本分支**重新审查后的结论，见下文 |
| `root_cause_group` | 共同根因分组，继承自 aria2-next |
| `required_action` | 本次回移采取的处理方式 |
| `test_ref` | 覆盖真实调用链的测试；未覆盖时标记为 `primitive-only:`、`regression:` 或 `none` |
| `evidence` | 支撑 `local_state` 结论的本分支证据 |

### `local_state` 取值（共 45 个 Issue）

| 值 | 数量 | 含义 |
|----|------|------|
| `verified-effective` | 25 | 修复能够到达生产调用链并确实生效；回归测试、构建和端到端测试均通过 |
| `defective-rewritten` | 5 | 回移的修复本身有缺陷；已在本分支**重写**，并用跨调用链测试验证 |
| `defective-fixed` | 4 | 回移带来了审查发现的回归或崩溃；已在本分支**修复**并补充回归测试 |
| `unsafe-port-resolved` | 2 | 有缺陷且危险的回移已移除；#1556 的根因已修复，#1727 的不受支持行为已安全回退 |
| `ported-ineffective` | 5 | 能到达生产路径但没有实际效果；问题可能已由其他代码解决，或实现没有覆盖报告输入；无害 |
| `needs-attention` | 3 | 仅部分生效、存在覆盖范围缺口或特定场景回归；已记录但未改动 |
| `ported-tests-pass` | 1 | 原样回移，回归测试和完整测试套件通过，但未进行独立深度审查 |

## 五项重写的缺陷（`defective-rewritten`）

| Issue | 缺陷 | 跨调用链测试 |
|-------|------|--------------|
| #1752 | `createJsonRpcErrorResponse` 将 `AUTHORIZED` 写死；任意格式错误的 WS 消息都会授权会话，使其随后收到所有通知 | `RpcHelperTest.cc`（7 项）+ `e2e/websocket-auth` |
| #2280 | `createCheckIntegrityEntry` 丢失了 `FileOpenMode`，导致 `RESTART_FROM_SCRATCH` 分支不可达；条件请求收到 200 后仍会从陈旧字节继续下载 | `RequestGroupTest.cc::testCreateCheckIntegrityEntryRestartFromScratch` + `e2e/conditional-get-restart` |
| #1407 | 现代 Schannel 路径把 `~SP_PROT` 与初值为 0 的禁用协议掩码执行 AND，结果始终为 0，即没有最低版本限制，使 `--min-tls-version` 失效；最终复审还通过显式禁用 PCT/SSL，让最低版本不再受机器策略削弱 | `WinTLSProtocolsTest.cc`（3 项）+ 过时协议断言 + mingw 一致性验证 |
| #1839 | 503 请求唤醒时会重置尝试次数，导致永久返回 503 的服务器被无限重试 | `e2e/503-max-tries` |
| #1280 | `sqlite3_open_v2` 延迟打开数据库，导致不可变 URI 回退永远不会触发，Firefox WAL Cookie 在实际读取时失败 | `Sqlite3CookieParserTest.cc::testMozParse_readOnlyWalSnapshot` |

以上每项都已证明：修复前代码测试失败，修复后测试通过。

## 重新审查发现并修复的其他缺陷（`defective-fixed`）

- **#886/#1115/#2061** —— 回移的“拒绝无 `Content-Range` 的分块 206 响应”
  缺少 `getSegment()` 保护，导致未分段的初始请求收到合法 206 时被强制失败，
  相比未知长度下载路径形成回归。该问题已修复。最终复审还关闭了两个文件损坏
  绕过：现在会校验携带 `Content-Range` 的传输编码响应，并拒绝分段请求收到的
  无 `Content-Range` 传输编码 200/206 响应。三种情况均有回归测试覆盖。
- **#1471** —— 参数化 URI 范围扩展回移引入了一个可通过 RPC 触发的除零错误
  （循环前出现空 `{}` 选择组），以及 `%d` 与 `int64_t` 的格式不匹配。最终复审
  还把聚合展开数量限制为 65,536 个字符串，因此 `[0-2147483647]` 会被拒绝，
  而不会尝试分配包含数十亿个元素的容器。问题已修复并补充回归测试。
- **构建集成** —— `WebSocketSessionManTest.cc` 被加入无条件测试源，导致
  `--disable-websocket` 配置下的 `make check` 失败；现已使用
  `ENABLE_WEBSOCKET` 保护。

## 已处理的危险回移（`unsafe-port-resolved`）

最终对抗性复审移除了两项危险实现，同时保留不相关且有效的修复。本分支不再掩盖
UAF，也不会破坏已持久化的完整性状态：

- **#1556（WrDiskCache）** —— 移除了恢复/重建索引的兜底逻辑。缓存条目缺失时
  重新采用失败关闭，不再把可能悬空的指针重新插入进程级缓存。同时修复了根因：
  从头重新下载时，会在销毁所有在途分片前先解除其写缓存条目。回归测试同时覆盖
  缓存生命周期和被拒绝的更新。
- **#1727（Metalink/BT 整文件校验和）** —— 消费端
  `BtCheckIntegrityEntry::onDownloadFinished` 在 BT 完成时不可达；而在唯一可触发
  的路径上，它会清除已经通过分片哈希验证、且会由本分支持久化到 SQLite3 的位图。
  现已移除校验和的跨上下文传递和不可达的调度逻辑，同时保留有效的 #2033 行为。
  回归测试验证了不受支持的整文件校验和不会被带入 BT 上下文。

## 值得关注的 `needs-attention` 项

- **#2285** —— 对第三方 RPC 客户端有效，但对 Motrix 不生效，因为 Turbo 从不通过
  RPC 发送 `max-concurrent-downloads`。分支风险：强制暂停/重启可能把
  `state=paused` 持久化到 SQLite3，崩溃恢复后任务仍为暂停状态。依赖该功能前需与
  `motrix-turbo` 协调。
- **#2119** —— 能正确拒绝无效 IPv6 字面量，但现在也会拒绝此前可解析的 RFC 6874
  区域标识符（`[fe80::1%eth0]`）。
- **#826** —— 仅修复了纯 AAAA 场景；并行 IPv4/IPv6 竞态仍未处理，属于既有问题。

## 最终对抗性结论

**High：0。** 剩余 `needs-attention` 项均为边界明确的兼容性或产品集成缺口；本分支
不存在高严重度的机密性、完整性、内存安全、身份验证或可远程触发的可用性问题。

## 已执行的验证

- `make check`（完整 CppUnit 测试套件）—— 通过，0 失败、0 错误。
- `test/e2e/` Node.js 测试套件 —— 15 项全部通过；确认 #2280 的
  `RequestGroup` 改动没有破坏 SQLite3 持久化钩子。
- 最终对抗性回归测试覆盖：传输编码响应的范围不匹配、分段请求收到无范围信息的
  200 响应、参数展开上限、写缓存更新失败关闭、从头重下时解除缓存、不受支持的
  校验和跨上下文传递，以及显式屏蔽 Schannel 的 TLS 之前协议。
- #1752、#2280、#1839 的新增端到端用例均已证明修复前失败、修复后通过；#1407
  的纯掩码逻辑已进行单元测试，并验证原始位逻辑为红、修复后为绿。在 Ubuntu
  mingw-w64 交叉工具链下，`WinTLSContext.cc` 相比原回移代码没有新增错误；剩余错误
  是该工具链既有的 `SCH_CREDENTIALS` 类型缺口。完整 Windows 编译和链接由
  Windows CI 验证。
- 45 个 Issue 均已由并行审查代理独立复审；逐项结论记录在 `matrix.csv`。
