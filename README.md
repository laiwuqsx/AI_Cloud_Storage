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
