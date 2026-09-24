# AI Cloud Storage

一个用 C / FastCGI 复刻并逐步改进的私有云存储项目。

## 当前阶段

当前已完成认证和文件元数据管理的基础闭环：

```text
Nginx -> C FastCGI -> MySQL / Redis
```

已实现注册、登录、退出登录、用户文件列表、MD5 上传预检、普通文件上传、受控私有/分享下载、逻辑删除、可撤销/可过期的随机分享链接、可选提取码，以及登录后转存。大文件分片上传已完成会话初始化、单分片接收和断点状态查询；完整文件合并、前端和 FAISS 检索仍在后续阶段。

首次普通上传已经具备内部入库事务：新的 `file_info` 和上传者的 `user_file_list` 必须同时提交。两个用户同时上传相同内容时，由 `file_info.md5` 唯一约束裁决胜者；失败方删除自己多上传的 FastDFS 对象，再以事务关联胜出的物理文件并增加引用数。两个请求最终关联同一个 storage key。`mysql_commit()` 返回错误会标记为“提交结果未知”，供后续 FastDFS 补偿层查询确认后再决定是否删除物理文件。

上传编排通过 `StorageClient` 的 `upload/remove` 回调与具体存储解耦。入库失败或并发产生重复物理对象时执行删除补偿；提交结果未知时先按 user、MD5、storage_key 查询确认，仍无法确认则保留对象并报告待处理状态，避免误删已被提交记录引用的文件。这些异常分支已用假存储覆盖；Docker 开发栈也已接入真实 FastDFS 服务。

`/api/md5` 只检查当前用户是否已经拥有文件，不再允许仅凭全局 MD5 认领其他用户的私有内容。未拥有时统一返回需要普通上传，不泄露该 MD5 是否存在；普通上传会计算服务端 MD5，在验证真实字节后仍可复用已有物理对象并清理重复上传的 FastDFS 副本。

若已经确认无人引用的本次上传对象无法立即从 FastDFS 删除，应用会按 `storage_key` 幂等写入 `storage_cleanup_job`。任务保存失败原因、重试次数、下次执行时间和 `pending/running/done/failed` 状态；Compose 中的 `cleanup_worker` 后台服务会持续领取任务，按指数退避重试删除，达到最大次数后进入 `failed`，并将中断超过五分钟的 running 任务重新排队。默认每 10 秒轮询、每轮最多处理 100 条、最多尝试 5 次，退避从 30 秒开始、最长 1 小时，可通过 `CLEANUP_POLL_INTERVAL_SECONDS`、`CLEANUP_MAX_RETRIES` 和 `CLEANUP_RETRY_BASE_SECONDS` 调整。已有数据库依次执行 `sql/migrations/001_storage_cleanup_job.sql` 和 `sql/migrations/004_cleanup_retry_policy.sql`。

用户删除最后一条文件关系时，应用不会在 HTTP 请求的数据库事务中调用 FastDFS。它会锁定用户关系和 `file_info`，在同一个 MySQL 事务中删除分享、用户关系和零引用的 `file_info`，并写入 `last_reference_removed` 清理任务；提交后由 worker 异步删除旧 storage key。即使服务在提交后崩溃，任务仍可恢复；若相同 MD5 在 worker 执行前重新上传，新记录会获得新的 storage key，旧任务不会误删新对象。

文件分享会生成 32 字节安全随机数编码成的 64 位十六进制 `share_id`，默认七天过期（可通过 `SHARE_TTL_SECONDS` 调整）。公开查询只返回文件名、大小、类型和过期时间，不暴露 MD5、FastDFS storage_key 或存储直链；取消分享或过期后旧链接立即不可查询，重新分享会生成新 ID。已有数据库需要执行 `sql/migrations/002_share_links.sql`。

登录用户可通过有效 `share_id` 把文件保存到自己的文件列表。事务会依次锁定分享记录、所有者逻辑文件和物理文件记录，再插入接收者的 `user_file_list` 并原子增加 `reference_count`。重复或并发重复转存返回成功但不会重复计数；转存与撤销并发时，以谁先取得分享记录锁为准，已经完成的转存不会因之后撤销而消失。

文件字节不再通过 `/storage/...` 公开暴露。私有下载先校验 Redis Token 和用户文件关系；分享下载每次检查分享记录仍存在且未过期。鉴权成功后 C FastCGI 只返回 `X-Accel-Redirect`，由 Nginx 的 `internal` location 发送 storage 数据卷中的文件。`pv` 统计通过鉴权的下载请求，不代表客户端一定完整接收了全部字节。

创建分享时可选提供 4–12 位字母数字 `access_code`。数据库只保存随机盐和 PBKDF2-HMAC-SHA256 摘要，不保存明文；公开元数据只返回 `requires_code`。转存通过 JSON 提交提取码，分享下载通过 `X-Share-Code` 请求头提交，避免进入 URL 和常规访问日志。Redis 按“分享 ID + 访问者”记录连续错误次数，默认 5 次后锁定 300 秒。

FastDFS 的 `StorageClient` 适配器使用 `fork/execvp` 分别调用 `fdfs_upload_file` 和 `fdfs_delete_file`，检查子进程状态，校验返回的 storage_key，并根据公开基础地址生成 URL。命令参数不经过 Shell 拼接。Docker 镜像从官方源码构建固定版本的 FastDFS 及其依赖，开发栈启动一个 tracker 和一个 storage。

文件接收层使用 `mkstemp` 创建权限受限的随机临时文件，并在分块写入时增量计算服务端 MD5、累计真实字节数和执行大小限制。声明大小或 MD5 不一致、写入失败、请求超限时会立即删除临时文件；成功后再把临时路径移交给存储层。该模块不使用用户文件名作为本地路径，也不需要把完整文件加载进内存。

`POST /api/upload` 已接入受限的单文件 `multipart/form-data` 流式解析：请求头 `X-Upload-User`、`X-Upload-Token`、`X-Upload-MD5`、`X-Upload-Size` 分别携带用户、Token、客户端 MD5 和文件字节数，文件 part 必须使用字段名 `file`。接口先验证 Redis Token，再将文件内容交给安全接收层，随后执行 FastDFS 上传和首次入库事务；本地临时文件在成功和失败路径都会清理。Nginx 对该路由关闭请求体缓存，当前限制为 12 MiB，应用文件限制为 10 MiB。

分片上传使用 `POST /api/uploads/init` 创建 24 小时有效的会话，再以 `PUT /api/uploads/{upload_id}/chunks/{index}` 上传原始分片字节。每个请求按会话所有者、索引范围、精确分片大小和 `X-Chunk-MD5` 校验；分片先流式写入随机临时文件，再以原子硬链接安装到独立 Docker 数据卷。MySQL 的 `chunk_upload_part` 用 `staging -> ready` 记录落盘状态：相同索引和内容重复上传会幂等成功，不同内容占用同一索引会返回冲突。全部分片就绪后，`POST /api/uploads/{upload_id}/complete` 会原子认领会话、按序流式合并、重新校验整文件大小和 MD5，并复用普通首次上传的 FastDFS 与数据库事务/失败补偿流程；只有最终文件提交成功后才删除本地分片。已有数据库需依次执行 `sql/migrations/005_chunk_upload_session.sql` 和 `sql/migrations/006_chunk_upload_part.sql`。

```sh
curl -X POST http://localhost:8080/api/upload \
  -H "X-Upload-User: alice" \
  -H "X-Upload-Token: <64-character-token>" \
  -H "X-Upload-MD5: <32-character-md5>" \
  -H "X-Upload-Size: <file-byte-count>" \
  -F "file=@./example.txt"
```

开发环境由 Nginx 只读挂载单个 storage 数据卷，但该 location 标记为 `internal`，外部直接请求旧 `/storage/...` 或内部路径都会得到 404。生产环境有多个 storage 时，可在保持同一鉴权入口的前提下接入 `fastdfs-nginx-module` 或内部对象存储代理。

## 目录

```text
src_cgi/    FastCGI 业务入口
common/     可复用的 C 实现
include/    头文件
tests/      不依赖容器的单元测试
conf/       运行配置
docker/     容器化部署文件
```

## 当前可测试功能

```bash
make test
```

该命令验证 JSON 请求解析、MD5 实现、用户输入校验和密码摘要辅助逻辑。

## 本地容器启动

    cd docker
    cp ../.env.example .env
    docker compose up --build

启动后，Nginx 在 http://localhost:8080 提供以下路由：

- POST /api/reg：注册。请求体包含 user、nickname 和客户端计算的 MD5 password。
- POST /api/login：登录。成功后在 Redis 保存会话并返回 Token。
- POST /api/myfiles：携带 user 和 Token，返回当前用户的文件元数据列表。
- POST /api/upload：流式接收并验证文件，成功响应返回 `/api/download`，不返回可绕过鉴权的存储直链。
- POST /api/uploads/init：创建 24 小时有效的分片上传会话。JSON 包含 `user`、`token`、`file_name`、`md5`、`total_size` 和 `chunk_size`；成功返回随机 `upload_id`、总分片数和当前已上传分片。
- PUT /api/uploads/{upload_id}/chunks/{index}：以原始二进制请求体上传一个分片，请求头携带 `X-Upload-User`、`X-Upload-Token` 和 `X-Chunk-MD5`；相同内容重试幂等成功，相同索引改传不同内容会被拒绝。
- GET /api/uploads/{upload_id}：通过 `X-Upload-User` 和 `X-Upload-Token` 查询自己的上传会话，返回会话状态、分片计划、剩余有效期以及按序排列的 `uploaded_chunks`，供客户端断点续传。
- POST /api/uploads/{upload_id}/complete：鉴权后原子认领全部已就绪分片，流式合并并校验整文件 MD5，再提交到 FastDFS 和文件元数据事务；成功后会话变为 `completed` 并清理本地分片，重复完成请求幂等成功。
- POST /api/download：携带 user、Token 和 md5，校验当前用户所有权后下载文件。
- POST /api/md5：安全上传预检。`code=1` 表示当前用户未拥有，必须普通上传并由服务端验证内容；全局文件存在与否返回相同结果。`code=3` 表示请求错误；`code=4` 表示 Token 无效；`code=5` 表示用户已经拥有；`code=6` 表示数据库故障。
- POST /api/dealfile?cmd=del：携带 user、Token、md5，事务删除当前用户关系；最后一个引用会同时删除 `file_info` 并持久化异步物理清理任务。
- POST /api/dealfile?cmd=share：携带 user、Token、md5，并可选携带 4–12 位字母数字 `access_code`；返回 `share_id`、`expires_in` 和 `requires_code`。
- GET /api/share?share_id=&lt;64位ID&gt;：匿名查看有效分享的最小文件元数据及 `requires_code`；不返回摘要或盐。
- POST /api/share/save：携带 `user`、Token、`share_id`，受保护分享还需在 JSON 中携带 `access_code`；重复转存幂等成功。
- GET /api/share/download?share_id=&lt;64位ID&gt;：下载仍有效的分享；受保护分享通过 `X-Share-Code` 头提交提取码，撤销、过期或限流后拒绝请求。
- POST /api/dealfile?cmd=unshare：携带 user、Token、md5，撤销当前分享；旧 `share_id` 不会复活。
- POST /api/logout：携带 user 和当前 Token，删除该 Redis 会话；重复请求仍返回成功。

## 认证端到端测试

Docker 守护进程运行后，在项目根目录执行：

    make e2e

测试会启动 MySQL、Redis、FastDFS tracker/storage、C FastCGI 与 Nginx，覆盖注册、登录、Redis Token、真实文件上传、分片会话和单分片落盘、分片幂等与冲突、断点状态从空列表到全部 ready 的变化、受控私有/分享下载及字节一致性、直链封锁、下载计数、提取码哈希与限错、上传事务入库、并发上传、跨用户 MD5 认领拒绝、验证后去重、分享过期、转存、并发重复转存、转存/撤销竞争、零引用物理回收、撤销、删除和退出登录。测试会确认多用户引用时不会误删；最后一个引用删除后任务可恢复；相同内容重新上传后，清理旧对象不会影响新对象。

`docker compose up -d --build` 会同时启动长期运行的 `cleanup_worker`。如需排障，也可以手动处理最多 100 个当前到期的 FastDFS 对象：

    docker compose -f docker/docker-compose.yml exec -T fastcgi_app \
      /app/bin_cgi/cleanup_worker 100

查看清理队列的 Prometheus 格式指标（各状态数量、可立即执行数量、最老 pending 年龄和最老 ready 逾期时间）：

    docker compose -f docker/docker-compose.yml exec -T cleanup_worker \
      /app/bin_cgi/cleanup_metrics
