#!/bin/bash
# export.sh - Excel数据导出脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== 导出Excel数据为Protobuf二进制 ==="

cd "$PROJECT_DIR"

# 默认配置
EXCEL_DIR="${EXCEL_DIR:-./example/excel}"
OUTPUT_DIR="${OUTPUT_DIR:-./example/data}"
DESCRIPTOR="${DESCRIPTOR:-./example/proto/descriptor.pb}"
FORMAT="${FORMAT:-binary}"

# 构建 excel2pb 工具
echo "构建 excel2pb..."
go build -o bin/excel2pb ./cmd/excel2pb

# 生成 descriptor set
echo "生成 proto descriptor set..."
protoc \
    --descriptor_set_out="$DESCRIPTOR" \
    --include_imports \
    -I. \
    example/proto/item.proto

# 导出配置
declare -a CONFIGS=(
    "game.ItemConfig"
    "game.SkillConfig"
    "game.MonsterConfig"
)

mkdir -p "$OUTPUT_DIR"

for MSG in "${CONFIGS[@]}"; do
    echo "导出 $MSG..."
    ./bin/excel2pb \
        -excel="$EXCEL_DIR" \
        -output="$OUTPUT_DIR" \
        -descriptor="$DESCRIPTOR" \
        -message="$MSG" \
        -format="$FORMAT" \
        || echo "警告: $MSG 导出失败(可能缺少Excel文件)"
done

echo "导出完成!"
echo "二进制文件位于 $OUTPUT_DIR 目录"
