FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
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

RUN make all && mkdir -p /app/logs

COPY docker/fastcgi_app/start.sh /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 10000 10001

CMD ["/app/start.sh"]
