// reverse_converter.h
// 逆向转换器：从Output消息转换回Input消息

#ifndef REVERSE_CONVERTER_H
#define REVERSE_CONVERTER_H

#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <string>
#include <set>
#include <map>

namespace tcaplus {

/**
 * @brief 逆向转换器类
 * 
 * 将输出消息（包含bytes字段）转换回原始输入消息格式
 */
class ReverseConverter {
public:
    /**
     * @brief 逆向转换接口
     * 
     * 将带有 data_X 字段的消息转换回原始消息格式
     * 
     * @param input 带有bytes字段的消息（如GenericTestOutput）
     * @param output 原始格式的消息（如GenericTest）
     * @return int 0表示成功，负值表示错误码
     */
    static int ReverseConvert(const ::google::protobuf::Message* input,
                               ::google::protobuf::Message* output);

    /**
     * @brief 建立字段映射关系
     * 
     * 分析输出消息的结构，建立 data_X 到原始字段的映射
     * 
     * @param output_desc 输出消息描述符
     * @return std::map<int, std::string> tag号到字段名的映射
     */
    static std::map<int, std::string> BuildFieldMapping(
        const ::google::protobuf::Descriptor* output_desc);

private:
    /**
     * @brief 从bytes反序列化到消息字段
     */
    static bool DeserializeBytesToField(
        const ::google::protobuf::Message* input,
        ::google::protobuf::Message* output,
        const ::google::protobuf::FieldDescriptor* input_field,
        const ::google::protobuf::FieldDescriptor* output_field);

    /**
     * @brief 复制字段（用于非bytes字段）
     */
    static bool CopyField(
        const ::google::protobuf::Message* input,
        ::google::protobuf::Message* output,
        const ::google::protobuf::FieldDescriptor* input_field,
        const ::google::protobuf::FieldDescriptor* output_field);
};

/**
 * @brief C风格逆向转换接口
 */
inline int ReverseConvert(::google::protobuf::Message* input,
                          ::google::protobuf::Message* output) {
    return ReverseConverter::ReverseConvert(input, output);
}

} // namespace tcaplus

#endif // REVERSE_CONVERTER_H
