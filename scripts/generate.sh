#!/bin/bash
# generate.sh - 代码生成脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== 生成代码 ==="

cd "$PROJECT_DIR"

# 确保bin目录存在
mkdir -p bin

# 检查 protoc 是否安装
if ! command -v protoc &> /dev/null; then
    echo "错误: protoc 未安装"
    echo "请安装 protobuf 编译器: https://github.com/protocolbuffers/protobuf/releases"
    exit 1
fi

# 构建 protoc-gen-gores 插件
echo "构建 protoc-gen-gores 插件..."
go build -o bin/protoc-gen-gores ./cmd/protoc-gen-gores

# 添加到 PATH
export PATH="$PROJECT_DIR/bin:$PATH"

# 生成选项定义
echo "生成 resoptions.pb.go..."
protoc \
    --go_out=. \
    --go_opt=paths=source_relative \
    proto/resoptions.proto

# 生成示例proto代码
echo "生成示例 proto 代码..."
protoc \
    --go_out=. \
    --go_opt=paths=source_relative \
    --gores_out=. \
    --gores_opt=paths=source_relative \
    -I. \
    example/proto/item.proto

echo "代码生成完成!"
