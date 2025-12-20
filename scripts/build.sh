#!/bin/bash
# build.sh - 构建脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== 构建 Go 资源管理工具 ==="

cd "$PROJECT_DIR"

# 下载依赖
echo "下载依赖..."
go mod tidy

# 构建 excel2pb 工具
echo "构建 excel2pb..."
go build -o bin/excel2pb ./cmd/excel2pb

# 构建 protoc-gen-gores 插件
echo "构建 protoc-gen-gores..."
go build -o bin/protoc-gen-gores ./cmd/protoc-gen-gores

echo "构建完成!"
echo "工具位于 bin/ 目录"
