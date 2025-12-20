// Package resloader 索引构建器
package resloader

import (
	"sync"
)

// Index 通用索引接口
type Index[K comparable, V any] interface {
	Get(key K) (V, bool)
	Set(key K, value V)
	Delete(key K)
	Range(fn func(key K, value V) bool)
	Len() int
}

// UniqueIndex 唯一索引
type UniqueIndex[K comparable, V any] struct {
	mu   sync.RWMutex
	data map[K]V
}

// NewUniqueIndex 创建唯一索引
func NewUniqueIndex[K comparable, V any]() *UniqueIndex[K, V] {
	return &UniqueIndex[K, V]{
		data: make(map[K]V),
	}
}

// Get 获取值
func (idx *UniqueIndex[K, V]) Get(key K) (V, bool) {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	v, ok := idx.data[key]
	return v, ok
}

// Set 设置值
func (idx *UniqueIndex[K, V]) Set(key K, value V) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.data[key] = value
}

// Delete 删除值
func (idx *UniqueIndex[K, V]) Delete(key K) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	delete(idx.data, key)
}

// Range 遍历
func (idx *UniqueIndex[K, V]) Range(fn func(key K, value V) bool) {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	for k, v := range idx.data {
		if !fn(k, v) {
			break
		}
	}
}

// Len 获取数量
func (idx *UniqueIndex[K, V]) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	return len(idx.data)
}

// Clear 清空索引
func (idx *UniqueIndex[K, V]) Clear() {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.data = make(map[K]V)
}

// MultiIndex 多值索引(一对多)
type MultiIndex[K comparable, V any] struct {
	mu   sync.RWMutex
	data map[K][]V
}

// NewMultiIndex 创建多值索引
func NewMultiIndex[K comparable, V any]() *MultiIndex[K, V] {
	return &MultiIndex[K, V]{
		data: make(map[K][]V),
	}
}

// Get 获取值列表
func (idx *MultiIndex[K, V]) Get(key K) []V {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	items := idx.data[key]
	if items == nil {
		return nil
	}
	result := make([]V, len(items))
	copy(result, items)
	return result
}

// Add 添加值
func (idx *MultiIndex[K, V]) Add(key K, value V) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.data[key] = append(idx.data[key], value)
}

// Set 设置值列表
func (idx *MultiIndex[K, V]) Set(key K, values []V) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.data[key] = values
}

// Delete 删除键
func (idx *MultiIndex[K, V]) Delete(key K) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	delete(idx.data, key)
}

// Range 遍历
func (idx *MultiIndex[K, V]) Range(fn func(key K, values []V) bool) {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	for k, v := range idx.data {
		if !fn(k, v) {
			break
		}
	}
}

// Len 获取键数量
func (idx *MultiIndex[K, V]) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	return len(idx.data)
}

// Clear 清空索引
func (idx *MultiIndex[K, V]) Clear() {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.data = make(map[K][]V)
}

// CompositeKey2 二元复合键
type CompositeKey2[K1, K2 comparable] struct {
	Key1 K1
	Key2 K2
}

// CompositeKey3 三元复合键
type CompositeKey3[K1, K2, K3 comparable] struct {
	Key1 K1
	Key2 K2
	Key3 K3
}

// CompositeIndex2 二元复合索引
type CompositeIndex2[K1, K2 comparable, V any] struct {
	idx *UniqueIndex[CompositeKey2[K1, K2], V]
}

// NewCompositeIndex2 创建二元复合索引
func NewCompositeIndex2[K1, K2 comparable, V any]() *CompositeIndex2[K1, K2, V] {
	return &CompositeIndex2[K1, K2, V]{
		idx: NewUniqueIndex[CompositeKey2[K1, K2], V](),
	}
}

// Get 获取值
func (idx *CompositeIndex2[K1, K2, V]) Get(k1 K1, k2 K2) (V, bool) {
	return idx.idx.Get(CompositeKey2[K1, K2]{k1, k2})
}

// Set 设置值
func (idx *CompositeIndex2[K1, K2, V]) Set(k1 K1, k2 K2, value V) {
	idx.idx.Set(CompositeKey2[K1, K2]{k1, k2}, value)
}

// Delete 删除值
func (idx *CompositeIndex2[K1, K2, V]) Delete(k1 K1, k2 K2) {
	idx.idx.Delete(CompositeKey2[K1, K2]{k1, k2})
}

// Clear 清空索引
func (idx *CompositeIndex2[K1, K2, V]) Clear() {
	idx.idx.Clear()
}

// CompositeIndex3 三元复合索引
type CompositeIndex3[K1, K2, K3 comparable, V any] struct {
	idx *UniqueIndex[CompositeKey3[K1, K2, K3], V]
}

// NewCompositeIndex3 创建三元复合索引
func NewCompositeIndex3[K1, K2, K3 comparable, V any]() *CompositeIndex3[K1, K2, K3, V] {
	return &CompositeIndex3[K1, K2, K3, V]{
		idx: NewUniqueIndex[CompositeKey3[K1, K2, K3], V](),
	}
}

// Get 获取值
func (idx *CompositeIndex3[K1, K2, K3, V]) Get(k1 K1, k2 K2, k3 K3) (V, bool) {
	return idx.idx.Get(CompositeKey3[K1, K2, K3]{k1, k2, k3})
}

// Set 设置值
func (idx *CompositeIndex3[K1, K2, K3, V]) Set(k1 K1, k2 K2, k3 K3, value V) {
	idx.idx.Set(CompositeKey3[K1, K2, K3]{k1, k2, k3}, value)
}

// Delete 删除值
func (idx *CompositeIndex3[K1, K2, K3, V]) Delete(k1 K1, k2 K2, k3 K3) {
	idx.idx.Delete(CompositeKey3[K1, K2, K3]{k1, k2, k3})
}

// Clear 清空索引
func (idx *CompositeIndex3[K1, K2, K3, V]) Clear() {
	idx.idx.Clear()
}
