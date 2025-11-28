# Pulsar容灾容错测试项目

## 项目简介

本项目为主项目提供了针对Pulsar组件的容灾容错测试用例。Pulsar作为主项目与副项目之间的通信工具，其稳定性和容错能力对系统整体可靠性至关重要。

本项目包含：
- Pulsar客户端封装代码，内置容灾容错机制
- 12个全面的容灾容错测试用例
- 详细的测试文档和预期结果说明

## 功能特性

### 容灾容错能力

1. **自动重连机制**: 连接断开时自动检测并尝试重连
2. **消息重试机制**: 发送失败时按照配置进行重试
3. **超时控制**: 防止操作无限等待
4. **故障检测**: 及时检测连接和操作故障
5. **资源管理**: 正确管理连接和资源，防止泄漏
6. **并发安全**: 支持多goroutine并发操作
7. **错误处理**: 提供详细的错误信息便于排查

## 项目结构

```
.
├── go.mod                      # Go模块定义
├── pulsar_client.go           # Pulsar客户端封装（含容灾容错逻辑）
├── pulsar_client_test.go      # 测试用例文件
├── TEST_DOCUMENTATION.md      # 详细测试文档
└── README.md                  # 项目说明文档
```

## 快速开始

### 前置要求

- Go 1.21 或更高版本
- Apache Pulsar服务器（可选，部分测试可在无服务器环境下运行）

### 安装依赖

```bash
go mod download
```

### 运行测试

运行所有测试用例：

```bash
go test -v ./...
```

运行特定测试用例：

```bash
go test -v -run TestCase1_ConnectionFailureAndReconnect
```

运行并发测试（检测数据竞争）：

```bash
go test -race -v ./...
```

生成测试覆盖率报告：

```bash
go test -cover -coverprofile=coverage.out ./...
go tool cover -html=coverage.out
```

## 测试用例列表

1. **TestCase1_ConnectionFailureAndReconnect**: 连接失败和自动重连
2. **TestCase2_MessageSendRetry**: 消息发送失败重试机制
3. **TestCase3_NetworkInterruption**: 网络中断恢复
4. **TestCase4_ServerFailureRecovery**: 服务端故障恢复
5. **TestCase5_MessageAcknowledgment**: 消息确认机制
6. **TestCase6_TimeoutHandling**: 超时处理
7. **TestCase7_ConcurrentOperations**: 并发操作容错
8. **TestCase8_ConnectionPoolManagement**: 连接池管理
9. **TestCase9_FailoverMechanism**: 故障转移机制
10. **TestCase10_MessageLossDetection**: 消息丢失检测
11. **TestCase11_ResourceCleanup**: 资源清理
12. **TestCase12_ConfigurationValidation**: 配置验证

详细的测试用例说明和预期结果请参考 [TEST_DOCUMENTATION.md](./TEST_DOCUMENTATION.md)

## 使用示例

### 基本使用

```go
package main

import (
    "context"
    "log"
    "time"
)

func main() {
    // 创建配置
    config := DefaultConfig()
    config.ServiceURL = "pulsar://localhost:6650"
    config.Topic = "my-topic"
    config.MaxRetries = 3
    config.RetryBackoff = time.Second * 2
    config.EnableAutoReconnect = true

    // 创建客户端
    client, err := NewPulsarClient(config)
    if err != nil {
        log.Fatal(err)
    }
    defer client.Close()

    // 发送消息
    ctx := context.Background()
    err = client.SendMessage(ctx, []byte("Hello, Pulsar!"))
    if err != nil {
        log.Printf("发送失败: %v", err)
    }

    // 接收消息
    msg, err := client.ReceiveMessage(ctx)
    if err != nil {
        log.Printf("接收失败: %v", err)
        return
    }

    log.Printf("收到消息: %s", string(msg.Payload()))
    
    // 确认消息
    client.Acknowledge(msg)
}
```

### 配置选项

```go
type ClientConfig struct {
    ServiceURL          string        // Pulsar服务URL
    Topic               string        // 主题名称
    SubscriptionName    string        // 订阅名称
    MaxRetries          int           // 最大重试次数
    RetryBackoff        time.Duration // 重试退避时间
    ConnectionTimeout   time.Duration // 连接超时
    OperationTimeout    time.Duration // 操作超时
    ReconnectInterval   time.Duration // 重连间隔
    EnableRetry         bool          // 是否启用重试
    EnableAutoReconnect bool          // 是否启用自动重连
}
```

## 容灾容错场景覆盖

本测试套件覆盖了以下容灾容错场景：

- ✅ 连接失败和自动重连
- ✅ 消息发送失败重试
- ✅ 网络中断恢复
- ✅ 服务端故障恢复
- ✅ 消息确认和去重
- ✅ 操作超时处理
- ✅ 并发操作安全
- ✅ 连接池管理
- ✅ 故障转移
- ✅ 消息丢失检测
- ✅ 资源清理
- ✅ 配置验证

## 最佳实践

1. **配置重试参数**: 根据业务需求合理设置`MaxRetries`和`RetryBackoff`
2. **启用自动重连**: 生产环境建议启用`EnableAutoReconnect`
3. **设置超时时间**: 合理设置`ConnectionTimeout`和`OperationTimeout`，避免无限等待
4. **错误处理**: 始终检查错误并记录日志，便于问题排查
5. **资源清理**: 确保在程序退出前调用`Close()`方法
6. **并发安全**: 客户端实例是并发安全的，可以在多个goroutine中使用

## 注意事项

1. **Pulsar服务器**: 部分测试需要Pulsar服务器运行，如果服务器不可用，相关测试会被跳过
2. **网络环境**: 网络中断测试可能需要特殊环境或工具
3. **时间敏感**: 某些测试涉及超时，需要确保测试环境时间准确
4. **资源限制**: 并发测试可能受系统资源限制影响

## 贡献指南

欢迎提交Issue和Pull Request来改进本项目。

## 许可证

本项目采用MIT许可证。

## 相关文档

- [Apache Pulsar官方文档](https://pulsar.apache.org/docs/)
- [Pulsar Go客户端文档](https://pkg.go.dev/github.com/apache/pulsar-client-go)
- [测试文档](./TEST_DOCUMENTATION.md)
