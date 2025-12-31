// proto_converter.h
// 通用protobuf消息转换接口

#ifndef PROTO_CONVERTER_H
#define PROTO_CONVERTER_H

#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <string>
#include <set>
#include <vector>

namespace tcaplus {

/**
 * @brief 转换错误码
 */
enum class ConvertResult {
    SUCCESS = 0,                    // 转换成功
    ERROR_NULL_INPUT = -1,          // 输入消息为空
    ERROR_NULL_OUTPUT = -2,         // 输出消息为空
    ERROR_NO_OPTIONS = -3,          // 输入消息没有定义转换选项
    ERROR_FIELD_NOT_FOUND = -4,     // 字段未找到
    ERROR_FIELD_TYPE_MISMATCH = -5, // 字段类型不匹配
    ERROR_SERIALIZATION = -6,       // 序列化失败
    ERROR_PARSE_FAILED = -7,        // 解析失败
    ERROR_UNKNOWN = -100            // 未知错误
};

/**
 * @brief Proto消息转换器类
 * 
 * 根据扩展选项将输入消息转换为输出消息：
 * - 主键字段（由 tcaplus_primary_key 指定）直接复制
 * - Metadata 类型字段直接复制
 * - 其他字段序列化为 bytes 类型，字段名变为 data_X（X为tag值）
 */
class ProtoConverter {
public:
    /**
     * @brief 通用转换接口
     * 
     * @param input 输入消息指针
     * @param output 输出消息指针
     * @return int 0表示成功，负值表示错误码
     */
    static int Convert(const ::google::protobuf::Message* input,
                       ::google::protobuf::Message* output);

    /**
     * @brief 获取消息的主键字段列表
     * 
     * @param message 消息指针
     * @return std::set<std::string> 主键字段名集合
     */
    static std::set<std::string> GetPrimaryKeys(const ::google::protobuf::Message* message);

    /**
     * @brief 判断是否启用字段转换
     * 
     * @param message 消息指针
     * @return bool 是否启用转换
     */
    static bool IsConvertEnabled(const ::google::protobuf::Message* message);

    /**
     * @brief 判断字段类型是否为Metadata
     * 
     * @param field 字段描述符
     * @return bool 是否为Metadata类型
     */
    static bool IsMetadataType(const ::google::protobuf::FieldDescriptor* field);

private:
    /**
     * @brief 复制字段值（用于主键和Metadata字段）
     */
    static bool CopyField(const ::google::protobuf::Message* input,
                          ::google::protobuf::Message* output,
                          const ::google::protobuf::FieldDescriptor* input_field,
                          const ::google::protobuf::FieldDescriptor* output_field);

    /**
     * @brief 将字段序列化为bytes
     */
    static bool SerializeFieldToBytes(const ::google::protobuf::Message* input,
                                      ::google::protobuf::Message* output,
                                      const ::google::protobuf::FieldDescriptor* input_field,
                                      const ::google::protobuf::FieldDescriptor* output_field);

    /**
     * @brief 解析主键字符串
     */
    static std::set<std::string> ParsePrimaryKeyString(const std::string& pk_str);
};

/**
 * @brief C风格接口，兼容原有设计
 * 
 * @param input 输入消息指针
 * @param output 输出消息指针
 * @return int 0表示成功，负值表示错误码
 */
inline int Convert(::google::protobuf::Message* input, ::google::protobuf::Message* output) {
    return ProtoConverter::Convert(input, output);
}

} // namespace tcaplus

#endif // PROTO_CONVERTER_H
