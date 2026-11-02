# 任务 Cookie RPC（Motrix 分支）

[English](task-cookies.md)

从 `v1.37.0-motrix.13` 开始提供。客户端可通过 `system.listMethods` 检测支持情况。
旧引擎会返回方法不存在；客户端不能退回不带认证的 `addUri` 或固定 `Cookie` 请求头。

## 创建任务

`aria2.addUriWithCookies(uris, cookies[, options[, position]])` 返回 GID。
认证使用常规 RPC token 参数。URI 必须全部使用 HTTP 或 HTTPS；选项及队列位置与
`aria2.addUri` 相同。整个 cookie 数组通过验证后才会创建任务。

```json
{
  "jsonrpc": "2.0",
  "id": "download",
  "method": "aria2.addUriWithCookies",
  "params": [
    "token:YOUR_RPC_SECRET",
    ["https://downloads.example.org/files/archive.zip"],
    [{"name": "session", "value": "EXAMPLE_ONLY", "domain": "downloads.example.org", "path": "/files", "hostOnly": true, "secure": true}],
    {"out": "archive.zip"}
  ]
}
```

Cookie 字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `name`、`value`、`domain` | 字符串，必填 | Cookie 名称、内容及 ASCII 主机或域名。国际化域名须使用 punycode。 |
| `path` | 字符串 | 默认为 `/`，必须以 `/` 开头。 |
| `hostOnly` | 布尔值 | 是否只匹配当前主机。省略时，域名以点开头默认为 false，否则为 true。IP 地址始终精确匹配。 |
| `secure` | 布尔值 | 默认为 false；为 true 时只在 HTTPS 请求中发送。 |
| `httpOnly` | 布尔值 | 默认为 false，保留为元数据；aria2 没有脚本读取 cookie 的接口。 |
| `expiresAt` | 整数 | Unix **毫秒**时间戳，范围为 0 到 JavaScript 最大安全整数。省略表示会话 cookie；显式传 0 或过去的时间表示已过期。实际匹配沿用原生存储的整秒时钟。 |

未知字段或错误类型会被拒绝。接口不接受 `sameSite`：浏览器的站点及导航策略应由调用方
在导出 cookie 前处理。调用方负责提供获授权的域名范围；aria2 不还原浏览上下文，也不
实现浏览器的公共后缀策略。值和路径不能包含控制字符或分号，名称须符合 HTTP token
语法。数组最多 300 条，每条名称和值合计最多 4096 字节，域名最多 254 字节，路径最多
4096 字节。沿用原生存储每个域名最多保留 50 条 cookie 的限制。

每个任务都有独立存储。空数组也会创建独立的空存储，不继承启动时的
`--load-cookies` 或其他任务的 cookie。响应中的 cookie 只更新当前任务的存储。
同一逻辑任务的连接、重试、暂停恢复及衍生任务共享该上下文。每次发出请求时，都会检查
域名、路径、过期时间和 Secure，包括重定向及 Range 请求。创建选项中传入或从全局选项
继承的固定 `Cookie` 请求头会被移除，以结构化存储为准。

## 持久化与重启

从 `v1.37.0-motrix.14` 开始，启用 SQLite 持久化时，aria2 会把每个任务的 Cookie
上下文存入专用的 `task_cookie_context` 和 `task_cookie` 表。任务行与 Cookie 快照在
同一事务中更新；显式空 jar 也有 context 行。响应中的 `Set-Cookie` 会立即落盘，包括
删除 Cookie 的响应，进程崩溃后通常不会退回到之前提交的旧值。短暂写入失败会标记该
任务，并在下次定时保存时重试；未变化的 jar 不会随每次定时保存重复重写。恢复时会先
删除已经过期的持久 Cookie 行。

Cookie value 以明文存放在本机 SQLite 数据库中，也可能短暂出现在 WAL。数据库及其父
目录应与其他 Motrix 应用数据一样使用仅当前用户可访问的权限。SQLite 删除使用
`secure_delete=FAST`。Cookie value 不会复制到文本 session、下载历史、引擎日志或
`--save-cookies` 输出。

启动时，aria2 在调度任务前恢复该任务的独立 jar。暂停、重试和引擎退出会保留。
启用 `--force-save=true` 时，未完成的失败任务会连同任务记录和断点保留 Cookie 上下文，
包括显式空 jar。正常完成、取消和显式删除结果会清理 Cookie 上下文；删除 task 行也会通过外键
级联清理两个 Cookie 表。

未启用 SQLite，或数据库丢失、损坏后带标记的任务没有 Cookie 上下文时，任务仍会在
发出网络请求之前失败。此时以暂停状态恢复任务，调用
`aria2.setTaskCookies(gid, cookies)`，再调用 `aria2.unpause(gid)`。
`setTaskCookies` 会原子替换整个 jar，也接受空数组；启用 SQLite 时会同时持久化。它只
允许用于带标记的等待中或已暂停任务；活动任务、已停止任务和普通旧任务会被拒绝。刷新
活动任务前，先暂停并等待 `status=paused`。

## 校验后的 HTTP 续传

通过 `system.listMethods` 检测 `aria2.getHttpRetryStatusV1` 和
`aria2.retryHttpDownloadV1`，两者都要求启用 SQLite 持久化。客户端可以为新建的单来源
HTTP 任务设置 `http-resume-context=true`。引擎会在首次写入下载内容前记录强 ETag
（或有效的 Last-Modified 与长度）、最终响应 URL 的哈希、原来源哈希和输出路径。
该模式不使用 HTTP Link 镜像。之后的每个下载内容响应都必须匹配这份记录，包括引擎重启后。

两个方法都接受 `(gid, originalUri, outputPath)`，用于核对原任务，不能覆盖来源或路径。
`getHttpRetryStatusV1` 返回 `{ available: "true" | "false", reason: string }`，
不会返回 Cookie 或其他请求凭证。`retryHttpDownloadV1` 返回原 GID，使用保留的请求、
独立 Cookie 存储和断点继续下载原文件。引擎会先持久化恢复状态并清理旧错误历史，再将任务
重新入队。重复调用会确认已经运行或完成的匹配任务，不会再次创建下载。

续传要求保留的历史资源校验信息和断点，不会把新获取的 ETag 当作旧内容的身份凭据。
Cookie、断点缺失，来源、路径或资源变化，或服务器不支持 Range，都不能静默清空并重下
部分文件。缺少历史校验信息的旧任务需要单独验证恢复，或明确选择重新下载。
完成任务、删除任务或清理结果时会清理资源绑定。保留的失败任务会以暂停状态恢复，
引擎重启不会自动重复失败请求。

## 旧接口与重定向

`aria2.addUri` 及启动时的 cookie 文件保留原有的全局存储行为。
对所有 HTTP 任务，如果当前请求与该 URI 最初的协议、主机或有效端口不同，显式提供的
`Cookie:` 和 `Authorization:` 请求头不会被发送。同源请求仍可使用这些请求头，其他
自定义请求头保留原行为。结构化 cookie 按自身的域名、路径及 Secure 规则匹配，cookie
作用域并不以端口为边界。此修改针对自定义请求头，并不替代 aria2 的所有认证及代理策略。

## 验收

运行 `make check` 后执行：

```sh
ARIA2_E2E_BIN="$PWD/src/aria2c" node --test --test-concurrency=1 test/e2e/task-cookies.e2e.test.mjs test/e2e/http-retry.e2e.test.mjs
```

测试覆盖任务隔离、重定向作用域、过期时间、HTTPS 降级、固定请求头、非法输入、Range
暂停恢复、SQLite 迁移、重启恢复、服务端 Cookie 轮换及终态清理。发布流程会对打包后
的六种 macOS、Windows 和 Linux x64/arm64 二进制执行同一套测试，其中 Windows 包含
ia32。
