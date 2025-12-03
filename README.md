# Pulsar容灾容错测试项目

## 项目简介

本项目为主项目提供了针对Pulsar组件的容灾容错测试用例。Pulsar作为主项目与副项目之间的通信工具，其稳定性和容错能力对系统整体可靠性至关重要。


## 功能特性

### 容灾容错能力

## 项目结构

```
.
├── go.mod                      # Go模块定义
├── pulsar_client.go           # Pulsar客户端封装（含容灾容错逻辑）
├── pulsar_client_test.go      # 测试用例文件
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


## 测试用例列表

1. **TestCase1_ConnectionFailureAndReconnect**: 连接失败和自动重连
2. **TestCase2_NetworkInterruption**: 网络中断恢复
3. **TestCase3_ConcurrentOperations**: 并发操作容错
4. **TestCase4_ConcumeMessageRetry**: 消费消息重试
5. **TestCase5_MessageAcknowledgment**: 消费死信队列测试


## 相关文档

- [Apache Pulsar官方文档](https://pulsar.apache.org/docs/)
- [Pulsar Go客户端文档](https://pkg.go.dev/github.com/apache/pulsar-client-go)
- [测试文档](./TEST_DOCUMENTATION.md)
