// reverse_converter.cpp
// 逆向转换器实现

#include "reverse_converter.h"
#include "proto_converter.h"

#include <google/protobuf/reflection.h>
#include <iostream>
#include <regex>

namespace tcaplus {

std::map<int, std::string> ReverseConverter::BuildFieldMapping(
    const ::google::protobuf::Descriptor* output_desc) {
    
    std::map<int, std::string> mapping;
    std::regex data_pattern("^data_(\\d+)$");
    
    for (int i = 0; i < output_desc->field_count(); ++i) {
        const auto* field = output_desc->field(i);
        std::string field_name = field->name();
        
        std::smatch match;
        if (std::regex_match(field_name, match, data_pattern)) {
            int tag = std::stoi(match[1].str());
            mapping[tag] = field_name;
        }
    }
    
    return mapping;
}

bool ReverseConverter::CopyField(
    const ::google::protobuf::Message* input,
    ::google::protobuf::Message* output,
    const ::google::protobuf::FieldDescriptor* input_field,
    const ::google::protobuf::FieldDescriptor* output_field) {
    
    if (!input || !output || !input_field || !output_field) {
        return false;
    }
    
    const auto* input_ref = input->GetReflection();
    auto* output_ref = output->GetReflection();
    
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
        case ::google::protobuf::FieldDescriptor::TYPE_BYTES:
            output_ref->SetString(output, output_field, 
                input_ref->GetString(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_ENUM:
            output_ref->SetEnumValue(output, output_field, 
                input_ref->GetEnumValue(*input, input_field));
            break;
            
        case ::google::protobuf::FieldDescriptor::TYPE_MESSAGE: {
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

bool ReverseConverter::DeserializeBytesToField(
    const ::google::protobuf::Message* input,
    ::google::protobuf::Message* output,
    const ::google::protobuf::FieldDescriptor* input_field,
    const ::google::protobuf::FieldDescriptor* output_field) {
    
    if (!input || !output || !input_field || !output_field) {
        return false;
    }
    
    // 确保输入字段是bytes类型
    if (input_field->type() != ::google::protobuf::FieldDescriptor::TYPE_BYTES) {
        std::cerr << "Input field " << input_field->name() 
                  << " is not bytes type" << std::endl;
        return false;
    }
    
    const auto* input_ref = input->GetReflection();
    auto* output_ref = output->GetReflection();
    
    std::string bytes_data = input_ref->GetString(*input, input_field);
    
    // 如果bytes为空，跳过
    if (bytes_data.empty()) {
        return true;
    }
    
    // 根据输出字段类型进行反序列化
    switch (output_field->type()) {
        case ::google::protobuf::FieldDescriptor::TYPE_MESSAGE: {
            auto* msg = output_ref->MutableMessage(output, output_field);
            if (!msg->ParseFromString(bytes_data)) {
                std::cerr << "Failed to parse message from bytes: " 
                          << output_field->name() << std::endl;
                return false;
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_STRING: {
            output_ref->SetString(output, output_field, bytes_data);
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_BYTES: {
            output_ref->SetString(output, output_field, bytes_data);
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_INT32:
        case ::google::protobuf::FieldDescriptor::TYPE_SFIXED32:
        case ::google::protobuf::FieldDescriptor::TYPE_SINT32: {
            if (bytes_data.size() >= sizeof(int32_t)) {
                int32_t value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetInt32(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_INT64:
        case ::google::protobuf::FieldDescriptor::TYPE_SFIXED64:
        case ::google::protobuf::FieldDescriptor::TYPE_SINT64: {
            if (bytes_data.size() >= sizeof(int64_t)) {
                int64_t value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetInt64(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_UINT32:
        case ::google::protobuf::FieldDescriptor::TYPE_FIXED32: {
            if (bytes_data.size() >= sizeof(uint32_t)) {
                uint32_t value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetUInt32(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_UINT64:
        case ::google::protobuf::FieldDescriptor::TYPE_FIXED64: {
            if (bytes_data.size() >= sizeof(uint64_t)) {
                uint64_t value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetUInt64(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_DOUBLE: {
            if (bytes_data.size() >= sizeof(double)) {
                double value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetDouble(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_FLOAT: {
            if (bytes_data.size() >= sizeof(float)) {
                float value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetFloat(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_BOOL: {
            if (bytes_data.size() >= sizeof(bool)) {
                bool value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetBool(output, output_field, value);
            }
            break;
        }
        
        case ::google::protobuf::FieldDescriptor::TYPE_ENUM: {
            if (bytes_data.size() >= sizeof(int)) {
                int value;
                memcpy(&value, bytes_data.data(), sizeof(value));
                output_ref->SetEnumValue(output, output_field, value);
            }
            break;
        }
        
        default:
            std::cerr << "Unsupported field type for deserialization: " 
                      << output_field->type_name() << std::endl;
            return false;
    }
    
    return true;
}

int ReverseConverter::ReverseConvert(
    const ::google::protobuf::Message* input,
    ::google::protobuf::Message* output) {
    
    if (!input) {
        return -1;
    }
    if (!output) {
        return -2;
    }
    
    const auto* input_desc = input->GetDescriptor();
    const auto* output_desc = output->GetDescriptor();
    
    // 获取输出消息的主键和转换配置（用于判断哪些字段需要反序列化）
    std::set<std::string> primary_keys = ProtoConverter::GetPrimaryKeys(output);
    bool convert_enabled = ProtoConverter::IsConvertEnabled(output);
    
    // 遍历输入消息的所有字段
    for (int i = 0; i < input_desc->field_count(); ++i) {
        const auto* input_field = input_desc->field(i);
        const std::string& field_name = input_field->name();
        int field_tag = input_field->number();
        
        // 判断是否为 data_X 格式的字段
        bool is_data_field = (field_name.substr(0, 5) == "data_" && 
                              input_field->type() == ::google::protobuf::FieldDescriptor::TYPE_BYTES);
        
        // 在输出消息中查找对应字段
        const ::google::protobuf::FieldDescriptor* output_field = nullptr;
        
        if (is_data_field) {
            // data_X 字段：按tag号查找
            output_field = output_desc->FindFieldByNumber(field_tag);
        } else {
            // 普通字段：先按名称查找，再按tag号查找
            output_field = output_desc->FindFieldByName(field_name);
            if (!output_field) {
                output_field = output_desc->FindFieldByNumber(field_tag);
            }
        }
        
        if (!output_field) {
            std::cerr << "Warning: Output field not found for: " 
                      << field_name << std::endl;
            continue;
        }
        
        // 执行转换
        bool success = false;
        if (is_data_field && convert_enabled) {
            // bytes字段需要反序列化
            success = DeserializeBytesToField(input, output, input_field, output_field);
        } else {
            // 直接复制
            success = CopyField(input, output, input_field, output_field);
        }
        
        if (!success) {
            std::cerr << "Failed to reverse convert field: " << field_name << std::endl;
            return -5;
        }
    }
    
    return 0;
}

} // namespace tcaplus
