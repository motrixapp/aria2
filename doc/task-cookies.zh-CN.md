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

每个任务都有独立的内存存储。空数组也会创建独立的空存储，不继承启动时的
`--load-cookies` 或其他任务的 cookie。响应中的 cookie 只更新当前任务的存储。
同一逻辑任务的连接、重试、暂停恢复及衍生任务共享该上下文。每次发出请求时，都会检查
域名、路径、过期时间和 Secure，包括重定向及 Range 请求。创建选项中传入或从全局选项
继承的固定 `Cookie` 请求头会被移除，以结构化存储为准。

## 重启与重新授权

结构化 cookie 的值不会写入文本会话、SQLite 持久化或 `--save-cookies` 文件。
会话只保存 `require-task-cookies=true` 标记。引擎重启后，任务缺少内存上下文时会在
发出网络请求之前失败，返回说明需要重新提供 cookie 的认证错误。客户端应请求重新授权。

以暂停状态恢复任务，再调用 `aria2.setTaskCookies(gid, cookies)`，最后调用
`aria2.unpause(gid)`。`setTaskCookies` 返回 `OK`，原子替换整个存储，也接受空数组。
只允许对带有上述标记的等待中或已暂停任务调用；活动任务、已停止任务和普通旧任务会被
拒绝。刷新活动任务前，先暂停并等待 `status=paused`。如果恢复的任务已经失败，需要
使用新 cookie 创建任务。客户端不应为了自动恢复而持久化原始 cookie 值。

## 旧接口与重定向

`aria2.addUri` 及启动时的 cookie 文件保留原有的全局存储行为。
对所有 HTTP 任务，如果当前请求与该 URI 最初的协议、主机或有效端口不同，显式提供的
`Cookie:` 和 `Authorization:` 请求头不会被发送。同源请求仍可使用这些请求头，其他
自定义请求头保留原行为。结构化 cookie 按自身的域名、路径及 Secure 规则匹配，cookie
作用域并不以端口为边界。此修改针对自定义请求头，并不替代 aria2 的所有认证及代理策略。

## 验收

运行 `make check` 后执行：

```sh
ARIA2_E2E_BIN="$PWD/src/aria2c" node --test test/e2e/task-cookies.e2e.test.mjs
```

测试覆盖任务隔离、重定向作用域、过期时间、HTTPS 降级、固定请求头、非法输入、Range
暂停恢复及重启后重新授权。发布流程会对打包后的六种 macOS、Windows 和 Linux
x64/arm64 二进制执行同一套测试，其中 Windows 包含 ia32。
