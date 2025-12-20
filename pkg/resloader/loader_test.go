package resloader

import (
	"testing"
)

func TestUniqueIndex(t *testing.T) {
	idx := NewUniqueIndex[int, string]()

	// Test Set and Get
	idx.Set(1, "one")
	idx.Set(2, "two")
	idx.Set(3, "three")

	val, ok := idx.Get(1)
	if !ok || val != "one" {
		t.Errorf("Expected 'one', got '%s'", val)
	}

	val, ok = idx.Get(2)
	if !ok || val != "two" {
		t.Errorf("Expected 'two', got '%s'", val)
	}

	// Test non-existent key
	_, ok = idx.Get(999)
	if ok {
		t.Error("Expected key 999 to not exist")
	}

	// Test Len
	if idx.Len() != 3 {
		t.Errorf("Expected length 3, got %d", idx.Len())
	}

	// Test Delete
	idx.Delete(2)
	_, ok = idx.Get(2)
	if ok {
		t.Error("Expected key 2 to be deleted")
	}

	// Test Clear
	idx.Clear()
	if idx.Len() != 0 {
		t.Errorf("Expected length 0 after clear, got %d", idx.Len())
	}
}

func TestMultiIndex(t *testing.T) {
	idx := NewMultiIndex[string, int]()

	// Test Add and Get
	idx.Add("a", 1)
	idx.Add("a", 2)
	idx.Add("a", 3)
	idx.Add("b", 10)

	vals := idx.Get("a")
	if len(vals) != 3 {
		t.Errorf("Expected 3 values for 'a', got %d", len(vals))
	}

	vals = idx.Get("b")
	if len(vals) != 1 || vals[0] != 10 {
		t.Errorf("Expected [10] for 'b', got %v", vals)
	}

	// Test non-existent key
	vals = idx.Get("c")
	if vals != nil {
		t.Error("Expected nil for non-existent key")
	}

	// Test Len
	if idx.Len() != 2 {
		t.Errorf("Expected length 2, got %d", idx.Len())
	}

	// Test Delete
	idx.Delete("a")
	vals = idx.Get("a")
	if vals != nil {
		t.Error("Expected 'a' to be deleted")
	}

	// Test Clear
	idx.Clear()
	if idx.Len() != 0 {
		t.Errorf("Expected length 0 after clear, got %d", idx.Len())
	}
}

func TestCompositeIndex2(t *testing.T) {
	idx := NewCompositeIndex2[int, string, float64]()

	// Test Set and Get
	idx.Set(1, "a", 1.1)
	idx.Set(1, "b", 1.2)
	idx.Set(2, "a", 2.1)

	val, ok := idx.Get(1, "a")
	if !ok || val != 1.1 {
		t.Errorf("Expected 1.1, got %f", val)
	}

	val, ok = idx.Get(1, "b")
	if !ok || val != 1.2 {
		t.Errorf("Expected 1.2, got %f", val)
	}

	val, ok = idx.Get(2, "a")
	if !ok || val != 2.1 {
		t.Errorf("Expected 2.1, got %f", val)
	}

	// Test non-existent key
	_, ok = idx.Get(2, "b")
	if ok {
		t.Error("Expected key (2, 'b') to not exist")
	}

	// Test Delete
	idx.Delete(1, "a")
	_, ok = idx.Get(1, "a")
	if ok {
		t.Error("Expected key (1, 'a') to be deleted")
	}

	// Test Clear
	idx.Clear()
	_, ok = idx.Get(1, "b")
	if ok {
		t.Error("Expected all keys to be cleared")
	}
}

func TestCompositeIndex3(t *testing.T) {
	idx := NewCompositeIndex3[int, int, string, float64]()

	// Test Set and Get
	idx.Set(1, 2, "a", 1.1)
	idx.Set(1, 2, "b", 1.2)
	idx.Set(1, 3, "a", 1.3)

	val, ok := idx.Get(1, 2, "a")
	if !ok || val != 1.1 {
		t.Errorf("Expected 1.1, got %f", val)
	}

	val, ok = idx.Get(1, 2, "b")
	if !ok || val != 1.2 {
		t.Errorf("Expected 1.2, got %f", val)
	}

	val, ok = idx.Get(1, 3, "a")
	if !ok || val != 1.3 {
		t.Errorf("Expected 1.3, got %f", val)
	}

	// Test non-existent key
	_, ok = idx.Get(1, 3, "b")
	if ok {
		t.Error("Expected key (1, 3, 'b') to not exist")
	}
}
