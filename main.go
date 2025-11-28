package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// main 函数作为示例程序入口
// 实际使用时，可以将pulsar_client.go作为库导入到其他项目中
func main() {
	log.Println("Pulsar容灾容错测试项目")
	log.Println("运行测试请使用: go test -v ./...")
	
	// 示例：创建客户端（需要Pulsar服务器运行）
	if len(os.Args) > 1 && os.Args[1] == "example" {
		runExample()
	}
}

func runExample() {
	config := DefaultConfig()
	config.ServiceURL = "pulsar://localhost:6650"
	config.Topic = "example-topic"
	config.SubscriptionName = "example-subscription"
	config.MaxRetries = 3
	config.EnableAutoReconnect = true

	client, err := NewPulsarClient(config)
	if err != nil {
		log.Printf("创建客户端失败: %v", err)
		log.Println("请确保Pulsar服务器正在运行")
		return
	}
	defer client.Close()

	log.Println("客户端创建成功")

	// 设置信号处理
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 发送消息示例
	go func() {
		for i := 0; i < 5; i++ {
			select {
			case <-ctx.Done():
				return
			default:
				payload := []byte("Hello from example")
				if err := client.SendMessage(ctx, payload); err != nil {
					log.Printf("发送消息失败: %v", err)
				} else {
					log.Printf("消息发送成功: %d", i+1)
				}
				time.Sleep(time.Second * 2)
			}
		}
	}()

	// 接收消息示例
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			default:
				msg, err := client.ReceiveMessage(ctx)
				if err != nil {
					log.Printf("接收消息失败: %v", err)
					return
				}
				log.Printf("收到消息: %s", string(msg.Payload()))
				client.Acknowledge(msg)
			}
		}
	}()

	// 等待信号
	<-sigChan
	log.Println("程序退出")
}
