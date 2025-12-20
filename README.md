# GoRes - Go 资源管理框架

基于 Protobuf 和 Excel 的 Go 游戏资源管理解决方案。

## 特性

- 📊 **Excel 数据存储** - 使用 Excel 文件作为配置数据源,方便策划编辑
- 📝 **Protobuf 元数据定义** - 使用 Protocol Buffers 描述数据结构
- 🔑 **主键和索引支持** - 通过扩展选项定义主键、唯一索引、普通索引和复合索引
- 🔄 **嵌套消息支持** - 完整支持 Protobuf 嵌套消息类型
- ⚡ **高效二进制格式** - 导出为 Protobuf 序列化二进制,加载速度快
- 🛠️ **代码生成工具** - 自动生成类型安全的 Go 访问接口

## 项目结构

```
gores/
├── proto/
│   └── resoptions.proto    # Protobuf 自定义选项定义
├── cmd/
│   ├── excel2pb/           # Excel 转 Protobuf 工具
│   └── protoc-gen-gores/   # protoc 代码生成插件
├── pkg/
│   ├── parser/             # Excel 解析器
│   └── resloader/          # 资源加载器核心库
├── example/
│   ├── proto/              # 示例 proto 定义
│   ├── excel/              # 示例 Excel 文件
│   ├── data/               # 导出的二进制数据
│   └── generated/          # 生成的 Go 代码
└── scripts/                # 构建和生成脚本
```

## 快速开始

### 1. 安装依赖

```bash
# 安装 protoc (Protocol Buffers 编译器)
# macOS
brew install protobuf

# Linux
apt-get install -y protobuf-compiler

# 安装 Go protobuf 插件
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
```

### 2. 构建工具

```bash
./scripts/build.sh
```

### 3. 定义 Proto 文件

```protobuf
syntax = "proto3";

package game;

import "proto/resoptions.proto";

message ItemConfig {
  option (gores.resource) = {
    name: "item"
    excel_file: "item_config"
    sheet_name: "Sheet1"
  };

  // 主键字段
  int32 id = 1 [(gores.field) = {
    primary_key: true
  }];

  string name = 2;

  // 唯一索引
  string code = 3 [(gores.field) = {
    index: INDEX_UNIQUE
  }];

  // 普通索引
  int32 item_type = 4 [(gores.field) = {
    index: INDEX_NORMAL
  }];
}
```

### 4. 创建 Excel 文件

Excel 文件格式 (`item_config.xlsx`):

| id | name | code | item_type |
|----|------|------|-----------|
| 1001 | 铁剑 | iron_sword | 1 |
| 1002 | 钢剑 | steel_sword | 1 |
| 1003 | 生命药水 | hp_potion | 3 |

### 5. 生成代码

```bash
./scripts/generate.sh
```

### 6. 导出数据

```bash
./scripts/export.sh
```

### 7. 使用资源

```go
package main

import (
    "log"
    "github.com/example/gores/example/generated"
)

func main() {
    // 获取资源管理器
    mgr := generated.GetItemConfigManager()
    
    // 加载资源
    if err := mgr.Load("./data/item_config.bin"); err != nil {
        log.Fatal(err)
    }
    
    // 通过主键访问
    item, ok := mgr.Get(1001)
    if ok {
        log.Printf("物品: %s", item.Name)
    }
    
    // 通过唯一索引访问
    item, ok = mgr.GetByCode("iron_sword")
    
    // 通过普通索引访问(返回列表)
    weapons := mgr.GetByType(ItemType_TYPE_WEAPON)
    
    // 遍历所有资源
    mgr.Range(func(item *ItemConfig) bool {
        log.Printf("ID: %d, Name: %s", item.Id, item.Name)
        return true // 继续遍历
    })
    
    // 过滤查询
    expensive := mgr.Filter(func(item *ItemConfig) bool {
        return item.Price > 1000
    })
}
```

## 自定义选项说明

### 消息选项 (ResourceOptions)

```protobuf
message ResourceOptions {
  string name = 1;        // 资源名称
  string excel_file = 2;  // Excel文件名(不含扩展名)
  string sheet_name = 3;  // 工作表名称
  bool hot_reload = 4;    // 是否支持热更新
  string description = 5; // 资源描述
}
```

### 字段选项 (FieldOptions)

```protobuf
message FieldOptions {
  bool primary_key = 1;       // 是否为主键
  IndexType index = 2;        // 索引类型
  string index_name = 3;      // 索引名称(复合索引)
  int32 index_order = 4;      // 复合索引顺序
  string column_name = 5;     // Excel列名映射
  bool required = 6;          // 是否必填
  string default_value = 7;   // 默认值
  string description = 8;     // 字段描述
  string nested_separator = 9;  // 嵌套分隔符
  string array_separator = 10;  // 数组分隔符
}
```

### 索引类型

- `INDEX_NONE` - 无索引
- `INDEX_UNIQUE` - 唯一索引
- `INDEX_NORMAL` - 普通索引(一对多)
- `INDEX_COMPOSITE` - 复合索引成员

## Excel 格式规范

### 基本规则

1. **第一行为表头**,对应 proto 字段名
2. 字段名支持 snake_case 和 camelCase
3. 空单元格表示该字段使用默认值

### 数据类型映射

| Proto 类型 | Excel 格式 | 示例 |
|-----------|-----------|------|
| int32/int64 | 整数 | `100` |
| float/double | 小数 | `3.14` |
| bool | true/false/1/0 | `true` |
| string | 文本 | `hello` |
| enum | 名称或数字 | `TYPE_WEAPON` 或 `1` |
| repeated | 分隔符分隔 | `1,2,3` |
| message | JSON 格式 | `{"name":"atk","value":10}` |

### 嵌套消息

嵌套消息支持两种格式:

**方式1: JSON 格式 (推荐)**
```
{"name":"attack","value":100}
```

**方式2: 点号分隔列名**
```
| attr.name | attr.value |
|-----------|------------|
| attack    | 100        |
```

### 数组类型

使用分隔符(默认逗号)分隔:
```
| tags          | drop_items |
|---------------|------------|
| weapon,sword  | 1001,1002  |
```

嵌套消息数组使用 `|` 分隔:
```
| attributes |
|------------|
| {"name":"atk","value":100}|{"name":"def","value":50} |
```

## 复合索引示例

```protobuf
message MonsterConfig {
  int32 id = 1 [(gores.field) = { primary_key: true }];
  
  // 场景ID + 怪物类型 组成复合索引
  int32 scene_id = 2 [(gores.field) = {
    index: INDEX_COMPOSITE
    index_name: "scene_type"
    index_order: 1
  }];
  
  int32 monster_type = 3 [(gores.field) = {
    index: INDEX_COMPOSITE
    index_name: "scene_type"
    index_order: 2
  }];
}
```

生成的访问接口:

```go
// 复合索引查询
monsters := mgr.GetBySceneAndType(sceneId, monsterType)
```

## 生成的 API

对于每个资源消息,生成以下接口:

```go
type XXXManager struct { ... }

// 单例获取
func GetXXXManager() *XXXManager

// 加载
func (m *XXXManager) Load(path string) error
func (m *XXXManager) LoadFromBytes(data []byte) error

// 主键访问
func (m *XXXManager) Get(key KeyType) (*XXX, bool)
func (m *XXXManager) MustGet(key KeyType) *XXX
func (m *XXXManager) Has(key KeyType) bool

// 索引访问 (对每个索引字段生成)
func (m *XXXManager) GetByFieldName(key FieldType) (*XXX, bool)     // 唯一索引
func (m *XXXManager) GetByFieldName(key FieldType) []*XXX           // 普通索引

// 遍历和过滤
func (m *XXXManager) GetAll() []*XXX
func (m *XXXManager) Count() int
func (m *XXXManager) Range(fn func(*XXX) bool)
func (m *XXXManager) Filter(fn func(*XXX) bool) []*XXX
func (m *XXXManager) Find(fn func(*XXX) bool) (*XXX, bool)
```

## 二进制文件格式

```
[4 bytes] 消息数量 (uint32, little-endian)
[4 bytes] 第1条消息长度 (uint32)
[N bytes] 第1条消息数据 (protobuf encoded)
[4 bytes] 第2条消息长度
[N bytes] 第2条消息数据
...
```

## 工具命令

### excel2pb

```bash
./bin/excel2pb \
  -excel=./excel \
  -output=./data \
  -descriptor=./proto/descriptor.pb \
  -message=game.ItemConfig \
  -format=binary
```

参数说明:
- `-excel`: Excel 文件目录
- `-output`: 输出目录
- `-descriptor`: Proto descriptor set 文件
- `-message`: 消息全名
- `-sheet`: 工作表名称(可选)
- `-format`: 输出格式 (binary/json)

### protoc-gen-gores

作为 protoc 插件使用:

```bash
protoc \
  --go_out=. \
  --gores_out=. \
  -I. \
  your_config.proto
```

## 最佳实践

1. **ID 设计**: 使用有意义的 ID 范围,如 1000-1999 为武器,2000-2999 为防具
2. **索引选择**: 只对频繁查询的字段建立索引
3. **数据验证**: 在导出时进行数据验证,确保主键唯一性
4. **版本控制**: 将 proto 文件和生成代码纳入版本控制
5. **热更新**: 资源管理器支持运行时重新加载

## 扩展开发

### 自定义解析器

```go
type CustomParser struct {
    *parser.ExcelParser
}

func (p *CustomParser) ParseCustomField(value string) (interface{}, error) {
    // 自定义解析逻辑
}
```

### 自定义索引

```go
store := resloader.NewIndexedStore("item", func(item *ItemConfig) int32 {
    return item.Id
})

// 添加自定义索引
priceIndex := resloader.AddMultiIndex(store, "price_range", 
    func(item *ItemConfig) string {
        if item.Price < 100 {
            return "cheap"
        } else if item.Price < 1000 {
            return "normal"
        }
        return "expensive"
    })
```

## License

MIT License
