# docker/Dockerfile
# 基于 ClickHouse 官方 binary-builder 镜像的构建环境
# 解决 Mac ARM 架构编译问题

FROM clickhouse/binary-builder:latest

# 设置工作目录
WORKDIR /work

# 安装额外的测试工具
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3-pip \
    docker.io \
    docker-compose \
    lsof \
    && rm -rf /var/lib/apt/lists/*

# 安装 Python 测试工具
RUN python3 -m pip install --no-cache-dir --break-system-packages \
    pytest pytest-xdist requests docker pyyaml

# 创建非 root 用户
RUN useradd -m builder && echo "builder ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers
USER builder

# 设置环境变量
ENV CC=clang \
    CXX=clang++ \
    LD=ld.lld

# 默认命令
CMD ["/bin/bash"]


