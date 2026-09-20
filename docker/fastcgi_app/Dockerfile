FROM ubuntu:22.04 AS fastdfs_runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    curl \
    libevent-dev \
    && rm -rf /var/lib/apt/lists/*

ARG LIBFASTCOMMON_VERSION=V1.0.87
ARG LIBSERVERFRAME_VERSION=V1.2.14
ARG FASTDFS_VERSION=V6.17.1

RUN set -eux; \
    mkdir -p /tmp/libfastcommon /tmp/libserverframe /tmp/fastdfs; \
    curl -fsSL "https://github.com/happyfish100/libfastcommon/archive/refs/tags/${LIBFASTCOMMON_VERSION}.tar.gz" \
        -o /tmp/libfastcommon.tar.gz; \
    tar -xzf /tmp/libfastcommon.tar.gz --strip-components=1 -C /tmp/libfastcommon; \
    cd /tmp/libfastcommon; \
    ./make.sh; \
    ./make.sh install; \
    curl -fsSL "https://github.com/happyfish100/libserverframe/archive/refs/tags/${LIBSERVERFRAME_VERSION}.tar.gz" \
        -o /tmp/libserverframe.tar.gz; \
    tar -xzf /tmp/libserverframe.tar.gz --strip-components=1 -C /tmp/libserverframe; \
    cd /tmp/libserverframe; \
    ./make.sh; \
    ./make.sh install; \
    curl -fsSL "https://github.com/happyfish100/fastdfs/archive/refs/tags/${FASTDFS_VERSION}.tar.gz" \
        -o /tmp/fastdfs.tar.gz; \
    tar -xzf /tmp/fastdfs.tar.gz --strip-components=1 -C /tmp/fastdfs; \
    cd /tmp/fastdfs; \
    ./make.sh; \
    ./make.sh install; \
    ldconfig; \
    rm -rf /tmp/libfastcommon /tmp/libserverframe /tmp/fastdfs /tmp/*.tar.gz

COPY docker/fastdfs/conf/client.conf /etc/fdfs/client.conf
COPY docker/fastdfs/conf/tracker.conf /etc/fdfs/tracker.conf
COPY docker/fastdfs/conf/storage.conf /etc/fdfs/storage.conf
COPY docker/fastdfs/start.sh /usr/local/bin/start-fastdfs
RUN chmod +x /usr/local/bin/start-fastdfs && mkdir -p /tmp/fastdfs-client

FROM fastdfs_runtime AS fastdfs_server

VOLUME ["/data/fastdfs"]
ENTRYPOINT ["/usr/local/bin/start-fastdfs"]

FROM fastdfs_runtime AS fastcgi_app

RUN apt-get update && apt-get install -y --no-install-recommends \
    default-libmysqlclient-dev \
    libfcgi-dev \
    libhiredis-dev \
    spawn-fcgi \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY common ./common
COPY include ./include
COPY src_cgi ./src_cgi
COPY tests ./tests
COPY tools ./tools

RUN make all integration-tools && mkdir -p /app/logs

COPY docker/fastcgi_app/start.sh /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 10000 10001 10002 10003 10004 10005 10006

CMD ["/app/start.sh"]
