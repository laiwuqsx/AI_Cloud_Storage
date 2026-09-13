# AI Cloud Storage

一个用 C / FastCGI 复刻并逐步改进的私有云存储项目。

## 当前阶段

当前已完成认证和文件元数据管理的基础闭环：

```text
Nginx -> C FastCGI -> MySQL / Redis
```

已实现注册、登录、退出登录、用户文件列表、MD5 秒传、逻辑删除以及分享状态管理。FastDFS 普通上传、分片上传、受控分享链接、前端和 FAISS 检索仍在后续阶段。

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
