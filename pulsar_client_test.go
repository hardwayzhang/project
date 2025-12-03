package main

import (
	"context"
	"fmt"
	"log"
	"sync"
	"testing"
	"time"

	"github.com/apache/pulsar-client-go/pulsar"
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
	assert.Equal(t, initialConnected, finalConnected, "连接状态应该保持不变(未连接上)")
	t.Logf("最终连接状态: %v", finalConnected)
}

// TestCase2_NetworkInterruption 测试用例2: 网络中断恢复
// 预期结果: 当网络中断时，客户端应该检测到并尝试重连；网络恢复后应该能够正常发送和接收消息
func TestCase2_NetworkInterruption(t *testing.T) {
	t.Log("=== 测试用例2: 网络中断恢复 ===")

	config := DefaultConfig()
	config.ReconnectInterval = time.Second * 2
	config.EnableAutoReconnect = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Errorf("测试失败: 无法连接到Pulsar服务器: %v", err)
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
	assert.True(t, reconnected, "应该能够重新连接")

	t.Log("预期结果:")
	t.Log("1. 网络中断后，客户端应该检测到连接断开")
	t.Log("2. 自动重连机制应该被触发")
	t.Log("3. 网络恢复后，客户端应该能够重新连接")
	t.Log("4. 重连后应该能够正常发送和接收消息")
}

// TestCase3_ConcurrentOperations 测试用例3: 并发操作容错
// 预期结果: 多个goroutine同时进行发送和接收操作时，应该能够正确处理，不会出现数据竞争
func TestCase3_ConcurrentOperations(t *testing.T) {
	t.Log("=== 测试用例3: 并发操作容错 ===")

	config := DefaultConfig()
	config.MaxRetries = 2
	config.EnableRetry = true

	client, err := NewPulsarClient(config)
	if err != nil {
		t.Errorf("测试失败: 无法连接到Pulsar服务器: %v", err)
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

// TestCase4_ConcumeMessageRetry 测试用例4: 消费消息重试
// 预期结果: 消费消息失败时，应该根据配置重试，直到成功或达到最大重试次数
func TestCase4_ConcumeMessageRetry(t *testing.T) {
	t.Log("=== 测试用例4: 消费消息重试 ===")

	config := DefaultConfig()
	client, err := NewPulsarClient(config)
	if err != nil {
		t.Errorf("测试失败: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	var wg sync.WaitGroup

	// 发送消息示例
	wg.Add(1)
	go func() {
		defer wg.Done()
		// 发送测试消息
		messageID, err := client.GetProducer().Send(context.Background(), &pulsar.ProducerMessage{
			Payload: []byte("test-retry-message"),
			Properties: map[string]string{
				"test-key": "test-value",
			},
		})
		assert.NoError(t, err)
		assert.NotNil(t, messageID)
		log.Printf("消息发送成功: %v", messageID)
	}()

	// 消费消息并模拟重试
	// 模拟消息处理失败，触发重试
	retryCount := 0
	maxRetries := 3
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case <-ctx.Done():
				t.Errorf("测试失败: 超时")
				return
			default:
				msg, err := client.GetConsumer().Receive(context.Background())
				if err != nil {
					log.Printf("接收消息失败: %v", err)
					return
				}
				retryCount++
				// 前两次失败，第三次成功
				if retryCount < maxRetries {
					client.GetConsumer().Nack(msg)
					t.Logf("消息处理失败，触发重试 (第 %d 次)", retryCount)
				} else {
					client.GetConsumer().Ack(msg)
					t.Logf("消息处理成功 (第 %d 次)", retryCount)
					return
				}
			}
		}
	}()
	wg.Wait()
}

// TestCase5_AbnormalConcumer 测试用例5: 异常消费者场景
// 预期结果: 消息超时未ack投入到死信队列
func TestCase5_AbnormalConcumer(t *testing.T) {
	t.Log("=== 测试用例5: 异常消费者场景 ===")

	config := DefaultConfig()
	client, err := NewPulsarClient(config)
	if err != nil {
		t.Errorf("测试失败: 无法连接到Pulsar服务器: %v", err)
		return
	}
	defer client.Close()

	var wg sync.WaitGroup

	// 发送消息示例
	wg.Add(1)
	go func() {
		defer wg.Done()
		// 发送测试消息
		messageID, err := client.GetProducer().Send(context.Background(), &pulsar.ProducerMessage{
			Payload: []byte("test-case5-message"),
			Properties: map[string]string{
				"test-key": "test-value",
			},
		})
		assert.NoError(t, err)
		assert.NotNil(t, messageID)
		log.Printf("消息发送成功: %v", messageID)
	}()

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case <-ctx.Done():
				t.Errorf("测试失败: 超时")
				return
			default:
				msg, err := client.GetConsumer().Receive(context.Background())
				if err != nil {
					log.Printf("接收消息失败: %v", err)
					return
				}
				log.Printf("接收消息成功: %v", msg)
				client.GetConsumer().Nack(msg)
			}
		}
	}()
	wg.Wait()
}
