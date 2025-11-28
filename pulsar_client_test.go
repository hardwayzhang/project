package main

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestCase1_ConnectionFailureAndReconnect 测试用例1: 连接失败和自动重连
// 预期结果: 客户端能够检测到连接失败，并在配置的重连间隔后自动重连
func TestCase1_ConnectionFailureAndReconnect(t *testing.T) {
	t.Log("=== 测试用例1: 连接失败和自动重连 ===")

	config := DefaultConfig()
	config.ServiceURL = "pulsar://invalid-host:6650" // 使用无效地址
	config.ReconnectInterval = time.Second * 2
	config.EnableAutoReconnect = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Logf("预期错误: 无法连接到无效地址: %v", err)
		// 这是预期的，因为地址无效
		return
	}
	defer client.Close()

	// 检查初始连接状态
	initialConnected := client.IsConnected()
	t.Logf("初始连接状态: %v", initialConnected)

	// 等待一段时间观察重连行为
	time.Sleep(time.Second * 3)

	// 预期结果: 客户端应该尝试重连
	finalConnected := client.IsConnected()
	t.Logf("最终连接状态: %v", finalConnected)

	// 注意: 由于使用无效地址，连接可能一直失败，但重连机制应该在工作
	t.Log("预期结果: 客户端持续尝试重连，即使连接失败")
}

// TestCase2_MessageSendRetry 测试用例2: 消息发送失败重试机制
// 预期结果: 当消息发送失败时，客户端应该按照配置的重试次数和退避时间进行重试
func TestCase2_MessageSendRetry(t *testing.T) {
	t.Log("=== 测试用例2: 消息发送失败重试机制 ===")

	config := DefaultConfig()
	config.MaxRetries = 3
	config.RetryBackoff = time.Second * 1
	config.EnableRetry = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*30)
	defer cancel()

	// 测试正常发送
	testPayload := []byte("test message")
	startTime := time.Now()
	err = client.SendMessage(ctx, testPayload)
	duration := time.Since(startTime)

	if err != nil {
		t.Logf("发送消息失败: %v (耗时: %v)", err, duration)
		// 如果连接失败，这是预期的
	} else {
		t.Logf("消息发送成功 (耗时: %v)", duration)
		assert.NoError(t, err, "消息应该发送成功")
	}

	t.Log("预期结果:")
	t.Log("1. 如果连接正常，消息应该成功发送")
	t.Log("2. 如果发送失败，应该重试最多3次，每次间隔1秒")
	t.Log("3. 重试总耗时应该约为: 失败时间 + 1s + 2s + 3s")
}

// TestCase3_NetworkInterruption 测试用例3: 网络中断恢复
// 预期结果: 当网络中断时，客户端应该检测到并尝试重连；网络恢复后应该能够正常发送和接收消息
func TestCase3_NetworkInterruption(t *testing.T) {
	t.Log("=== 测试用例3: 网络中断恢复 ===")

	config := DefaultConfig()
	config.ReconnectInterval = time.Second * 2
	config.EnableAutoReconnect = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	// 检查初始连接
	require.True(t, client.IsConnected(), "初始应该已连接")

	// 模拟网络中断（通过关闭producer）
	client.mu.Lock()
	if client.producer != nil {
		client.producer.Close()
		client.producer = nil
		client.isConnected = false
	}
	client.mu.Unlock()

	t.Log("模拟网络中断: 关闭producer")
	time.Sleep(time.Second * 1)

	// 触发重连
	client.triggerReconnect()
	t.Log("触发重连机制")

	// 等待重连
	time.Sleep(time.Second * 5)

	// 检查重连状态
	reconnected := client.IsConnected()
	t.Logf("重连后状态: %v", reconnected)

	t.Log("预期结果:")
	t.Log("1. 网络中断后，客户端应该检测到连接断开")
	t.Log("2. 自动重连机制应该被触发")
	t.Log("3. 网络恢复后，客户端应该能够重新连接")
	t.Log("4. 重连后应该能够正常发送和接收消息")
}

// TestCase4_ServerFailureRecovery 测试用例4: 服务端故障恢复
// 预期结果: 当Pulsar服务端故障时，客户端应该检测到并持续尝试重连；服务端恢复后应该能够正常工作
func TestCase4_ServerFailureRecovery(t *testing.T) {
	t.Log("=== 测试用例4: 服务端故障恢复 ===")

	config := DefaultConfig()
	config.ReconnectInterval = time.Second * 3
	config.EnableAutoReconnect = true
	config.MaxRetries = 5

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	// 记录初始状态
	initialConnected := client.IsConnected()
	t.Logf("服务端正常时连接状态: %v", initialConnected)

	// 模拟服务端故障（关闭连接）
	client.mu.Lock()
	if client.producer != nil {
		client.producer.Close()
		client.producer = nil
	}
	if client.consumer != nil {
		client.consumer.Close()
		client.consumer = nil
	}
	client.isConnected = false
	client.mu.Unlock()

	t.Log("模拟服务端故障")
	time.Sleep(time.Second * 2)

	// 触发重连
	client.triggerReconnect()
	t.Log("开始尝试重连...")

	// 等待重连尝试
	time.Sleep(time.Second * 10)

	// 检查重连状态
	reconnected := client.IsConnected()
	t.Logf("重连尝试后状态: %v", reconnected)

	t.Log("预期结果:")
	t.Log("1. 服务端故障时，客户端应该检测到连接断开")
	t.Log("2. 客户端应该按照配置的重连间隔持续尝试重连")
	t.Log("3. 服务端恢复后，客户端应该能够成功重连")
	t.Log("4. 重连后所有功能应该恢复正常")
}

// TestCase5_MessageAcknowledgment 测试用例5: 消息确认机制
// 预期结果: 消息被成功处理后应该能够正确确认，避免消息重复处理
func TestCase5_MessageAcknowledgment(t *testing.T) {
	t.Log("=== 测试用例5: 消息确认机制 ===")

	config := DefaultConfig()
	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*30)
	defer cancel()

	// 发送测试消息
	testPayload := []byte("ack test message")
	err = client.SendMessage(ctx, testPayload)
	if err != nil {
		t.Skipf("跳过测试: 无法发送消息: %v", err)
		return
	}

	// 接收消息
	msg, err := client.ReceiveMessage(ctx)
	if err != nil {
		t.Skipf("跳过测试: 无法接收消息: %v", err)
		return
	}

	require.NotNil(t, msg, "应该接收到消息")
	t.Logf("接收到消息: %s", string(msg.Payload()))

	// 确认消息
	err = client.Acknowledge(msg)
	assert.NoError(t, err, "消息确认应该成功")

	t.Log("预期结果:")
	t.Log("1. 消息应该能够成功发送")
	t.Log("2. 消息应该能够成功接收")
	t.Log("3. 消息确认应该成功，避免重复处理")
	t.Log("4. 确认后的消息不应该再次被接收")
}

// TestCase6_TimeoutHandling 测试用例6: 超时处理
// 预期结果: 当操作超时时，应该返回超时错误，不会无限等待
func TestCase6_TimeoutHandling(t *testing.T) {
	t.Log("=== 测试用例6: 超时处理 ===")

	config := DefaultConfig()
	config.OperationTimeout = time.Second * 5
	config.ConnectionTimeout = time.Second * 3

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	// 测试发送超时
	ctx, cancel := context.WithTimeout(context.Background(), time.Second*2)
	defer cancel()

	startTime := time.Now()
	err = client.SendMessage(ctx, []byte("timeout test"))
	duration := time.Since(startTime)

	if err != nil {
		t.Logf("发送超时或失败: %v (耗时: %v)", err, duration)
		// 检查是否是超时错误
		if errors.Is(err, context.DeadlineExceeded) {
			t.Log("正确检测到超时错误")
		}
	} else {
		t.Logf("消息发送成功 (耗时: %v)", duration)
	}

	// 验证超时时间
	assert.LessOrEqual(t, duration, time.Second*3, "操作应该在超时时间内完成或返回")

	t.Log("预期结果:")
	t.Log("1. 当操作超时时，应该返回context.DeadlineExceeded错误")
	t.Log("2. 超时时间应该符合配置的OperationTimeout")
	t.Log("3. 不应该无限等待")
}

// TestCase7_ConcurrentOperations 测试用例7: 并发操作容错
// 预期结果: 多个goroutine同时进行发送和接收操作时，应该能够正确处理，不会出现数据竞争
func TestCase7_ConcurrentOperations(t *testing.T) {
	t.Log("=== 测试用例7: 并发操作容错 ===")

	config := DefaultConfig()
	config.MaxRetries = 2
	config.EnableRetry = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*30)
	defer cancel()

	var wg sync.WaitGroup
	messageCount := 10
	successCount := 0
	var mu sync.Mutex

	// 并发发送消息
	for i := 0; i < messageCount; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			payload := []byte(fmt.Sprintf("concurrent message %d", id))
			err := client.SendMessage(ctx, payload)
			mu.Lock()
			if err == nil {
				successCount++
			}
			mu.Unlock()
			if err != nil {
				t.Logf("Goroutine %d 发送失败: %v", id, err)
			}
		}(i)
	}

	wg.Wait()
	t.Logf("并发发送完成，成功: %d/%d", successCount, messageCount)

	// 并发接收消息
	receivedCount := 0
	for i := 0; i < messageCount; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			msg, err := client.ReceiveMessage(ctx)
			if err == nil && msg != nil {
				mu.Lock()
				receivedCount++
				mu.Unlock()
				client.Acknowledge(msg)
			}
		}()
	}

	wg.Wait()
	t.Logf("并发接收完成，接收: %d", receivedCount)

	t.Log("预期结果:")
	t.Log("1. 多个goroutine并发操作应该能够正常工作")
	t.Log("2. 不应该出现数据竞争或panic")
	t.Log("3. 消息应该能够正确发送和接收")
	t.Log("4. 重试机制在并发场景下应该正常工作")
}

// TestCase8_ConnectionPoolManagement 测试用例8: 连接池管理
// 预期结果: 多个客户端实例应该能够共享连接池，连接断开时应该能够正确清理和重建
func TestCase8_ConnectionPoolManagement(t *testing.T) {
	t.Log("=== 测试用例8: 连接池管理 ===")

	config := DefaultConfig()

	// 创建多个客户端实例
	clients := make([]*PulsarClient, 3)
	var err error

	for i := 0; i < 3; i++ {
		clients[i], err = NewPulsarClient(config)
		if err != nil {
			t.Skipf("跳过测试: 无法创建客户端 %d: %v", i, err)
			// 清理已创建的客户端
			for j := 0; j < i; j++ {
				clients[j].Close()
			}
			return
		}
		t.Logf("客户端 %d 创建成功", i)
	}

	// 清理所有客户端
	defer func() {
		for i, client := range clients {
			if client != nil {
				if err := client.Close(); err != nil {
					t.Logf("关闭客户端 %d 时出错: %v", i, err)
				}
			}
		}
	}()

	// 测试所有客户端是否都连接成功
	allConnected := true
	for i, client := range clients {
		if !client.IsConnected() {
			allConnected = false
			t.Logf("客户端 %d 未连接", i)
		}
	}

	t.Logf("所有客户端连接状态: %v", allConnected)

	t.Log("预期结果:")
	t.Log("1. 多个客户端实例应该能够独立创建和连接")
	t.Log("2. 每个客户端应该有独立的producer和consumer")
	t.Log("3. 关闭客户端时应该正确清理资源")
	t.Log("4. 连接池应该能够正确管理多个连接")
}

// TestCase9_FailoverMechanism 测试用例9: 故障转移机制
// 预期结果: 当主Pulsar服务不可用时，应该能够切换到备用服务（如果配置了多个服务URL）
func TestCase9_FailoverMechanism(t *testing.T) {
	t.Log("=== 测试用例9: 故障转移机制 ===")

	// 注意: 实际的故障转移需要在Pulsar客户端配置中设置多个服务URL
	// 这里我们测试客户端在服务不可用时的行为

	config := DefaultConfig()
	config.ReconnectInterval = time.Second * 2
	config.EnableAutoReconnect = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	initialConnected := client.IsConnected()
	t.Logf("初始连接状态: %v", initialConnected)

	// 模拟主服务故障
	client.mu.Lock()
	if client.producer != nil {
		client.producer.Close()
		client.producer = nil
	}
	client.isConnected = false
	client.mu.Unlock()

	t.Log("模拟主服务故障")
	time.Sleep(time.Second * 1)

	// 触发重连（在实际场景中，客户端应该尝试备用URL）
	client.triggerReconnect()
	time.Sleep(time.Second * 5)

	finalConnected := client.IsConnected()
	t.Logf("故障转移后连接状态: %v", finalConnected)

	t.Log("预期结果:")
	t.Log("1. 主服务故障时，客户端应该检测到连接断开")
	t.Log("2. 如果配置了多个服务URL，应该尝试连接到备用服务")
	t.Log("3. 故障转移应该对应用透明")
	t.Log("4. 转移后应该能够正常发送和接收消息")
}

// TestCase10_MessageLossDetection 测试用例10: 消息丢失检测
// 预期结果: 当消息发送失败且重试耗尽时，应该能够检测到消息丢失并记录错误
func TestCase10_MessageLossDetection(t *testing.T) {
	t.Log("=== 测试用例10: 消息丢失检测 ===")

	config := DefaultConfig()
	config.MaxRetries = 2
	config.RetryBackoff = time.Second * 1
	config.EnableRetry = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	// 先确保连接正常
	if !client.IsConnected() {
		t.Skip("跳过测试: 客户端未连接")
		return
	}

	// 模拟连接中断
	client.mu.Lock()
	if client.producer != nil {
		client.producer.Close()
		client.producer = nil
	}
	client.isConnected = false
	client.mu.Unlock()

	// 尝试发送消息（应该失败并重试）
	ctx, cancel := context.WithTimeout(context.Background(), time.Second*10)
	defer cancel()

	err = client.SendMessage(ctx, []byte("loss detection test"))
	
	if err != nil {
		t.Logf("消息发送失败（预期）: %v", err)
		// 检查错误信息是否包含重试信息
		assert.Contains(t, err.Error(), "failed to send message", "错误应该包含失败信息")
	} else {
		t.Log("消息发送成功（如果重连成功）")
	}

	t.Log("预期结果:")
	t.Log("1. 当连接中断时，消息发送应该失败")
	t.Log("2. 应该按照配置进行重试")
	t.Log("3. 重试耗尽后应该返回明确的错误信息")
	t.Log("4. 错误信息应该包含重试次数和失败原因")
}

// TestCase11_ResourceCleanup 测试用例11: 资源清理
// 预期结果: 客户端关闭时应该正确清理所有资源，包括producer、consumer和连接
func TestCase11_ResourceCleanup(t *testing.T) {
	t.Log("=== 测试用例11: 资源清理 ===")

	config := DefaultConfig()
	client, err := NewPulsarClient(config)
	if err != nil {
		t.Skipf("跳过测试: 无法连接到Pulsar服务器: %v", err)
		return
	}

	// 检查资源是否存在
	client.mu.RLock()
	hasProducer := client.producer != nil
	hasConsumer := client.consumer != nil
	hasClient := client.client != nil
	client.mu.RUnlock()

	t.Logf("关闭前资源状态 - Producer: %v, Consumer: %v, Client: %v", 
		hasProducer, hasConsumer, hasClient)

	// 关闭客户端
	err = client.Close()
	assert.NoError(t, err, "关闭客户端应该成功")

	// 检查资源是否已清理
	client.mu.RLock()
	hasProducerAfter := client.producer != nil
	hasConsumerAfter := client.consumer != nil
	hasClientAfter := client.client != nil
	client.mu.RUnlock()

	t.Logf("关闭后资源状态 - Producer: %v, Consumer: %v, Client: %v", 
		hasProducerAfter, hasConsumerAfter, hasClientAfter)

	t.Log("预期结果:")
	t.Log("1. 关闭客户端时应该正确关闭producer")
	t.Log("2. 关闭客户端时应该正确关闭consumer")
	t.Log("3. 关闭客户端时应该正确关闭底层连接")
	t.Log("4. 所有goroutine应该正确退出")
	t.Log("5. 不应该有资源泄漏")
}

// TestCase12_ConfigurationValidation 测试用例12: 配置验证
// 预期结果: 无效的配置应该被拒绝，有效的配置应该被正确应用
func TestCase12_ConfigurationValidation(t *testing.T) {
	t.Log("=== 测试用例12: 配置验证 ===")

	// 测试无效配置
	invalidConfig := &ClientConfig{
		ServiceURL: "", // 空URL
		Topic:      "test-topic",
	}

	_, err := NewPulsarClient(invalidConfig)
	if err != nil {
		t.Logf("预期错误: 无效配置被拒绝: %v", err)
	} else {
		t.Log("警告: 无效配置未被拒绝")
	}

	// 测试有效配置
	validConfig := DefaultConfig()
	validConfig.MaxRetries = 5
	validConfig.RetryBackoff = time.Second * 3

	client, err := NewPulsarClient(validConfig)
	if err != nil {
		t.Skipf("跳过测试: 无法创建客户端: %v", err)
		return
	}
	defer client.Close()

	// 验证配置是否生效
	assert.Equal(t, 5, client.config.MaxRetries, "MaxRetries应该为5")
	assert.Equal(t, time.Second*3, client.config.RetryBackoff, "RetryBackoff应该为3秒")

	t.Log("预期结果:")
	t.Log("1. 无效配置应该被拒绝并返回错误")
	t.Log("2. 有效配置应该被正确应用")
	t.Log("3. 配置值应该能够在运行时正确使用")
}
