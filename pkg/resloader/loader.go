// Package resloader 提供资源加载和管理功能
package resloader

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"

	"google.golang.org/protobuf/proto"
)

// ResourceLoader 资源加载器接口
type ResourceLoader interface {
	// Load 从文件加载资源
	Load(path string) error
	// LoadFromBytes 从字节数据加载资源
	LoadFromBytes(data []byte) error
	// Name 获取资源名称
	Name() string
}

// Manager 资源管理器
type Manager struct {
	mu       sync.RWMutex
	loaders  map[string]ResourceLoader
	basePath string
}

// NewManager 创建资源管理器
func NewManager(basePath string) *Manager {
	return &Manager{
		loaders:  make(map[string]ResourceLoader),
		basePath: basePath,
	}
}

// Register 注册资源加载器
func (m *Manager) Register(loader ResourceLoader) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.loaders[loader.Name()] = loader
}

// LoadAll 加载所有已注册的资源
func (m *Manager) LoadAll() error {
	m.mu.RLock()
	defer m.mu.RUnlock()

	for name, loader := range m.loaders {
		path := filepath.Join(m.basePath, name+".bin")
		if err := loader.Load(path); err != nil {
			return fmt.Errorf("failed to load resource %s: %w", name, err)
		}
	}
	return nil
}

// Load 加载指定资源
func (m *Manager) Load(name string) error {
	m.mu.RLock()
	loader, ok := m.loaders[name]
	m.mu.RUnlock()

	if !ok {
		return fmt.Errorf("resource loader %s not found", name)
	}

	path := filepath.Join(m.basePath, name+".bin")
	return loader.Load(path)
}

// ReadBinaryFile 读取二进制资源文件
func ReadBinaryFile[T proto.Message](path string, factory func() T) ([]T, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("failed to read file: %w", err)
	}
	return ParseBinaryData(data, factory)
}

// ParseBinaryData 解析二进制资源数据
func ParseBinaryData[T proto.Message](data []byte, factory func() T) ([]T, error) {
	if len(data) < 4 {
		return nil, fmt.Errorf("invalid data: too short")
	}

	count := binary.LittleEndian.Uint32(data[:4])
	offset := 4

	items := make([]T, 0, count)

	for i := uint32(0); i < count; i++ {
		if offset+4 > len(data) {
			return nil, io.ErrUnexpectedEOF
		}
		length := binary.LittleEndian.Uint32(data[offset : offset+4])
		offset += 4

		if offset+int(length) > len(data) {
			return nil, io.ErrUnexpectedEOF
		}

		item := factory()
		if err := proto.Unmarshal(data[offset:offset+int(length)], item); err != nil {
			return nil, fmt.Errorf("failed to unmarshal item %d: %w", i, err)
		}
		offset += int(length)

		items = append(items, item)
	}

	return items, nil
}

// WriteBinaryFile 写入二进制资源文件
func WriteBinaryFile[T proto.Message](path string, items []T) error {
	data, err := MarshalBinaryData(items)
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0644)
}

// MarshalBinaryData 序列化为二进制资源数据
func MarshalBinaryData[T proto.Message](items []T) ([]byte, error) {
	data := make([]byte, 0)

	// 写入消息数量
	count := uint32(len(items))
	countBytes := make([]byte, 4)
	binary.LittleEndian.PutUint32(countBytes, count)
	data = append(data, countBytes...)

	for _, item := range items {
		msgData, err := proto.Marshal(item)
		if err != nil {
			return nil, fmt.Errorf("failed to marshal message: %w", err)
		}

		// 写入消息长度
		lengthBytes := make([]byte, 4)
		binary.LittleEndian.PutUint32(lengthBytes, uint32(len(msgData)))
		data = append(data, lengthBytes...)

		// 写入消息数据
		data = append(data, msgData...)
	}

	return data, nil
}
