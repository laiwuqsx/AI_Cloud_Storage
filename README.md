# AI Cloud Storage

一个用 C / FastCGI 复刻并逐步改进的私有云存储项目。

## 当前阶段

当前已完成认证和文件元数据管理的基础闭环：

```text
Nginx -> C FastCGI -> MySQL / Redis
```

已实现注册、登录、退出登录、用户文件列表、MD5 上传预检、普通文件上传与下载、逻辑删除，以及可撤销、可过期的随机分享链接。分享转存、分享下载、提取码、大文件分片上传、前端和 FAISS 检索仍在后续阶段。

首次普通上传已经具备内部入库事务：新的 `file_info` 和上传者的 `user_file_list` 必须同时提交。两个用户同时上传相同内容时，由 `file_info.md5` 唯一约束裁决胜者；失败方删除自己多上传的 FastDFS 对象，再以事务关联胜出的物理文件并增加引用数。两个请求最终返回同一个 URL。`mysql_commit()` 返回错误会标记为“提交结果未知”，供后续 FastDFS 补偿层查询确认后再决定是否删除物理文件。

上传编排通过 `StorageClient` 的 `upload/remove` 回调与具体存储解耦。入库失败或并发产生重复物理对象时执行删除补偿；提交结果未知时先按 user、MD5、storage_key 查询确认，仍无法确认则保留对象并报告待处理状态，避免误删已被提交记录引用的文件。这些异常分支已用假存储覆盖；Docker 开发栈也已接入真实 FastDFS 服务。

`/api/md5` 只检查当前用户是否已经拥有文件，不再允许仅凭全局 MD5 认领其他用户的私有内容。未拥有时统一返回需要普通上传，不泄露该 MD5 是否存在；普通上传会计算服务端 MD5，在验证真实字节后仍可复用已有物理对象并清理重复上传的 FastDFS 副本。

若已经确认无人引用的本次上传对象无法立即从 FastDFS 删除，应用会按 `storage_key` 幂等写入 `storage_cleanup_job`。任务保存失败原因、重试次数和 `pending/running/done` 状态；手动 worker 会领取任务、重试删除，并将中断超过五分钟的 running 任务重新排队。已有数据库需要先执行 `sql/migrations/001_storage_cleanup_job.sql`。

文件分享会生成 32 字节安全随机数编码成的 64 位十六进制 `share_id`，默认七天过期（可通过 `SHARE_TTL_SECONDS` 调整）。公开查询只返回文件名、大小、类型和过期时间，不暴露 MD5、FastDFS storage_key 或存储直链；取消分享或过期后旧链接立即不可查询，重新分享会生成新 ID。已有数据库需要执行 `sql/migrations/002_share_links.sql`。

FastDFS 的 `StorageClient` 适配器使用 `fork/execvp` 分别调用 `fdfs_upload_file` 和 `fdfs_delete_file`，检查子进程状态，校验返回的 storage_key，并根据公开基础地址生成 URL。命令参数不经过 Shell 拼接。Docker 镜像从官方源码构建固定版本的 FastDFS 及其依赖，开发栈启动一个 tracker 和一个 storage。

文件接收层使用 `mkstemp` 创建权限受限的随机临时文件，并在分块写入时增量计算服务端 MD5、累计真实字节数和执行大小限制。声明大小或 MD5 不一致、写入失败、请求超限时会立即删除临时文件；成功后再把临时路径移交给存储层。该模块不使用用户文件名作为本地路径，也不需要把完整文件加载进内存。

`POST /api/upload` 已接入受限的单文件 `multipart/form-data` 流式解析：请求头 `X-Upload-User`、`X-Upload-Token`、`X-Upload-MD5`、`X-Upload-Size` 分别携带用户、Token、客户端 MD5 和文件字节数，文件 part 必须使用字段名 `file`。接口先验证 Redis Token，再将文件内容交给安全接收层，随后执行 FastDFS 上传和首次入库事务；本地临时文件在成功和失败路径都会清理。Nginx 对该路由关闭请求体缓存，当前限制为 12 MiB，应用文件限制为 10 MiB。

```sh
curl -X POST http://localhost:8080/api/upload \
  -H "X-Upload-User: alice" \
  -H "X-Upload-Token: <64-character-token>" \
  -H "X-Upload-MD5: <32-character-md5>" \
  -H "X-Upload-Size: <file-byte-count>" \
  -F "file=@./example.txt"
```

开发环境的下载 URL 由 Nginx 只读映射单个 storage 数据卷，例如 `/storage/group1/M00/...`。这个映射便于本地学习和端到端验证；生产环境有多个 storage 时，应使用 `fastdfs-nginx-module` 或后续规划中的鉴权下载接口，不能依赖单节点数据卷映射。

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
- POST /api/md5：安全上传预检。`code=1` 表示当前用户未拥有，必须普通上传并由服务端验证内容；全局文件存在与否返回相同结果。`code=3` 表示请求错误；`code=4` 表示 Token 无效；`code=5` 表示用户已经拥有；`code=6` 表示数据库故障。
- POST /api/dealfile?cmd=del：携带 user、Token、md5，删除当前用户的文件关联。
- POST /api/dealfile?cmd=share：携带 user、Token、md5，创建当前用户的限时分享并返回 `share_id` 和 `expires_in`。
- GET /api/share?share_id=&lt;64位ID&gt;：匿名查看有效分享的最小文件元数据；撤销、过期或未知链接返回 `code=2`。
- POST /api/dealfile?cmd=unshare：携带 user、Token、md5，撤销当前分享；旧 `share_id` 不会复活。
- POST /api/logout：携带 user 和当前 Token，删除该 Redis 会话；重复请求仍返回成功。

## 认证端到端测试

Docker 守护进程运行后，在项目根目录执行：

    make e2e

测试会启动 MySQL、Redis、FastDFS tracker/storage、C FastCGI 与 Nginx，覆盖注册、登录、Redis Token、真实文件上传、下载内容校验、上传事务入库、两个用户并发上传相同内容、跨用户 MD5 认领拒绝、真实字节验证后的物理去重、文件列表、分享创建与公开查看、过期、重新分享、撤销、删除和退出登录。测试会确认公开分享不泄露存储地址或 MD5，旧分享 ID 不会在重新分享后复活。

手动处理最多 100 个待清理 FastDFS 对象：

    docker compose -f docker/docker-compose.yml exec -T fastcgi_app \
      /app/bin_cgi/cleanup_worker 100
