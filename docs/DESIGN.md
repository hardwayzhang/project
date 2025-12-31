# Proto 消息转换器设计文档

## 1. 需求概述

设计一个通用的 Protobuf 消息转换系统，实现以下功能：

- 通过自定义扩展选项标记主键字段
- 非主键且非 Metadata 类型的字段自动转换为 bytes 类型
- 转换后的字段名遵循 `data_X` 格式（X 为原字段的 tag 值）
- 支持双向转换（正向和逆向）

## 2. 扩展选项设计

### 2.1 选项定义

```protobuf
extend google.protobuf.MessageOptions {
    // 主键字段列表，逗号分隔
    optional string tcaplus_primary_key = 60000;
    
    // 是否启用字段转换
    optional bool convert_normal_field = 60001;
}
```

### 2.2 选项使用

```protobuf
message GenericTest {
    option (tcaplus_primary_key) = "ruid,type";
    option (convert_normal_field) = true;
    
    uint64 ruid = 1;        // 主键
    uint64 type = 2;        // 主键
    Metadata meta = 3;      // 特殊类型，保留
    TestValue value1 = 4;   // 转换为 bytes
    string value2 = 5;      // 转换为 bytes
}
```

## 3. 转换规则

### 3.1 字段分类

| 分类 | 识别条件 | 转换行为 |
|-----|---------|---------|
| 主键字段 | 字段名在 `tcaplus_primary_key` 列表中 | 直接复制，保持类型不变 |
| Metadata 类型 | 字段类型名为 "Metadata" | 直接复制，保持类型不变 |
| 普通字段 | 不满足以上条件 | 序列化为 bytes，字段名变为 `data_X` |

### 3.2 序列化策略

```
┌─────────────────────────────────────────────────────────────┐
│                    字段序列化策略                            │
├─────────────────────────┬───────────────────────────────────┤
│ 消息类型 (message)       │ 使用 SerializeToString() 序列化   │
├─────────────────────────┼───────────────────────────────────┤
│ 字符串类型 (string)      │ 直接使用字符串内容作为 bytes      │
├─────────────────────────┼───────────────────────────────────┤
│ bytes 类型              │ 直接复制                          │
├─────────────────────────┼───────────────────────────────────┤
│ 数值类型 (int32/64 等)   │ 二进制表示转换为 bytes            │
└─────────────────────────┴───────────────────────────────────┘
```

## 4. 接口设计

### 4.1 正向转换接口

```cpp
namespace tcaplus {

class ProtoConverter {
public:
    /**
     * @brief 将输入消息转换为输出消息
     * 
     * @param input  源消息（如 GenericTest）
     * @param output 目标消息（如 GenericTestOutput）
     * @return 0 成功，负值表示错误
     */
    static int Convert(const google::protobuf::Message* input,
                       google::protobuf::Message* output);
};

// C 风格兼容接口
int Convert(google::protobuf::Message* input,
            google::protobuf::Message* output);

}
```

### 4.2 逆向转换接口

```cpp
namespace tcaplus {

class ReverseConverter {
public:
    /**
     * @brief 将输出消息逆向转换回输入消息格式
     * 
     * @param input  带 data_X 字段的消息
     * @param output 原始格式的消息
     * @return 0 成功，负值表示错误
     */
    static int ReverseConvert(const google::protobuf::Message* input,
                               google::protobuf::Message* output);
};

}
```

## 5. 实现细节

### 5.1 核心算法流程

```
正向转换流程:
┌─────────────────────────────────────────────────────────────┐
│  1. 验证输入输出参数                                         │
│  2. 获取主键字段集合 (解析 tcaplus_primary_key)              │
│  3. 检查 convert_normal_field 是否启用                      │
│  4. 遍历输入消息的所有字段:                                  │
│     ├─ 判断字段类型 (主键/Metadata/普通)                     │
│     ├─ 确定输出字段名                                        │
│     ├─ 在输出消息中查找对应字段                              │
│     └─ 执行复制或序列化                                      │
│  5. 返回结果                                                 │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 字段匹配策略

输出字段查找优先级：
1. 按名称查找（用于主键和 Metadata 字段）
2. 按 tag 号查找（用于 `data_X` 格式字段）

### 5.3 Metadata 类型识别

```cpp
bool IsMetadataType(const FieldDescriptor* field) {
    if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
        return false;
    }
    return field->message_type()->name() == "Metadata";
}
```

## 6. 错误处理

### 6.1 错误码定义

```cpp
enum class ConvertResult {
    SUCCESS = 0,
    ERROR_NULL_INPUT = -1,
    ERROR_NULL_OUTPUT = -2,
    ERROR_NO_OPTIONS = -3,
    ERROR_FIELD_NOT_FOUND = -4,
    ERROR_FIELD_TYPE_MISMATCH = -5,
    ERROR_SERIALIZATION = -6,
    ERROR_PARSE_FAILED = -7,
    ERROR_UNKNOWN = -100
};
```

### 6.2 错误恢复策略

- 字段未找到：输出警告，继续处理其他字段
- 序列化失败：返回错误码，停止转换
- 类型不匹配：返回错误码，停止转换

## 7. 使用示例

### 7.1 基本使用

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

// 正向转换
tcaplus::GenericTestOutput output;
int result = tcaplus::Convert(&input, &output);

// 逆向转换
tcaplus::GenericTest restored;
result = tcaplus::ReverseConvert(&output, &restored);
```

### 7.2 从 bytes 恢复数据

```cpp
// 从 data_4 恢复 TestValue
tcaplus::TestValue value;
value.ParseFromString(output.data_4());

// data_5 直接是字符串内容
std::string str = output.data_5();
```

## 8. 扩展性

### 8.1 添加新的特殊类型

修改 `IsMetadataType` 函数，支持更多保留类型：

```cpp
bool IsSpecialType(const FieldDescriptor* field) {
    static const std::set<std::string> special_types = {
        "Metadata",
        "AuditInfo",
        "SystemFields"
    };
    
    if (field->type() != FieldDescriptor::TYPE_MESSAGE) {
        return false;
    }
    return special_types.count(field->message_type()->name()) > 0;
}
```

### 8.2 自定义字段命名

```cpp
// 自定义命名规则
std::string GetOutputFieldName(const FieldDescriptor* field) {
    // 默认: data_X
    return "data_" + std::to_string(field->number());
    
    // 或自定义格式: field_X_bytes
    // return "field_" + std::to_string(field->number()) + "_bytes";
}
```

## 9. 性能考虑

- 使用反射 API 实现通用性，有一定性能开销
- 对于高性能场景，可考虑代码生成方式
- 消息序列化使用 protobuf 内置函数，效率较高

## 10. 测试验证

测试用例覆盖：
- ✅ 基本转换功能
- ✅ 主键字段保留
- ✅ Metadata 类型识别
- ✅ 空消息处理
- ✅ 错误参数处理
- ✅ 双向转换完整性（Roundtrip）
