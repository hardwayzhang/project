// Package resloader 资源存储
package resloader

import (
	"fmt"
	"sync"

	"google.golang.org/protobuf/proto"
)

// Store 泛型资源存储
type Store[K comparable, T proto.Message] struct {
	mu           sync.RWMutex
	name         string
	items        []T
	primaryIndex *UniqueIndex[K, T]
	keyFunc      func(T) K
}

// NewStore 创建资源存储
func NewStore[K comparable, T proto.Message](name string, keyFunc func(T) K) *Store[K, T] {
	return &Store[K, T]{
		name:         name,
		items:        make([]T, 0),
		primaryIndex: NewUniqueIndex[K, T](),
		keyFunc:      keyFunc,
	}
}

// Name 获取资源名称
func (s *Store[K, T]) Name() string {
	return s.name
}

// Load 从文件加载资源
func (s *Store[K, T]) Load(path string) error {
	items, err := ReadBinaryFile(path, func() T {
		var zero T
		return proto.Clone(zero).(T)
	})
	if err != nil {
		return err
	}
	return s.SetItems(items)
}

// LoadFromBytes 从字节数据加载资源
func (s *Store[K, T]) LoadFromBytes(data []byte) error {
	items, err := ParseBinaryData(data, func() T {
		var zero T
		return proto.Clone(zero).(T)
	})
	if err != nil {
		return err
	}
	return s.SetItems(items)
}

// SetItems 设置资源项目
func (s *Store[K, T]) SetItems(items []T) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.items = items
	s.primaryIndex.Clear()

	for _, item := range items {
		key := s.keyFunc(item)
		s.primaryIndex.Set(key, item)
	}

	return nil
}

// Get 根据主键获取资源
func (s *Store[K, T]) Get(key K) (T, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.primaryIndex.Get(key)
}

// MustGet 根据主键获取资源,不存在时panic
func (s *Store[K, T]) MustGet(key K) T {
	item, ok := s.Get(key)
	if !ok {
		panic(fmt.Sprintf("resource %s with key %v not found", s.name, key))
	}
	return item
}

// Has 检查主键是否存在
func (s *Store[K, T]) Has(key K) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	_, ok := s.primaryIndex.Get(key)
	return ok
}

// GetAll 获取所有资源
func (s *Store[K, T]) GetAll() []T {
	s.mu.RLock()
	defer s.mu.RUnlock()
	result := make([]T, len(s.items))
	copy(result, s.items)
	return result
}

// Count 获取资源数量
func (s *Store[K, T]) Count() int {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.items)
}

// Range 遍历所有资源
func (s *Store[K, T]) Range(fn func(item T) bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, item := range s.items {
		if !fn(item) {
			break
		}
	}
}

// Filter 过滤资源
func (s *Store[K, T]) Filter(fn func(item T) bool) []T {
	s.mu.RLock()
	defer s.mu.RUnlock()
	var result []T
	for _, item := range s.items {
		if fn(item) {
			result = append(result, item)
		}
	}
	return result
}

// Find 查找第一个匹配的资源
func (s *Store[K, T]) Find(fn func(item T) bool) (T, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, item := range s.items {
		if fn(item) {
			return item, true
		}
	}
	var zero T
	return zero, false
}

// IndexedStore 带索引的资源存储
type IndexedStore[K comparable, T proto.Message] struct {
	*Store[K, T]
	uniqueIndexes map[string]interface{}
	multiIndexes  map[string]interface{}
}

// NewIndexedStore 创建带索引的资源存储
func NewIndexedStore[K comparable, T proto.Message](name string, keyFunc func(T) K) *IndexedStore[K, T] {
	return &IndexedStore[K, T]{
		Store:         NewStore(name, keyFunc),
		uniqueIndexes: make(map[string]interface{}),
		multiIndexes:  make(map[string]interface{}),
	}
}

// AddUniqueIndex 添加唯一索引
func AddUniqueIndex[K comparable, IK comparable, T proto.Message](
	s *IndexedStore[K, T],
	name string,
	indexFunc func(T) IK,
) *UniqueIndex[IK, T] {
	idx := NewUniqueIndex[IK, T]()
	s.uniqueIndexes[name] = &indexEntry[IK, T]{
		index:     idx,
		indexFunc: indexFunc,
	}
	return idx
}

// AddMultiIndex 添加多值索引
func AddMultiIndex[K comparable, IK comparable, T proto.Message](
	s *IndexedStore[K, T],
	name string,
	indexFunc func(T) IK,
) *MultiIndex[IK, T] {
	idx := NewMultiIndex[IK, T]()
	s.multiIndexes[name] = &multiIndexEntry[IK, T]{
		index:     idx,
		indexFunc: indexFunc,
	}
	return idx
}

type indexEntry[IK comparable, T any] struct {
	index     *UniqueIndex[IK, T]
	indexFunc func(T) IK
}

type multiIndexEntry[IK comparable, T any] struct {
	index     *MultiIndex[IK, T]
	indexFunc func(T) IK
}

// RebuildIndexes 重建索引
func (s *IndexedStore[K, T]) RebuildIndexes() {
	s.mu.Lock()
	defer s.mu.Unlock()

	// 清空并重建唯一索引
	for _, entry := range s.uniqueIndexes {
		switch e := entry.(type) {
		case *indexEntry[int32, T]:
			e.index.Clear()
			for _, item := range s.items {
				e.index.Set(e.indexFunc(item), item)
			}
		case *indexEntry[int64, T]:
			e.index.Clear()
			for _, item := range s.items {
				e.index.Set(e.indexFunc(item), item)
			}
		case *indexEntry[string, T]:
			e.index.Clear()
			for _, item := range s.items {
				e.index.Set(e.indexFunc(item), item)
			}
		}
	}

	// 清空并重建多值索引
	for _, entry := range s.multiIndexes {
		switch e := entry.(type) {
		case *multiIndexEntry[int32, T]:
			e.index.Clear()
			for _, item := range s.items {
				e.index.Add(e.indexFunc(item), item)
			}
		case *multiIndexEntry[int64, T]:
			e.index.Clear()
			for _, item := range s.items {
				e.index.Add(e.indexFunc(item), item)
			}
		case *multiIndexEntry[string, T]:
			e.index.Clear()
			for _, item := range s.items {
				e.index.Add(e.indexFunc(item), item)
			}
		}
	}
}

// SetItems 设置资源项目并重建索引
func (s *IndexedStore[K, T]) SetItems(items []T) error {
	if err := s.Store.SetItems(items); err != nil {
		return err
	}
	s.RebuildIndexes()
	return nil
}
