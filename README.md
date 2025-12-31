# Proto Message Converter

通用的 Protobuf 消息转换器，根据自定义扩展选项将 proto 消息中的特定字段序列化为 bytes 类型。

## 功能特性

- **主键字段保留**：通过 `tcaplus_primary_key` 选项指定的主键字段直接复制
- **Metadata 类型保留**：`Metadata` 类型的字段直接复制，不进行序列化
- **普通字段转换**：其他字段序列化为 `bytes` 类型，字段名变为 `data_X`（X为原字段的tag值）
- **通用接口**：支持任意 proto 消息类型的转换

## 项目结构

```
/workspace/
├── CMakeLists.txt          # CMake 构建配置
├── README.md               # 本文档
├── proto/
│   ├── options.proto       # 自定义扩展选项定义
│   └── messages.proto      # 消息结构定义
├── include/
│   └── proto_converter.h   # 转换器头文件
└── src/
    ├── proto_converter.cpp # 转换器实现
    └── main.cpp            # 测试程序
```

## 扩展选项说明

### `tcaplus_primary_key`

指定消息的主键字段，多个字段用逗号分隔：

```protobuf
option (tcaplus_primary_key) = "ruid,type";
```

### `convert_normal_field`

控制是否将普通字段转换为 bytes：

```protobuf
option (convert_normal_field) = true;
```

## 转换规则

| 字段类型 | 转换规则 |
|---------|---------|
| 主键字段 | 直接复制，保持原类型 |
| Metadata 类型 | 直接复制，保持原类型 |
| 消息类型 | 序列化为 bytes，字段名 → data_X |
| 字符串类型 | 内容作为 bytes，字段名 → data_X |
| 其他类型 | 二进制序列化为 bytes，字段名 → data_X |

## 示例

### 输入消息定义

```protobuf
message GenericTest {
  option (tcaplus_primary_key) = "ruid,type";
  option (convert_normal_field) = true;

  uint64 ruid = 1;        // 主键 → 保留
  uint64 type = 2;        // 主键 → 保留
  Metadata meta = 3;      // Metadata → 保留
  TestValue value1 = 4;   // 转换 → bytes data_4
  string value2 = 5;      // 转换 → bytes data_5
}
```

### 输出消息定义

```protobuf
message GenericTestOutput {
  uint64 ruid = 1;
  uint64 type = 2;
  Metadata meta = 3;
  bytes data_4 = 4;
  bytes data_5 = 5;
}
```

### 使用方式

```cpp
#include "proto_converter.h"
#include "messages.pb.h"

// 创建输入消息
tcaplus::GenericTest input;
input.set_ruid(12345);
input.set_type(1);
input.mutable_meta()->set_version(100);
input.mutable_value1()->set_value(42);
input.set_value2("Hello");

// 创建输出消息
tcaplus::GenericTestOutput output;

// 执行转换
int result = tcaplus::Convert(&input, &output);

if (result == 0) {
    // 转换成功
    // output.ruid() == 12345
    // output.type() == 1
    // output.meta().version() == 100
    // output.data_4() 包含序列化的 TestValue
    // output.data_5() 包含 "Hello" 的字节
}
```

### 反序列化 bytes 字段

```cpp
// 从 data_4 恢复 TestValue
tcaplus::TestValue restored;
restored.ParseFromString(output.data_4());
// restored.value() == 42

// data_5 直接就是字符串内容
std::string value2 = output.data_5();
// value2 == "Hello"
```

## 构建

### 依赖项

- CMake >= 3.14
- Protobuf >= 3.x
- C++17 兼容的编译器

### 构建步骤

```bash
# 创建构建目录
mkdir build && cd build

# 配置
cmake ..

# 编译
make

# 运行测试
./converter_test
```

## API 参考

### 主要接口

```cpp
namespace tcaplus {

// 通用转换函数
int Convert(::google::protobuf::Message* input, 
            ::google::protobuf::Message* output);

// 转换器类
class ProtoConverter {
public:
    // 执行转换
    static int Convert(const ::google::protobuf::Message* input,
                       ::google::protobuf::Message* output);
    
    // 获取主键字段列表
    static std::set<std::string> GetPrimaryKeys(
        const ::google::protobuf::Message* message);
    
    // 判断是否启用字段转换
    static bool IsConvertEnabled(
        const ::google::protobuf::Message* message);
    
    // 判断字段是否为 Metadata 类型
    static bool IsMetadataType(
        const ::google::protobuf::FieldDescriptor* field);
};

}
```

### 返回值

| 值 | 含义 |
|----|-----|
| 0 | 成功 |
| -1 | 输入消息为空 |
| -2 | 输出消息为空 |
| -3 | 没有定义转换选项 |
| -4 | 字段未找到 |
| -5 | 字段类型不匹配 |
| -6 | 序列化失败 |
| -7 | 解析失败 |

## 扩展

### 添加新的特殊类型

如果需要像 `Metadata` 一样保留其他类型，修改 `IsMetadataType` 函数：

```cpp
bool ProtoConverter::IsMetadataType(const FieldDescriptor* field) {
    if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
        return false;
    }
    const auto* msg_type = field->message_type();
    if (msg_type) {
        std::string type_name = msg_type->name();
        // 添加新的特殊类型
        return (type_name == "Metadata" || 
                type_name == "AnotherSpecialType");
    }
    return false;
}
```

### 自定义字段命名规则

修改 `Convert` 函数中的字段名生成逻辑：

```cpp
// 默认规则: data_X
output_field_name = "data_" + std::to_string(field_tag);

// 自定义规则示例: field_X_bytes
output_field_name = "field_" + std::to_string(field_tag) + "_bytes";
```

## 许可证

MIT License
