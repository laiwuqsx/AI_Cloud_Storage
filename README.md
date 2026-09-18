# AI Cloud Storage

一个用 C / FastCGI 复刻并逐步改进的私有云存储项目。

## 当前阶段

当前已完成认证和文件元数据管理的基础闭环：

```text
Nginx -> C FastCGI -> MySQL / Redis
```

已实现注册、登录、退出登录、用户文件列表、MD5 秒传、普通文件上传与下载、逻辑删除以及分享状态管理。大文件分片上传、受控分享链接、前端和 FAISS 检索仍在后续阶段。

首次普通上传已经具备内部入库事务：新的 `file_info` 和上传者的 `user_file_list` 必须同时提交，MD5 唯一约束用于识别并发首传冲突。`mysql_commit()` 返回错误会标记为“提交结果未知”，供后续 FastDFS 补偿层查询确认后再决定是否删除物理文件。

上传编排通过 `StorageClient` 的 `upload/remove` 回调与具体存储解耦。入库失败或并发产生重复物理对象时执行删除补偿；提交结果未知时先按 user、MD5、storage_key 查询确认，仍无法确认则保留对象并报告待处理状态，避免误删已被提交记录引用的文件。这些异常分支已用假存储覆盖；Docker 开发栈也已接入真实 FastDFS 服务。

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
- POST /api/md5：命中已有物理文件时，仅创建用户文件关联，实现秒传。`code=1` 表示确认未命中、前端可以继续普通上传；`code=3` 表示请求错误；`code=4` 表示 Token 无效；`code=5` 表示用户已经拥有；`code=6` 表示数据库故障，前端不得继续上传。
- POST /api/dealfile?cmd=del：携带 user、Token、md5，删除当前用户的文件关联。
- POST /api/dealfile?cmd=share：携带 user、Token、md5，将当前用户的文件标记为分享。
- POST /api/dealfile?cmd=unshare：携带 user、Token、md5，取消当前用户的分享状态。
- POST /api/logout：携带 user 和当前 Token，删除该 Redis 会话；重复请求仍返回成功。

## 认证端到端测试

Docker 守护进程运行后，在项目根目录执行：

    make e2e

测试会启动 MySQL、Redis、FastDFS tracker/storage、C FastCGI 与 Nginx，覆盖注册、登录、Redis Token、真实文件上传、下载内容校验、上传事务入库、秒传、文件列表、分享、取消分享、删除和退出登录。退出后会检查 Redis key 已删除、旧 Token 被拒绝，并验证重复退出可安全重试。
