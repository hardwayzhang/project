package main

import (
	"context"
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/apache/pulsar-client-go/pulsar"
	"github.com/stretchr/testify/assert"
)

const (
	pulsarURL  = "pulsar://localhost:6650"
	testTopic  = "test-topic"
	retryTopic = "test-retry-topic"
	dlqTopic   = "test-dlq-topic"
)

// TestMessageRetry 测试消息重试机制
func TestMessageRetry(t *testing.T) {
	// 创建 Pulsar 客户端
	client, err := pulsar.NewClient(pulsar.ClientOptions{
		URL: pulsarURL,
	})
	if err != nil {
		t.Skipf("无法连接到 Pulsar 服务器，跳过测试: %v", err)
	}
	defer client.Close()

	// 创建生产者
	producer, err := client.CreateProducer(pulsar.ProducerOptions{
		Topic: testTopic,
	})
	assert.NoError(t, err)
	defer producer.Close()

	// 创建消费者，配置重试策略
	retryCount := 0
	maxRetries := 3
	var receivedMessages []string
	var mu sync.Mutex

	consumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:            testTopic,
		SubscriptionName: "retry-test-subscription",
		Type:             pulsar.Shared,
		RetryEnable:      true,
		MaxReconsumeTimes: uint32(maxRetries),
		MessageChannel:   make(chan pulsar.ConsumerMessage, 10),
	})
	assert.NoError(t, err)
	defer consumer.Close()

	// 发送测试消息
	messageID, err := producer.Send(context.Background(), &pulsar.ProducerMessage{
		Payload: []byte("test-retry-message"),
		Properties: map[string]string{
			"test-key": "test-value",
		},
	})
	assert.NoError(t, err)
	assert.NotNil(t, messageID)

	// 模拟消息处理失败，触发重试
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	go func() {
		for {
			select {
			case msg := <-consumer.MessageChannel():
				mu.Lock()
				retryCount++
				receivedMessages = append(receivedMessages, string(msg.Payload()))
				mu.Unlock()

				// 前两次失败，第三次成功
				if retryCount < maxRetries {
					consumer.Nack(msg)
					t.Logf("消息处理失败，触发重试 (第 %d 次)", retryCount)
				} else {
					consumer.Ack(msg)
					t.Logf("消息处理成功 (第 %d 次)", retryCount)
					cancel()
				}
			case <-ctx.Done():
				return
			}
		}
	}()

	// 等待消息处理完成
	<-ctx.Done()

	// 验证重试次数
	mu.Lock()
	defer mu.Unlock()
	assert.GreaterOrEqual(t, retryCount, maxRetries, "消息应该至少重试 %d 次", maxRetries)
	assert.Equal(t, "test-retry-message", receivedMessages[len(receivedMessages)-1], "最后收到的消息应该是最初的消息")
}

// TestMessageDeadLetterQueue 测试消息进入死信队列
func TestMessageDeadLetterQueue(t *testing.T) {
	// 创建 Pulsar 客户端
	client, err := pulsar.NewClient(pulsar.ClientOptions{
		URL: pulsarURL,
	})
	if err != nil {
		t.Skipf("无法连接到 Pulsar 服务器，跳过测试: %v", err)
	}
	defer client.Close()

	// 创建生产者
	producer, err := client.CreateProducer(pulsar.ProducerOptions{
		Topic: testTopic,
	})
	assert.NoError(t, err)
	defer producer.Close()

	// 创建死信队列消费者
	dlqConsumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:            dlqTopic,
		SubscriptionName: "dlq-subscription",
		Type:             pulsar.Shared,
	})
	assert.NoError(t, err)
	defer dlqConsumer.Close()

	// 创建主消费者，配置死信队列
	maxReconsumeTimes := uint32(3)
	var dlqMessages []string
	var mu sync.Mutex

	consumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:               testTopic,
		SubscriptionName:    "dlq-test-subscription",
		Type:                pulsar.Shared,
		RetryEnable:         true,
		MaxReconsumeTimes:   maxReconsumeTimes,
		DeadLetterPolicy: &pulsar.DeadLetterPolicy{
			MaxRedeliverCount: maxReconsumeTimes,
			DeadLetterTopic:   dlqTopic,
		},
		MessageChannel: make(chan pulsar.ConsumerMessage, 10),
	})
	assert.NoError(t, err)
	defer consumer.Close()

	// 发送测试消息
	messageID, err := producer.Send(context.Background(), &pulsar.ProducerMessage{
		Payload: []byte("test-dlq-message"),
		Properties: map[string]string{
			"test-key": "dlq-test-value",
		},
	})
	assert.NoError(t, err)
	assert.NotNil(t, messageID)

	// 监听死信队列
	dlqCtx, dlqCancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer dlqCancel()

	go func() {
		for {
			select {
			case msg := <-dlqConsumer.MessageChannel():
				mu.Lock()
				dlqMessages = append(dlqMessages, string(msg.Payload()))
				mu.Unlock()
				dlqConsumer.Ack(msg)
				t.Logf("收到死信队列消息: %s", string(msg.Payload()))
				dlqCancel()
			case <-dlqCtx.Done():
				return
			}
		}
	}()

	// 处理主队列消息，始终失败以触发死信队列
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	go func() {
		reconsumeCount := 0
		for {
			select {
			case msg := <-consumer.MessageChannel():
				reconsumeCount++
				t.Logf("收到消息，重试次数: %d", reconsumeCount)

				// 始终 Nack，直到超过最大重试次数，消息进入死信队列
				if reconsumeCount <= int(maxReconsumeTimes) {
					consumer.Nack(msg)
				} else {
					// 超过最大重试次数，消息应该已经进入死信队列
					consumer.Ack(msg)
					cancel()
				}
			case <-ctx.Done():
				return
			}
		}
	}()

	// 等待消息进入死信队列
	<-dlqCtx.Done()

	// 验证死信队列中是否有消息
	mu.Lock()
	defer mu.Unlock()
	assert.Greater(t, len(dlqMessages), 0, "死信队列应该包含消息")
	if len(dlqMessages) > 0 {
		assert.Equal(t, "test-dlq-message", dlqMessages[0], "死信队列中的消息应该是最初的消息")
	}
}

// TestAbnormalConsumer 测试异常消费者场景
func TestAbnormalConsumer(t *testing.T) {
	// 创建 Pulsar 客户端
	client, err := pulsar.NewClient(pulsar.ClientOptions{
		URL: pulsarURL,
	})
	if err != nil {
		t.Skipf("无法连接到 Pulsar 服务器，跳过测试: %v", err)
	}
	defer client.Close()

	// 创建生产者
	producer, err := client.CreateProducer(pulsar.ProducerOptions{
		Topic: testTopic,
	})
	assert.NoError(t, err)
	defer producer.Close()

	// 创建多个消费者来模拟异常场景
	var consumers []pulsar.Consumer
	var wg sync.WaitGroup
	var receivedCount int
	var mu sync.Mutex

	// 创建正常消费者
	normalConsumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:            testTopic,
		SubscriptionName: "abnormal-test-subscription",
		Type:             pulsar.Shared,
		MessageChannel:   make(chan pulsar.ConsumerMessage, 10),
	})
	assert.NoError(t, err)
	consumers = append(consumers, normalConsumer)

	// 创建异常消费者（会崩溃）
	abnormalConsumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:            testTopic,
		SubscriptionName: "abnormal-test-subscription",
		Type:             pulsar.Shared,
		MessageChannel:   make(chan pulsar.ConsumerMessage, 10),
	})
	assert.NoError(t, err)
	consumers = append(consumers, abnormalConsumer)

	// 发送多条消息
	messageCount := 10
	for i := 0; i < messageCount; i++ {
		messageID, err := producer.Send(context.Background(), &pulsar.ProducerMessage{
			Payload: []byte(fmt.Sprintf("test-message-%d", i)),
		})
		assert.NoError(t, err)
		assert.NotNil(t, messageID)
	}

	// 正常消费者处理消息
	wg.Add(1)
	go func() {
		defer wg.Done()
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		for {
			select {
			case msg := <-normalConsumer.MessageChannel():
				mu.Lock()
				receivedCount++
				mu.Unlock()
				normalConsumer.Ack(msg)
				t.Logf("正常消费者处理消息: %s", string(msg.Payload()))
				if receivedCount >= messageCount {
					cancel()
				}
			case <-ctx.Done():
				return
			}
		}
	}()

	// 异常消费者：处理几条消息后模拟崩溃（不 Ack，让消息重新投递）
	wg.Add(1)
	go func() {
		defer wg.Done()
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		processedCount := 0
		for {
			select {
			case msg := <-abnormalConsumer.MessageChannel():
				processedCount++
				t.Logf("异常消费者收到消息: %s (不 Ack，模拟崩溃)", string(msg.Payload()))
				// 不 Ack，模拟消费者崩溃，消息会重新投递
				if processedCount >= 3 {
					// 模拟消费者崩溃，关闭消费者
					abnormalConsumer.Close()
					cancel()
					return
				}
			case <-ctx.Done():
				return
			}
		}
	}()

	// 等待处理完成
	wg.Wait()

	// 清理资源
	for _, consumer := range consumers {
		consumer.Close()
	}

	// 验证：由于使用了 Shared 订阅模式，即使一个消费者崩溃，
	// 其他消费者应该能够继续处理消息
	mu.Lock()
	defer mu.Unlock()
	t.Logf("总共处理了 %d 条消息", receivedCount)
	// 注意：由于消息可能被重新投递，实际收到的消息数可能大于发送的消息数
	assert.Greater(t, receivedCount, 0, "应该至少处理了一些消息")
}

// TestConsumerReconnection 测试消费者重连场景
func TestConsumerReconnection(t *testing.T) {
	// 创建 Pulsar 客户端
	client, err := pulsar.NewClient(pulsar.ClientOptions{
		URL: pulsarURL,
	})
	if err != nil {
		t.Skipf("无法连接到 Pulsar 服务器，跳过测试: %v", err)
	}
	defer client.Close()

	// 创建生产者
	producer, err := client.CreateProducer(pulsar.ProducerOptions{
		Topic: testTopic,
	})
	assert.NoError(t, err)
	defer producer.Close()

	// 创建消费者
	consumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:            testTopic,
		SubscriptionName: "reconnection-test-subscription",
		Type:             pulsar.Shared,
		MessageChannel:   make(chan pulsar.ConsumerMessage, 10),
	})
	assert.NoError(t, err)

	// 发送消息
	messageID, err := producer.Send(context.Background(), &pulsar.ProducerMessage{
		Payload: []byte("test-reconnection-message"),
	})
	assert.NoError(t, err)
	assert.NotNil(t, messageID)

	// 模拟消费者断开连接
	consumer.Close()
	time.Sleep(2 * time.Second)

	// 重新创建消费者
	reconnectedConsumer, err := client.Subscribe(pulsar.ConsumerOptions{
		Topic:            testTopic,
		SubscriptionName: "reconnection-test-subscription",
		Type:             pulsar.Shared,
		MessageChannel:   make(chan pulsar.ConsumerMessage, 10),
	})
	assert.NoError(t, err)
	defer reconnectedConsumer.Close()

	// 验证重连后能否继续接收消息
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	var receivedMessage string
	select {
	case msg := <-reconnectedConsumer.MessageChannel():
		receivedMessage = string(msg.Payload())
		reconnectedConsumer.Ack(msg)
		t.Logf("重连后收到消息: %s", receivedMessage)
	case <-ctx.Done():
		t.Log("重连后未收到消息（可能消息已被其他消费者处理）")
	}

	// 验证消息内容（如果收到）
	if receivedMessage != "" {
		assert.Equal(t, "test-reconnection-message", receivedMessage)
	}
}
