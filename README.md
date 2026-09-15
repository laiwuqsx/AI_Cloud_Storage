# AI Cloud Storage

一个用 C / FastCGI 复刻并逐步改进的私有云存储项目。

## 当前阶段

当前已完成认证和文件元数据管理的基础闭环：

```text
Nginx -> C FastCGI -> MySQL / Redis
```

已实现注册、登录、退出登录、用户文件列表、MD5 秒传、逻辑删除以及分享状态管理。FastDFS 普通上传、分片上传、受控分享链接、前端和 FAISS 检索仍在后续阶段。

首次普通上传已经具备内部入库事务：新的 `file_info` 和上传者的 `user_file_list` 必须同时提交，MD5 唯一约束用于识别并发首传冲突。`mysql_commit()` 返回错误会标记为“提交结果未知”，供后续 FastDFS 补偿层查询确认后再决定是否删除物理文件。

上传编排通过 `StorageClient` 的 `upload/remove` 回调与具体存储解耦。入库失败或并发产生重复物理对象时执行删除补偿；提交结果未知时先按 user、MD5、storage_key 查询确认，仍无法确认则保留对象并报告待处理状态，避免误删已被提交记录引用的文件。当前已用假存储覆盖这些分支，FastDFS 适配器尚未接入。

FastDFS 的 `StorageClient` 适配器已实现：它使用 `fork/execvp` 分别调用 `fdfs_upload_file` 和 `fdfs_delete_file`，检查子进程状态，校验返回的 storage_key，并根据公开基础地址生成 URL。命令参数不经过 Shell 拼接。配置示例位于 `conf/fastdfs-client.conf.example`；当前 Docker 开发栈尚未安装 FastDFS CLI 或启动 tracker/storage，所以适配器还没有接入公开上传接口。

文件接收层使用 `mkstemp` 创建权限受限的随机临时文件，并在分块写入时增量计算服务端 MD5、累计真实字节数和执行大小限制。声明大小或 MD5 不一致、写入失败、请求超限时会立即删除临时文件；成功后再把临时路径移交给存储层。该模块不使用用户文件名作为本地路径，也不需要把完整文件加载进内存。

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

测试会启动开发栈，覆盖注册、登录、Redis Token、秒传、文件列表、分享、取消分享、删除和退出登录。退出后会检查 Redis key 已删除、旧 Token 被拒绝，并验证重复退出可安全重试。
