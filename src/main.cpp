// main.cpp
// 使用示例和测试程序

#include <iostream>
#include <iomanip>
#include <string>

#include "proto_converter.h"
#include "reverse_converter.h"
#include "messages.pb.h"
#include "options.pb.h"

// 辅助函数：打印bytes内容为十六进制
void PrintBytes(const std::string& bytes, const std::string& label) {
    std::cout << label << " (size=" << bytes.size() << "): ";
    for (unsigned char c : bytes) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') 
                  << static_cast<int>(c) << " ";
    }
    std::cout << std::dec << std::endl;
}

// 测试基本转换功能
void TestBasicConversion() {
    std::cout << "\n========== Test Basic Conversion ==========" << std::endl;
    
    // 创建输入消息
    tcaplus::GenericTest input;
    input.set_ruid(12345);
    input.set_type(1);
    
    // 设置Metadata
    auto* meta = input.mutable_meta();
    meta->set_version(100);
    meta->set_lock_time(3600);
    meta->set_lock_id(999);
    
    // 设置TestValue
    auto* value1 = input.mutable_value1();
    value1->set_value(42);
    
    // 设置string字段
    input.set_value2("Hello, World!");
    
    std::cout << "Input message:" << std::endl;
    std::cout << "  ruid: " << input.ruid() << std::endl;
    std::cout << "  type: " << input.type() << std::endl;
    std::cout << "  meta.version: " << input.meta().version() << std::endl;
    std::cout << "  meta.lock_time: " << input.meta().lock_time() << std::endl;
    std::cout << "  meta.lock_id: " << input.meta().lock_id() << std::endl;
    std::cout << "  value1.value: " << input.value1().value() << std::endl;
    std::cout << "  value2: " << input.value2() << std::endl;
    
    // 创建输出消息
    tcaplus::GenericTestOutput output;
    
    // 执行转换
    int result = tcaplus::Convert(&input, &output);
    
    std::cout << "\nConversion result: " << result << std::endl;
    
    if (result == 0) {
        std::cout << "\nOutput message:" << std::endl;
        std::cout << "  ruid: " << output.ruid() << std::endl;
        std::cout << "  type: " << output.type() << std::endl;
        std::cout << "  meta.version: " << output.meta().version() << std::endl;
        std::cout << "  meta.lock_time: " << output.meta().lock_time() << std::endl;
        std::cout << "  meta.lock_id: " << output.meta().lock_id() << std::endl;
        
        // 打印bytes字段
        PrintBytes(output.data_4(), "  data_4");
        PrintBytes(output.data_5(), "  data_5");
        
        // 验证data_4可以反序列化回TestValue
        tcaplus::TestValue restored_value;
        if (restored_value.ParseFromString(output.data_4())) {
            std::cout << "\n  Restored value1.value from data_4: " 
                      << restored_value.value() << std::endl;
        }
        
        // data_5直接就是字符串内容
        std::cout << "  Restored value2 from data_5: " << output.data_5() << std::endl;
    }
}

// 测试获取主键
void TestGetPrimaryKeys() {
    std::cout << "\n========== Test Get Primary Keys ==========" << std::endl;
    
    tcaplus::GenericTest msg;
    auto keys = tcaplus::ProtoConverter::GetPrimaryKeys(&msg);
    
    std::cout << "Primary keys: ";
    for (const auto& key : keys) {
        std::cout << key << " ";
    }
    std::cout << std::endl;
}

// 测试检查转换选项
void TestIsConvertEnabled() {
    std::cout << "\n========== Test Is Convert Enabled ==========" << std::endl;
    
    tcaplus::GenericTest msg;
    bool enabled = tcaplus::ProtoConverter::IsConvertEnabled(&msg);
    
    std::cout << "Convert enabled: " << (enabled ? "true" : "false") << std::endl;
}

// 测试Metadata类型检测
void TestIsMetadataType() {
    std::cout << "\n========== Test Is Metadata Type ==========" << std::endl;
    
    tcaplus::GenericTest msg;
    const auto* desc = msg.GetDescriptor();
    
    for (int i = 0; i < desc->field_count(); ++i) {
        const auto* field = desc->field(i);
        bool is_meta = tcaplus::ProtoConverter::IsMetadataType(field);
        std::cout << "  Field '" << field->name() << "' is Metadata: " 
                  << (is_meta ? "true" : "false") << std::endl;
    }
}

// 测试空消息
void TestEmptyMessage() {
    std::cout << "\n========== Test Empty Message ==========" << std::endl;
    
    tcaplus::GenericTest input;
    tcaplus::GenericTestOutput output;
    
    // 只设置主键字段
    input.set_ruid(1);
    input.set_type(2);
    
    int result = tcaplus::Convert(&input, &output);
    
    std::cout << "Conversion result: " << result << std::endl;
    std::cout << "Output ruid: " << output.ruid() << std::endl;
    std::cout << "Output type: " << output.type() << std::endl;
    std::cout << "Output data_4 size: " << output.data_4().size() << std::endl;
    std::cout << "Output data_5 size: " << output.data_5().size() << std::endl;
}

// 测试错误情况
void TestErrorCases() {
    std::cout << "\n========== Test Error Cases ==========" << std::endl;
    
    tcaplus::GenericTest input;
    tcaplus::GenericTestOutput output;
    
    // 测试null输入
    int result1 = tcaplus::Convert(nullptr, &output);
    std::cout << "Null input result: " << result1 << " (expected: -1)" << std::endl;
    
    // 测试null输出
    int result2 = tcaplus::Convert(&input, nullptr);
    std::cout << "Null output result: " << result2 << " (expected: -2)" << std::endl;
}

// 测试逆向转换（从Output转回Input）
void TestReverseConversion() {
    std::cout << "\n========== Test Reverse Conversion ==========" << std::endl;
    
    // 首先创建一个原始消息并转换
    tcaplus::GenericTest original;
    original.set_ruid(99999);
    original.set_type(88);
    original.mutable_meta()->set_version(200);
    original.mutable_meta()->set_lock_time(7200);
    original.mutable_meta()->set_lock_id(12345);
    original.mutable_value1()->set_value(100);
    original.set_value2("Test Reverse");
    
    std::cout << "Original message:" << std::endl;
    std::cout << "  ruid: " << original.ruid() << std::endl;
    std::cout << "  type: " << original.type() << std::endl;
    std::cout << "  meta.version: " << original.meta().version() << std::endl;
    std::cout << "  value1.value: " << original.value1().value() << std::endl;
    std::cout << "  value2: " << original.value2() << std::endl;
    
    // 正向转换
    tcaplus::GenericTestOutput intermediate;
    int forward_result = tcaplus::Convert(&original, &intermediate);
    std::cout << "\nForward conversion result: " << forward_result << std::endl;
    
    // 逆向转换
    tcaplus::GenericTest restored;
    int reverse_result = tcaplus::ReverseConvert(&intermediate, &restored);
    std::cout << "Reverse conversion result: " << reverse_result << std::endl;
    
    // 验证结果
    std::cout << "\nRestored message:" << std::endl;
    std::cout << "  ruid: " << restored.ruid() 
              << (restored.ruid() == original.ruid() ? " ✓" : " ✗") << std::endl;
    std::cout << "  type: " << restored.type() 
              << (restored.type() == original.type() ? " ✓" : " ✗") << std::endl;
    std::cout << "  meta.version: " << restored.meta().version() 
              << (restored.meta().version() == original.meta().version() ? " ✓" : " ✗") << std::endl;
    std::cout << "  value1.value: " << restored.value1().value() 
              << (restored.value1().value() == original.value1().value() ? " ✓" : " ✗") << std::endl;
    std::cout << "  value2: " << restored.value2() 
              << (restored.value2() == original.value2() ? " ✓" : " ✗") << std::endl;
    
    // 完整性检查
    bool all_match = (restored.ruid() == original.ruid() &&
                      restored.type() == original.type() &&
                      restored.meta().version() == original.meta().version() &&
                      restored.value1().value() == original.value1().value() &&
                      restored.value2() == original.value2());
    
    std::cout << "\nRoundtrip test: " << (all_match ? "PASSED" : "FAILED") << std::endl;
}

int main() {
    std::cout << "Proto Message Converter Test" << std::endl;
    std::cout << "=============================" << std::endl;
    
    // 运行所有测试
    TestGetPrimaryKeys();
    TestIsConvertEnabled();
    TestIsMetadataType();
    TestBasicConversion();
    TestEmptyMessage();
    TestErrorCases();
    TestReverseConversion();
    
    std::cout << "\n=============================" << std::endl;
    std::cout << "All tests completed!" << std::endl;
    
    return 0;
}
