package main

import (
	"context"
	"fmt"
	"log"
	"math/rand"
	"strconv"
	"sync"
	"time"

	"github.com/apache/pulsar-client-go/pulsar"
)

// PulsarClient 封装Pulsar客户端，提供容灾容错能力
type PulsarClient struct {
	client      pulsar.Client
	producer    pulsar.Producer
	consumer    pulsar.Consumer
	config      *ClientConfig
	mu          sync.RWMutex
	isConnected bool
	reconnectCh chan struct{}
	ctx         context.Context
	cancel      context.CancelFunc
	wg          sync.WaitGroup
}

// ClientConfig Pulsar客户端配置
type ClientConfig struct {
	ServiceURL       string
	Topic            string
	SubscriptionName string
	// 容灾容错配置
	MaxRetries          int           // 最大重试次数
	RetryBackoff        time.Duration // 重试退避时间
	ConnectionTimeout   time.Duration // 连接超时
	OperationTimeout    time.Duration // 操作超时
	ReconnectInterval   time.Duration // 重连间隔
	EnableRetry         bool          // 是否启用重试
	EnableAutoReconnect bool          // 是否启用自动重连
}

// DefaultConfig 返回默认配置
func DefaultConfig() *ClientConfig {
	return &ClientConfig{
		ServiceURL: "http://pulsar-rkrz2zpdnpv9.eap-jov4d79q.tdmq.ap-nj.internal.tencenttdmq.com:8080",
		// topic完整路径，格式为persistent://集群（租户）ID/命名空间/Topic名称
		Topic:               "persistent://pulsar-rkrz2zpdnpv9/user00_9_134_133_147/example-topic",
		SubscriptionName:    "test-subscription",
		MaxRetries:          3,
		RetryBackoff:        time.Second * 2,
		ConnectionTimeout:   time.Second * 10,
		OperationTimeout:    time.Second * 30,
		ReconnectInterval:   time.Second * 5,
		EnableRetry:         true,
		EnableAutoReconnect: true,
	}
}

// NewPulsarClient 创建新的Pulsar客户端
func NewPulsarClient(config *ClientConfig) (*PulsarClient, error) {
	if config == nil {
		config = DefaultConfig()
	}

	ctx, cancel := context.WithCancel(context.Background())

	clientOptions := pulsar.ClientOptions{
		URL:               config.ServiceURL,
		ConnectionTimeout: config.ConnectionTimeout,
		OperationTimeout:  config.OperationTimeout,
		Authentication:    pulsar.NewAuthenticationToken("eyJrZXlJZCI6InB1bHNhci1ya3J6MnpwZG5wdjkiLCJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJsZXRzZ28ifQ.Tr_GMjBqqaRJk5I_MvxejcvHCrgRCS2oLeu95Dn6eEI"),
	}

	client, err := pulsar.NewClient(clientOptions)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("failed to create pulsar client: %w", err)
	}

	pc := &PulsarClient{
		client:      client,
		config:      config,
		isConnected: true,
		reconnectCh: make(chan struct{}, 1),
		ctx:         ctx,
		cancel:      cancel,
	}
	log.Printf("pulsar client created successfully for service: %s", config.ServiceURL)

	// 创建生产者和消费者
	if err := pc.initProducer(); err != nil {
		client.Close()
		cancel()
		return nil, fmt.Errorf("failed to create producer: %w", err)
	}

	if err := pc.initConsumer(); err != nil {
		pc.producer.Close()
		client.Close()
		cancel()
		return nil, fmt.Errorf("failed to create consumer: %w", err)
	}

	// 启动自动重连监控
	if config.EnableAutoReconnect {
		pc.wg.Add(1)
		go pc.monitorConnection()
	}

	return pc, nil
}

// initProducer 初始化生产者
func (pc *PulsarClient) initProducer() error {
	producerOptions := pulsar.ProducerOptions{
		Topic: pc.config.Topic,
	}

	producer, err := pc.client.CreateProducer(producerOptions)
	if err != nil {
		return err
	}

	pc.mu.Lock()
	pc.producer = producer
	pc.mu.Unlock()
	log.Printf("producer created successfully for topic: %s", pc.config.Topic)

	return nil
}

// GetProducer 获取生产者
func (pc *PulsarClient) GetProducer() pulsar.Producer {
	pc.mu.RLock()
	producer := pc.producer
	pc.mu.RUnlock()
	return producer
}

// initConsumer 初始化消费者
func (pc *PulsarClient) initConsumer() error {
	dlqPolicy := &pulsar.DLQPolicy{
		MaxDeliveries:    3,
		DeadLetterTopic:  "persistent://pulsar-rkrz2zpdnpv9/user00_9_134_133_147/example-topic-test-subscription-DLQ",
		RetryLetterTopic: "persistent://pulsar-rkrz2zpdnpv9/user00_9_134_133_147/example-topic-test-subscription-RETRY",
	}
	consumerOptions := pulsar.ConsumerOptions{
		Topic:               pc.config.Topic,
		SubscriptionName:    pc.config.SubscriptionName,
		Type:                pulsar.Shared,
		RetryEnable:         true,
		NackRedeliveryDelay: 1 * time.Second,
		DLQ:                 dlqPolicy,
		//Type: pulsar.KeyShared, // key共享模式
	}

	consumer, err := pc.client.Subscribe(consumerOptions)
	if err != nil {
		return err
	}

	pc.mu.Lock()
	pc.consumer = consumer
	pc.mu.Unlock()
	log.Printf("consumer created successfully for topic: %s", pc.config.Topic)

	return nil
}

// GetConsumer 获取消费者
func (pc *PulsarClient) GetConsumer() pulsar.Consumer {
	pc.mu.RLock()
	consumer := pc.consumer
	pc.mu.RUnlock()
	return consumer
}

// SendMessage 发送消息，带重试机制
func (pc *PulsarClient) SendMessage(ctx context.Context, payload []byte) error {
	pc.mu.RLock()
	producer := pc.producer
	isConnected := pc.isConnected
	pc.mu.RUnlock()

	if !isConnected {
		return fmt.Errorf("client is not connected")
	}

	var lastErr error
	maxRetries := pc.config.MaxRetries
	if !pc.config.EnableRetry {
		maxRetries = 0
	}

	seq := rand.Intn(101)

	for attempt := 0; attempt <= maxRetries; attempt++ {
		if attempt > 0 {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(pc.config.RetryBackoff * time.Duration(attempt)):
			}
		}

		msg := &pulsar.ProducerMessage{
			Key:     strconv.Itoa(int(seq)),
			Payload: payload,
		}

		_, err := producer.Send(ctx, msg)
		if err == nil {
			return nil
		}

		lastErr = err
		log.Printf("SendMessage attempt %d failed: %v", attempt+1, err)

		// 检查是否需要重连
		if pc.shouldReconnect(err) {
			pc.triggerReconnect()
		}
	}

	return fmt.Errorf("failed to send message after %d attempts: %w", maxRetries+1, lastErr)
}

// ReceiveMessage 接收消息，带超时
func (pc *PulsarClient) ReceiveMessage(ctx context.Context) (pulsar.Message, error) {
	pc.mu.RLock()
	consumer := pc.consumer
	isConnected := pc.isConnected
	pc.mu.RUnlock()

	if !isConnected {
		return nil, fmt.Errorf("client is not connected")
	}

	msg, err := consumer.Receive(ctx)
	if err != nil {
		if pc.shouldReconnect(err) {
			pc.triggerReconnect()
		}
		return nil, err
	}
	log.Printf("Received message: key:%s,content:%s", msg.Key(), string(msg.Payload()))

	return msg, nil
}

// Acknowledge 确认消息
func (pc *PulsarClient) Acknowledge(msg pulsar.Message) error {
	pc.mu.RLock()
	consumer := pc.consumer
	pc.mu.RUnlock()

	if consumer == nil {
		return fmt.Errorf("consumer is not initialized")
	}

	return consumer.Ack(msg)
}

// shouldReconnect 判断是否需要重连
func (pc *PulsarClient) shouldReconnect(err error) bool {
	if !pc.config.EnableAutoReconnect {
		return false
	}

	// 检查是否是连接相关错误
	errStr := err.Error()
	reconnectErrors := []string{
		"connection closed",
		"connection refused",
		"timeout",
		"network error",
		"EOF",
	}

	for _, reconnectErr := range reconnectErrors {
		if contains(errStr, reconnectErr) {
			return true
		}
	}

	return false
}

// triggerReconnect 触发重连
func (pc *PulsarClient) triggerReconnect() {
	select {
	case pc.reconnectCh <- struct{}{}:
	default:
	}
}

// monitorConnection 监控连接状态并自动重连
func (pc *PulsarClient) monitorConnection() {
	defer pc.wg.Done()

	ticker := time.NewTicker(pc.config.ReconnectInterval)
	defer ticker.Stop()

	for {
		select {
		case <-pc.ctx.Done():
			return
		case <-pc.reconnectCh:
			pc.reconnect()
		case <-ticker.C:
			pc.checkConnection()
		}
	}
}

// checkConnection 检查连接状态
func (pc *PulsarClient) checkConnection() {
	pc.mu.RLock()
	producer := pc.producer
	pc.mu.RUnlock()

	if producer == nil {
		return
	}

	// 这里可以通过检查producer的状态来判断连接是否正常
	// 简化处理：如果producer存在，认为连接正常
	pc.mu.Lock()
	pc.isConnected = (producer != nil)
	pc.mu.Unlock()
}

// reconnect 执行重连
func (pc *PulsarClient) reconnect() {
	log.Println("Attempting to reconnect...")

	pc.mu.Lock()
	pc.isConnected = false

	// 关闭旧的producer和consumer
	if pc.producer != nil {
		pc.producer.Close()
		pc.producer = nil
	}
	if pc.consumer != nil {
		pc.consumer.Close()
		pc.consumer = nil
	}
	pc.mu.Unlock()

	// 重试连接
	maxReconnectAttempts := 5
	for attempt := 0; attempt < maxReconnectAttempts; attempt++ {
		select {
		case <-pc.ctx.Done():
			return
		default:
		}

		if err := pc.initProducer(); err != nil {
			log.Printf("Reconnect attempt %d failed (producer): %v", attempt+1, err)
			time.Sleep(pc.config.ReconnectInterval)
			continue
		}

		if err := pc.initConsumer(); err != nil {
			log.Printf("Reconnect attempt %d failed (consumer): %v", attempt+1, err)
			pc.mu.Lock()
			if pc.producer != nil {
				pc.producer.Close()
				pc.producer = nil
			}
			pc.mu.Unlock()
			time.Sleep(pc.config.ReconnectInterval)
			continue
		}

		pc.mu.Lock()
		pc.isConnected = true
		pc.mu.Unlock()

		log.Println("Reconnected successfully")
		return
	}

	log.Println("Failed to reconnect after all attempts")
}

// Close 关闭客户端
func (pc *PulsarClient) Close() error {
	pc.cancel()

	pc.mu.Lock()
	defer pc.mu.Unlock()

	if pc.producer != nil {
		pc.producer.Close()
		pc.producer = nil
	}

	if pc.consumer != nil {
		pc.consumer.Close()
		pc.consumer = nil
	}

	if pc.client != nil {
		pc.client.Close()
		pc.client = nil
	}

	pc.wg.Wait()

	return nil
}

// IsConnected 检查是否已连接
func (pc *PulsarClient) IsConnected() bool {
	pc.mu.RLock()
	defer pc.mu.RUnlock()
	return pc.isConnected
}

// contains 检查字符串是否包含子串（忽略大小写）
func contains(s, substr string) bool {
	return len(s) >= len(substr) && (s == substr || len(substr) == 0 ||
		containsIgnoreCase(s, substr))
}

func containsIgnoreCase(s, substr string) bool {
	if len(s) < len(substr) {
		return false
	}
	for i := 0; i <= len(s)-len(substr); i++ {
		match := true
		for j := 0; j < len(substr); j++ {
			if toLower(s[i+j]) != toLower(substr[j]) {
				match = false
				break
			}
		}
		if match {
			return true
		}
	}
	return false
}

func toLower(b byte) byte {
	if b >= 'A' && b <= 'Z' {
		return b + ('a' - 'A')
	}
	return b
}
