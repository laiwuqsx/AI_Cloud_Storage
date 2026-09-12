# AI Cloud Storage

一个用 C / FastCGI 复刻并逐步改进的私有云存储项目。

## 当前阶段

第一阶段只建立与原项目一致的基础骨架，并完成最小登录请求闭环：

```text
Nginx /api/login -> FastCGI login -> JSON response
```

后续将按原项目的模块逐步实现注册、文件上传、MD5 秒传、分片上传、MySQL、Redis、FastDFS 和 FAISS 检索。

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

该命令验证最小 JSON 请求解析：`{"user":"alice","password":"secret"}`。

## 本地容器启动

    cd docker
    cp ../.env.example .env
    docker compose up --build

启动后，Nginx 在 http://localhost:8080 提供两个路由：

- POST /api/reg：注册。请求体包含 user、nickname 和客户端计算的 MD5 password。
- POST /api/login：登录。成功后在 Redis 保存会话并返回 Token。
- POST /api/myfiles：携带 user 和 Token，返回当前用户的文件元数据列表。
- POST /api/md5：命中已有物理文件时，仅创建用户文件关联，实现秒传。
- POST /api/dealfile?cmd=del：携带 user、Token、md5，删除当前用户的文件关联。
- POST /api/dealfile?cmd=share：携带 user、Token、md5，将当前用户的文件标记为分享。

## 认证端到端测试

Docker 守护进程运行后，在项目根目录执行：

    make e2e

测试会启动开发栈，注册唯一测试用户，验证登录 Token 写入 Redis，并验证错误密码被拒绝。
