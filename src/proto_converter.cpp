// proto_converter.cpp
// 通用protobuf消息转换实现

#include "proto_converter.h"
#include "options.pb.h"

#include <google/protobuf/reflection.h>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace tcaplus {

std::set<std::string> ProtoConverter::ParsePrimaryKeyString(const std::string& pk_str) {
    std::set<std::string> keys;
    std::stringstream ss(pk_str);
    std::string key;
    
    while (std::getline(ss, key, ',')) {
        // 去除前后空格
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (!key.empty()) {
            keys.insert(key);
        }
    }
    
    return keys;
}

std::set<std::string> ProtoConverter::GetPrimaryKeys(const ::google::protobuf::Message* message) {
    std::set<std::string> keys;
    
    if (!message) {
        return keys;
    }
    
    const auto* descriptor = message->GetDescriptor();
    const auto& options = descriptor->options();
    
    // 获取 tcaplus_primary_key 扩展选项
    if (options.HasExtension(tcaplus_primary_key)) {
        const std::string& pk_str = options.GetExtension(tcaplus_primary_key);
        keys = ParsePrimaryKeyString(pk_str);
    }
    
    return keys;
}

bool ProtoConverter::IsConvertEnabled(const ::google::protobuf::Message* message) {
    if (!message) {
        return false;
    }
    
    const auto* descriptor = message->GetDescriptor();
    const auto& options = descriptor->options();
    
    // 获取 convert_normal_field 扩展选项，默认为 false
    if (options.HasExtension(convert_normal_field)) {
        return options.GetExtension(convert_normal_field);
    }
    
    return false;
}

bool ProtoConverter::IsMetadataType(const ::google::protobuf::FieldDescriptor* field) {
    if (!field) {
        return false;
    }
    
    // 检查字段是否为消息类型
    if (field->type() != ::google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
        return false;
    }
    
    // 检查消息类型名是否为 "Metadata"
    const auto* msg_type = field->message_type();
    if (msg_type) {
        // 支持完全限定名和简单名
        std::string type_name = msg_type->name();
        return (type_name == "Metadata");
    }
    
    return false;
}

bool ProtoConverter::CopyField(const ::google::protobuf::Message* input,
                                ::google::protobuf::Message* output,
                                const ::google::protobuf::FieldDescriptor* input_field,
                                const ::google::protobuf::FieldDescriptor* output_field) {
    if (!input || !output || !input_field || !output_field) {
        return false;
    }
    
    const auto* input_ref = input->GetReflection();
    auto* output_ref = output->GetReflection();
    
    // 根据字段类型进行复制
    switch (input_field->type()) {
        case ::google::protobuf::FieldDescriptor::TYPE_DOUBLE:
            output_ref->SetDouble(output, output_field, 
                input_ref->GetDouble(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_FLOAT:
            output_ref->SetFloat(output, output_field, 
                input_ref->GetFloat(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_INT64:
        case ::google::protobuf::FieldDescriptor::TYPE_SFIXED64:
        case ::google::protobuf::FieldDescriptor::TYPE_SINT64:
            output_ref->SetInt64(output, output_field, 
                input_ref->GetInt64(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_UINT64:
        case ::google::protobuf::FieldDescriptor::TYPE_FIXED64:
            output_ref->SetUInt64(output, output_field, 
                input_ref->GetUInt64(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_INT32:
        case ::google::protobuf::FieldDescriptor::TYPE_SFIXED32:
        case ::google::protobuf::FieldDescriptor::TYPE_SINT32:
            output_ref->SetInt32(output, output_field, 
                input_ref->GetInt32(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_UINT32:
        case ::google::protobuf::FieldDescriptor::TYPE_FIXED32:
            output_ref->SetUInt32(output, output_field, 
                input_ref->GetUInt32(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_BOOL:
            output_ref->SetBool(output, output_field, 
                input_ref->GetBool(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_STRING:
            output_ref->SetString(output, output_field, 
                input_ref->GetString(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_BYTES:
            output_ref->SetString(output, output_field, 
                input_ref->GetString(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_ENUM:
            output_ref->SetEnumValue(output, output_field, 
                input_ref->GetEnumValue(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_MESSAGE: {
            // 对于消息类型，复制整个子消息
            const auto& input_msg = input_ref->GetMessage(*input, input_field);
            auto* output_msg = output_ref->MutableMessage(output, output_field);
            output_msg->CopyFrom(input_msg);
            break;
        }
            
        default:
            return false;
    }
    
    return true;
}

bool ProtoConverter::SerializeFieldToBytes(const ::google::protobuf::Message* input,
                                            ::google::protobuf::Message* output,
                                            const ::google::protobuf::FieldDescriptor* input_field,
                                            const ::google::protobuf::FieldDescriptor* output_field) {
    if (!input || !output || !input_field || !output_field) {
        return false;
    }
    
    // 确保输出字段是 bytes 类型
    if (output_field->type() != ::google::protobuf::FieldDescriptor::TYPE_BYTES) {
        std::cerr << "Output field " << output_field->name() 
                  << " is not bytes type" << std::endl;
        return false;
    }
    
    const auto* input_ref = input->GetReflection();
    auto* output_ref = output->GetReflection();
    
    std::string serialized_data;
    
    // 根据输入字段类型进行序列化
    switch (input_field->type()) {
        case ::google::protobuf::FieldDescriptor::TYPE_MESSAGE: {
            // 消息类型：序列化整个消息
            const auto& msg = input_ref->GetMessage(*input, input_field);
            if (!msg.SerializeToString(&serialized_data)) {
                std::cerr << "Failed to serialize message field: " 
                          << input_field->name() << std::endl;
                return false;
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_STRING: {
            // 字符串类型：直接使用字符串内容作为bytes
            serialized_data = input_ref->GetString(*input, input_field);
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_BYTES: {
            // bytes类型：直接复制
            serialized_data = input_ref->GetString(*input, input_field);
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_INT32:
        case ::google::protobuf::FieldDescriptor::TYPE_SFIXED32:
        case ::google::protobuf::FieldDescriptor::TYPE_SINT32: {
            int32_t value = input_ref->GetInt32(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_INT64:
        case ::google::protobuf::FieldDescriptor::TYPE_SFIXED64:
        case ::google::protobuf::FieldDescriptor::TYPE_SINT64: {
            int64_t value = input_ref->GetInt64(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_UINT32:
        case ::google::protobuf::FieldDescriptor::TYPE_FIXED32: {
            uint32_t value = input_ref->GetUInt32(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_UINT64:
        case ::google::protobuf::FieldDescriptor::TYPE_FIXED64: {
            uint64_t value = input_ref->GetUInt64(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_DOUBLE: {
            double value = input_ref->GetDouble(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_FLOAT: {
            float value = input_ref->GetFloat(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_BOOL: {
            bool value = input_ref->GetBool(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_ENUM: {
            int value = input_ref->GetEnumValue(*input, input_field);
            serialized_data.assign(reinterpret_cast<const char*>(&value), sizeof(value));
            break;
        }
        
        default:
            std::cerr << "Unsupported field type for serialization: " 
                      << input_field->type_name() << std::endl;
            return false;
    }
    
    // 设置输出字段的bytes值
    output_ref->SetString(output, output_field, serialized_data);
    return true;
}

int ProtoConverter::Convert(const ::google::protobuf::Message* input,
                             ::google::protobuf::Message* output) {
    // 参数检查
    if (!input) {
        return static_cast<int>(ConvertResult::ERROR_NULL_INPUT);
    }
    if (!output) {
        return static_cast<int>(ConvertResult::ERROR_NULL_OUTPUT);
    }
    
    // 获取输入消息的描述符和反射
    const auto* input_desc = input->GetDescriptor();
    const auto* output_desc = output->GetDescriptor();
    
    // 获取主键字段集合
    std::set<std::string> primary_keys = GetPrimaryKeys(input);
    
    // 检查是否启用字段转换
    bool convert_enabled = IsConvertEnabled(input);
    
    // 遍历输入消息的所有字段
    for (int i = 0; i < input_desc->field_count(); ++i) {
        const auto* input_field = input_desc->field(i);
        const std::string& field_name = input_field->name();
        int field_tag = input_field->number();
        
        // 判断字段类型
        bool is_primary_key = (primary_keys.find(field_name) != primary_keys.end());
        bool is_metadata = IsMetadataType(input_field);
        
        // 确定输出字段名
        std::string output_field_name;
        if (is_primary_key || is_metadata || !convert_enabled) {
            // 主键字段和Metadata字段保持原名
            output_field_name = field_name;
        } else {
            // 其他字段转换为 data_X 格式
            output_field_name = "data_" + std::to_string(field_tag);
        }
        
        // 查找输出字段
        const auto* output_field = output_desc->FindFieldByName(output_field_name);
        
        // 如果按名称找不到，尝试按tag号查找
        if (!output_field) {
            output_field = output_desc->FindFieldByNumber(field_tag);
        }
        
        if (!output_field) {
            std::cerr << "Warning: Output field not found for input field: " 
                      << field_name << " (looking for: " << output_field_name << ")" 
                      << std::endl;
            continue;
        }
        
        // 执行转换
        bool success = false;
        if (is_primary_key || is_metadata || !convert_enabled) {
            // 直接复制字段
            success = CopyField(input, output, input_field, output_field);
        } else {
            // 序列化为bytes
            success = SerializeFieldToBytes(input, output, input_field, output_field);
        }
        
        if (!success) {
            std::cerr << "Failed to convert field: " << field_name << std::endl;
            return static_cast<int>(ConvertResult::ERROR_FIELD_TYPE_MISMATCH);
        }
    }
    
    return static_cast<int>(ConvertResult::SUCCESS);
}

} // namespace tcaplus
