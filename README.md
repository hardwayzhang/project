## Go 资源管理（Excel + Protobuf options + pb 二进制）最小可用版本

### 目录结构

- `proto/`：资源元数据（protobuf），包含自定义 extend options（主键、索引、sheet/out_file）
- `tools/excel2pb/`：Excel → protobuf 二进制（`.pb`）导出器
- `runtime/resource/`：运行期加载 `.pb` 并基于主键/索引提供访问 API
- `cmd/make_sample_excel/`：生成示例 Excel（`res_excel/item.xlsx`）
- `cmd/demo/`：演示运行期加载与查询

### 快速跑通

1) 生成示例 Excel：

```bash
go run ./cmd/make_sample_excel
```

2) 导出为 protobuf 二进制：

```bash
go run ./tools/excel2pb \
  -proto_dir proto \
  -entry game/res/item.proto \
  -message game.res.ItemTable \
  -excel res_excel/item.xlsx \
  -out res_bin/item.pb
```

3) 运行期加载并按主键/索引查询：

```bash
go run ./cmd/demo
```

### Excel 约定

- 第一行必须是 protobuf 字段名（示例：`id,name,type,quality`）
- 空列或 `#` 开头列会被忽略
- `repeated` 字段用 `;` 分隔
